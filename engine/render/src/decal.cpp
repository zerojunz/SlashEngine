// Decals, as Godot 4.6's Forward+ renderer draws them: the atlas of their textures as
// TextureStorage::update_decal_atlas packs it, the decals as update_decal_buffer places them, and
// the CPU twin of the decal loop of scene_forward_clustered.glsl, which shaders/mesh.hlsl's
// apply_decals follows. Godot samples the atlas with the GPU's derivatives of the pixel's place;
// both twins work them out from the surface's plane instead, so that every device picks the same
// levels. Float math, built with -ffp-contract=off.

#include "decal.hpp"

#include "mesh_shading.hpp"
#include "post_shading.hpp"

#include "slash/core/json.hpp"
#include "slash/core/math.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <memory>
#include <numbers>
#include <string>
#include <string_view>
#include <vector>

namespace slash::render {

DecalFilter decal_filter(const json::Value& rendering) {
    const json::Value* found = rendering.is_object() ? rendering.find("textures") : nullptr;
    found = found != nullptr && found->is_object() ? found->find("decals") : nullptr;
    found = found != nullptr && found->is_object() ? found->find("filter") : nullptr;
    if (found != nullptr && found->is_string()) {
        for (std::size_t i = 0; i < kDecalFilterNames.size(); ++i) {
            if (found->as_string() == kDecalFilterNames.at(i)) {
                return static_cast<DecalFilter>(i);
            }
        }
    }
    return DecalFilter::LinearMipmaps;
}

} // namespace slash::render

