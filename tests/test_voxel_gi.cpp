#include "devices.hpp"

#include "slash/core/jobs.hpp"
#include "slash/core/json.hpp"
#include "slash/render/device.hpp"
#include "slash/render/mesh.hpp"
#include "slash/scene3d/primitive.hpp"
#include "slash/scene3d/voxel_gi.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/tree3d.hpp"
#include "slash/world/world.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace json = slash::json;
namespace jobs = slash::jobs;
namespace render = slash::render;
namespace scene3d = slash::scene3d;
namespace world = slash::world;

namespace {

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE_MESSAGE(parsed.ok(), (parsed.ok() ? "" : parsed.error().message));
    return std::move(parsed).value();
}

// A world with the built-in components.
struct Scene {
    world::ComponentRegistry components;
    world::World world{components};

    Scene() { world::register_builtin_components(components); }

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
};

// A mesh made of a primitive, placed at x, y, z in a VoxelGI's space, of one material.
scene3d::VoxelGIMesh mesh_of(const scene3d::MeshData& data, float x, float y, float z,
                             const world::StandardMaterial3D& standard = {}) {
    scene3d::VoxelGIMesh mesh;
    mesh.transform.origin = {x, y, z};
    mesh.vertices = data.vertices;
    mesh.indices = data.indices;
    mesh.surfaces.push_back({.first_index = 0,
                             .material = std::make_shared<const scene3d::VoxelGIMaterial>(
                                 scene3d::voxel_gi_material(standard, nullptr, nullptr, 1.0F))});
    return mesh;
}

// A floor of about 3 by 3 meters in the middle of a VoxelGI of 4 meters at 64 cells a side, a
// sixteenth of a meter each: its cells are those of row y = 16 from 8 to 55 along x and z. With a
// box of a meter floating a meter over it where asked, its faces in columns and rows 24 and 40 and
// its bottom and top in rows 32 and 48 (each face half a cell off the cells' edges, so it is in one
// cell).
std::vector<scene3d::VoxelGIMesh> floor_meshes(bool with_box) {
    world::PlaneMesh plane;
    plane.size = {2.96875F, 2.96875F};
    std::vector<scene3d::VoxelGIMesh> meshes;
    meshes.push_back(mesh_of(scene3d::plane_mesh(plane), 0.0F, -0.96875F, 0.0F));
    if (with_box) {
        meshes.push_back(mesh_of(scene3d::box_mesh({}), 0.03125F, 0.53125F, 0.03125F));
    }
    return meshes;
}

constexpr std::array<float, 3> kSize{4.0F, 4.0F, 4.0F};

std::array<std::uint32_t, 3> position(const scene3d::VoxelGICell& cell) {
    return {cell[0] & 0x7FFU, (cell[0] >> 11U) & 0x3FFU, cell[0] >> 21U};
}

std::int8_t normal_y(const scene3d::VoxelGICell& cell) {
    return static_cast<std::int8_t>((cell[3] >> 8U) & 0xFFU);
}

// A texel of a level of a VoxelGI's light texture.
std::array<std::uint8_t, 4> texel(const scene3d::VoxelGITexture& texture, std::size_t level,
                                  std::uint32_t x, std::uint32_t y, std::uint32_t z) {
    const auto& size = texture.sizes.at(level);
    const std::size_t at =
        ((std::size_t{z} * size[1] * size[0]) + (std::size_t{y} * size[0]) + x) * 4;
    const auto& bytes = texture.levels.at(level);
    return {bytes.at(at), bytes.at(at + 1), bytes.at(at + 2), bytes.at(at + 3)};
}

// A light straight down onto the floor, in the cells' space.
scene3d::VoxelGILight sun(bool shadow) {
    render::Light3D light;
    light.type = render::LightType::Directional;
    light.direction = {0.0F, -1.0F, 0.0F};
    light.shadow = shadow;
    scene3d::VoxelGIData data;
    data.bounds_position = {-2.0F, -2.0F, -2.0F};
    data.bounds_size = kSize;
    data.octree_size = {64, 64, 64};
    return scene3d::voxel_gi_light(light, 1.0F, data, {});
}

} // namespace

TEST_CASE("VoxelGI octrees are as big as Godot's for a box and its subdiv") {
    CHECK(scene3d::voxel_gi_subdiv("64") == 6);
    CHECK(scene3d::voxel_gi_subdiv("512") == 9);
    CHECK(scene3d::voxel_gi_subdiv("100") == 0);
    CHECK(scene3d::voxel_gi_octree_size({20.0F, 20.0F, 20.0F}, 7) ==
          std::array<std::uint32_t, 3>{128, 128, 128});
    // Each shorter side is halved while half the longest side still covers it.
    CHECK(scene3d::voxel_gi_octree_size({20.0F, 10.0F, 5.0F}, 7) ==
          std::array<std::uint32_t, 3>{128, 64, 32});
    CHECK(scene3d::voxel_gi_octree_size({11.0F, 20.0F, 4.9F}, 6) ==
          std::array<std::uint32_t, 3>{64, 64, 16});

    scene3d::VoxelGIData data;
    data.bounds_position = {-10.0F, -5.0F, -2.5F};
    data.bounds_size = {20.0F, 10.0F, 5.0F};
    data.octree_size = {128, 64, 32};
    const world::Affine3D to_cell = scene3d::voxel_gi_to_cell(data);
    // The box's corner at 0, a cell a unit along every axis.
    const slash::Vec3 corner = to_cell.apply({-10.0F, -5.0F, -2.5F});
    const slash::Vec3 far = to_cell.apply({10.0F, 5.0F, 2.5F});
    CHECK(corner.x == doctest::Approx(0.0F));
    CHECK(corner.y == doctest::Approx(0.0F));
    CHECK(corner.z == doctest::Approx(0.0F));
    CHECK(far.x == doctest::Approx(128.0F));
    CHECK(far.y == doctest::Approx(64.0F));
    CHECK(far.z == doctest::Approx(32.0F));
}

