// Heightmap terrains, as the HTerrain plugin for Godot (zylann.hterrain, MIT): its data and range
// map (hterrain_data.gd, hterrain_data_range_map.gd), quad tree (util/quad_tree_lod.gd), chunk
// seams (hterrain.gd's _update_chunk) and mesher (hterrain_mesher.gd), its generator's
// perlin_noise and erode shaders and its brushes' shaders, ported to the CPU.
//
// HeightMap terrain for Godot Engine
// Copyright (c) 2016-2020 Marc Gilleron
//
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and
// associated documentation files (the "Software"), to deal in the Software without restriction,
// including without limitation the rights to use, copy, modify, merge, publish, distribute,
// sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all copies or
// substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
// NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

#include "slash/scene3d/terrain.hpp"

#include "slash/core/log.hpp"
#include "slash/world/world.hpp"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace slash::scene3d {
namespace {

constexpr std::string_view kField = "/components/HTerrain";
constexpr int kMinResolution = 65;
constexpr int kMaxResolution = 4097;
constexpr int kMaxTextures = 4;
constexpr float kMaxValue = 65535.0F;

std::uint64_t next_version() {
    static std::atomic<std::uint64_t> counter{0};
    return ++counter;
}

int clamp_cell(int value, int resolution) {
    return std::clamp(value, 0, resolution - 1);
}

float lerp(float a, float b, float t) {
    return a + ((b - a) * t);
}

float smoothstep(float edge0, float edge1, float x) {
    if (edge0 == edge1) {
        return x < edge0 ? 0.0F : 1.0F;
    }
    const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0F, 1.0F);
    return t * t * (3.0F - (2.0F * t));
}

Vec3 unit(Vec3 v) {
    const float l = std::sqrt((v.x * v.x) + (v.y * v.y) + (v.z * v.z));
    return l > 0.0F ? Vec3{v.x / l, v.y / l, v.z / l} : Vec3{0.0F, 1.0F, 0.0F};
}

bool valid_resolution(int resolution) {
    return resolution >= kMinResolution && resolution <= kMaxResolution &&
           std::has_single_bit(static_cast<unsigned>(resolution - 1));
}

bool valid_chunk_size(int size) {
    return size == 16 || size == 32 || size == 64;
}

std::string field(std::string_view name) {
    return std::string(kField) + "/" + std::string(name);
}

std::vector<entt::entity> terrains(const world::World& world) {
    std::vector<entt::entity> out;
    for (const entt::entity entity : world.ecs().view<const world::HTerrain>()) {
        out.push_back(entity);
    }
    std::ranges::sort(out, {}, [](entt::entity entity) { return entt::to_entity(entity); });
    return out;
}

// ---- Data

// What a terrain's data is made from: a change in any makes it again.
struct Sources {
    std::shared_ptr<const render::Gray16> heights;
    std::shared_ptr<const render::Image> splat;
    std::shared_ptr<const render::Image> color;
    std::string heights_error;
    std::string splat_error;
    std::string color_error;

    friend bool operator==(const Sources&, const Sources&) = default;
};

// The parts of an HTerrain its data is made from.
struct Settings {
    std::string heightmap;
    int resolution = 0;
    Vec2 height_range;
    std::string splatmap;
    std::string colormap;

    friend bool operator==(const Settings&, const Settings&) = default;
};

Settings settings_of(const world::HTerrain& terrain) {
    return {.heightmap = terrain.heightmap,
            .resolution = terrain.resolution,
            .height_range = terrain.height_range,
            .splatmap = terrain.splatmap,
            .colormap = terrain.colormap};
}

// A map's pointer, or why it didn't load: a path of "" or a file that doesn't exist yet is
// neither, and the map is its default.
template <typename T, typename Loader>
void load(const std::string& path, const Loader& loader, std::shared_ptr<const T>& out,
          std::string& error) {
    if (path.empty() || !loader) {
        return;
    }
    auto loaded = loader(path);
    if (loaded) {
        out = std::move(loaded).value();
    } else if (loaded.error().code != ErrorCode::NotFound) {
        error = loaded.error().message;
    }
}

Sources sources_of(const world::HTerrain& terrain, const HeightmapLoader& heightmaps,
                   const ImageLoader& images) {
    Sources sources;
    load(terrain.heightmap, heightmaps, sources.heights, sources.heights_error);
    load(terrain.splatmap, images, sources.splat, sources.splat_error);
    load(terrain.colormap, images, sources.color, sources.color_error);
    return sources;
}

std::shared_ptr<const TerrainData> make_data(const world::HTerrain& terrain, entt::entity entity,
                                             const Sources& sources) {
    auto data = std::make_shared<TerrainData>();
    data->version = next_version();
    const int resolution = terrain.resolution;
    if (!valid_resolution(resolution)) {
        data->problems.push_back({entity, field("resolution"),
                                  "is a power of two plus one from 65 to 4097, such as 513, not " +
                                      std::to_string(resolution)});
        return data;
    }
    data->resolution = resolution;
    const auto r = static_cast<std::uint32_t>(resolution);
    const std::string size = std::to_string(resolution) + "x" + std::to_string(resolution);
    const auto check_size = [&](std::uint32_t width, std::uint32_t height, std::string_view name,
                                const std::string& path) {
        if (width != r || height != r) {
            data->problems.push_back({entity, field(name),
                                      "\"" + path + "\" is " + std::to_string(width) + "x" +
                                          std::to_string(height) +
                                          " pixels, but the resolution makes the maps " + size});
        }
    };
    // Images whose texels are only compressed blocks have no pixels to read holes and weights
    // from.
    const auto check_pixels = [&](const render::Image& image, std::string_view name,
                                  const std::string& path) {
        if (image.width == r && image.height == r &&
            image.pixels.size() != std::size_t{r} * r * 4) {
            data->problems.push_back(
                {entity, field(name), "\"" + path + "\" has no pixels to read: use a PNG"});
        }
    };
    if (!sources.heights_error.empty()) {
        data->problems.push_back({entity, field("heightmap"), sources.heights_error});
    } else if (sources.heights != nullptr) {
        check_size(sources.heights->width, sources.heights->height, "heightmap", terrain.heightmap);
    }
    if (!sources.splat_error.empty()) {
        data->problems.push_back({entity, field("splatmap"), sources.splat_error});
    } else if (sources.splat != nullptr) {
        check_size(sources.splat->width, sources.splat->height, "splatmap", terrain.splatmap);
        check_pixels(*sources.splat, "splatmap", terrain.splatmap);
    }
    if (!sources.color_error.empty()) {
        data->problems.push_back({entity, field("colormap"), sources.color_error});
    } else if (sources.color != nullptr) {
        check_size(sources.color->width, sources.color->height, "colormap", terrain.colormap);
        check_pixels(*sources.color, "colormap", terrain.colormap);
    }
    if (!data->problems.empty()) {
        data->resolution = 0;
        return data;
    }
    if (sources.heights != nullptr) {
        data->heights = decode_heights(*sources.heights, terrain.height_range);
    } else {
        data->heights.assign(std::size_t{r} * r, terrain.height_range.x);
    }
    data->splatmap = sources.splat;
    data->colormap = sources.color;
    update_ranges(*data);
    return data;
}

