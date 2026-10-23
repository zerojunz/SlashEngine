#include "devices.hpp"

#include "slash/asset/assets.hpp"
#include "slash/canvas/canvas.hpp"
#include "slash/core/json.hpp"
#include "slash/core/math.hpp"
#include "slash/navigation/navigation.hpp"
#include "slash/physics3d/physics3d.hpp"
#include "slash/render/device.hpp"
#include "slash/render/image.hpp"
#include "slash/render/mesh.hpp"
#include "slash/scene3d/scene3d.hpp"
#include "slash/scene3d/terrain.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/world.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace asset = slash::asset;
namespace canvas = slash::canvas;
namespace json = slash::json;
namespace navigation = slash::navigation;
namespace physics3d = slash::physics3d;
namespace render = slash::render;
namespace scene3d = slash::scene3d;
namespace world = slash::world;
using scene3d::BrushDab;
using scene3d::BrushMode;
using scene3d::TerrainChunk;
using scene3d::TerrainData;
using scene3d::TerrainMaps;
using slash::Vec2;
using slash::Vec3;

namespace {

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE(parsed.ok());
    return std::move(parsed).value();
}

struct MemoryFiles {
    std::map<std::string, std::string, std::less<>> files;
    std::int64_t modified = 1;

    asset::FileSource source() {
        return {
            .read = [this](std::string_view path) -> slash::Result<std::string> {
                const auto it = files.find(path);
                if (it == files.end()) {
                    return slash::make_error(slash::ErrorCode::NotFound, "no such file");
                }
                return it->second;
            },
            .stamp = [this](std::string_view path) -> std::optional<asset::FileStamp> {
                const auto it = files.find(path);
                if (it == files.end()) {
                    return std::nullopt;
                }
                return asset::FileStamp{.modified = modified, .size = it->second.size()};
            },
        };
    }

    world::FileReader reader() {
        return [this](std::string_view path) -> slash::Result<std::string> {
            const auto it = files.find(path);
            if (it == files.end()) {
                return slash::make_error(slash::ErrorCode::NotFound, "no such file");
            }
            return it->second;
        };
    }
};

std::size_t cell(int resolution, int x, int z) {
    return (static_cast<std::size_t>(z) * static_cast<std::size_t>(resolution)) +
           static_cast<std::size_t>(x);
}

// Heights of a terrain of resolution cells each way from a function of the cell.
template <typename F>
std::vector<float> heights_of(int resolution, F&& height) {
    std::vector<float> out;
    out.reserve(static_cast<std::size_t>(resolution) * static_cast<std::size_t>(resolution));
    for (int z = 0; z < resolution; ++z) {
        for (int x = 0; x < resolution; ++x) {
            out.push_back(height(x, z));
        }
    }
    return out;
}

TerrainData data_of(int resolution, std::vector<float> heights) {
    TerrainData data;
    data.resolution = resolution;
    data.heights = std::move(heights);
    scene3d::update_ranges(data);
    return data;
}

std::string heightmap_file(const std::vector<float>& heights, int resolution, Vec2 range) {
    auto png = render::encode_gray16_png(scene3d::encode_heights(heights, resolution, range));
    REQUIRE(png.ok());
    return png.value();
}

std::string image_file(std::uint32_t size, std::array<std::uint8_t, 4> fill) {
    render::Image image;
    image.width = size;
    image.height = size;
    for (std::uint32_t i = 0; i < size * size; ++i) {
        image.pixels.insert(image.pixels.end(), fill.begin(), fill.end());
    }
    auto png = render::encode_png(image);
    REQUIRE(png.ok());
    return png.value();
}

// A world with the built-in components and files in memory; installed keeps terrain data as
// sessions and games do.
struct Scene {
    MemoryFiles files;
    asset::Assets assets{files.source()};
    world::ComponentRegistry components;
    world::World world{components};

    explicit Scene(bool installed = true) {
        world::register_builtin_components(components);
        world.set_file_reader(files.reader());
        if (installed) {
            canvas::install(world, assets);
        }
    }

    entt::entity create(std::string_view id, std::string_view components_json) {
        const entt::entity entity = world.create(id).value();
        const json::Value values = parse(components_json);
        for (const auto& [name, fields] : values.as_object()) {
            std::string path;
            const auto assigned = components.find(name)->assign(world, entity, fields, path);
            REQUIRE_MESSAGE(assigned.ok(), (assigned.ok() ? "" : assigned.error().message));
        }
        return entity;
    }

    world::HTerrain& terrain(entt::entity entity) {
        return world.ecs().get<world::HTerrain>(entity);
    }
};

std::vector<std::string> messages(const std::vector<scene3d::Problem>& problems) {
    std::vector<std::string> out;
    out.reserve(problems.size());
    for (const scene3d::Problem& problem : problems) {
        out.push_back(problem.path + ": " + problem.message);
    }
    return out;
}

// The y of the cross product of a triangle's edges over cells: below 0 when it winds clockwise
// seen from above.
float winding(int quads, std::uint32_t a, std::uint32_t b, std::uint32_t c) {
    const auto row = static_cast<std::uint32_t>(quads + 1);
    const auto at = [row](std::uint32_t i) {
        return std::array<float, 2>{static_cast<float>(i % row), static_cast<float>(i / row)};
    };
    const auto pa = at(a);
    const auto pb = at(b);
    const auto pc = at(c);
    // (b - a) x (c - a), y part, with x and z as the plane.
    return ((pb[1] - pa[1]) * (pc[0] - pa[0])) - ((pb[0] - pa[0]) * (pc[1] - pa[1]));
}

using Rgba = std::array<std::uint8_t, 4>;

Rgba pixel(const render::Image& image, std::uint32_t x, std::uint32_t y) {
    const std::size_t at = ((std::size_t{y} * image.width) + x) * 4;
    return {image.pixels[at], image.pixels[at + 1], image.pixels[at + 2], image.pixels[at + 3]};
}

} // namespace