TEST_CASE("a VoxelGI voxelizes meshes into an octree of cells, as Godot's Voxelizer") {
    world::StandardMaterial3D red;
    red.albedo_color = {1.0F, 0.2F, 0.2F, 1.0F};
    world::PlaneMesh plane;
    plane.size = {2.96875F, 2.96875F};
    const std::vector<scene3d::VoxelGIMesh> meshes{
        mesh_of(scene3d::plane_mesh(plane), 0.0F, -0.96875F, 0.0F, red)};
    const scene3d::VoxelGIData data = scene3d::voxelize(meshes, kSize, 6);

    CHECK(data.octree_size == std::array<std::uint32_t, 3>{64, 64, 64});
    CHECK(data.bounds_position == std::array<float, 3>{-2.0F, -2.0F, -2.0F});
    REQUIRE(data.level_counts.size() == 7);
    CHECK(data.level_counts[0] == 1);
    // The floor covers 48 of the 64 cells along x and z: 2304 at the deepest level, a quarter as
    // many a level up, and so on. (Faces on the cells' edges are in the cells on both sides.)
    CHECK(data.level_counts[6] == 48 * 48);
    CHECK(data.level_counts[5] == 24 * 24);
    CHECK(data.level_counts[4] == 12 * 12);
    std::size_t total = 0;
    for (const std::uint32_t count : data.level_counts) {
        total += count;
    }
    CHECK(data.cells.size() == total);

    const std::size_t first_leaf = data.cells.size() - data.level_counts[6];
    for (std::size_t i = first_leaf; i < data.cells.size(); ++i) {
        const scene3d::VoxelGICell& cell = data.cells[i];
        const auto p = position(cell);
        REQUIRE(p[1] == 16);
        REQUIRE(p[0] >= 8);
        REQUIRE(p[0] < 56);
        // Red, linear (0.2 in sRGB is 0.033), and solid.
        REQUIRE((cell[1] & 0xFFU) == 255);
        REQUIRE(((cell[1] >> 8U) & 0xFFU) == 8);
        REQUIRE(((cell[1] >> 16U) & 0xFFU) == 8);
        REQUIRE((cell[1] >> 24U) == 255);
        // No emission, and facing up.
        REQUIRE(normal_y(cell) == 127);
    }
    // Sorted by x, then y, then z, within each level.
    for (std::size_t i = first_leaf + 1; i < data.cells.size(); ++i) {
        const auto a = position(data.cells[i - 1]);
        const auto b = position(data.cells[i]);
        REQUIRE((a[0] < b[0] || (a[0] == b[0] && (a[1] < b[1] || (a[1] == b[1] && a[2] < b[2])))));
    }
    // A mesh outside the box leaves no cells.
    const std::vector<scene3d::VoxelGIMesh> outside{
        mesh_of(scene3d::plane_mesh(plane), 0.0F, 3.0F, 0.0F)};
    const scene3d::VoxelGIData empty = scene3d::voxelize(outside, kSize, 6);
    CHECK(empty.cells.size() == 1);
    CHECK(empty.level_counts[0] == 1);
    // The same cells every time.
    CHECK(scene3d::voxelize(meshes, kSize, 6) == data);
}

TEST_CASE("emissive materials voxelize their emission at the bake's exposure") {
    world::StandardMaterial3D glow;
    glow.emission_enabled = true;
    glow.emission = {1.0F, 0.5F, 0.0F, 1.0F};
    glow.emission_energy_multiplier = 2.0F;
    const scene3d::VoxelGIMaterial material =
        scene3d::voxel_gi_material(glow, nullptr, nullptr, 0.5F);
    REQUIRE(material.emission.size() ==
            std::size_t{scene3d::kVoxelGIMaterialSize} * scene3d::kVoxelGIMaterialSize);
    // Linear, times the energy and the exposure.
    CHECK(material.emission[0][0] == doctest::Approx(1.0F));
    CHECK(material.emission[0][1] == doctest::Approx(0.214F).epsilon(0.01));
    CHECK(material.emission[0][2] == doctest::Approx(0.0F));
    CHECK(material.albedo[0][0] == doctest::Approx(1.0F));
}