// The data of a world's terrains, kept until they change.
struct TerrainCache {
    struct Entry {
        Settings settings;
        Sources sources;
        std::shared_ptr<const TerrainData> data;
    };

    std::mutex mutex;
    HeightmapLoader heightmaps;
    ImageLoader images;
    std::unordered_map<entt::entity, Entry> entries;

    world::MemoryPart memory() {
        const std::scoped_lock lock(mutex);
        world::MemoryPart part{"terrain", 0, {}, {}};
        std::uint64_t cells = 0;
        for (const auto& [entity, entry] : entries) {
            const TerrainData& data = *entry.data;
            part.bytes += sizeof(TerrainData) + (data.heights.capacity() * sizeof(float)) +
                          (data.ranges.capacity() * sizeof(std::array<float, 2>));
            cells += data.heights.size();
        }
        part.counts = {{"terrains", entries.size()}, {"cells", cells}};
        return part;
    }
};

TerrainCache* find_cache(const world::World& world) {
    const auto* found = world.ecs().ctx().find<std::shared_ptr<TerrainCache>>();
    return found != nullptr ? found->get() : nullptr;
}

Result<std::string> read_file(const world::World& world, const std::string& path) {
    const world::FileReader& reader = world.file_reader();
    if (!reader) {
        return Error{.code = ErrorCode::NotFound, .message = "files can't be read here"};
    }
    auto bytes = reader(path);
    if (!bytes) {
        return Error{.code = bytes.error().code,
                     .message = "\"" + path + "\" could not be read: " + bytes.error().message};
    }
    return bytes;
}

// Reads maps through the world's file reader, for worlds without install_terrain.
HeightmapLoader file_heightmaps(const world::World& world) {
    return [&world](const std::string& path) -> Result<std::shared_ptr<const render::Gray16>> {
        auto bytes = read_file(world, path);
        if (!bytes) {
            return bytes.error();
        }
        auto decoded = render::decode_gray16(bytes.value());
        if (!decoded) {
            return Error{.code = decoded.error().code,
                         .message = "\"" + path + "\": " + decoded.error().message};
        }
        return std::make_shared<const render::Gray16>(std::move(decoded).value());
    };
}

ImageLoader file_images(const world::World& world) {
    return [&world](const std::string& path) -> Result<std::shared_ptr<const render::Image>> {
        auto bytes = read_file(world, path);
        if (!bytes) {
            return bytes.error();
        }
        auto decoded = render::decode_image(bytes.value());
        if (!decoded) {
            return Error{.code = decoded.error().code,
                         .message = "\"" + path + "\": " + decoded.error().message};
        }
        return std::make_shared<const render::Image>(std::move(decoded).value());
    };
}

// ---- Noise, as HTerrain's perlin_noise shader: Stefan Gustavson's classic Perlin noise
// (webgl-noise).
//
// Copyright (C) 2011 by Ashima Arts (Simplex noise)
// Copyright (C) 2011-2016 by Stefan Gustavson (Classic noise and others)
//
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and
// associated documentation files (the "Software"), to deal in the Software without restriction,
// including without limitation the rights to use, copy, modify, merge, publish, distribute,
// sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all copies or
// substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
// NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

struct Float4 {
    float x, y, z, w;
};

float fract(float v) {
    return v - std::floor(v);
}

float mod289(float v) {
    return v - (std::floor(v * (1.0F / 289.0F)) * 289.0F);
}

float permute(float v) {
    return mod289(((v * 34.0F) + 1.0F) * v);
}

float taylor_inv_sqrt(float r) {
    return 1.79284291400159F - (0.85373472095314F * r);
}

float fade(float t) {
    return t * t * t * ((t * ((t * 6.0F) - 15.0F)) + 10.0F);
}

float cnoise(Vec2 p) {
    // Pi = floor(P.xyxy) + (0, 0, 1, 1); Pf = fract(P.xyxy) - (0, 0, 1, 1).
    const Float4 pi{mod289(std::floor(p.x)), mod289(std::floor(p.y)),
                    mod289(std::floor(p.x) + 1.0F), mod289(std::floor(p.y) + 1.0F)};
    const Float4 pf{fract(p.x), fract(p.y), fract(p.x) - 1.0F, fract(p.y) - 1.0F};
    const Float4 ix{pi.x, pi.z, pi.x, pi.z};
    const Float4 iy{pi.y, pi.y, pi.w, pi.w};
    const Float4 fx{pf.x, pf.z, pf.x, pf.z};
    const Float4 fy{pf.y, pf.y, pf.w, pf.w};
    const Float4 i{permute(permute(ix.x) + iy.x), permute(permute(ix.y) + iy.y),
                   permute(permute(ix.z) + iy.z), permute(permute(ix.w) + iy.w)};
    const auto grad = [](float v) {
        float gx = (fract(v * (1.0F / 41.0F)) * 2.0F) - 1.0F;
        const float gy = std::abs(gx) - 0.5F;
        const float tx = std::floor(gx + 0.5F);
        gx = gx - tx;
        return Vec2{gx, gy};
    };
    Vec2 g00 = grad(i.x);
    Vec2 g10 = grad(i.y);
    Vec2 g01 = grad(i.z);
    Vec2 g11 = grad(i.w);
    const auto dot = [](Vec2 a, Vec2 b) { return (a.x * b.x) + (a.y * b.y); };
    g00 = g00 * taylor_inv_sqrt(dot(g00, g00));
    g01 = g01 * taylor_inv_sqrt(dot(g01, g01));
    g10 = g10 * taylor_inv_sqrt(dot(g10, g10));
    g11 = g11 * taylor_inv_sqrt(dot(g11, g11));
    const float n00 = dot(g00, {fx.x, fy.x});
    const float n10 = dot(g10, {fx.y, fy.y});
    const float n01 = dot(g01, {fx.z, fy.z});
    const float n11 = dot(g11, {fx.w, fy.w});
    const Vec2 fade_xy{fade(pf.x), fade(pf.y)};
    const Vec2 n_x{lerp(n00, n10, fade_xy.x), lerp(n01, n11, fade_xy.x)};
    return 2.3F * lerp(n_x.x, n_x.y, fade_xy.y);
}