namespace slash::render::detail {

namespace {

float dot3(const Float3& a, const Float3& b) {
    return (a[0] * b[0]) + (a[1] * b[1]) + (a[2] * b[2]);
}

float dot4(const Float4& a, const Float4& b) {
    return (a[0] * b[0]) + (a[1] * b[1]) + (a[2] * b[2]) + (a[3] * b[3]);
}

Float3 xyz(const Float4& v) {
    return {v[0], v[1], v[2]};
}

Float3 scale(const Float3& a, float s) {
    return {a[0] * s, a[1] * s, a[2] * s};
}

Float3 add(const Float3& a, const Float3& b) {
    return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
}

Float3 sub(const Float3& a, const Float3& b) {
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}

Float3 cross(const Float3& a, const Float3& b) {
    return {(a[1] * b[2]) - (a[2] * b[1]), (a[2] * b[0]) - (a[0] * b[2]),
            (a[0] * b[1]) - (a[1] * b[0])};
}

Float3 normalize(const Float3& a) {
    return scale(a, 1.0F / std::sqrt(dot3(a, a)));
}

float lerp(float a, float b, float t) {
    return a + ((b - a) * t);
}

Float3 lerp3(const Float3& a, const Float3& b, float t) {
    return {lerp(a[0], b[0], t), lerp(a[1], b[1], t), lerp(a[2], b[2], t)};
}

Float4 lerp4(const Float4& a, const Float4& b, float t) {
    return {lerp(a[0], b[0], t), lerp(a[1], b[1], t), lerp(a[2], b[2], t), lerp(a[3], b[3], t)};
}

float smoothstep(float edge0, float edge1, float x) {
    const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0F, 1.0F);
    return t * t * (3.0F - (2.0F * t));
}

bool finite(std::initializer_list<float> values) {
    return std::ranges::all_of(values, [](float value) { return std::isfinite(value); });
}

// decal_fade in shaders/mesh.hlsl: x to the power y, as Godot's pow, with 0 to the power 0 and
// anything to the power 0 taken as 1 and 0 to any other power as 0.
float fade_power(float x, float y) {
    if (y == 0.0F) {
        return 1.0F;
    }
    return x > 0.0F ? slash::exp(y * slash::ln(x)) : 0.0F;
}

std::array<std::uint32_t, 2> image_size(const Image& image) {
    return {image.width, image.height};
}

// A level of an atlas: where its texels start and its size.
struct AtlasLevel {
    std::size_t offset = 0;
    std::uint32_t width = 1;
    std::uint32_t height = 1;
};

AtlasLevel atlas_level(std::uint32_t width, std::uint32_t height, std::uint32_t level) {
    AtlasLevel out;
    for (std::uint32_t i = 0; i < level; ++i) {
        out.offset += std::size_t{std::max(width >> i, 1U)} * std::max(height >> i, 1U);
    }
    out.width = std::max(width >> level, 1U);
    out.height = std::max(height >> level, 1U);
    return out;
}

// decal_texel in shaders/mesh.hlsl: a texel of a level, clamped to its edges, decoded from sRGB
// for the albedo and emission textures.
Float4 decal_texel(const DecalAtlas& atlas, const AtlasLevel& level, std::int64_t x, std::int64_t y,
                   bool srgb) {
    x = std::clamp<std::int64_t>(x, 0, std::int64_t{level.width} - 1);
    y = std::clamp<std::int64_t>(y, 0, std::int64_t{level.height} - 1);
    const std::uint32_t word =
        atlas.texels[level.offset + (static_cast<std::size_t>(y) * level.width) +
                     static_cast<std::size_t>(x)];
    Float4 texel{static_cast<float>(word & 0xFFU) / 255.0F,
                 static_cast<float>((word >> 8U) & 0xFFU) / 255.0F,
                 static_cast<float>((word >> 16U) & 0xFFU) / 255.0F,
                 static_cast<float>(word >> 24U) / 255.0F};
    if (srgb) {
        const Float3 linear = srgb_to_linear(xyz(texel));
        texel = {linear[0], linear[1], linear[2], texel[3]};
    }
    return texel;
}

// A float coordinate as the shader's int conversion takes it, kept to a range that converts.
std::int64_t texel_index(float value, float size) {
    return static_cast<std::int64_t>(std::isnan(value) ? 0.0F
                                                       : std::clamp(value, -2.0F, size + 1.0F));
}

// decal_level in shaders/mesh.hlsl: a level at uv, by its nearest texel or between the four
// around uv.
Float4 sample_level(const DecalAtlas& atlas, std::uint32_t index, std::array<float, 2> uv,
                    bool linear, bool srgb) {
    const AtlasLevel level = atlas_level(atlas.width, atlas.height, index);
    const auto width = static_cast<float>(level.width);
    const auto height = static_cast<float>(level.height);
    if (!linear) {
        return decal_texel(atlas, level, texel_index(std::floor(uv[0] * width), width),
                           texel_index(std::floor(uv[1] * height), height), srgb);
    }
    const float at_x = (uv[0] * width) - 0.5F;
    const float at_y = (uv[1] * height) - 0.5F;
    const float low_x = std::floor(at_x);
    const float low_y = std::floor(at_y);
    const float fx = at_x - low_x;
    const float fy = at_y - low_y;
    const std::int64_t x = texel_index(low_x, width);
    const std::int64_t y = texel_index(low_y, height);
    const Float4 a = decal_texel(atlas, level, x, y, srgb);
    const Float4 b = decal_texel(atlas, level, x + 1, y, srgb);
    const Float4 c = decal_texel(atlas, level, x, y + 1, srgb);
    const Float4 d = decal_texel(atlas, level, x + 1, y + 1, srgb);
    return lerp4(lerp4(a, b, fx), lerp4(c, d, fx), fy);
}

// decal_sample in shaders/mesh.hlsl: the atlas at uv, which moves by dx and dy across a pixel, as
// the decals' filter samples it. With mipmaps, a level as log2 of the farther the texture moves
// across a pixel, in texels, blended between the two levels around it.
Float4 sample_decal(const DecalAtlas& atlas, std::uint32_t filter, std::array<float, 2> uv,
                    std::array<float, 2> dx, std::array<float, 2> dy, bool srgb) {
    const bool linear = filter == 1 || filter == 3 || filter == 5;
    if (filter < 2) {
        return sample_level(atlas, 0, uv, linear, srgb);
    }
    const auto width = static_cast<float>(atlas.width);
    const auto height = static_cast<float>(atlas.height);
    const float across_x =
        std::sqrt(((dx[0] * width) * (dx[0] * width)) + ((dx[1] * height) * (dx[1] * height)));
    const float across_y =
        std::sqrt(((dy[0] * width) * (dy[0] * width)) + ((dy[1] * height) * (dy[1] * height)));
    const float rho = std::max(across_x, across_y);
    float lambda = rho >= 16.0F ? 4.0F : slash::ln(rho) * (1.0F / std::numbers::ln2_v<float>);
    if (!(lambda > 0.0F)) {
        return sample_level(atlas, 0, uv, linear, srgb);
    }
    lambda = std::min(lambda, static_cast<float>(kDecalAtlasLevels - 1));
    const float low = std::floor(lambda);
    const Float4 color = sample_level(atlas, static_cast<std::uint32_t>(low), uv, linear, srgb);
    if (lambda > low) {
        const Float4 next =
            sample_level(atlas, static_cast<std::uint32_t>(low) + 1, uv, linear, srgb);
        return lerp4(color, next, lambda - low);
    }
    return color;
}

// surface_steps in shaders/mesh.hlsl: how far the point moves across a pixel to the right and down,
// along the plane through it facing geometry, from the camera's rows; Godot takes these from the
// GPU's derivatives.
struct Steps {
    Float3 dx{};
    Float3 dy{};
};
Steps surface_steps(const MeshSceneUniforms& scene, const Float3& world, const Float3& geometry) {
    const Float3 helper =
        std::abs(geometry[1]) < 0.999F ? Float3{0.0F, 1.0F, 0.0F} : Float3{1.0F, 0.0F, 0.0F};
    const Float3 t1 = normalize(cross(helper, geometry));
    const Float3 t2 = cross(geometry, t1);
    const Float4 point{world[0], world[1], world[2], 1.0F};
    const float x = dot4(scene.clip_rows[0], point);
    const float y = dot4(scene.clip_rows[1], point);
    const float w = dot4(scene.clip_rows[2], point);
    const float half_width = 0.5F * scene.screen[0];
    const float half_height = 0.5F * scene.screen[1];
    const auto step = [&](const Float3& t) {
        const float dw = dot3(xyz(scene.clip_rows[2]), t);
        const float sx = ((dot3(xyz(scene.clip_rows[0]), t) * w) - (x * dw)) / (w * w);
        const float sy = ((dot3(xyz(scene.clip_rows[1]), t) * w) - (y * dw)) / (w * w);
        return std::array<float, 2>{sx * half_width, -sy * half_height};
    };
    const std::array<float, 2> s1 = step(t1);
    const std::array<float, 2> s2 = step(t2);
    const float det = (s1[0] * s2[1]) - (s2[0] * s1[1]);
    if (!(std::abs(det) > 1.0e-20F)) {
        return {.dx = scale(t1, 1.0e10F), .dy = scale(t2, 1.0e10F)};
    }
    return {.dx = scale(sub(scale(t1, s2[1]), scale(t2, s1[1])), 1.0F / det),
            .dy = scale(sub(scale(t2, s1[0]), scale(t1, s2[0])), 1.0F / det)};
}

bool has_rect(const Float4& rect) {
    return rect[0] != 0.0F || rect[1] != 0.0F || rect[2] != 0.0F || rect[3] != 0.0F;
}

// apply_decal in shaders/mesh.hlsl: one decal's textures over the surface, as Godot's loop body.
void apply_decal(const MeshDecalUniforms& decal, const DecalAtlas& atlas, std::uint32_t filter,
                 const Float3& world, const Float3& geometry, const Steps& steps,
                 DecalSurface& surface) {
    const Float4 point{world[0], world[1], world[2], 1.0F};
    const float x = dot4(decal.rows[0], point);
    const float y = dot4(decal.rows[1], point);
    const float z = dot4(decal.rows[2], point);
    if (x < 0.0F || y < -1.0F || z < 0.0F || x > 1.0F || y > 1.0F || z > 1.0F) {
        return; // outside its box
    }
    float fade = fade_power(1.0F - std::abs(y), y > 0.0F ? decal.params[2] : decal.params[3]);
    if (decal.normal[3] > 0.0F) {
        fade *=
            smoothstep(decal.normal[3], 1.0F, (dot3(geometry, xyz(decal.normal)) * 0.5F) + 0.5F);
    }
    const std::array<float, 2> dx{dot3(xyz(decal.rows[0]), steps.dx),
                                  dot3(xyz(decal.rows[2]), steps.dx)};
    const std::array<float, 2> dy{dot3(xyz(decal.rows[0]), steps.dy),
                                  dot3(xyz(decal.rows[2]), steps.dy)};
    const auto sample = [&](const Float4& rect, bool srgb) {
        return sample_decal(atlas, filter, {(x * rect[2]) + rect[0], (z * rect[3]) + rect[1]},
                            {dx[0] * rect[2], dx[1] * rect[3]}, {dy[0] * rect[2], dy[1] * rect[3]},
                            srgb);
    };
    if (has_rect(decal.rects[0])) {
        Float4 albedo = sample(decal.rects[0], true);
        for (std::size_t i = 0; i < 4; ++i) {
            albedo.at(i) *= decal.modulate.at(i);
        }
        albedo[3] *= fade;
        surface.base = lerp3(surface.base, xyz(albedo), albedo[3] * decal.params[1]);
        if (has_rect(decal.rects[1])) {
            // Y down, as most tools draw normal maps, and turned from the decal's space, whose up
            // is y, by its xzy.
            const Float4 texel = sample(decal.rects[1], false);
            const float nx = (texel[0] * 2.0F) - 1.0F;
            const float ny = (texel[1] * -2.0F) + 1.0F;
            const float nz = std::sqrt(std::max(0.0F, 1.0F - ((nx * nx) + (ny * ny))));
            const Float3 turned{dot3(xyz(decal.normal_rows[0]), {nx, nz, ny}),
                                dot3(xyz(decal.normal_rows[1]), {nx, nz, ny}),
                                dot3(xyz(decal.normal_rows[2]), {nx, nz, ny})};
            surface.normal = normalize(lerp3(surface.normal, turned, albedo[3]));
        }
        if (has_rect(decal.rects[2])) {
            const Float4 orm = sample(decal.rects[2], false);
            surface.ao = lerp(surface.ao, orm[0], albedo[3]);
            surface.roughness = lerp(surface.roughness, orm[1], albedo[3]);
            surface.metallic = lerp(surface.metallic, orm[2], albedo[3]);
        }
    }
    if (has_rect(decal.rects[3])) {
        // Emission adds, apart from the albedo.
        const Float4 emission = sample(decal.rects[3], true);
        const float energy = decal.params[0] * fade;
        surface.emitted = add(surface.emitted, {emission[0] * decal.modulate[0] * energy,
                                                emission[1] * decal.modulate[1] * energy,
                                                emission[2] * decal.modulate[2] * energy});
    }
}

std::string decal_name(std::size_t index) {
    return "decal " + std::to_string(index);
}

} // namespace