TEST_CASE(".voxelgi.json files keep a VoxelGI's cells, and say what's wrong with them") {
    const scene3d::VoxelGIData data = scene3d::voxelize(floor_meshes(true), kSize, 6, 0.75F);
    const json::Value doc = scene3d::voxel_gi_json(data);
    CHECK(scene3d::validate_voxel_gi(doc).empty());
    const auto parsed = scene3d::parse_voxel_gi(doc);
    REQUIRE(parsed.ok());
    CHECK(parsed.value() == data);
    CHECK(parsed.value().baked_exposure == 0.75F);

    const auto first_error = [](const json::Value& bad) {
        const std::vector<json::ValidationError> errors = scene3d::validate_voxel_gi(bad);
        REQUIRE_FALSE(errors.empty());
        return errors.front();
    };
    json::Value bad = doc;
    bad["octree_size"] = json::Array{64, 48, 64};
    CHECK(first_error(bad).path == "/octree_size/1");
    bad = doc;
    bad["level_counts"] = json::Array{1, 2};
    CHECK(first_error(bad).path == "/level_counts");
    bad = doc;
    bad["cells"] = "not base64!";
    CHECK(first_error(bad).path == "/cells");
    bad = doc;
    bad["bounds"] = json::Array{-2, -2, -2, 4, 0, 4};
    CHECK(first_error(bad).path == "/bounds/4");
    bad = doc;
    bad["cellz"] = 1;
    CHECK_FALSE(scene3d::validate_voxel_gi(bad).empty());

    // A deepest cell with no parent.
    scene3d::VoxelGIData orphan = data;
    orphan.cells.back()[0] = 63U | (63U << 11U) | (63U << 21U);
    CHECK(first_error(scene3d::voxel_gi_json(orphan)).path.starts_with("/cells"));
}

TEST_CASE("a VoxelGI's octree links its cells and measures how far each place is from one") {
    const scene3d::VoxelGIData data = scene3d::voxelize(floor_meshes(false), kSize, 6);
    const scene3d::VoxelGIOctree octree = scene3d::voxel_gi_octree(data);
    REQUIRE(octree.level_offsets.size() == 8);
    CHECK(octree.level_offsets.back() == data.cells.size());
    // Every cell above the deepest level has a child, and every child is a level down.
    for (std::size_t level = 0; level + 1 < data.level_counts.size(); ++level) {
        for (std::size_t i = octree.level_offsets[level]; i < octree.level_offsets[level + 1];
             ++i) {
            bool any = false;
            for (const std::uint32_t child : octree.children[i]) {
                if (child == scene3d::kVoxelGIEmpty) {
                    continue;
                }
                any = true;
                REQUIRE(child >= octree.level_offsets[level + 1]);
                REQUIRE(child < octree.level_offsets[level + 2]);
            }
            REQUIRE(any);
        }
    }
    REQUIRE(octree.distance_field.size() == std::size_t{64} * 64 * 64);
    const auto distance = [&](std::size_t x, std::size_t y, std::size_t z) {
        return octree.distance_field[x + (y * 64) + (z * 64 * 64)];
    };
    CHECK(distance(32, 16, 32) == 0);
    CHECK(distance(32, 17, 32) == 2);
    CHECK(distance(32, 26, 32) == 11);
    CHECK(distance(32, 6, 32) == 11);
    // Off the floor's edge, the nearest cell is its corner's.
    CHECK(distance(60, 16, 32) == 6);

    jobs::Pool pool(3);
    const scene3d::VoxelGIOctree threaded = scene3d::voxel_gi_octree(data, &pool);
    CHECK(threaded.children == octree.children);
    CHECK(threaded.distance_field == octree.distance_field);
}

TEST_CASE("light injected into a VoxelGI's cells lights them, casts shadows and bounces") {
    const scene3d::VoxelGIData data = scene3d::voxelize(floor_meshes(true), kSize, 6);
    const scene3d::VoxelGIOctree octree = scene3d::voxel_gi_octree(data);
    scene3d::VoxelGISettings settings;
    settings.use_two_bounces = false;
    const std::array<scene3d::VoxelGILight, 1> lit{sun(true)};
    const scene3d::VoxelGITexture texture = scene3d::light_voxel_gi(data, octree, lit, settings);

    REQUIRE(texture.sizes.size() == 7);
    CHECK(texture.sizes[0] == std::array<std::uint32_t, 3>{64, 64, 64});
    CHECK(texture.sizes[1] == std::array<std::uint32_t, 3>{32, 32, 32});
    CHECK(texture.sizes[6] == std::array<std::uint32_t, 3>{1, 1, 1});
    REQUIRE(texture.levels[0].size() == std::size_t{64} * 64 * 64 * 4);

    // Out in the sun the white floor takes all of a light of energy 1 straight down, stored over
    // a dynamic range of 2; under the box it is in shadow; where there's no cell, nothing.
    const auto open = texel(texture, 0, 12, 16, 12);
    CAPTURE(int{open[0]});
    CHECK(open[0] >= 120);
    CHECK(open[0] <= 130);
    CHECK(open[0] == open[1]);
    CHECK(open[1] == open[2]);
    CHECK(open[3] == 255);
    const auto shadowed = texel(texture, 0, 32, 16, 32);
    CHECK(shadowed[0] == 0);
    CHECK(shadowed[3] == 255);
    CHECK(texel(texture, 0, 32, 30, 32) == std::array<std::uint8_t, 4>{0, 0, 0, 0});
    // The box's top is lit too, and its inside has no cells.
    CHECK(texel(texture, 0, 32, 48, 32)[0] >= 120);
    CHECK(texel(texture, 0, 32, 40, 32)[3] == 0);
    // Levels up average the light of their cells.
    const auto up = texel(texture, 1, 6, 8, 6);
    CHECK(up[0] > 0);
    CHECK(up[3] > 0);

    // Without shadows the light falls under the box too.
    const std::array<scene3d::VoxelGILight, 1> unshadowed{sun(false)};
    const scene3d::VoxelGITexture through =
        scene3d::light_voxel_gi(data, octree, unshadowed, settings);
    CHECK(texel(through, 0, 32, 16, 32)[0] >= 120);

    // A second bounce brings light from the lit floor to the box's sides and the shade under it.
    settings.use_two_bounces = true;
    const scene3d::VoxelGITexture bounced = scene3d::light_voxel_gi(data, octree, lit, settings);
    const auto side = texel(bounced, 0, 24, 33, 32);
    CAPTURE(int{side[0]});
    CHECK(texel(texture, 0, 24, 33, 32)[0] == 0);
    CHECK(texel(texture, 0, 24, 33, 32)[3] == 255);
    CHECK(side[3] == 255);
    CHECK(side[0] > 0);
    CHECK(texel(bounced, 0, 12, 16, 12)[0] >= open[0]);

    // The same texels for any number of threads.
    jobs::Pool pool(3);
    const scene3d::VoxelGITexture threaded =
        scene3d::light_voxel_gi(data, octree, lit, settings, &pool);
    CHECK(threaded.levels == bounced.levels);
}