float fractal_noise(const TerrainNoise& noise, Vec2 uv) {
    float scale = 1.0F;
    float sum = 0.0F;
    float amp = 0.0F;
    float p = 1.0F;
    uv.x += static_cast<float>(noise.seed) * 61.0F;
    for (int i = 0; i < noise.octaves; ++i) {
        sum += p * cnoise(uv * scale);
        amp += p;
        scale *= 2.0F;
        p *= noise.roughness;
    }
    return amp > 0.0F ? sum / amp : 0.0F;
}

float smooth_union(float a, float b, float k) {
    const float h = std::clamp(0.5F + (0.5F * (b - a) / k), 0.0F, 1.0F);
    return lerp(b, a, h) - (k * h * (1.0F - h));
}

float squareish_distance(Vec2 a, Vec2 b, float r, float s) {
    const Vec2 v = b - a;
    const float da = -smooth_union(v.x + s, v.y + s, r) + s;
    const float db = -smooth_union(s - v.x, s - v.y, r) + s;
    const float dc = -smooth_union(s - v.x, v.y + s, r) + s;
    const float dd = -smooth_union(v.x + s, s - v.y, r) + s;
    return std::max(std::max(da, db), std::max(dc, dd));
}

float noise_height(const TerrainNoise& noise, Vec2 pos, float terrain_size, float noise_scale) {
    const Vec2 uv{(pos.x / terrain_size) * noise_scale, (pos.y / terrain_size) * noise_scale};
    float h = 0.5F + (0.5F * fractal_noise(noise, uv));
    // pow(h, curve), with h kept from going below 0, where GLSL's pow is undefined.
    h = h > 0.0F ? slash::exp(noise.curve * slash::ln(h)) : 0.0F;
    const Vec2 center{0.5F * terrain_size, 0.5F * terrain_size};
    const float height_ratio = 0.5F + (0.5F * noise.island_height_ratio);
    const Vec2 d = pos - center;
    const float rd = std::sqrt((d.x * d.x) + (d.y * d.y));
    const float sd = squareish_distance(pos, center, terrain_size * 0.1F, terrain_size);
    const float island_distance = lerp(rd, sd, noise.island_shape);
    const float distance_ratio = std::clamp(island_distance / (0.5F * terrain_size), 0.0F, 1.0F);
    const float island_ratio =
        noise.island_weight *
        smoothstep(std::min(0.999F, noise.island_sharpness), 1.0F, distance_ratio);
    h = lerp(h, height_ratio, island_ratio);
    return noise.base_height + (h * noise.height_range);
}

// A height between cells, bilinear, the map's edges stretched on, as a GPU samples a texture
// with linear filtering.
float sample(const std::vector<float>& heights, int resolution, float x, float z) {
    x = std::clamp(x, 0.0F, static_cast<float>(resolution - 1));
    z = std::clamp(z, 0.0F, static_cast<float>(resolution - 1));
    const int x0 = static_cast<int>(std::floor(x));
    const int z0 = static_cast<int>(std::floor(z));
    const int x1 = std::min(x0 + 1, resolution - 1);
    const int z1 = std::min(z0 + 1, resolution - 1);
    const float fx = x - static_cast<float>(x0);
    const float fz = z - static_cast<float>(z0);
    const auto at = [&](int cx, int cz) {
        return heights[(static_cast<std::size_t>(cz) * static_cast<std::size_t>(resolution)) +
                       static_cast<std::size_t>(cx)];
    };
    return lerp(lerp(at(x0, z0), at(x1, z0), fx), lerp(at(x0, z1), at(x1, z1), fx), fz);
}

// One pass of HTerrain's erode shader: a morphological erosion and dilation by a disc of radius
// 3 a third of a cell apart, mixed by weight, then by slope.
std::vector<float> erode(const std::vector<float>& heights, int resolution,
                         const TerrainNoise& noise) {
    constexpr int kRadius = 3;
    constexpr auto kR = static_cast<float>(kRadius);
    const float eps = 1.0F / (0.99F * kR);
    const float pixel = 1.0F / static_cast<float>(resolution);
    std::vector<float> out(heights.size());
    for (int z = 0; z < resolution; ++z) {
        for (int x = 0; x < resolution; ++x) {
            const auto fx = static_cast<float>(x);
            const auto fz = static_cast<float>(z);
            const std::size_t index =
                (static_cast<std::size_t>(z) * static_cast<std::size_t>(resolution)) +
                static_cast<std::size_t>(x);
            const float h = heights[index];
            float eh = h;
            float dh = h;
            for (int iy = -kRadius; iy <= kRadius; ++iy) {
                for (int ix = -kRadius; ix <= kRadius; ++ix) {
                    const auto px = static_cast<float>(ix);
                    const auto py = static_cast<float>(iy);
                    const float nh = sample(heights, resolution, fx + (px * eps), fz + (py * eps));
                    const float length = std::sqrt((px * px) + (py * py));
                    eh = std::min(eh, nh + std::max(length - kR, 0.0F));
                    dh = std::max(dh, nh + std::min(kR - length, 0.0F));
                }
            }
            eh = lerp(h, eh, noise.erosion_weight);
            dh = lerp(h, dh, noise.erosion_weight);
            float ph = lerp(eh, dh, noise.dilation);
            if (noise.erosion_slope_factor > 0.0F) {
                const float left = sample(heights, resolution, fx - 1.0F, fz);
                const float right = sample(heights, resolution, fx + 1.0F, fz);
                const float top = sample(heights, resolution, fx, fz + 1.0F);
                const float bottom = sample(heights, resolution, fx, fz - 1.0F);
                const Vec3 normal = unit({left - right, pixel + pixel, bottom - top});
                const Vec3 up =
                    unit({noise.erosion_slope_direction.x, 1.0F, noise.erosion_slope_direction.y});
                float f = std::max((normal.x * up.x) + (normal.y * up.y) + (normal.z * up.z), 0.0F);
                if (noise.erosion_slope_invert) {
                    f = 1.0F - f;
                }
                ph = lerp(h, ph, lerp(1.0F, f, noise.erosion_slope_factor));
            }
            out[index] = ph;
        }
    }
    return out;
}