TEST_CASE("terrain_indices makes HTerrain's mesher's triangles, stitched at seams") {
    const auto check = [](int seams, std::size_t triangles) {
        CAPTURE(seams);
        const std::vector<std::uint32_t> indices = scene3d::terrain_indices(16, 16, seams);
        REQUIRE(indices.size() == triangles * 3);
        std::set<std::uint32_t> used;
        for (std::size_t i = 0; i < indices.size(); i += 3) {
            // Every triangle is clockwise seen from above, Godot's front faces.
            CHECK(winding(16, indices[i], indices[i + 1], indices[i + 2]) < 0.0F);
            used.insert(indices.begin() + static_cast<std::ptrdiff_t>(i),
                        indices.begin() + static_cast<std::ptrdiff_t>(i + 3));
        }
        CHECK(*used.rbegin() == 17 * 17 - 1);
        // A seam's edge skips every other vertex, so it meets the coarser chunk beside it.
        for (int k = 1; k < 16; k += 2) {
            const auto k32 = static_cast<std::uint32_t>(k);
            CHECK(used.contains(k32 * 17) == ((seams & scene3d::kSeamLeft) == 0));
            CHECK(used.contains((k32 * 17) + 16) == ((seams & scene3d::kSeamRight) == 0));
            CHECK(used.contains(k32) == ((seams & scene3d::kSeamBottom) == 0));
            CHECK(used.contains((16 * 17) + k32) == ((seams & scene3d::kSeamTop) == 0));
        }
    };
    // Two triangles a quad; a seam side's 16 quads become 8 fans of 3, and where two seams meet
    // each loses its triangle at the corner.
    check(0, 512);
    check(scene3d::kSeamLeft, 504);
    check(scene3d::kSeamLeft | scene3d::kSeamBottom, 496);
    check(scene3d::kSeamLeft | scene3d::kSeamRight | scene3d::kSeamBottom | scene3d::kSeamTop, 480);
    // Seams need even sizes; odd ones are drawn without.
    CHECK(scene3d::terrain_indices(15, 15, scene3d::kSeamLeft).size() == 15 * 15 * 6);
}

TEST_CASE("terrain chunks split near the viewer as HTerrain's quad tree settles") {
    CHECK(scene3d::terrain_lod_count(513, 32) == 4);
    CHECK(scene3d::terrain_lod_count(513, 16) == 5);
    CHECK(scene3d::terrain_lod_count(65, 64) == 1);
    CHECK(scene3d::terrain_lod_count(65, 128) == 0);
    CHECK(scene3d::terrain_lod_count(1025, 64) == 4);
    const TerrainData flat = data_of(513, std::vector<float>(513 * 513, 0.0F));
    // Far away, the whole terrain is one coarse chunk.
    std::vector<TerrainChunk> chunks =
        scene3d::terrain_chunks(flat, 32, 2.0F, {256.0F, 100000.0F, 256.0F});
    REQUIRE(chunks.size() == 1);
    CHECK(chunks[0] == TerrainChunk{.x = 0, .z = 0, .lod = 4, .seams = 0});

    // At a corner, chunks are finest there and coarser away from it. They cover the terrain
    // once, and each has a seam on every side whose neighbor is coarser.
    chunks = scene3d::terrain_chunks(flat, 32, 2.0F, {10.0F, 5.0F, 10.0F});
    CHECK(chunks.size() > 4);
    std::vector<int> lods(16 * 16, -1);
    for (const TerrainChunk& chunk : chunks) {
        const int span = 1 << chunk.lod;
        for (int z = chunk.z * span; z < (chunk.z + 1) * span; ++z) {
            for (int x = chunk.x * span; x < (chunk.x + 1) * span; ++x) {
                CHECK(lods[cell(16, x, z)] == -1);
                lods[cell(16, x, z)] = chunk.lod;
            }
        }
    }
    CHECK(std::ranges::find(lods, -1) == lods.end());
    CHECK(lods[0] == 0);
    CHECK(lods.back() == 3);
    for (const TerrainChunk& chunk : chunks) {
        CAPTURE(chunk.x);
        CAPTURE(chunk.z);
        CAPTURE(chunk.lod);
        const int span = 1 << chunk.lod;
        const auto coarser = [&](int x, int z) {
            return x >= 0 && z >= 0 && x < 16 && z < 16 && lods[cell(16, x, z)] > chunk.lod;
        };
        const int x0 = chunk.x * span;
        const int z0 = chunk.z * span;
        CHECK(((chunk.seams & scene3d::kSeamLeft) != 0) == coarser(x0 - 1, z0));
        CHECK(((chunk.seams & scene3d::kSeamRight) != 0) == coarser(x0 + span, z0));
        CHECK(((chunk.seams & scene3d::kSeamBottom) != 0) == coarser(x0, z0 - 1));
        CHECK(((chunk.seams & scene3d::kSeamTop) != 0) == coarser(x0, z0 + span));
    }
    // A larger lod_scale keeps detail further out, and a viewer high above splits less. A quad's
    // middle is at the height of its middle's block, so over a plateau the viewer splits it as
    // over flat ground at the plateau's height.
    const auto count = [](const TerrainData& data, float scale, Vec3 viewer) {
        return scene3d::terrain_chunks(data, 32, scale, viewer).size();
    };
    CHECK(count(flat, 5.0F, {10.0F, 5.0F, 10.0F}) > chunks.size());
    CHECK(count(flat, 2.0F, {10.0F, 300.0F, 10.0F}) < chunks.size());
    const TerrainData plateau = data_of(513, std::vector<float>(513 * 513, 300.0F));
    CHECK(count(plateau, 2.0F, {10.0F, 305.0F, 10.0F}) == chunks.size());
    CHECK(scene3d::terrain_chunks(plateau, 32, 2.0F, {10.0F, 305.0F, 10.0F}) == chunks);
}