std::vector<std::shared_ptr<const Image>> decal_images(const Decals3D& decals) {
    std::vector<std::shared_ptr<const Image>> out;
    for (const Decal3D& decal : decals.decals) {
        for (const std::shared_ptr<const Image>& image :
             {decal.albedo, decal.normal, decal.orm, decal.emission}) {
            if (image != nullptr && std::ranges::find(out, image) == out.end()) {
                out.push_back(image);
            }
        }
    }
    return out;
}

DecalAtlasLayout layout_decal_atlas(const std::vector<std::array<std::uint32_t, 2>>& sizes) {
    DecalAtlasLayout out;
    if (sizes.empty()) {
        return out; // a level's worth of texels across, so it has every level
    }
    struct Item {
        std::uint32_t width = 0; // in cells of kDecalAtlasBorder texels
        std::uint32_t height = 0;
        std::size_t index = 0;
        std::uint32_t x = 0;
        std::uint32_t y = 0;
    };
    std::vector<Item> items;
    items.reserve(sizes.size());
    std::uint32_t base = 8;
    for (std::size_t i = 0; i < sizes.size(); ++i) {
        const Item item{.width = (sizes[i][0] / kDecalAtlasBorder) + 1,
                        .height = (sizes[i][1] / kDecalAtlasBorder) + 1,
                        .index = i};
        if (base < item.width) {
            base = std::bit_ceil(item.width);
        }
        items.push_back(item);
    }
    std::ranges::stable_sort(items, [](const Item& a, const Item& b) {
        return a.height != b.height ? a.height > b.height : a.width > b.width;
    });
    std::uint32_t atlas_height = 0;
    while (true) {
        // Each item goes where the skyline under it is lowest, leftmost first.
        std::vector<std::uint32_t> skyline(base, 0);
        std::uint32_t max_height = 0;
        for (Item& item : items) {
            std::uint32_t best_x = 0;
            std::uint32_t best_height = std::numeric_limits<std::uint32_t>::max();
            for (std::uint32_t x = 0; x + item.width <= base; ++x) {
                std::uint32_t height = 0;
                for (std::uint32_t k = 0; k < item.width; ++k) {
                    if (skyline[x + k] > height) {
                        height = skyline[x + k];
                        if (height > best_height) {
                            break;
                        }
                    }
                }
                if (height < best_height) {
                    best_height = height;
                    best_x = x;
                }
            }
            for (std::uint32_t k = 0; k < item.width; ++k) {
                skyline[best_x + k] = best_height + item.height;
            }
            item.x = best_x;
            item.y = best_height;
            max_height = std::max(max_height, item.y + item.height);
        }
        if (max_height <= base * 2) {
            atlas_height = max_height;
            break;
        }
        base *= 2;
    }
    out.width = base * kDecalAtlasBorder;
    out.height = std::bit_ceil(atlas_height * kDecalAtlasBorder);
    out.places.resize(sizes.size());
    for (const Item& item : items) {
        out.places[item.index] = {(item.x * kDecalAtlasBorder) + (kDecalAtlasBorder / 2),
                                  (item.y * kDecalAtlasBorder) + (kDecalAtlasBorder / 2)};
    }
    return out;
}