// ---- Brushes

float brush_value(const BrushDab& dab, int x, int z) {
    const float dx = static_cast<float>(x) - dab.center.x;
    const float dz = static_cast<float>(z) - dab.center.y;
    const float d = std::sqrt((dx * dx) + (dz * dz));
    if (d >= dab.radius) {
        return 0.0F;
    }
    const float inner = dab.radius * (1.0F - std::clamp(dab.falloff, 0.0F, 1.0F));
    return dab.opacity * (d <= inner ? 1.0F : smoothstep(dab.radius, inner, d));
}

// apply_slope_limit of HTerrain's splat4 and color shaders.
float slope_limit(float value, float normal_y, float min_y, float max_y) {
    constexpr float kFalloff = 0.2F;
    min_y = min_y <= 0.0F ? -2.0F : min_y;
    max_y = max_y >= 1.0F ? 2.0F : max_y;
    value *= 1.0F - smoothstep(max_y - kFalloff, max_y + kFalloff, normal_y);
    value *= smoothstep(min_y - kFalloff, min_y + kFalloff, normal_y);
    return value;
}

std::uint8_t to_byte(float v) {
    return static_cast<std::uint8_t>(std::lround(std::clamp(v, 0.0F, 1.0F) * 255.0F));
}

float from_byte(std::uint8_t v) {
    return static_cast<float>(v) / 255.0F;
}

} // namespace

// ---- Data

void add_heightmap_type(asset::Assets& assets) {
    assets.add_type<render::Gray16>(
        "heightmap", {std::string(kHeightmapSuffix)},
        [](std::string_view bytes, std::string_view /*path*/) {
            return render::decode_gray16(bytes);
        },
        [](const render::Gray16& map) {
            return sizeof(render::Gray16) + (map.values.capacity() * sizeof(std::uint16_t));
        });
}

float TerrainData::height(int x, int z) const {
    if (heights.empty()) {
        return 0.0F;
    }
    x = clamp_cell(x, resolution);
    z = clamp_cell(z, resolution);
    return heights[(static_cast<std::size_t>(z) * static_cast<std::size_t>(resolution)) +
                   static_cast<std::size_t>(x)];
}

float TerrainData::interpolated_height(float x, float z) const {
    const auto x0 = static_cast<int>(std::floor(x));
    const auto z0 = static_cast<int>(std::floor(z));
    const float xf = x - static_cast<float>(x0);
    const float zf = z - static_cast<float>(z0);
    const float h00 = height(x0, z0);
    const float h10 = height(x0 + 1, z0);
    const float h01 = height(x0, z0 + 1);
    const float h11 = height(x0 + 1, z0 + 1);
    return lerp(lerp(h00, h10, xf), lerp(h01, h11, xf), zf);
}

bool TerrainData::hole(int x, int z) const {
    if (colormap == nullptr || x < 0 || z < 0 || x >= resolution || z >= resolution) {
        return false;
    }
    const std::size_t pixel = (static_cast<std::size_t>(z) * static_cast<std::size_t>(resolution)) +
                              static_cast<std::size_t>(x);
    return colormap->pixels[(pixel * 4) + 3] < 128;
}

std::array<float, 2> TerrainData::point_range(int x, int z) const {
    if (ranges.empty()) {
        return {0.0F, 0.0F};
    }
    const int cx = std::clamp(x / kTerrainRangeBlock, 0, range_size - 1);
    const int cz = std::clamp(z / kTerrainRangeBlock, 0, range_size - 1);
    return ranges[(static_cast<std::size_t>(cz) * static_cast<std::size_t>(range_size)) +
                  static_cast<std::size_t>(cx)];
}

void update_ranges(TerrainData& data) {
    const int r = data.resolution;
    data.range_size = r / kTerrainRangeBlock;
    data.ranges.assign(static_cast<std::size_t>(data.range_size) *
                           static_cast<std::size_t>(data.range_size),
                       {0.0F, 0.0F});
    data.lowest = std::numeric_limits<float>::max();
    data.highest = std::numeric_limits<float>::lowest();
    for (int bz = 0; bz < data.range_size; ++bz) {
        for (int bx = 0; bx < data.range_size; ++bx) {
            // Blocks share their edge cells: each is kTerrainRangeBlock + 1 across.
            const int x0 = bx * kTerrainRangeBlock;
            const int z0 = bz * kTerrainRangeBlock;
            const int x1 = std::min(x0 + kTerrainRangeBlock + 1, r);
            const int z1 = std::min(z0 + kTerrainRangeBlock + 1, r);
            float low = data.height(x0, z0);
            float high = low;
            for (int z = z0; z < z1; ++z) {
                for (int x = x0; x < x1; ++x) {
                    const float h = data.height(x, z);
                    low = std::min(low, h);
                    high = std::max(high, h);
                }
            }
            data.ranges[(static_cast<std::size_t>(bz) * static_cast<std::size_t>(data.range_size)) +
                        static_cast<std::size_t>(bx)] = {low, high};
        }
    }
    for (const float h : data.heights) {
        data.lowest = std::min(data.lowest, h);
        data.highest = std::max(data.highest, h);
    }
    if (data.heights.empty()) {
        data.lowest = 0.0F;
        data.highest = 0.0F;
    }
}