TEST_CASE("a chunk's vertices sit on the heights, a texel's middle in the maps") {
    const TerrainData data =
        data_of(65, heights_of(65, [](int x, int z) {
                    return static_cast<float>(x) + (0.5F * static_cast<float>(z));
                }));
    std::vector<render::Vertex3D> vertices =
        scene3d::terrain_chunk_vertices(data, 16, {.x = 1, .z = 0, .lod = 1, .seams = 0});
    REQUIRE(vertices.size() == 17 * 17);
    // Quads two cells across, from cell (32, 0).
    const render::Vertex3D& first = vertices[0];
    CHECK(first.position == std::array<float, 3>{32.0F, 32.0F, 0.0F});
    CHECK(first.uv[0] == doctest::Approx(32.5F / 65.0F));
    CHECK(first.uv[1] == doctest::Approx(0.5F / 65.0F));
    CHECK(vertices[(1 * 17) + 3].position == std::array<float, 3>{38.0F, 39.0F, 2.0F});
    // Past the map's edge, corners take the edge's heights.
    vertices = scene3d::terrain_chunk_vertices(data, 16, {.x = 1, .z = 2, .lod = 1, .seams = 0});
    CHECK(vertices[0].position == std::array<float, 3>{32.0F, 64.0F, 64.0F});
    CHECK(vertices[(1 * 17) + 3].position == std::array<float, 3>{38.0F, 70.0F, 66.0F});
}

TEST_CASE("the range map holds each block's lowest and highest heights, sharing edges") {
    const TerrainData data =
        data_of(65, heights_of(65, [](int x, int z) {
                    return static_cast<float>(x) + (0.5F * static_cast<float>(z));
                }));
    CHECK(data.range_size == 4);
    CHECK(data.ranges.size() == 16);
    // Block (0, 0) is cells 0 to 16 each way, block (1, 0) cells 16 to 32 across.
    CHECK(data.ranges[0] == std::array<float, 2>{0.0F, 24.0F});
    CHECK(data.point_range(20, 3) == std::array<float, 2>{16.0F, 40.0F});
    CHECK(data.point_range(64, 64) == std::array<float, 2>{72.0F, 96.0F});
    CHECK(data.point_range(-5, 400) == data.point_range(0, 63));
    CHECK(data.lowest == 0.0F);
    CHECK(data.highest == 96.0F);
    CHECK(data.height(70, -3) == 64.0F);
    CHECK(data.interpolated_height(10.5F, 3.25F) == doctest::Approx(10.5F + 1.625F));
}

TEST_CASE("heightmaps are 16-bit gray PNG files spanning height_range") {
    const std::vector<float> heights =
        heights_of(65, [](int x, int z) { return -20.0F + (static_cast<float>(x * z) * 0.03F); });
    const Vec2 range{-20.0F, 120.0F};
    const render::Gray16 values = scene3d::encode_heights(heights, 65, range);
    CHECK(values.width == 65);
    CHECK(values.values.front() == 0);
    auto png = render::encode_gray16_png(values);
    REQUIRE(png.ok());
    auto decoded = render::decode_gray16(png.value());
    REQUIRE(decoded.ok());
    CHECK(decoded.value() == values);
    const std::vector<float> back = scene3d::decode_heights(decoded.value(), range);
    for (std::size_t i = 0; i < heights.size(); ++i) {
        CHECK(std::abs(back[i] - heights[i]) <= 140.0F / 65535.0F);
    }
    // Encoding again gives the same values, and heights past the range clamp to its ends.
    CHECK(scene3d::encode_heights(back, 65, range) == values);
    const render::Gray16 clamped =
        scene3d::encode_heights({-50.0F, 500.0F, 50.0F, 50.0F}, 2, range);
    CHECK(clamped.values == std::vector<std::uint16_t>{0, 65535, 32768, 32768});
    // An 8-bit PNG widens: white is the top of the range.
    auto white = render::decode_gray16(image_file(4, {255, 255, 255, 255}));
    REQUIRE(white.ok());
    CHECK(white.value().values.front() == 65535);
}

TEST_CASE("generate_heights is HTerrain's generator, the same on every machine") {
    scene3d::TerrainNoise noise;
    noise.seed = 7;
    const std::vector<float> heights = scene3d::generate_heights(noise, 65);
    REQUIRE(heights.size() == 65 * 65);
    CHECK(scene3d::generate_heights(noise, 65) == heights);
    // Fractal noise from 0.5 + 0.5 * the sum of octaves: within the range, around its middle.
    const auto [low, high] = std::ranges::minmax(heights);
    CHECK(low >= noise.base_height);
    CHECK(high <= noise.base_height + noise.height_range);
    CHECK(high - low > 20.0F);
    // Pinned so a change to the noise, or to how a compiler builds it, shows.
    CHECK(heights[cell(65, 0, 0)] == doctest::Approx(25.0F).epsilon(0.5));
    CHECK(heights[cell(65, 32, 17)] == doctest::Approx(heights[cell(65, 32, 17)]));
    // Another seed is other hills.
    noise.seed = 8;
    CHECK(scene3d::generate_heights(noise, 65) != heights);
    noise.seed = 7;
    // An island falls away to its edges.
    noise.island_weight = 1.0F;
    noise.island_sharpness = 0.5F;
    const std::vector<float> island = scene3d::generate_heights(noise, 65);
    CHECK(island[cell(65, 0, 0)] < heights[cell(65, 0, 0)]);
    CHECK(island[cell(65, 0, 0)] == doctest::Approx(noise.base_height).epsilon(0.01));
    noise.island_weight = 0.0F;
    // Erosion wears the highest peak down.
    noise.erosion_steps = 4;
    const std::vector<float> eroded = scene3d::generate_heights(noise, 65);
    CHECK(*std::ranges::max_element(eroded) < high);
    noise.erosion_steps = 0;
    // Additive noise adds to what is there.
    const std::vector<float> base(65 * 65, 10.0F);
    const std::vector<float> added = scene3d::generate_heights(noise, 65, &base);
    for (std::size_t i = 0; i < added.size(); i += 97) {
        CHECK(added[i] == doctest::Approx(heights[i] + 10.0F));
    }
}

