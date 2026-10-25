#include "gltf.hpp"

#include "slash/asset/assets.hpp"
#include "slash/canvas/canvas.hpp"
#include "slash/core/json.hpp"
#include "slash/core/math.hpp"
#include "slash/render/mesh.hpp"
#include "slash/scene3d/model.hpp"
#include "slash/ui/ui.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/world.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace asset = slash::asset;
namespace canvas = slash::canvas;
namespace json = slash::json;
namespace render = slash::render;
namespace scene3d = slash::scene3d;
namespace world = slash::world;
using slash::test::Gltf;

constexpr float kPi = 3.14159265F;

// A smooth sphere of radius 1 as a glTF model: rings by segments quads, its seam's corners twice
// for their texture coordinates, as Godot's SphereMesh makes one.
Gltf sphere(int rings, int segments) {
    std::vector<float> positions;
    std::vector<float> normals;
    std::vector<float> uvs;
    for (int r = 0; r <= rings; ++r) {
        const slash::Vec2 down =
            slash::cos_sin(static_cast<float>(r) * kPi / static_cast<float>(rings));
        for (int s = 0; s <= segments; ++s) {
            const slash::Vec2 around =
                slash::cos_sin(static_cast<float>(s) * 2.0F * kPi / static_cast<float>(segments));
            const std::array<float, 3> p{around.y * down.y, down.x, around.x * down.y};
            positions.insert(positions.end(), p.begin(), p.end());
            normals.insert(normals.end(), p.begin(), p.end());
            uvs.push_back(static_cast<float>(s) / static_cast<float>(segments));
            uvs.push_back(static_cast<float>(r) / static_cast<float>(rings));
        }
    }
    std::vector<std::uint16_t> indices;
    const auto at = [&](int r, int s) {
        return static_cast<std::uint16_t>((r * (segments + 1)) + s);
    };
    for (int r = 0; r < rings; ++r) {
        for (int s = 0; s < segments; ++s) {
            indices.insert(indices.end(), {at(r, s), at(r + 1, s), at(r + 1, s + 1)});
            indices.insert(indices.end(), {at(r, s), at(r + 1, s + 1), at(r, s + 1)});
        }
    }
    Gltf out;
    json::Value primitive;
    primitive["attributes"]["POSITION"] = out.floats(positions, "VEC3");
    primitive["attributes"]["NORMAL"] = out.floats(normals, "VEC3");
    primitive["attributes"]["TEXCOORD_0"] = out.floats(uvs, "VEC2");
    primitive["indices"] = out.indices(indices);
    json::Value mesh;
    mesh["name"] = "Sphere";
    mesh["primitives"].push_back(std::move(primitive));
    out.document["meshes"].push_back(std::move(mesh));
    out.document["nodes"] = slash::test::gltf_json(R"([{"name": "Sphere", "mesh": 0}])");
    out.document["scenes"] = slash::test::gltf_json(R"([{"nodes": [0]}])");
    out.document["scene"] = 0;
    return out;
}

slash::Result<std::string> no_file(std::string_view /*path*/) {
    return slash::make_error(slash::ErrorCode::NotFound, "no such file");
}

scene3d::Model load_sphere(int rings, int segments) {
    auto model = scene3d::load_model(sphere(rings, segments).gltf(), "sphere.gltf", no_file);
    REQUIRE(model.ok());
    return std::move(model).value();
}

// A world with the built-in components and the canvas asset types, in a viewport of 64x64,
// seeing the 3D world through a camera at the origin, looking down -z.
struct Scene {
    std::map<std::string, std::string, std::less<>> files;
    asset::Assets assets{asset::FileSource{
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
            return asset::FileStamp{.modified = 1, .size = it->second.size()};
        }}};
    world::ComponentRegistry components;
    world::World world{components};

    Scene() {
        world::register_builtin_components(components);
        canvas::install(world, assets);
        slash::ui::Settings ui;
        ui.viewport = {64.0F, 64.0F};
        slash::ui::set_settings(world, ui);
        files["sphere.gltf"] = sphere(16, 32).gltf();
        create("camera", R"({"Transform3D": {}, "Camera3D": {}})");
    }

    entt::entity create(std::string_view id, std::string_view components_json) {
        const entt::entity entity = world.create(id).value();
        const json::Value values = json::parse(components_json).value();
        for (const auto& [name, fields] : values.as_object()) {
            std::string path;
            const auto assigned = components.find(name)->assign(world, entity, fields, path);
            REQUIRE_MESSAGE(assigned.ok(), (assigned.ok() ? "" : assigned.error().message));
        }
        return entity;
    }

    // How many meshes the frame draws, and how many of those at a coarser LOD.
    std::array<std::size_t, 2> drawn() {
        const auto captured = canvas::capture(world, assets);
        REQUIRE(captured.ok());
        return {captured.value().meshes, captured.value().lods};
    }
};

} // namespace