std::size_t decal_atlas_texels(std::uint32_t width, std::uint32_t height) {
    const AtlasLevel last = atlas_level(width, height, kDecalAtlasLevels - 1);
    return last.offset + (std::size_t{last.width} * last.height);
}

Float4 DecalAtlas::rect(const std::shared_ptr<const Image>& image) const {
    if (image != nullptr) {
        for (std::size_t i = 0; i < images.size(); ++i) {
            if (images[i] == image) {
                return rects[i];
            }
        }
    }
    return {};
}

DecalAtlas pack_decal_atlas(const Decals3D& decals) {
    DecalAtlas out;
    out.images = decal_images(decals);
    std::vector<std::array<std::uint32_t, 2>> sizes;
    sizes.reserve(out.images.size());
    for (const std::shared_ptr<const Image>& image : out.images) {
        sizes.push_back(image_size(*image));
    }
    const DecalAtlasLayout layout = layout_decal_atlas(sizes);
    out.width = layout.width;
    out.height = layout.height;
    out.texels.assign(decal_atlas_texels(out.width, out.height), 0);
    const auto width = static_cast<float>(out.width);
    const auto height = static_cast<float>(out.height);
    for (std::size_t i = 0; i < out.images.size(); ++i) {
        const Image& image = *out.images[i];
        const auto [left, top] = layout.places[i];
        for (std::uint32_t y = 0; y < image.height; ++y) {
            for (std::uint32_t x = 0; x < image.width; ++x) {
                const std::size_t from = ((std::size_t{y} * image.width) + x) * 4;
                out.texels[(std::size_t{top + y} * out.width) + left + x] =
                    std::uint32_t{image.pixels[from]} |
                    (std::uint32_t{image.pixels[from + 1]} << 8U) |
                    (std::uint32_t{image.pixels[from + 2]} << 16U) |
                    (std::uint32_t{image.pixels[from + 3]} << 24U);
            }
        }
        out.rects.push_back({static_cast<float>(left) / width, static_cast<float>(top) / height,
                             static_cast<float>(image.width) / width,
                             static_cast<float>(image.height) / height});
    }
    for (std::uint32_t index = 1; index < kDecalAtlasLevels; ++index) {
        const AtlasLevel above = atlas_level(out.width, out.height, index - 1);
        const AtlasLevel level = atlas_level(out.width, out.height, index);
        const auto texel = [&](std::uint32_t x, std::uint32_t y) {
            x = std::min(x, above.width - 1);
            y = std::min(y, above.height - 1);
            return out.texels[above.offset + (std::size_t{y} * above.width) + x];
        };
        for (std::uint32_t y = 0; y < level.height; ++y) {
            for (std::uint32_t x = 0; x < level.width; ++x) {
                const std::array<std::uint32_t, 4> four{
                    texel(2 * x, 2 * y), texel((2 * x) + 1, 2 * y), texel(2 * x, (2 * y) + 1),
                    texel((2 * x) + 1, (2 * y) + 1)};
                std::uint32_t word = 0;
                for (std::uint32_t channel = 0; channel < 4; ++channel) {
                    std::uint32_t sum = 0;
                    for (const std::uint32_t t : four) {
                        sum += (t >> (channel * 8U)) & 0xFFU;
                    }
                    word |= ((sum + 2U) >> 2U) << (channel * 8U);
                }
                out.texels[level.offset + (std::size_t{y} * level.width) + x] = word;
            }
        }
    }
    return out;
}