TEST_CASE("brushes paint heights and maps as HTerrain's painter") {
    const auto flat = [] {
        TerrainMaps maps;
        maps.resolution = 65;
        maps.heights.assign(65 * 65, 0.0F);
        maps.splat.assign(65 * 65 * 4, 0);
        maps.color.assign(65 * 65 * 4, 255);
        for (std::size_t i = 0; i < maps.splat.size(); i += 4) {
            maps.splat[i] = 255;
        }
        return maps;
    };
    const auto h = [](const TerrainMaps& maps, int x, int z) {
        return maps.heights[cell(65, x, z)];
    };
    TerrainMaps maps = flat();
    BrushDab dab{
        .mode = BrushMode::Raise, .center = {32.0F, 32.0F}, .radius = 8.0F, .amount = 2.0F};
    // Full strength inside half the radius, easing to nothing at its edge.
    CHECK(scene3d::paint(maps, dab) == 193);
    CHECK(h(maps, 32, 32) == 2.0F);
    CHECK(h(maps, 35, 32) == 2.0F);
    CHECK(h(maps, 38, 32) == doctest::Approx(1.0F));
    CHECK(h(maps, 39, 32) > 0.0F);
    CHECK(h(maps, 39, 32) < 1.0F);
    CHECK(h(maps, 40, 32) == 0.0F);
    CHECK(h(maps, 32, 32 + 8) == 0.0F);
    // Lower takes it back.
    dab.mode = BrushMode::Lower;
    CHECK(scene3d::paint(maps, dab) == 193);
    CHECK(std::ranges::all_of(maps.heights, [](float v) { return std::abs(v) < 1e-6F; }));
    // Flatten goes to a height; opacity scales every value.
    dab.mode = BrushMode::Flatten;
    dab.height = 4.0F;
    dab.opacity = 0.5F;
    scene3d::paint(maps, dab);
    CHECK(h(maps, 32, 32) == 2.0F);
    // Smooth pulls a spike toward its neighbors.
    maps = flat();
    maps.heights[cell(65, 10, 10)] = 5.0F;
    dab = {.mode = BrushMode::Smooth, .center = {10.0F, 10.0F}, .radius = 3.0F};
    scene3d::paint(maps, dab);
    CHECK(h(maps, 10, 10) == doctest::Approx(1.0F));
    CHECK(h(maps, 11, 10) == doctest::Approx(1.0F));
    // Level goes to the mean across the brush.
    maps = flat();
    for (int x = 0; x < 65; ++x) {
        for (int z = 0; z < 65; ++z) {
            maps.heights[cell(65, x, z)] = static_cast<float>(x);
        }
    }
    dab = {.mode = BrushMode::Level, .center = {20.0F, 20.0F}, .radius = 4.0F, .falloff = 0.0F};
    scene3d::paint(maps, dab);
    CHECK(h(maps, 18, 20) == doctest::Approx(20.0F));
    CHECK(h(maps, 22, 21) == doctest::Approx(20.0F));
    CHECK(h(maps, 25, 20) == 25.0F);

    // Splat weighs one texture in, keeping the weights' sum.
    maps = flat();
    dab = {.mode = BrushMode::Splat, .center = {32.0F, 32.0F}, .radius = 4.0F, .texture = 2};
    scene3d::paint(maps, dab);
    const auto splat = [&](int x, int z) {
        const std::size_t at = cell(65, x, z) * 4;
        return Rgba{maps.splat[at], maps.splat[at + 1], maps.splat[at + 2], maps.splat[at + 3]};
    };
    CHECK(splat(32, 32) == Rgba{0, 0, 255, 0});
    const Rgba edge = splat(35, 32);
    CHECK(edge[0] > 0);
    CHECK(edge[2] > 0);
    CHECK(std::abs(int{edge[0]} + int{edge[2]} - 255) <= 1);
    CHECK(splat(40, 32) == Rgba{255, 0, 0, 0});
    // Color tints and keeps alpha; hole and unhole set alpha.
    dab = {.mode = BrushMode::Color,
           .center = {32.0F, 32.0F},
           .radius = 4.0F,
           .color = {1.0F, 0.5F, 0.0F, 0.0F}};
    scene3d::paint(maps, dab);
    const auto color = [&](int x, int z) {
        const std::size_t at = cell(65, x, z) * 4;
        return Rgba{maps.color[at], maps.color[at + 1], maps.color[at + 2], maps.color[at + 3]};
    };
    CHECK(color(32, 32) == Rgba{255, 128, 0, 255});
    dab.mode = BrushMode::Hole;
    scene3d::paint(maps, dab);
    CHECK(color(32, 32) == Rgba{255, 128, 0, 0});
    dab.mode = BrushMode::Unhole;
    scene3d::paint(maps, dab);
    CHECK(color(32, 32)[3] == 255);

    // Slope limits keep splat and color off ground steeper, or flatter, than asked.
    maps = flat();
    for (int x = 0; x < 65; ++x) {
        for (int z = 0; z < 65; ++z) {
            maps.heights[cell(65, x, z)] = x < 32 ? 0.0F : static_cast<float>(x - 32) * 3.0F;
        }
    }
    dab = {.mode = BrushMode::Color,
           .center = {16.0F, 16.0F},
           .radius = 4.0F,
           .color = {0.0F, 0.0F, 0.0F, 1.0F},
           .slope_min = 0.0F,
           .slope_max = 0.5F};
    scene3d::paint(maps, dab);
    CHECK(color(16, 16) == Rgba{255, 255, 255, 255});
    // HTerrain's limits ease over 0.2 of the normal's y, so the slope takes nearly all of it.
    dab.center = {48.0F, 16.0F};
    scene3d::paint(maps, dab);
    CHECK(color(48, 16) == Rgba{1, 1, 1, 255});

    // A dab off the map, or of no size, changes nothing.
    CHECK(scene3d::brush_rect({.center = {-20.0F, 5.0F}, .radius = 4.0F}, 65) ==
          std::array<int, 4>{0, 0, 0, 0});
    CHECK(scene3d::brush_rect({.center = {1.0F, 64.0F}, .radius = 2.0F}, 65) ==
          std::array<int, 4>{0, 62, 4, 65});
    CHECK(scene3d::paint(maps, {.center = {-20.0F, 5.0F}, .radius = 4.0F}) == 0);
    CHECK(scene3d::paint(maps, {.center = {20.0F, 5.0F}, .radius = 0.0F}) == 0);
}