TEST_CASE("omni and spot lights reach a VoxelGI's cells as far as their range") {
    const scene3d::VoxelGIData data = scene3d::voxelize(floor_meshes(false), kSize, 6);
    const scene3d::VoxelGIOctree octree = scene3d::voxel_gi_octree(data);
    scene3d::VoxelGISettings settings;
    settings.use_two_bounces = false;

    render::Light3D omni;
    omni.type = render::LightType::Omni;
    omni.position = {-1.0F, 0.0F, -1.0F};
    omni.range = 1.5F;
    const world::Affine3D at_origin;
    const scene3d::VoxelGILight light = scene3d::voxel_gi_light(omni, 1.0F, data, at_origin);
    CHECK(light.radius == doctest::Approx(24.0F));
    CHECK(light.position[0] == doctest::Approx(16.0F));
    CHECK(light.position[1] == doctest::Approx(32.0F));
    const std::array<scene3d::VoxelGILight, 1> lights{light};
    const scene3d::VoxelGITexture texture = scene3d::light_voxel_gi(data, octree, lights, settings);
    CHECK(texel(texture, 0, 16, 16, 16)[0] > 0);
    CHECK(texel(texture, 0, 48, 16, 48)[0] == 0);

    render::Light3D spot;
    spot.type = render::LightType::Spot;
    spot.position = {0.0F, 1.0F, 0.0F};
    spot.direction = {0.0F, -1.0F, 0.0F};
    spot.range = 4.0F;
    spot.spot_angle = 20.0F;
    const std::array<scene3d::VoxelGILight, 1> spots{
        scene3d::voxel_gi_light(spot, 1.0F, data, at_origin)};
    const scene3d::VoxelGITexture cone = scene3d::light_voxel_gi(data, octree, spots, settings);
    CHECK(texel(cone, 0, 32, 16, 32)[0] > 0);
    CHECK(texel(cone, 0, 10, 16, 10)[0] == 0);
}

TEST_CASE("a VoxelGI bakes the static meshes in its box, as Godot's VoxelGI::bake") {
    Scene scene;
    scene.create("floor", R"({"Transform3D": {"position": [0, -0.96875, 0]},
        "MeshInstance3D": {}, "PlaneMesh": {"size": [2.96875, 2.96875]}})");
    scene.create("box", R"({"Transform3D": {"position": [0.03125, 0.53125, 0.03125]},
        "MeshInstance3D": {}, "BoxMesh": {}})");
    scene.create("moving", R"({"Transform3D": {"position": [1, 0.5, 1]},
        "MeshInstance3D": {"gi_mode": "dynamic"}, "BoxMesh": {}})");
    scene.create("far", R"({"Transform3D": {"position": [10, 0, 0]}, "MeshInstance3D": {},
        "BoxMesh": {}})");
    const entt::entity gi = scene.create(
        "gi", R"({"Transform3D": {}, "VoxelGI": {"subdiv": "64", "size": [4, 4, 4]}})");
    const scene3d::VoxelGIBake bake = scene3d::bake_voxel_gi(scene.world, gi, {}).value();
    CHECK(bake.problems.empty());
    CHECK(bake.meshes == 2);
    CHECK(bake.data == scene3d::voxelize(floor_meshes(true), kSize, 6));

    // Baked where the VoxelGI is: moved, the meshes are elsewhere in its box.
    const entt::entity moved = scene.create("moved", R"({"Transform3D": {"position": [0, 1, 0]},
        "VoxelGI": {"subdiv": "64", "size": [4, 4, 4]}})");
    const scene3d::VoxelGIBake higher = scene3d::bake_voxel_gi(scene.world, moved, {}).value();
    CHECK(higher.meshes == 2);
    CHECK(higher.data != bake.data);

    // Its emission at the exposure of its camera attributes.
    scene.create("camera", R"({"CameraAttributesPractical": {"exposure_multiplier": 0.5}})");
    const entt::entity exposed = scene.create(
        "exposed", R"({"Transform3D": {}, "VoxelGI": {"subdiv": "64", "size": [4, 4, 4],
        "camera_attributes": "camera"}})");
    CHECK(scene3d::bake_voxel_gi(scene.world, exposed, {}).value().data.baked_exposure == 0.5F);

    const entt::entity bad = scene.create("bad", R"({"VoxelGI": {"subdiv": "100"}})");
    CHECK_FALSE(scene3d::bake_voxel_gi(scene.world, bad, {}).ok());
    CHECK_FALSE(scene3d::bake_voxel_gi(scene.world, scene.world.find("floor"), {}).ok());
}