bool DecalAtlasCache::update(const Decals3D& decals) {
    if (packed_ != 0 && decal_images(decals) == atlas_.images) {
        return false;
    }
    atlas_ = pack_decal_atlas(decals);
    atlas_.generation = ++packed_;
    return true;
}

PlacedDecals place_decals(const Scene3D& scene, const DecalAtlas& atlas) {
    PlacedDecals out;
    if (scene.method != RenderMethod::ForwardPlus) {
        return out;
    }
    struct Candidate {
        const Decal3D* decal = nullptr;
        float distance = 0.0F;
        float depth = 0.0F; // the distance less the sorting offset
    };
    const Float3 eye = camera_place(scene.camera);
    std::vector<Candidate> candidates;
    for (const Decal3D& decal : scene.decals.decals) {
        if (candidates.size() == kMaxDecals3D) {
            break;
        }
        const Mat4& t = decal.transform;
        const Float3 offset = sub({t[12], t[13], t[14]}, eye);
        const float distance = std::sqrt(dot3(offset, offset));
        if (decal.distance_fade &&
            distance > decal.distance_fade_begin + decal.distance_fade_length) {
            continue; // faded out
        }
        candidates.push_back(
            {.decal = &decal, .distance = distance, .depth = distance - decal.sorting_offset});
    }
    std::ranges::stable_sort(candidates, {}, &Candidate::depth);
    for (const Candidate& candidate : candidates) {
        const Decal3D& decal = *candidate.decal;
        if (decal.albedo == nullptr && decal.emission == nullptr) {
            continue; // no albedo and no emission: nothing to draw
        }
        float fade = 1.0F;
        if (decal.distance_fade && candidate.distance > decal.distance_fade_begin) {
            fade = smoothstep(0.0F, 1.0F,
                              1.0F - ((candidate.distance - decal.distance_fade_begin) /
                                      decal.distance_fade_length));
        }
        // The box: the decal's transform times half its size times Godot's uv_xform, which takes
        // x and z from 0 to 1 to -1 to 1. Its inverse, worked in doubles.
        const Mat4& t = decal.transform;
        const std::array<double, 3> half{double{decal.size[0]} * 0.5, double{decal.size[1]} * 0.5,
                                         double{decal.size[2]} * 0.5};
        std::array<std::array<double, 3>, 4> columns{};
        for (std::size_t r = 0; r < 3; ++r) {
            columns[0].at(r) = double{t.at(r)} * half[0] * 2.0;
            columns[1].at(r) = double{t.at(4 + r)} * half[1];
            columns[2].at(r) = double{t.at(8 + r)} * half[2] * 2.0;
            columns[3].at(r) = double{t.at(12 + r)} - (double{t.at(r)} * half[0]) -
                               (double{t.at(8 + r)} * half[2]);
        }
        const auto cross_d = [](const std::array<double, 3>& a, const std::array<double, 3>& b) {
            return std::array<double, 3>{(a[1] * b[2]) - (a[2] * b[1]),
                                         (a[2] * b[0]) - (a[0] * b[2]),
                                         (a[0] * b[1]) - (a[1] * b[0])};
        };
        const auto dot_d = [](const std::array<double, 3>& a, const std::array<double, 3>& b) {
            return (a[0] * b[0]) + (a[1] * b[1]) + (a[2] * b[2]);
        };
        const std::array<std::array<double, 3>, 3> inverse{cross_d(columns[1], columns[2]),
                                                           cross_d(columns[2], columns[0]),
                                                           cross_d(columns[0], columns[1])};
        const double det = dot_d(columns[0], inverse[0]);
        if (!(std::abs(det) > 1.0e-30) || !std::isfinite(det)) {
            continue; // a box with no inside
        }
        MeshDecalUniforms d;
        for (std::size_t r = 0; r < 3; ++r) {
            const std::array<double, 3>& row = inverse.at(r);
            d.rows.at(r) = {static_cast<float>(row[0] / det), static_cast<float>(row[1] / det),
                            static_cast<float>(row[2] / det),
                            static_cast<float>(-dot_d(row, columns[3]) / det)};
        }
        const Float3 x_axis{t[0], t[1], t[2]};
        const Float3 y_axis{t[4], t[5], t[6]};
        const Float3 z_axis{t[8], t[9], t[10]};
        const Float3 up = normalize(y_axis);
        d.normal = {up[0], up[1], up[2], std::min(decal.normal_fade, 0.999F)};
        // Basis::orthonormalized: Gram-Schmidt from x.
        const Float3 ox = normalize(x_axis);
        const Float3 oy = normalize(sub(y_axis, scale(ox, dot3(ox, y_axis))));
        const Float3 oz =
            normalize(sub(sub(z_axis, scale(ox, dot3(ox, z_axis))), scale(oy, dot3(oy, z_axis))));
        for (std::size_t r = 0; r < 3; ++r) {
            d.normal_rows.at(r) = {ox.at(r), oy.at(r), oz.at(r), 0.0F};
        }
        d.rects = {atlas.rect(decal.albedo), atlas.rect(decal.normal), atlas.rect(decal.orm),
                   atlas.rect(decal.emission)};
        d.modulate = {srgb_to_linear_exact(decal.modulate[0]),
                      srgb_to_linear_exact(decal.modulate[1]),
                      srgb_to_linear_exact(decal.modulate[2]), decal.modulate[3] * fade};
        d.params = {decal.emission_energy * fade, decal.albedo_mix, decal.upper_fade,
                    decal.lower_fade};
        d.options = {decal.cull_mask & kRenderLayers, 0, 0, 0};
        out.uniforms.push_back(d);
        Mat4 box{};
        for (std::size_t c = 0; c < 4; ++c) {
            for (std::size_t r = 0; r < 3; ++r) {
                box.at((c * 4) + r) = static_cast<float>(columns.at(c).at(r));
            }
        }
        box[15] = 1.0F;
        out.boxes.push_back(box);
    }
    return out;
}