std::vector<float> decode_heights(const render::Gray16& values, Vec2 range) {
    std::vector<float> heights(values.values.size());
    const float span = range.y - range.x;
    for (std::size_t i = 0; i < heights.size(); ++i) {
        heights[i] = range.x + (static_cast<float>(values.values[i]) / kMaxValue * span);
    }
    return heights;
}

render::Gray16 encode_heights(const std::vector<float>& heights, int resolution, Vec2 range) {
    render::Gray16 out;
    out.width = static_cast<std::uint32_t>(resolution);
    out.height = static_cast<std::uint32_t>(resolution);
    out.values.resize(heights.size());
    const float span = range.y - range.x;
    for (std::size_t i = 0; i < heights.size(); ++i) {
        const float t = span != 0.0F ? (heights[i] - range.x) / span : 0.0F;
        out.values[i] =
            static_cast<std::uint16_t>(std::lround(std::clamp(t, 0.0F, 1.0F) * kMaxValue));
    }
    return out;
}

std::shared_ptr<const TerrainData> terrain_data(const world::World& world, entt::entity entity) {
    const auto* terrain =
        world.valid(entity) ? world.ecs().try_get<const world::HTerrain>(entity) : nullptr;
    if (terrain == nullptr) {
        return std::make_shared<const TerrainData>();
    }
    TerrainCache* cache = find_cache(world);
    if (cache == nullptr) {
        return make_data(*terrain, entity,
                         sources_of(*terrain, file_heightmaps(world), file_images(world)));
    }
    const std::scoped_lock lock(cache->mutex);
    Settings settings = settings_of(*terrain);
    Sources sources = sources_of(*terrain, cache->heightmaps, cache->images);
    const auto found = cache->entries.find(entity);
    if (found != cache->entries.end() && found->second.settings == settings &&
        found->second.sources == sources) {
        return found->second.data;
    }
    std::shared_ptr<const TerrainData> data = make_data(*terrain, entity, sources);
    // Terrains that are gone go with it.
    std::erase_if(cache->entries, [&](const auto& entry) {
        return !world.valid(entry.first) || !world.ecs().all_of<world::HTerrain>(entry.first);
    });
    cache->entries.insert_or_assign(entity, TerrainCache::Entry{.settings = std::move(settings),
                                                                .sources = std::move(sources),
                                                                .data = data});
    return data;
}

void install_terrain(world::World& world, HeightmapLoader heightmaps, ImageLoader images) {
    if (TerrainCache* cache = find_cache(world); cache != nullptr) {
        const std::scoped_lock lock(cache->mutex);
        cache->heightmaps = std::move(heightmaps);
        cache->images = std::move(images);
        cache->entries.clear();
        return;
    }
    auto cache = std::make_shared<TerrainCache>();
    cache->heightmaps = std::move(heightmaps);
    cache->images = std::move(images);
    world.ecs().ctx().emplace<std::shared_ptr<TerrainCache>>(cache);
    world.add_reset([cache](world::World& /*world*/) {
        const std::scoped_lock lock(cache->mutex);
        cache->entries.clear();
    });
    world.add_memory_source("terrain",
                            [cache](const world::World& /*world*/) { return cache->memory(); });
}

world::Affine3D terrain_transform(const world::HTerrain& terrain, int resolution,
                                  const world::Affine3D& global) {
    world::Affine3D internal = global;
    internal.x = global.x * terrain.map_scale.x;
    internal.y = global.y * terrain.map_scale.y;
    internal.z = global.z * terrain.map_scale.z;
    if (terrain.centered && resolution > 0) {
        const float half = 0.5F * (static_cast<float>(resolution) - 1.0F);
        internal.origin = internal.origin + internal.basis({-half, 0.0F, -half});
    }
    return internal;
}

int terrain_lod_count(int resolution, int chunk_size) {
    int count = 0;
    while (resolution > chunk_size && chunk_size > 0) {
        resolution >>= 1;
        ++count;
    }
    return count;
}

std::vector<TerrainChunk> terrain_chunks(const TerrainData& data, int chunk_size, float lod_scale,
                                         Vec3 viewer) {
    std::vector<TerrainChunk> leaves;
    if (data.empty() || !valid_chunk_size(chunk_size)) {
        return leaves;
    }
    const int max_lod = terrain_lod_count(data.resolution, chunk_size);
    const float split_scale = std::clamp(lod_scale, 2.0F, 5.0F);
    const auto visit = [&](const auto& self, int qx, int qz, int lod) -> void {
        const int factor = 1 << lod;
        const int size = chunk_size * factor;
        const auto fsize = static_cast<float>(size);
        const std::array<float, 2> bounds =
            data.point_range((qx * size) + (size / 2), (qz * size) + (size / 2));
        const Vec3 center{fsize * (static_cast<float>(qx) + 0.5F), (bounds[0] + bounds[1]) / 2.0F,
                          fsize * (static_cast<float>(qz) + 0.5F)};
        const Vec3 d = center - viewer;
        const float distance = std::sqrt((d.x * d.x) + (d.y * d.y) + (d.z * d.z));
        const float split = static_cast<float>(chunk_size * factor) * split_scale;
        if (lod > 0 && distance < split) {
            for (int i = 0; i < 4; ++i) {
                self(self, (qx * 2) + (i & 1), (qz * 2) + ((i & 2) >> 1), lod - 1);
            }
            return;
        }
        leaves.push_back({.x = qx, .z = qz, .lod = lod, .seams = 0});
    };
    visit(visit, 0, 0, max_lod);
    // Seams toward coarser neighbors, as _update_chunk finds them: the chunk a level up in each
    // direction, when it is another one than this chunk's own and is drawn.
    const int grid = data.resolution / chunk_size;
    const auto drawn = [&](int x, int z, int lod) {
        const int size = grid >> lod;
        if (x < 0 || z < 0 || x >= size || z >= size) {
            return false;
        }
        return std::ranges::any_of(leaves, [&](const TerrainChunk& leaf) {
            return leaf.x == x && leaf.z == z && leaf.lod == lod;
        });
    };
    constexpr std::array<std::array<int, 2>, 4> kDirections{{{-1, 0}, {1, 0}, {0, -1}, {0, 1}}};
    for (TerrainChunk& chunk : leaves) {
        // Integer division truncates, as GDScript's does: the chunk at -1 is up at 0.
        const int lower_x = chunk.x / 2;
        const int lower_z = chunk.z / 2;
        for (std::size_t d = 0; d < kDirections.size(); ++d) {
            const int nx = (chunk.x + kDirections.at(d)[0]) / 2;
            const int nz = (chunk.z + kDirections.at(d)[1]) / 2;
            if ((nx != lower_x || nz != lower_z) && drawn(nx, nz, chunk.lod + 1)) {
                chunk.seams |= 1 << d;
            }
        }
    }
    return leaves;
}