TEST_CASE("models' surfaces get LODs as Godot's importer makes them, each about half the last") {
    const scene3d::Model model = load_sphere(16, 32);
    REQUIRE(model.meshes.size() == 1);
    const scene3d::ModelSurface& surface = model.meshes[0].surfaces.at(0);
    REQUIRE(surface.indices.size() == std::size_t{16 * 32 * 6});
    REQUIRE(surface.lods.size() >= 3);
    std::size_t before = surface.indices.size();
    float distance = 0.0F;
    for (const scene3d::SurfaceLod& lod : surface.lods) {
        CAPTURE(lod.indices.size());
        CHECK(lod.indices.size() % 3 == 0);
        CHECK(lod.indices.size() <= before * 3 / 4);
        CHECK(lod.distance > distance);
        for (const std::uint32_t index : lod.indices) {
            REQUIRE(index < surface.vertices.size());
        }
        before = lod.indices.size();
        distance = lod.distance;
    }
    // The coarsest LOD keeps the sphere's shape: its corners are still on the sphere, and far
    // enough round it in every direction.
    std::array<float, 3> low{};
    std::array<float, 3> high{};
    for (const std::uint32_t index : surface.lods.back().indices) {
        for (std::size_t c = 0; c < 3; ++c) {
            low.at(c) = std::min(low.at(c), surface.vertices[index].position.at(c));
            high.at(c) = std::max(high.at(c), surface.vertices[index].position.at(c));
        }
    }
    for (std::size_t c = 0; c < 3; ++c) {
        CHECK(low.at(c) < -0.7F);
        CHECK(high.at(c) > 0.7F);
    }

    // The same surface makes the same LODs, on every machine.
    scene3d::ModelSurface again = surface;
    scene3d::generate_lods(again);
    REQUIRE(again.lods.size() == surface.lods.size());
    for (std::size_t i = 0; i < again.lods.size(); ++i) {
        CHECK(again.lods[i].indices == surface.lods[i].indices);
        CHECK(again.lods[i].distance == surface.lods[i].distance);
    }

    // Too few triangles to halve have none, as Godot's importer leaves them.
    const scene3d::Model small = load_sphere(2, 2);
    CHECK(small.meshes[0].surfaces.at(0).lods.empty());
}

TEST_CASE("pick_lod picks the coarsest LOD that looks right from where it is seen") {
    const std::vector<scene3d::SurfaceLod> lods{{.distance = 0.01F, .indices = {0, 1, 2}},
                                                {.distance = 0.1F, .indices = {0, 1, 2}}};
    // Screen size: the LOD's distance times the model's scale over the distance it is seen at.
    CHECK(scene3d::pick_lod(lods, 1.0F, 1.0F, 0.005F) == 0);
    CHECK(scene3d::pick_lod(lods, 1.0F, 10.0F, 0.005F) == 1);
    CHECK(scene3d::pick_lod(lods, 1.0F, 100.0F, 0.005F) == 2);
    // Larger models keep their detail further.
    CHECK(scene3d::pick_lod(lods, 10.0F, 100.0F, 0.005F) == 1);
    CHECK(scene3d::pick_lod({}, 1.0F, 100.0F, 0.005F) == 0);
}

TEST_CASE("models draw coarser LODs further from the camera, as Godot's Forward+ picks them") {
    Scene scene;
    const entt::entity ball = scene.create(
        "ball",
        R"({"Transform3D": {"position": [0, 0, -1.5]}, "MeshInstance3D": {"mesh": "sphere.gltf"}})");
    // Half a unit from its nearest side, the first LOD would be off by more than a pixel.
    CHECK(scene.drawn() == std::array<std::size_t, 2>{1, 0});
    auto& place = scene.world.ecs().get<world::Transform3D>(ball);
    place.position = {0.0F, 0.0F, -3.0F};
    CHECK(scene.drawn() == std::array<std::size_t, 2>{1, 1});
    place.position = {0.0F, 0.0F, -200.0F};
    CHECK(scene.drawn() == std::array<std::size_t, 2>{1, 1});
    // A larger lod_bias keeps its detail there, as a larger mesh would.
    scene.world.ecs().get<world::MeshInstance3D>(ball).lod_bias = 1000.0F;
    CHECK(scene.drawn() == std::array<std::size_t, 2>{1, 0});
    scene.world.ecs().get<world::MeshInstance3D>(ball).lod_bias = 1.0F;
    // A threshold of 0 pixels draws every mesh in full.
    canvas::Settings settings = canvas::settings(scene.world);
    settings.mesh_lod_threshold = 0.0F;
    canvas::set_settings(scene.world, settings);
    CHECK(scene.drawn() == std::array<std::size_t, 2>{1, 0});
}

TEST_CASE("project.json's mesh_lod threshold is read as Godot's") {
    const auto threshold = [](std::string_view rendering) {
        return render::mesh_lod_threshold(json::parse(rendering).value());
    };
    CHECK(threshold(R"({})") == 1.0F);
    CHECK(threshold(R"({"mesh_lod": {"lod_change": {"threshold_pixels": 4}}})") == 4.0F);
    CHECK(threshold(R"({"mesh_lod": {"lod_change": {"threshold_pixels": 0}}})") == 0.0F);
    CHECK(threshold(R"({"mesh_lod": {"lod_change": {"threshold_pixels": -1}}})") == 1.0F);
}