void set_cluster_uniforms(MeshSceneUniforms& uniforms, const LightClusters& clusters,
                          const DecalAtlas* atlas, DecalFilter filter) {
    uniforms.cluster = clusters.uniform();
    uniforms.screen = {static_cast<float>(clusters.width), static_cast<float>(clusters.height),
                       0.0F, 0.0F};
    if (atlas != nullptr && clusters.decal_words > 0) {
        uniforms.decal_info = {static_cast<float>(clusters.decals.size()),
                               static_cast<float>(filter), static_cast<float>(atlas->width),
                               static_cast<float>(atlas->height)};
    }
}

void apply_decals(const MeshSceneUniforms& scene, const MeshMaps& maps, const MeshPixel& pixel,
                  std::uint32_t layers, const Float3& geometry, DecalSurface& surface) {
    if (maps.clusters == nullptr || maps.uniforms == nullptr || maps.decal_atlas == nullptr ||
        !(scene.decal_info[0] > 0.0F)) {
        return;
    }
    const LightClusters& clusters = *maps.clusters;
    const std::uint32_t tile_x = std::min(pixel.x / kClusterSize, clusters.tiles_across - 1);
    const std::uint32_t tile_y = std::min(pixel.y / kClusterSize, clusters.tiles_down - 1);
    const std::size_t cell =
        (((std::size_t{tile_y} * clusters.tiles_across) + tile_x) * clusters.stride()) +
        clusters.words + kClusterSlices;
    const Float4& camera_z = maps.uniforms->camera_z;
    const float depth = -(dot3(pixel.world, xyz(camera_z)) + camera_z[3]);
    const auto slice = static_cast<std::uint32_t>(std::clamp(
        depth * clusters.slices_per_depth, 0.0F, static_cast<float>(kClusterSlices - 1)));
    const std::uint32_t range = clusters.cells[cell + clusters.decal_words + slice];
    const std::uint32_t first = range & 0xFFFFU;
    const std::uint32_t end = range >> 16U;
    const Float3 geo = normalize(geometry);
    const Steps steps = surface_steps(scene, pixel.world, geo);
    const auto filter = static_cast<std::uint32_t>(scene.decal_info[1]);
    for (std::uint32_t word = first >> 5U; word < (end + 31U) >> 5U; ++word) {
        std::uint32_t mask = clusters.cells[cell + word] & range_mask(word, first, end);
        while (mask != 0) {
            // From the highest bit, as Godot's findMSB.
            const auto bit = static_cast<std::uint32_t>(31 - std::countl_zero(mask));
            mask &= ~(1U << bit);
            const MeshDecalUniforms& decal = clusters.decals[(word * 32U) + bit];
            if ((decal.options[0] & layers) == 0) {
                continue;
            }
            apply_decal(decal, *maps.decal_atlas, filter, pixel.world, geo, steps, surface);
        }
    }
}

} // namespace slash::render::detail