std::vector<std::uint32_t> terrain_indices(int quads_x, int quads_z, int seams) {
    std::vector<std::uint32_t> out;
    if (quads_x < 1 || quads_z < 1) {
        return out;
    }
    // Seams can't be made on odd chunk sizes.
    if (quads_x % 2 != 0 || quads_z % 2 != 0) {
        seams = 0;
    }
    const auto push = [&](int a, int b, int c) {
        out.push_back(static_cast<std::uint32_t>(a));
        out.push_back(static_cast<std::uint32_t>(b));
        out.push_back(static_cast<std::uint32_t>(c));
    };
    int reg_origin_x = 0;
    int reg_origin_y = 0;
    int reg_size_x = quads_x;
    int reg_size_y = quads_z;
    int reg_hstride = 1;
    if ((seams & kSeamLeft) != 0) {
        reg_origin_x += 1;
        reg_size_x -= 1;
        reg_hstride += 1;
    }
    if ((seams & kSeamBottom) != 0) {
        reg_origin_y += 1;
        reg_size_y -= 1;
    }
    if ((seams & kSeamRight) != 0) {
        reg_size_x -= 1;
        reg_hstride += 1;
    }
    if ((seams & kSeamTop) != 0) {
        reg_size_y -= 1;
    }
    // Regular triangles, their diagonals flipped in a checker pattern.
    int ii = reg_origin_x + (reg_origin_y * (quads_x + 1));
    for (int y = 0; y < reg_size_y; ++y) {
        for (int x = 0; x < reg_size_x; ++x) {
            const int i00 = ii;
            const int i10 = ii + 1;
            const int i01 = ii + quads_x + 1;
            const int i11 = i01 + 1;
            const bool flip = ((x + reg_origin_x) + ((y + reg_origin_y) % 2)) % 2 != 0;
            if (flip) {
                push(i00, i10, i01);
                push(i10, i11, i01);
            } else {
                push(i00, i11, i01);
                push(i00, i10, i11);
            }
            ++ii;
        }
        ii += reg_hstride;
    }
    if ((seams & kSeamLeft) != 0) {
        int i = 0;
        const int n = quads_z / 2;
        for (int j = 0; j < n; ++j) {
            const int i0 = i;
            const int i1 = i + 1;
            const int i3 = i + quads_x + 2;
            const int i4 = i + (2 * (quads_x + 1));
            const int i5 = i4 + 1;
            push(i0, i3, i4);
            if (j != 0 || (seams & kSeamBottom) == 0) {
                push(i0, i1, i3);
            }
            if (j != n - 1 || (seams & kSeamTop) == 0) {
                push(i3, i5, i4);
            }
            i = i4;
        }
    }
    if ((seams & kSeamRight) != 0) {
        int i = quads_x - 1;
        const int n = quads_z / 2;
        for (int j = 0; j < n; ++j) {
            const int i0 = i;
            const int i1 = i + 1;
            const int i2 = i + quads_x + 1;
            const int i4 = i + (2 * (quads_x + 1));
            const int i5 = i4 + 1;
            push(i1, i5, i2);
            if (j != 0 || (seams & kSeamBottom) == 0) {
                push(i0, i1, i2);
            }
            if (j != n - 1 || (seams & kSeamTop) == 0) {
                push(i2, i5, i4);
            }
            i = i4;
        }
    }
    if ((seams & kSeamBottom) != 0) {
        int i = 0;
        const int n = quads_x / 2;
        for (int j = 0; j < n; ++j) {
            const int i0 = i;
            const int i2 = i + 2;
            const int i3 = i + quads_x + 1;
            const int i4 = i3 + 1;
            const int i5 = i4 + 1;
            push(i0, i2, i4);
            if (j != 0 || (seams & kSeamLeft) == 0) {
                push(i0, i4, i3);
            }
            if (j != n - 1 || (seams & kSeamRight) == 0) {
                push(i2, i5, i4);
            }
            i = i2;
        }
    }
    if ((seams & kSeamTop) != 0) {
        int i = (quads_z - 1) * (quads_x + 1);
        const int n = quads_x / 2;
        for (int j = 0; j < n; ++j) {
            const int i0 = i;
            const int i1 = i + 1;
            const int i2 = i + 2;
            const int i3 = i + quads_x + 1;
            const int i5 = i3 + 2;
            push(i3, i1, i5);
            if (j != 0 || (seams & kSeamLeft) == 0) {
                push(i0, i1, i3);
            }
            if (j != n - 1 || (seams & kSeamRight) == 0) {
                push(i1, i2, i5);
            }
            i = i2;
        }
    }
    return out;
}

std::vector<render::Vertex3D> terrain_chunk_vertices(const TerrainData& data, int chunk_size,
                                                     const TerrainChunk& chunk) {
    std::vector<render::Vertex3D> out;
    if (data.empty() || chunk_size < 1) {
        return out;
    }
    const int stride = 1 << chunk.lod;
    const int origin_x = chunk.x * chunk_size * stride;
    const int origin_z = chunk.z * chunk_size * stride;
    const auto r = static_cast<float>(data.resolution);
    out.reserve(static_cast<std::size_t>(chunk_size + 1) *
                static_cast<std::size_t>(chunk_size + 1));
    for (int z = 0; z <= chunk_size; ++z) {
        for (int x = 0; x <= chunk_size; ++x) {
            const int cx = origin_x + (x * stride);
            const int cz = origin_z + (z * stride);
            render::Vertex3D vertex;
            vertex.position = {static_cast<float>(cx), data.height(cx, cz), static_cast<float>(cz)};
            vertex.uv = {(static_cast<float>(cx) + 0.5F) / r, (static_cast<float>(cz) + 0.5F) / r};
            out.push_back(vertex);
        }
    }
    return out;
}