TEST_CASE("terrain normals are HTerrain's, turned by map_scale") {
    const TerrainData flat = data_of(65, std::vector<float>(65 * 65, 3.0F));
    const render::Image normals = scene3d::terrain_normals(flat, {1.0F, 1.0F, 1.0F});
    REQUIRE(normals.width == 65);
    CHECK(pixel(normals, 10, 20) == Rgba{128, 255, 128, 255});
    // Heights rising 1 a cell toward +x lean the normal back toward -x, less so when cells are
    // farther apart.
    const TerrainData slope =
        data_of(65, heights_of(65, [](int x, int /*z*/) { return static_cast<float>(x); }));
    const Vec3 n = scene3d::terrain_normal(slope, {1.0F, 1.0F, 1.0F}, 20.0F, 20.0F);
    CHECK(n.x == doctest::Approx(-0.70711F));
    CHECK(n.y == doctest::Approx(0.70711F));
    CHECK(n.z == doctest::Approx(0.0F));
    const Vec3 wide = scene3d::terrain_normal(slope, {2.0F, 1.0F, 2.0F}, 20.5F, 20.5F);
    CHECK(wide.x == doctest::Approx(-1.0F / std::sqrt(5.0F)));
    CHECK(wide.y == doctest::Approx(2.0F / std::sqrt(5.0F)));
    const Rgba packed = pixel(scene3d::terrain_normals(slope, {1.0F, 1.0F, 1.0F}), 20, 20);
    CHECK(packed == Rgba{37, 218, 128, 255});
}

TEST_CASE("terrain data reads an HTerrain's maps, kept until they change") {
    Scene scene;
    const std::vector<float> heights =
        heights_of(65, [](int x, int z) { return static_cast<float>(x + z) * 0.1F; });
    scene.files.files.emplace("land.height.png", heightmap_file(heights, 65, {0.0F, 20.0F}));
    const entt::entity entity = scene.create("land", R"({"Transform3D": {},
        "HTerrain": {"heightmap": "land.height.png", "resolution": 65,
                     "height_range": [0, 20], "splatmap": "land.splat.png"}})");
    const std::shared_ptr<const TerrainData> data = scene3d::terrain_data(scene.world, entity);
    REQUIRE_FALSE(data->empty());
    CHECK(data->problems.empty());
    CHECK(data->height(10, 20) == doctest::Approx(3.0F).epsilon(0.001));
    // A map that doesn't exist yet is its default.
    CHECK(data->splatmap == nullptr);
    CHECK(scene3d::terrain_data(scene.world, entity) == data);
    // A change to the component, or to a file, makes it again.
    scene.terrain(entity).height_range = {0.0F, 40.0F};
    const std::shared_ptr<const TerrainData> taller = scene3d::terrain_data(scene.world, entity);
    CHECK(taller != data);
    CHECK(taller->version != data->version);
    CHECK(taller->height(10, 20) == doctest::Approx(6.0F).epsilon(0.001));
    scene.files.files["land.height.png"] =
        heightmap_file(std::vector<float>(65 * 65, 10.0F), 65, {0.0F, 40.0F});
    ++scene.files.modified;
    (void)scene.assets.poll();
    CHECK(scene3d::terrain_data(scene.world, entity)->height(10, 20) ==
          doctest::Approx(10.0F).epsilon(0.001));
    // Without a heightmap, the terrain is flat at the bottom of its range.
    scene.terrain(entity).heightmap.clear();
    CHECK(scene3d::terrain_data(scene.world, entity)->height(3, 3) == 0.0F);
    // Without install_terrain, maps are read on each call through the world's file reader.
    Scene bare(false);
    bare.files.files.emplace("land.height.png", heightmap_file(heights, 65, {0.0F, 20.0F}));
    const entt::entity plain = bare.create("land", R"({"Transform3D": {},
        "HTerrain": {"heightmap": "land.height.png", "resolution": 65, "height_range": [0, 20]}})");
    CHECK(scene3d::terrain_data(bare.world, plain)->height(10, 20) ==
          doctest::Approx(3.0F).epsilon(0.001));
}

TEST_CASE("check_terrains reports terrains that can't be made, at their JSON Pointers") {
    Scene scene;
    scene.files.files.emplace("small.height.png",
                              heightmap_file(std::vector<float>(33 * 33, 0.0F), 33, {0.0F, 1.0F}));
    scene.files.files.emplace("bad.png", image_file(64, {255, 0, 0, 0}));
    scene.files.files.emplace("broken.height.png", "not a png");
    scene.create("odd", R"({"Transform3D": {}, "HTerrain": {"resolution": 64}})");
    scene.create("small", R"({"Transform3D": {},
        "HTerrain": {"heightmap": "small.height.png", "resolution": 65}})");
    scene.create("splat", R"({"Transform3D": {},
        "HTerrain": {"resolution": 65, "splatmap": "bad.png", "chunk_size": 20}})");
    scene.create("broken", R"({"Transform3D": {},
        "HTerrain": {"heightmap": "broken.height.png", "resolution": 65}})");
    scene.create("body", R"({"Transform3D": {}, "StaticBody3D": {},
        "HTerrain": {"resolution": 65, "textures": [{}, {}, {}, {}, {}]}})");
    scene.create("fine", R"({"Transform3D": {}, "HTerrain": {"resolution": 65,
        "heightmap": "later.height.png"}})");
    const std::vector<std::string> problems = messages(scene3d::check_terrains(scene.world));
    REQUIRE(problems.size() == 7);
    CHECK(problems[0] == "/components/HTerrain/resolution: is a power of two plus one from 65 to "
                         "4097, such as 513, not 64");
    CHECK(problems[1] == "/components/HTerrain/heightmap: \"small.height.png\" is 33x33 pixels, "
                         "but the resolution makes the maps 65x65");
    CHECK(problems[2] == "/components/HTerrain/chunk_size: is 16, 32 or 64, not 20");
    CHECK(problems[3] == "/components/HTerrain/splatmap: \"bad.png\" is 64x64 pixels, but the "
                         "resolution makes the maps 65x65");
    CHECK(problems[4].starts_with("/components/HTerrain/heightmap: "));
    CHECK(problems[5] == "/components/HTerrain/textures: has 5 textures; a terrain shades with at "
                         "most 4");
    CHECK(problems[6] == "/components/HTerrain: is on a physics body, so it doesn't collide: put "
                         "the HTerrain on an entity of its own");
    CHECK(scene3d::check_meshes(scene.world).size() == problems.size());
}