TEST_CASE("VoxelGIs say what's wrong with their settings and data") {
    Scene scene;
    scene.create("gi", R"({"VoxelGI": {"subdiv": "100", "size": [4, 0, 4],
        "camera_attributes": "nobody", "data": "gi.json"}})");
    const std::vector<scene3d::Problem> problems = scene3d::check_voxel_gis(scene.world);
    REQUIRE(problems.size() == 4);
    CHECK(problems[0].path == "/components/VoxelGI/subdiv");
    CHECK(problems[1].path == "/components/VoxelGI/size");
    CHECK(problems[2].path == "/components/VoxelGI/camera_attributes");
    CHECK(problems[3].path == "/components/VoxelGI/data");
    CHECK(problems[3].message.find(".voxelgi.json") != std::string::npos);
}

TEST_CASE("a VoxelGI's texture packs into an atlas, each level's slices side by side") {
    scene3d::VoxelGITexture texture;
    for (const std::array<std::uint32_t, 3> size :
         {std::array<std::uint32_t, 3>{8, 4, 2}, std::array<std::uint32_t, 3>{4, 2, 1}}) {
        texture.sizes.push_back(size);
        std::vector<std::uint8_t> bytes;
        for (std::uint32_t i = 0; i < size[0] * size[1] * size[2]; ++i) {
            for (std::uint8_t c = 0; c < 4; ++c) {
                bytes.push_back(static_cast<std::uint8_t>((i * 4) + c + (size[0] * 10)));
            }
        }
        texture.levels.push_back(bytes);
    }
    const auto atlas = scene3d::voxel_gi_atlas(texture);
    REQUIRE(atlas.ok());
    // Both of level 0's slices in a row, level 1 under them.
    CHECK(atlas.value().image.width == 16);
    CHECK(atlas.value().image.height == 6);
    CHECK(atlas.value().levels == 2);
    CHECK(atlas.value().placement[0] == std::array<std::uint32_t, 3>{0, 0, 2});
    CHECK(atlas.value().placement[1] == std::array<std::uint32_t, 3>{0, 4, 1});
    const auto atlas_texel = [&](std::uint32_t x, std::uint32_t y) {
        return atlas.value().image.pixels.at(((std::size_t{y} * 16) + x) * 4);
    };
    // Level 0's texel (3, 2, 1) is in its second slice; level 1's (1, 1, 0) under it.
    CHECK(atlas_texel(8 + 3, 2) == texel(texture, 0, 3, 2, 1)[0]);
    CHECK(atlas_texel(1, 4 + 1) == texel(texture, 1, 1, 1, 0)[0]);
    CHECK(atlas_texel(5, 5) == 0);

    // Slices too wide for one row of 4096 texels wrap onto more rows.
    scene3d::VoxelGITexture wide;
    wide.sizes = {{1024, 2, 8}};
    wide.levels = {std::vector<std::uint8_t>(std::size_t{1024} * 2 * 8 * 4, 1)};
    const auto rows = scene3d::voxel_gi_atlas(wide);
    REQUIRE(rows.ok());
    CHECK(rows.value().image.width == 4096);
    CHECK(rows.value().image.height == 4);
    CHECK(rows.value().placement[0][2] == 4);

    wide.levels[0].pop_back();
    CHECK_FALSE(scene3d::voxel_gi_atlas(wide).ok());
    CHECK_FALSE(scene3d::voxel_gi_atlas({}).ok());
}