namespace slash::render {

Result<void> check_decal(const Decal3D& decal, std::string_view name) {
    const std::string owner(name);
    if (!std::ranges::all_of(decal.transform, [](float v) { return std::isfinite(v); }) ||
        !detail::finite({decal.size[0], decal.size[1], decal.size[2], decal.emission_energy,
                         decal.modulate[0], decal.modulate[1], decal.modulate[2], decal.modulate[3],
                         decal.albedo_mix, decal.normal_fade, decal.upper_fade, decal.lower_fade,
                         decal.distance_fade_begin, decal.distance_fade_length,
                         decal.sorting_offset})) {
        return make_error(ErrorCode::InvalidArgument, owner + "'s numbers are finite");
    }
    if (!(decal.size[0] > 0.0F && decal.size[1] > 0.0F && decal.size[2] > 0.0F)) {
        return make_error(ErrorCode::InvalidArgument, owner + "'s size is above 0");
    }
    if (decal.upper_fade < 0.0F || decal.lower_fade < 0.0F || decal.normal_fade < 0.0F ||
        decal.normal_fade > 1.0F || decal.distance_fade_length < 0.0F) {
        return make_error(ErrorCode::InvalidArgument,
                          owner + "'s fades are 0 or more, and its normal fade at most 1");
    }
    const std::array<const char*, 4> names{"albedo", "normal", "orm", "emission"};
    const std::array<const Image*, 4> images{decal.albedo.get(), decal.normal.get(),
                                             decal.orm.get(), decal.emission.get()};
    for (std::size_t k = 0; k < images.size(); ++k) {
        const Image* image = images.at(k);
        if (image != nullptr &&
            (image->width == 0 || image->height == 0 || image->width > kMaxTextureSize ||
             image->height > kMaxTextureSize ||
             image->pixels.size() != std::size_t{image->width} * image->height * 4)) {
            return make_error(ErrorCode::InvalidArgument,
                              owner + "'s " + names.at(k) + " texture is RGBA8 pixels, 1 to " +
                                  std::to_string(kMaxTextureSize) + " texels across");
        }
    }
    return {};
}

Result<void> check_decals(const Decals3D& decals) {
    if (static_cast<std::size_t>(decals.filter) >= kDecalFilterNames.size()) {
        return make_error(ErrorCode::InvalidArgument, "the decals' filter is not one of Godot's");
    }
    for (std::size_t i = 0; i < decals.decals.size(); ++i) {
        if (auto checked = check_decal(decals.decals[i], detail::decal_name(i)); !checked) {
            return checked;
        }
    }
    std::vector<std::array<std::uint32_t, 2>> sizes;
    for (const std::shared_ptr<const Image>& image : detail::decal_images(decals)) {
        sizes.push_back(detail::image_size(*image));
    }
    const detail::DecalAtlasLayout layout = detail::layout_decal_atlas(sizes);
    if (const std::size_t texels = detail::decal_atlas_texels(layout.width, layout.height);
        texels > detail::kMaxDecalAtlasTexels) {
        return make_error(ErrorCode::InvalidArgument,
                          "the decals' textures need an atlas of " + std::to_string(texels) +
                              " texels, past the " + std::to_string(detail::kMaxDecalAtlasTexels) +
                              " it holds");
    }
    return {};
}

} // namespace slash::render