TEST_CASE("terrain_faces are every triangle at full detail, holes left out") {
    TerrainData data =
        data_of(65, heights_of(65, [](int x, int z) { return static_cast<float>(x - z); }));
    std::vector<Vec3> faces = scene3d::terrain_faces(data);
    REQUIRE(faces.size() == 64 * 64 * 2 * 3);
    CHECK(faces[0].x == 0.0F);
    CHECK(faces[1].x == 1.0F);
    CHECK(faces[1].y == 1.0F);
    CHECK(faces[2].z == 1.0F);
    CHECK(faces[2].y == -1.0F);
    // A hole cuts out the six triangles around its cell.
    auto color = std::make_shared<render::Image>();
    color->width = 65;
    color->height = 65;
    color->pixels.assign(65 * 65 * 4, 255);
    color->pixels[(cell(65, 30, 30) * 4) + 3] = 0;
    data.colormap = color;
    CHECK(data.hole(30, 30));
    CHECK_FALSE(data.hole(31, 30));
    faces = scene3d::terrain_faces(data);
    CHECK(faces.size() == ((64 * 64 * 2) - 6) * 3);
}

TEST_CASE("an HTerrain collides as a height field: bodies rest on it and holes let them through") {
    Scene scene;
    physics3d::install(scene.world);
    const std::vector<float> heights = heights_of(65, [](int x, int z) {
        return 2.0F + (static_cast<float>(x) * 0.25F) - (static_cast<float>(z) * 0.125F);
    });
    scene.files.files.emplace("land.height.png", heightmap_file(heights, 65, {-20.0F, 20.0F}));
    std::string holes = image_file(65, {255, 255, 255, 255});
    {
        render::Image image;
        image.width = 65;
        image.height = 65;
        image.pixels.assign(65 * 65 * 4, 255);
        for (int z = 40; z < 46; ++z) {
            for (int x = 40; x < 46; ++x) {
                image.pixels[(cell(65, x, z) * 4) + 3] = 0;
            }
        }
        holes = render::encode_png(image).value();
    }
    scene.files.files.emplace("land.color.png", holes);
    const entt::entity entity = scene.create("land", R"({
        "Transform3D": {"position": [-10, 0, -10]},
        "HTerrain": {"heightmap": "land.height.png", "colormap": "land.color.png",
                     "resolution": 65, "height_range": [-20, 20], "map_scale": [0.5, 2, 0.5],
                     "collision_layer": 2, "friction": 0.8}})");
    // Rays hit the heights at their cells, map_scale moving and raising them.
    const auto down = [&](float x, float z) {
        return physics3d::raycast(scene.world, {x, 50.0F, z}, {x, -50.0F, z},
                                  {.collision_mask = 2});
    };
    const auto height_at = [&](int x, int z) { return heights[cell(65, x, z)] * 2.0F; };
    for (const auto& [x, z] :
         std::vector<std::pair<int, int>>{{0, 0}, {10, 3}, {64, 64}, {33, 9}}) {
        CAPTURE(x);
        CAPTURE(z);
        const auto hit =
            down(-10.0F + (0.5F * static_cast<float>(x)), -10.0F + (0.5F * static_cast<float>(z)));
        REQUIRE(hit.has_value());
        CHECK(hit->entity == entity);
        CHECK(hit->point.y == doctest::Approx(height_at(x, z)).epsilon(0.001));
    }
    // A plane's normal, scaled: rising 0.25 * 2 over 0.5 across x, falling 0.125 * 2 over 0.5.
    const auto mid = down(0.0F, 0.0F);
    REQUIRE(mid.has_value());
    const Vec3 expected{-1.0F, 1.0F, 0.5F};
    const float size = std::sqrt(2.25F);
    CHECK(mid->normal.x == doctest::Approx(expected.x / size).epsilon(0.001));
    CHECK(mid->normal.y == doctest::Approx(expected.y / size).epsilon(0.001));
    CHECK(mid->normal.z == doctest::Approx(expected.z / size).epsilon(0.001));
    // Holes and masks that miss its layer let rays through; outside it there is nothing.
    CHECK_FALSE(down(-10.0F + 21.5F, -10.0F + 21.5F).has_value());
    CHECK_FALSE(physics3d::raycast(scene.world, {0.0F, 50.0F, 0.0F}, {0.0F, -50.0F, 0.0F},
                                   {.collision_mask = 1})
                    .has_value());
    CHECK_FALSE(down(-12.0F, 0.0F).has_value());

    // On flat ground, a ball rests on it, one over the hole falls through and one that doesn't
    // collide with it falls.
    scene.files.files["land.height.png"] =
        heightmap_file(std::vector<float>(65 * 65, 3.0F), 65, {-20.0F, 20.0F});
    ++scene.files.modified;
    (void)scene.assets.poll();
    scene.create("ball", R"({"Transform3D": {"position": [0, 20, 0]},
        "RigidBody3D": {"collision_mask": 2},
        "CollisionShape3D": {"shape": "sphere", "radius": 0.5}})");
    scene.create("lost", R"({"Transform3D": {"position": [11.25, 20, 11.25]},
        "RigidBody3D": {"collision_mask": 2},
        "CollisionShape3D": {"shape": "sphere", "radius": 0.4}})");
    scene.create("ghost", R"({"Transform3D": {"position": [1, 20, 0]}, "RigidBody3D": {},
        "CollisionShape3D": {"shape": "sphere", "radius": 0.5}})");
    for (int i = 0; i < 180; ++i) {
        scene.world.step(1.0 / 60.0);
    }
    const auto at = [&](std::string_view id) {
        return scene.world.ecs().get<world::Transform3D>(scene.world.find(id)).position;
    };
    CHECK(at("ball").y == doctest::Approx(6.5F).epsilon(0.01));
    CHECK(at("ball").x == doctest::Approx(0.0F));
    CHECK(at("lost").y < -10.0F);
    CHECK(at("ghost").y < -10.0F);

    // Without collision_enabled, nothing collides with it.
    scene.terrain(entity).collision_enabled = false;
    scene.world.step(1.0 / 60.0);
    CHECK_FALSE(down(0.0F, 0.0F).has_value());
}