namespace {

// The floor and box voxelized at 64 cells, lit by a sun from above at a slant that the box shades,
// in the atlas a device samples, and the probe of a VoxelGI at the world's origin.
struct LitVoxelGI {
    scene3d::VoxelGIAtlas atlas;
    render::VoxelGIProbe probe;
};
LitVoxelGI lit_voxel_gi(bool interior = false) {
    const scene3d::VoxelGIData data = scene3d::voxelize(floor_meshes(true), kSize, 6);
    const scene3d::VoxelGIOctree octree = scene3d::voxel_gi_octree(data);
    render::Light3D light;
    light.type = render::LightType::Directional;
    light.direction = {-0.3F, -0.9F, -0.3F};
    light.energy = 2.0F;
    light.shadow = true;
    const scene3d::VoxelGILight sunlight = scene3d::voxel_gi_light(light, 1.0F, data, {});
    const scene3d::VoxelGITexture texture =
        scene3d::light_voxel_gi(data, octree, std::span(&sunlight, 1), {});
    auto atlas = scene3d::voxel_gi_atlas(texture);
    REQUIRE(atlas.ok());
    world::VoxelGI component;
    component.interior = interior;
    LitVoxelGI out{.atlas = std::move(atlas).value(), .probe = {}};
    out.probe = scene3d::voxel_gi_probe(component, data, out.atlas, {});
    return out;
}

struct VoxelGIDraw {
    bool traced = true;
    bool high_quality = false;
    render::RenderMethod method = render::RenderMethod::ForwardPlus;
};

// The floor and box drawn with no lights, under a blue ambient light, their ambient light and
// reflections from the VoxelGI where traced.
render::Image draw_voxel_gi(render::RenderDevice& device, const LitVoxelGI& gi,
                            const VoxelGIDraw& options = {}) {
    auto target = device.create_texture({.width = 48, .height = 48, .render_target = true});
    REQUIRE(target.ok());
    auto atlas = device.create_texture(
        {.width = gi.atlas.image.width, .height = gi.atlas.image.height}, gi.atlas.image.pixels);
    REQUIRE(atlas.ok());
    world::PlaneMesh plane;
    plane.size = {2.96875F, 2.96875F};
    const scene3d::MeshData floor_data = scene3d::plane_mesh(plane);
    const scene3d::MeshData box_data = scene3d::box_mesh({});
    auto floor = device.create_mesh(floor_data.vertices, floor_data.indices);
    auto box = device.create_mesh(box_data.vertices, box_data.indices);
    REQUIRE(floor.ok());
    REQUIRE(box.ok());
    render::Scene3D scene;
    scene.method = options.method;
    scene.camera = render::look_at({1.6F, 1.4F, 2.6F}, {0.0F, -0.4F, 0.0F}, {0.0F, 1.0F, 0.0F});
    scene.projection = render::perspective(60.0F, 1.0F, 0.1F, 10.0F);
    scene.ambient = {0.1F, 0.2F, 0.6F};
    scene.voxel_gi_high_quality = options.high_quality;
    REQUIRE(device
                .begin_frame(
                    {.target = target.value(), .clear = {0.0F, 0.0F, 0.0F, 1.0F}, .scene = scene})
                .ok());
    render::Material3D material{.albedo = {0.8F, 0.8F, 0.8F, 1.0F}, .roughness = 0.6F};
    if (options.traced) {
        material.voxel_gi = {.atlas = atlas.value(), .probes = {gi.probe, {}}, .count = 1};
    }
    world::Affine3D place;
    place.origin = {0.0F, -0.96875F, 0.0F};
    auto drawn = device.draw_mesh(floor.value(), place.matrix(), material);
    CHECK_MESSAGE(drawn.ok(), (drawn.ok() ? "" : drawn.error().message));
    material.albedo = {1.0F, 0.2F, 0.2F, 1.0F};
    place.origin = {0.03125F, 0.53125F, 0.03125F};
    drawn = device.draw_mesh(box.value(), place.matrix(), material);
    CHECK_MESSAGE(drawn.ok(), (drawn.ok() ? "" : drawn.error().message));
    REQUIRE(device.end_frame().ok());
    auto image = device.read_texture(target.value());
    REQUIRE(image.ok());
    device.destroy_mesh(floor.value());
    device.destroy_mesh(box.value());
    device.destroy_texture(atlas.value());
    device.destroy_texture(target.value());
    return std::move(image).value();
}

std::array<int, 4> image_pixel(const render::Image& image, std::uint32_t x, std::uint32_t y) {
    const std::size_t at = ((std::size_t{y} * image.width) + x) * 4;
    return {image.pixels[at], image.pixels[at + 1], image.pixels[at + 2], image.pixels[at + 3]};
}

// How many pixels of two images differ by more than 2 in a channel, and the most any does.
std::array<int, 2> differences(const render::Image& a, const render::Image& b) {
    int differing = 0;
    int worst = 0;
    for (std::uint32_t y = 0; y < a.height; ++y) {
        for (std::uint32_t x = 0; x < a.width; ++x) {
            const std::array<int, 4> p = image_pixel(a, x, y);
            const std::array<int, 4> q = image_pixel(b, x, y);
            int most = 0;
            for (std::size_t c = 0; c < 4; ++c) {
                most = std::max(most, std::abs(p.at(c) - q.at(c)));
            }
            differing += most > 2 ? 1 : 0;
            worst = std::max(worst, most);
        }
    }
    return {differing, worst};
}

} // namespace