namespace {

Vec3 cell_normal(const TerrainData& data, Vec3 map_scale, int x, int z) {
    const float left = data.height(x - 1, z);
    const float right = data.height(x + 1, z);
    const float back = data.height(x, z - 1);
    const float front = data.height(x, z + 1);
    return unit({(left - right) / map_scale.x, 2.0F / map_scale.y, (back - front) / map_scale.z});
}

} // namespace

render::Image terrain_normals(const TerrainData& data, Vec3 map_scale) {
    render::Image image;
    if (data.empty()) {
        return image;
    }
    const int r = data.resolution;
    image.width = static_cast<std::uint32_t>(r);
    image.height = static_cast<std::uint32_t>(r);
    image.pixels.resize(static_cast<std::size_t>(r) * static_cast<std::size_t>(r) * 4);
    std::size_t i = 0;
    for (int z = 0; z < r; ++z) {
        for (int x = 0; x < r; ++x) {
            const Vec3 n = cell_normal(data, map_scale, x, z);
            image.pixels[i++] = to_byte((n.x * 0.5F) + 0.5F);
            image.pixels[i++] = to_byte((n.y * 0.5F) + 0.5F);
            image.pixels[i++] = to_byte((n.z * 0.5F) + 0.5F);
            image.pixels[i++] = 255;
        }
    }
    return image;
}

Vec3 terrain_normal(const TerrainData& data, Vec3 map_scale, float x, float z) {
    if (data.empty()) {
        return {0.0F, 1.0F, 0.0F};
    }
    const auto x0 = static_cast<int>(std::floor(x));
    const auto z0 = static_cast<int>(std::floor(z));
    const float xf = x - static_cast<float>(x0);
    const float zf = z - static_cast<float>(z0);
    const auto at = [&](int cx, int cz) {
        return cell_normal(data, map_scale, clamp_cell(cx, data.resolution),
                           clamp_cell(cz, data.resolution));
    };
    const Vec3 a = at(x0, z0);
    const Vec3 b = at(x0 + 1, z0);
    const Vec3 c = at(x0, z0 + 1);
    const Vec3 d = at(x0 + 1, z0 + 1);
    const auto mix = [](Vec3 p, Vec3 q, float t) { return p + ((q - p) * t); };
    return unit(mix(mix(a, b, xf), mix(c, d, xf), zf));
}

std::vector<Vec3> terrain_faces(const TerrainData& data) {
    std::vector<Vec3> faces;
    if (data.empty()) {
        return faces;
    }
    const int r = data.resolution;
    faces.reserve(static_cast<std::size_t>(r - 1) * static_cast<std::size_t>(r - 1) * 6);
    const auto point = [&](int x, int z) {
        return Vec3{static_cast<float>(x), data.height(x, z), static_cast<float>(z)};
    };
    for (int z = 0; z < r - 1; ++z) {
        for (int x = 0; x < r - 1; ++x) {
            // Godot's HeightMapShape3D parser's two triangles of each quad.
            if (!data.hole(x, z) && !data.hole(x + 1, z) && !data.hole(x, z + 1)) {
                faces.push_back(point(x, z));
                faces.push_back(point(x + 1, z));
                faces.push_back(point(x, z + 1));
            }
            if (!data.hole(x + 1, z) && !data.hole(x + 1, z + 1) && !data.hole(x, z + 1)) {
                faces.push_back(point(x + 1, z));
                faces.push_back(point(x + 1, z + 1));
                faces.push_back(point(x, z + 1));
            }
        }
    }
    return faces;
}

std::vector<TerrainBody> terrain_bodies(const world::World& world) {
    std::vector<TerrainBody> bodies;
    const auto& ecs = world.ecs();
    for (const entt::entity entity : terrains(world)) {
        const auto& terrain = ecs.get<const world::HTerrain>(entity);
        if (!terrain.collision_enabled) {
            continue;
        }
        std::shared_ptr<const TerrainData> data = terrain_data(world, entity);
        if (data->empty()) {
            continue;
        }
        bodies.push_back({.entity = entity,
                          .layer = terrain.collision_layer,
                          .mask = terrain.collision_mask,
                          .friction = terrain.friction,
                          .bounce = terrain.bounce,
                          .data = std::move(data),
                          .map_scale = terrain.map_scale,
                          .centered = terrain.centered});
    }
    return bodies;
}

std::vector<Problem> check_terrains(const world::World& world) {
    std::vector<Problem> problems;
    const auto& ecs = world.ecs();
    for (const entt::entity entity : terrains(world)) {
        const auto& terrain = ecs.get<const world::HTerrain>(entity);
        if (!valid_chunk_size(terrain.chunk_size)) {
            problems.push_back({entity, field("chunk_size"),
                                "is 16, 32 or 64, not " + std::to_string(terrain.chunk_size)});
        }
        if (terrain.textures.size() > kMaxTextures) {
            problems.push_back({entity, field("textures"),
                                "has " + std::to_string(terrain.textures.size()) +
                                    " textures; a terrain shades with at most 4"});
        }
        if (ecs.any_of<world::RigidBody3D, world::CharacterBody3D, world::AnimatableBody3D,
                       world::StaticBody3D, world::Area3D>(entity)) {
            problems.push_back({entity, std::string(kField),
                                "is on a physics body, so it doesn't collide: put the HTerrain "
                                "on an entity of its own"});
        }
        const std::shared_ptr<const TerrainData> data = terrain_data(world, entity);
        problems.insert(problems.end(), data->problems.begin(), data->problems.end());
    }
    return problems;
}

// ---- Making and painting heights