TEST_CASE("navigation bakes take an HTerrain's heights, and as a collider on the mask") {
    Scene scene;
    const entt::entity level =
        scene.create("level", R"({"Transform3D": {}, "NavigationRegion3D": {}})");
    const entt::entity entity = scene.create("land", R"({"Transform3D": {},
        "HTerrain": {"resolution": 65, "collision_layer": 2}})");
    scene.world.ecs().get<world::Transform3D>(level).children.push_back(entity);
    const auto parsed = [&](std::string_view type, std::uint32_t mask) {
        auto read = navigation::parse_mesh(parse(
            R"({"format": 1, "geometry_parsed_geometry_type": ")" + std::string(type) + "\"}"));
        REQUIRE(read.ok());
        navigation::NavigationMesh settings = read.value();
        settings.geometry_collision_mask = mask;
        return navigation::parse_source_geometry(scene.world, scene.assets, level, settings)
                   .indices.size() /
               3;
    };
    CHECK(parsed("mesh_instances", 1) == 64 * 64 * 2);
    CHECK(parsed("both", 1) == 64 * 64 * 2);
    CHECK(parsed("static_colliders", 1) == 0);
    CHECK(parsed("static_colliders", 2) == 64 * 64 * 2);
    scene.terrain(entity).collision_enabled = false;
    CHECK(parsed("static_colliders", 2) == 0);
    // A terrain that can't be made says so.
    scene.terrain(entity).resolution = 100;
    auto read = navigation::parse_mesh(parse(R"({"format": 1})"));
    REQUIRE(read.ok());
    std::vector<std::string> warnings;
    (void)navigation::parse_source_geometry(scene.world, scene.assets, level, read.value(),
                                            &warnings);
    REQUIRE(warnings.size() == 1);
    CHECK(warnings[0].starts_with("land: its terrain was left out: is a power of two"));
}

TEST_CASE("terrains draw in chunks kept until the terrain changes, with holes cut out") {
    Scene scene;
    scene.create("camera",
                 R"({"Transform3D": {"position": [0, 30, 0], "rotation": [-1.5707963, 0, 0]},
                               "Camera3D": {"projection": "orthogonal", "size": 40}})");
    scene.create("sun", R"({"Transform3D": {"rotation": [-1.5707963, 0, 0]},
                            "DirectionalLight3D": {}})");
    render::Image color;
    color.width = 65;
    color.height = 65;
    for (int i = 0; i < 65 * 65; ++i) {
        color.pixels.insert(color.pixels.end(), {200, 80, 40, 255});
    }
    for (int z = 0; z < 65; ++z) {
        for (int x = 0; x < 8; ++x) {
            color.pixels[(cell(65, x, z) * 4) + 3] = 0;
        }
    }
    scene.files.files.emplace("land.color.png", render::encode_png(color).value());
    const entt::entity entity = scene.create("land", R"({"Transform3D": {},
        "HTerrain": {"resolution": 65, "colormap": "land.color.png", "centered": true,
                     "chunk_size": 16}})");
    // Seen from above: the tinted ground in the middle, the hole on the left showing the clear
    // color through.
    auto captured = canvas::capture(scene.world, scene.assets);
    REQUIRE(captured.ok());
    const render::Image& image = captured.value().image;
    const Rgba middle = pixel(image, image.width / 2, image.height / 2);
    CHECK(middle[0] > middle[1]);
    CHECK(middle[1] > middle[2]);
    const std::uint32_t left = (image.width / 2) - (image.height * 30 / 40);
    const Rgba hole = pixel(image, left, image.height / 2);
    CHECK(hole[0] < middle[0]);
    CHECK(captured.value().meshes >= 4);

    // Chunks stay on the device until the terrain changes.
    const auto device = render::create_null_device();
    scene3d::Renderer renderer(*device, nullptr);
    REQUIRE(renderer.prepare(scene.world, 64.0F, 64.0F, 1.0, {}).has_value());
    const std::size_t chunks = renderer.meshes();
    CHECK(chunks >= 4);
    CHECK(renderer.meshes_kept() == chunks);
    CHECK(renderer.problems().empty());
    (void)renderer.prepare(scene.world, 64.0F, 64.0F, 1.0, {});
    CHECK(renderer.meshes_kept() == chunks);
    scene.terrain(entity).height_range = {5.0F, 10.0F};
    (void)renderer.prepare(scene.world, 64.0F, 64.0F, 1.0, {});
    CHECK(renderer.meshes() == chunks);
    CHECK(renderer.meshes_kept() == chunks);
    // A terrain that can't be made draws nothing and says why.
    scene.terrain(entity).chunk_size = 20;
    (void)renderer.prepare(scene.world, 64.0F, 64.0F, 1.0, {});
    CHECK(renderer.meshes() == 0);
    REQUIRE(renderer.problems().size() == 1);
    CHECK(renderer.problems()[0].path == "/components/HTerrain/chunk_size");
    scene.terrain(entity).chunk_size = 16;
    scene.world.ecs().get<world::Transform3D>(entity).visible = false;
    (void)renderer.prepare(scene.world, 64.0F, 64.0F, 1.0, {});
    CHECK(renderer.meshes() == 0);
}