TEST_CASE("meshes take ambient light and reflections from a VoxelGI's cones, as Godot's") {
    const auto software = render::create_software_device();
    const LitVoxelGI gi = lit_voxel_gi();
    const render::Image traced = draw_voxel_gi(*software, gi);
    const render::Image plain = draw_voxel_gi(*software, gi, {.traced = false});
    // The sunlit floor lights itself and the box where only the blue ambient light would.
    const std::array<int, 4> lit = image_pixel(traced, 26, 38);
    const std::array<int, 4> ambient = image_pixel(plain, 26, 38);
    CAPTURE(lit[0]);
    CAPTURE(ambient[0]);
    CHECK(lit[0] > ambient[0] + 10);
    CHECK(ambient[2] > ambient[0]);
    // High quality's six narrower cones differ from the four of 45 degrees.
    CHECK(draw_voxel_gi(*software, gi, {.high_quality = true}).pixels != traced.pixels);
    // An interior VoxelGI lets none of the ambient light through where its cones see nothing.
    CHECK(draw_voxel_gi(*software, lit_voxel_gi(true)).pixels != traced.pixels);
    // Compatibility traces no VoxelGIs.
    CHECK(draw_voxel_gi(*software, gi, {.method = render::RenderMethod::Compatibility}).pixels ==
          draw_voxel_gi(*software, gi,
                        {.traced = false, .method = render::RenderMethod::Compatibility})
              .pixels);

    // Devices refuse VoxelGIs they can't trace.
    auto target = software->create_texture({.width = 4, .height = 4, .render_target = true});
    auto atlas =
        software->create_texture({.width = 1, .height = 1}, std::vector<std::uint8_t>{0, 0, 0, 0});
    const scene3d::MeshData box_data = scene3d::box_mesh({});
    auto box = software->create_mesh(box_data.vertices, box_data.indices);
    REQUIRE(target.ok());
    REQUIRE(atlas.ok());
    REQUIRE(box.ok());
    render::Scene3D scene;
    scene.camera = render::look_at({0.0F, 0.0F, 3.0F}, {}, {0.0F, 1.0F, 0.0F});
    scene.projection = render::perspective(60.0F, 1.0F, 0.1F, 10.0F);
    REQUIRE(software->begin_frame({.target = target.value(), .clear = {}, .scene = scene}).ok());
    const std::array<float, 16> identity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    const auto draws = [&](const std::function<void(render::VoxelGIShading&)>& change) {
        render::Material3D material;
        material.voxel_gi = {.atlas = atlas.value(), .probes = {gi.probe, {}}, .count = 1};
        change(material.voxel_gi);
        return software->draw_mesh(box.value(), identity, material).ok();
    };
    CHECK(draws([](render::VoxelGIShading&) {}));
    CHECK_FALSE(draws([](render::VoxelGIShading& v) { v.count = 3; }));
    CHECK_FALSE(draws([](render::VoxelGIShading& v) { v.probes[0].bias = -1.0F; }));
    CHECK_FALSE(draws([](render::VoxelGIShading& v) {
        v.probes[0].to_cell[3] = std::numeric_limits<float>::quiet_NaN();
    }));
    CHECK_FALSE(draws([](render::VoxelGIShading& v) { v.probes[0].levels = 11; }));
    CHECK_FALSE(draws([](render::VoxelGIShading& v) { v.probes[0].size[1] = 2048; }));
    CHECK_FALSE(draws([](render::VoxelGIShading& v) { v.atlas = render::TextureId{999}; }));
    REQUIRE(software->end_frame().ok());
}

namespace {

// The floor and box under a sun, with a VoxelGI around them whose data is "gi.voxelgi.json", seen
// from above.
struct VoxelGIWorld {
    Scene scene;
    entt::entity sun = entt::null;
    entt::entity gi = entt::null;
    std::shared_ptr<const scene3d::VoxelGIData> data;

    VoxelGIWorld() {
        scene.create("camera", R"({"Transform3D": {"position": [0.8, 0.9, 1.4],
            "rotation": [-0.6, 0.4, 0]}, "Camera3D": {"current": true}})");
        scene.create("floor", R"({"Transform3D": {"position": [0, -0.96875, 0]},
            "MeshInstance3D": {}, "PlaneMesh": {"size": [2.96875, 2.96875]},
            "StandardMaterial3D": {"roughness": 0.6}})");
        scene.create("box", R"({"Transform3D": {"position": [0.03125, 0.53125, 0.03125]},
            "MeshInstance3D": {}, "BoxMesh": {},
            "StandardMaterial3D": {"albedo_color": [1, 0.2, 0.2, 1]}})");
        scene.create("environment", R"({"WorldEnvironment": {"ambient_light_source": "color",
            "ambient_light_color": [0.1, 0.2, 0.6, 1]}})");
        sun = scene.create("sun", R"({"Transform3D": {"rotation": [-1.2, 0.3, 0]},
            "DirectionalLight3D": {"shadow_enabled": true, "light_energy": 2}})");
        gi = scene.create("gi", R"({"Transform3D": {}, "VoxelGI": {"subdiv": "64",
            "size": [4, 4, 4], "data": "gi.voxelgi.json"}})");
        data = std::make_shared<const scene3d::VoxelGIData>(
            scene3d::bake_voxel_gi(scene.world, gi, {}).value().data);
    }

    // The cells of "gi.voxelgi.json", counting how often they load.
    scene3d::VoxelGISource source(int* loads = nullptr) const {
        return [this, loads](const std::string& path)
                   -> slash::Result<std::shared_ptr<const scene3d::VoxelGIData>> {
            if (loads != nullptr) {
                ++*loads;
            }
            if (path != "gi.voxelgi.json") {
                return slash::make_error(slash::ErrorCode::NotFound,
                                         "\"" + path + "\" doesn't exist");
            }
            return std::shared_ptr<const scene3d::VoxelGIData>(data);
        };
    }
};