std::vector<float> generate_heights(const TerrainNoise& noise, int resolution,
                                    const std::vector<float>* additive) {
    std::vector<float> heights(static_cast<std::size_t>(resolution) *
                               static_cast<std::size_t>(resolution));
    const auto size = static_cast<float>(resolution);
    // HTerrain's dialog turns its scale into the shader's: 512 / scale, as its 512 pixel
    // viewport spans the terrain.
    constexpr float kViewport = 512.0F;
    const float noise_scale = std::abs(noise.scale) < 0.01F ? 0.0F : kViewport / noise.scale;
    std::size_t i = 0;
    for (int z = 0; z < resolution; ++z) {
        for (int x = 0; x < resolution; ++x) {
            const Vec2 pos{static_cast<float>(x) + noise.offset.x,
                           static_cast<float>(z) + noise.offset.y};
            float h = noise_height(noise, pos, size, noise_scale);
            if (additive != nullptr && i < additive->size()) {
                h += (*additive)[i];
            }
            heights[i++] = h;
        }
    }
    for (int step = 0; step < noise.erosion_steps; ++step) {
        heights = erode(heights, resolution, noise);
    }
    return heights;
}

std::array<int, 4> brush_rect(const BrushDab& dab, int resolution) {
    const int x0 = std::max(0, static_cast<int>(std::floor(dab.center.x - dab.radius)));
    const int z0 = std::max(0, static_cast<int>(std::floor(dab.center.y - dab.radius)));
    const int x1 = std::min(resolution, static_cast<int>(std::ceil(dab.center.x + dab.radius)) + 1);
    const int z1 = std::min(resolution, static_cast<int>(std::ceil(dab.center.y + dab.radius)) + 1);
    if (x0 >= x1 || z0 >= z1) {
        return {0, 0, 0, 0};
    }
    return {x0, z0, x1, z1};
}

std::size_t paint(TerrainMaps& maps, const BrushDab& dab) {
    const int r = maps.resolution;
    const std::array<int, 4> rect = brush_rect(dab, r);
    if (rect[0] == rect[2] || dab.radius <= 0.0F) {
        return 0;
    }
    const std::vector<float> source = maps.heights;
    const auto index = [r](int x, int z) {
        return (static_cast<std::size_t>(z) * static_cast<std::size_t>(r)) +
               static_cast<std::size_t>(x);
    };
    const auto height = [&](int x, int z) {
        return source[index(clamp_cell(x, r), clamp_cell(z, r))];
    };
    // Level's target: the mean of 5 x 5 heights across the brush's square.
    float level = 0.0F;
    if (dab.mode == BrushMode::Level) {
        for (int i = 0; i < 5; ++i) {
            for (int j = 0; j < 5; ++j) {
                const float x = lerp(dab.center.x - dab.radius, dab.center.x + dab.radius,
                                     static_cast<float>(i) / 4.0F);
                const float z = lerp(dab.center.y - dab.radius, dab.center.y + dab.radius,
                                     static_cast<float>(j) / 4.0F);
                level += sample(source, r, x, z);
            }
        }
        level /= 25.0F;
    }
    // Slopes are the ground's: a height step over a cell map_scale stretches.
    const auto stretch = [](float up, float across) { return across > 0.0F ? up / across : 1.0F; };
    const float slope_x = stretch(dab.map_scale.y, dab.map_scale.x);
    const float slope_z = stretch(dab.map_scale.y, dab.map_scale.z);
    std::size_t changed = 0;
    for (int z = rect[1]; z < rect[3]; ++z) {
        for (int x = rect[0]; x < rect[2]; ++x) {
            float value = brush_value(dab, x, z);
            if (value <= 0.0F) {
                continue;
            }
            const std::size_t at = index(x, z);
            const float h = source[at];
            float out = 0.0F;
            switch (dab.mode) {
            case BrushMode::Raise:
                out = h + (dab.amount * value);
                break;
            case BrushMode::Lower:
                out = h - (dab.amount * value);
                break;
            case BrushMode::Smooth:
                out = lerp(h,
                           (h + height(x - 1, z) + height(x + 1, z) + height(x, z - 1) +
                            height(x, z + 1)) *
                               0.2F,
                           value);
                break;
            case BrushMode::Flatten:
                out = lerp(h, dab.height, value);
                break;
            case BrushMode::Level:
                out = lerp(h, level, value);
                break;
            case BrushMode::Splat:
            case BrushMode::Color: {
                const Vec3 normal = unit({(height(x - 1, z) - height(x + 1, z)) * slope_x, 2.0F,
                                          (height(x, z + 1) - height(x, z - 1)) * slope_z});
                value = slope_limit(value, normal.y, dab.slope_min, dab.slope_max);
                std::uint8_t* pixel =
                    dab.mode == BrushMode::Splat ? &maps.splat[at * 4] : &maps.color[at * 4];
                const std::array<std::uint8_t, 4> before{pixel[0], pixel[1], pixel[2], pixel[3]};
                if (dab.mode == BrushMode::Splat) {
                    std::array<float, 4> s{};
                    float sum = 0.0F;
                    for (std::size_t c = 0; c < s.size(); ++c) {
                        const float target = static_cast<int>(c) == dab.texture ? 1.0F : 0.0F;
                        s.at(c) = lerp(from_byte(pixel[c]), target, value);
                        sum += s.at(c);
                    }
                    for (std::size_t c = 0; c < s.size(); ++c) {
                        pixel[c] = to_byte(sum > 0.0F ? s.at(c) / sum : 0.0F);
                    }
                } else {
                    const std::array<float, 3> color{dab.color.x, dab.color.y, dab.color.z};
                    for (std::size_t c = 0; c < color.size(); ++c) {
                        pixel[c] = to_byte(lerp(from_byte(pixel[c]), color.at(c), value));
                    }
                }
                if (before != std::array<std::uint8_t, 4>{pixel[0], pixel[1], pixel[2], pixel[3]}) {
                    ++changed;
                }
                continue;
            }
            case BrushMode::Hole:
            case BrushMode::Unhole: {
                std::uint8_t& alpha = maps.color[(at * 4) + 3];
                const std::uint8_t before = alpha;
                alpha = to_byte(
                    lerp(from_byte(alpha), dab.mode == BrushMode::Hole ? 0.0F : 1.0F, value));
                changed += alpha != before ? 1 : 0;
                continue;
            }
            }
            if (out != h) {
                maps.heights[at] = out;
                ++changed;
            }
        }
    }
    return changed;
}

} // namespace slash::scene3d