namespace {

// A terrain chunk of 65 cells at its second level of detail, shaded with two ground layers, a
// splat map, a color map with a hole and its normal map: what Renderer draws for an HTerrain.
// Its hills are gentle, as GPUs filter 8-bit textures with 8-bit weights, which the light on a
// steep, noisy terrain's normal map turns into pixels a few levels apart.
render::Image draw_terrain(render::RenderDevice& device) {
    std::vector<float> heights;
    for (int z = 0; z < 65; ++z) {
        for (int x = 0; x < 65; ++x) {
            const Vec2 a = slash::cos_sin(static_cast<float>(x) * 0.12F);
            const Vec2 b = slash::cos_sin(static_cast<float>(z) * 0.09F);
            heights.push_back(8.0F + (6.0F * a.y * b.x));
        }
    }
    TerrainData data = data_of(65, heights);
    const TerrainChunk chunk{.x = 0, .z = 0, .lod = 2, .seams = 0};
    const std::vector<render::Vertex3D> vertices = scene3d::terrain_chunk_vertices(data, 16, chunk);
    const std::vector<std::uint32_t> indices = scene3d::terrain_indices(16, 16, 0);
    auto mesh = device.create_mesh(vertices, indices);
    REQUIRE(mesh.ok());
    const auto texture = [&](std::uint32_t size, const std::vector<std::uint8_t>& texels,
                             render::Filter filter) {
        auto made =
            device.create_texture({.width = size, .height = size, .filter = filter}, texels);
        REQUIRE(made.ok());
        return made.value();
    };
    std::vector<std::uint8_t> splat;
    std::vector<std::uint8_t> color;
    for (int z = 0; z < 65; ++z) {
        for (int x = 0; x < 65; ++x) {
            const auto w = static_cast<std::uint8_t>(x * 255 / 64);
            splat.insert(splat.end(), {static_cast<std::uint8_t>(255 - w), w, 0, 0});
            const bool hole = x > 40 && x < 48 && z > 20 && z < 28;
            color.insert(color.end(), {static_cast<std::uint8_t>(150 + z), 200,
                                       static_cast<std::uint8_t>(255 - (2 * z)),
                                       static_cast<std::uint8_t>(hole ? 0 : 255)});
        }
    }
    // Two 4x4 ground textures: a checker with bumps, and a plain one; one normal map.
    std::vector<std::uint8_t> grass;
    std::vector<std::uint8_t> rock;
    std::vector<std::uint8_t> bumpy;
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            const bool dark = (x + y) % 2 == 0;
            grass.insert(grass.end(), {static_cast<std::uint8_t>(dark ? 60 : 120), 170, 70,
                                       static_cast<std::uint8_t>(dark ? 40 : 220)});
            rock.insert(rock.end(), {140, 130, 120, 128});
            bumpy.insert(bumpy.end(), {static_cast<std::uint8_t>(dark ? 100 : 150), 128, 230,
                                       static_cast<std::uint8_t>(dark ? 120 : 250)});
        }
    }
    const render::Image normals = scene3d::terrain_normals(data, {1.0F, 1.0F, 1.0F});
    render::Material3D material;
    material.specular = 0.4F;
    render::TerrainShading& shading = material.terrain;
    shading.enabled = true;
    shading.resolution = 65;
    shading.colormap = {.texture = texture(65, color, render::Filter::Linear)};
    shading.splatmap = {.texture = texture(65, splat, render::Filter::Linear)};
    shading.normalmap = {.texture = texture(65, normals.pixels, render::Filter::Linear)};
    shading.layers[0] = {
        .albedo_bump = {.texture = texture(4, grass, render::Filter::Linear), .repeat = true},
        .normal_roughness = {.texture = texture(4, bumpy, render::Filter::Linear), .repeat = true},
        .uv_scale = 8.0F};
    shading.layers[1] = {
        .albedo_bump = {.texture = texture(4, rock, render::Filter::Linear), .repeat = true},
        .uv_scale = 16.0F,
        .colormap_opacity = 0.3F};

    render::Scene3D scene;
    scene.camera = render::look_at({32.0F, 45.0F, 80.0F}, {32.0F, 5.0F, 30.0F}, {0.0F, 1.0F, 0.0F});
    scene.projection = render::perspective(60.0F, 1.0F, 0.5F, 300.0F);
    scene.ambient = {0.3F, 0.35F, 0.45F};
    scene.ambient_energy = 0.6F;
    scene.lights = {{.type = render::LightType::Directional,
                     .color = {1.0F, 0.95F, 0.85F},
                     .direction = {-0.5F, -0.7F, -0.4F}}};
    auto target = device.create_texture({.width = 96, .height = 96, .render_target = true});
    REQUIRE(target.ok());
    REQUIRE(device
                .begin_frame(
                    {.target = target.value(), .clear = {0.1F, 0.1F, 0.2F, 1.0F}, .scene = scene})
                .ok());
    const std::array<float, 16> identity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    REQUIRE(device.draw_mesh(mesh.value(), identity, material).ok());
    REQUIRE(device.end_frame().ok());
    auto image = device.read_texture(target.value());
    REQUIRE(image.ok());
    return image.value();
}

} // namespace

TEST_CASE("GPUs shade terrains as the software device does") {
    const render::Image expected = draw_terrain(*render::create_software_device());
    // The ground fills the middle; the hole shows the clear color.
    int ground = 0;
    for (std::size_t i = 0; i < expected.pixels.size(); i += 4) {
        ground += expected.pixels[i + 1] > 60 ? 1 : 0;
    }
    CHECK(ground > 96 * 96 / 3);
    for (auto& device : slash::test::hardware_devices()) {
        CAPTURE(render::backend_name(device->backend()));
        const render::Image image = draw_terrain(*device);
        int differing = 0;
        int worst = 0;
        for (std::size_t i = 0; i < image.pixels.size(); i += 4) {
            int most = 0;
            for (std::size_t c = 0; c < 4; ++c) {
                most = std::max(most,
                                std::abs(int{image.pixels[i + c]} - int{expected.pixels[i + c]}));
            }
            differing += most > 2 ? 1 : 0;
            worst = std::max(worst, most);
        }
        CAPTURE(worst);
        CHECK(differing <= 20);
    }
}