// A frame of the world drawn by renderer on the software device.
render::Image draw_world(render::RenderDevice& device, scene3d::Renderer& renderer,
                         const world::World& world,
                         render::RenderMethod method = render::RenderMethod::ForwardPlus,
                         bool high_quality = false) {
    auto frame = renderer.prepare(world, 48.0F, 48.0F, 1.0, {}, {}, 0.0, {}, {}, {}, {}, method,
                                  high_quality);
    REQUIRE(frame.has_value());
    frame->scene.method = method;
    auto target = device.create_texture({.width = 48, .height = 48, .render_target = true});
    REQUIRE(target.ok());
    REQUIRE(
        device.begin_frame({.target = target.value(), .clear = {}, .scene = frame->scene}).ok());
    REQUIRE(renderer.draw().ok());
    REQUIRE(device.end_frame().ok());
    auto image = device.read_texture(target.value());
    REQUIRE(image.ok());
    device.destroy_texture(target.value());
    return std::move(image).value();
}

} // namespace

TEST_CASE("the 3D renderer lights a world's VoxelGIs with its lights and traces them") {
    VoxelGIWorld gi;
    const auto device = render::create_software_device();
    int loads = 0;
    scene3d::Renderer renderer(*device, nullptr, {}, {}, gi.source(&loads));
    scene3d::Renderer without(*device, nullptr);
    const render::Image traced = draw_world(*device, renderer, gi.scene.world);
    CHECK(renderer.problems().empty());
    CHECK(loads == 1);
    // A renderer with no VoxelGIs to load reports it, and draws as before.
    const render::Image plain = draw_world(*device, without, gi.scene.world);
    REQUIRE(without.problems().size() == 1);
    CHECK(without.problems()[0].path == "/components/VoxelGI/data");
    CHECK(without.problems()[0].message == "VoxelGIs can't load here");
    CHECK(differences(traced, plain)[0] > 100);

    // Unchanged, the next frame is the same; the source is asked each frame, as for lightmaps.
    CHECK(draw_world(*device, renderer, gi.scene.world).pixels == traced.pixels);
    CHECK(loads == 2);
    // Its lights are injected again when they change.
    auto& sun = gi.scene.world.ecs().get<world::DirectionalLight3D>(gi.sun);
    const auto white = sun.light_color;
    sun.light_color = {1.0F, 0.5F, 0.1F, 1.0F};
    const render::Image orange = draw_world(*device, renderer, gi.scene.world);
    CHECK(differences(orange, traced)[0] > 100);
    // A light's indirect energy scales only what it injects; a VoxelGI's energy what it gives.
    sun.light_color = white;
    sun.light_indirect_energy = 0.0F;
    const render::Image unlit = draw_world(*device, renderer, gi.scene.world);
    CHECK(differences(unlit, traced)[0] > 100);
    sun.light_indirect_energy = 1.0F;
    CHECK(draw_world(*device, renderer, gi.scene.world).pixels == traced.pixels);
    gi.scene.world.ecs().get<world::VoxelGI>(gi.gi).energy = 2.0F;
    CHECK(differences(draw_world(*device, renderer, gi.scene.world), traced)[0] > 100);
    gi.scene.world.ecs().get<world::VoxelGI>(gi.gi).energy = 1.0F;

    // High quality traces other cones; Compatibility none.
    CHECK(draw_world(*device, renderer, gi.scene.world, render::RenderMethod::ForwardPlus, true)
              .pixels != traced.pixels);
    CHECK(
        draw_world(*device, renderer, gi.scene.world, render::RenderMethod::Compatibility).pixels ==
        draw_world(*device, without, gi.scene.world, render::RenderMethod::Compatibility).pixels);

    // Meshes outside its box take none of its light.
    gi.scene.world.ecs().get<world::Transform3D>(gi.gi).position = {6.0F, 0.0F, 0.0F};
    CHECK(draw_world(*device, renderer, gi.scene.world).pixels == plain.pixels);
    gi.scene.world.ecs().get<world::Transform3D>(gi.gi).position = {};

    // Data that doesn't load is a problem at the VoxelGI's data.
    gi.scene.world.ecs().get<world::VoxelGI>(gi.gi).data = "other.voxelgi.json";
    (void)draw_world(*device, renderer, gi.scene.world);
    REQUIRE(renderer.problems().size() == 1);
    CHECK(renderer.problems()[0].message == "\"other.voxelgi.json\" doesn't exist");
}

TEST_CASE("every Forward+ device traces VoxelGIs as the software device does") {
    const auto software = render::create_software_device();
    const LitVoxelGI gi = lit_voxel_gi();
    for (const bool high_quality : {false, true}) {
        CAPTURE(high_quality);
        const render::Image expected = draw_voxel_gi(*software, gi, {.high_quality = high_quality});
        for (auto& device : slash::test::hardware_devices()) {
            if (device->backend() == render::Backend::OpenGl ||
                device->backend() == render::Backend::OpenGlEs) {
                continue; // Compatibility only: no VoxelGIs
            }
            CAPTURE(render::backend_name(device->backend()));
            const std::array<int, 2> found =
                differences(draw_voxel_gi(*device, gi, {.high_quality = high_quality}), expected);
            CAPTURE(found[1]);
            CHECK(found[0] <= 10);
        }
    }
}
