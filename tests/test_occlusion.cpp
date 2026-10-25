#include "slash/asset/assets.hpp"
#include "slash/canvas/canvas.hpp"
#include "slash/core/json.hpp"
#include "slash/core/math.hpp"
#include "slash/render/mesh.hpp"
#include "slash/scene3d/occlusion.hpp"
#include "slash/scene3d/scene3d.hpp"
#include "slash/ui/ui.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/tree3d.hpp"
#include "slash/world/world.hpp"

#include <doctest/doctest.h>

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
using slash::Vec3;

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

    void use_occlusion(bool on, bool jitter = false) {
        canvas::Settings settings = canvas::settings(world);
        settings.occlusion_culling = {.enabled = on, .rays_per_thread = 512, .jitter = jitter};
        canvas::set_settings(world, settings);
    }

    canvas::Capture capture() {
        auto captured = canvas::capture(world, assets);
        REQUIRE(captured.ok());
        return std::move(captured).value();
    }
};

world::OccluderInstance3D occluder(std::string_view json_text) {
    world::OccluderInstance3D out;
    world::ComponentRegistry registry;
    world::register_builtin_components(registry);
    world::World w{registry};
    const entt::entity e = w.create("o").value();
    std::string path;
    REQUIRE(registry.find("OccluderInstance3D")
                ->assign(w, e, json::parse(json_text).value(), path)
                .ok());
    return w.ecs().get<world::OccluderInstance3D>(e);
}

// A camera at the origin looking down -z, 75 degrees across a square view.
scene3d::OcclusionCamera camera() {
    return {.transform = {},
            .projection = render::perspective(75.0F, 1.0F, 0.05F, 100.0F),
            .near = 0.05F,
            .far = 100.0F,
            .orthogonal = false};
}

scene3d::Bounds cube(Vec3 center, float half) {
    return {.min = {center.x - half, center.y - half, center.z - half},
            .max = {center.x + half, center.y + half, center.z + half}};
}

} // namespace

TEST_CASE("occluder shapes are made as Godot's Occluder3Ds make them") {
    auto quad = scene3d::occluder_mesh(occluder(R"({"shape": "quad", "size": [2, 4, 0]})"));
    REQUIRE(quad.ok());
    CHECK(quad.value().vertices ==
          std::vector<Vec3>{{-1, -2, 0}, {-1, 2, 0}, {1, 2, 0}, {1, -2, 0}});
    CHECK(quad.value().indices == std::vector<int>{0, 1, 2, 0, 2, 3});

    auto box = scene3d::occluder_mesh(occluder(R"({"shape": "box", "size": [2, 2, 2]})"));
    REQUIRE(box.ok());
    CHECK(box.value().vertices.size() == 8);
    CHECK(box.value().indices.size() == 36);

    // SphereOccluder3D: 7 rings and 7 segments, poles included.
    auto sphere = scene3d::occluder_mesh(occluder(R"({"shape": "sphere", "radius": 2})"));
    REQUIRE(sphere.ok());
    CHECK(sphere.value().vertices.size() == 72);
    CHECK(sphere.value().indices.size() == 336);
    CHECK(sphere.value().vertices.front().y == doctest::Approx(2.0F));
    CHECK(sphere.value().vertices.back().y == doctest::Approx(-2.0F));

    // A counter-clockwise polygon is turned clockwise, then cut into triangles.
    auto polygon = scene3d::occluder_mesh(
        occluder(R"({"shape": "polygon", "polygon": [[0, 0], [1, 0], [1, 1], [0, 1]]})"));
    REQUIRE(polygon.ok());
    CHECK(polygon.value().vertices ==
          std::vector<Vec3>{{0, 1, 0}, {1, 1, 0}, {1, 0, 0}, {0, 0, 0}});
    CHECK(polygon.value().indices.size() == 6);
    // Fewer than three points make nothing.
    auto line =
        scene3d::occluder_mesh(occluder(R"({"shape": "polygon", "polygon": [[0, 0], [1, 0]]})"));
    REQUIRE(line.ok());
    CHECK(line.value().indices.empty());

    auto array = scene3d::occluder_mesh(occluder(
        R"({"shape": "array", "vertices": [[0, 0, 0], [1, 0, 0], [0, 1, 0]], "indices": [0, 1, 2]})"));
    REQUIRE(array.ok());
    CHECK(array.value().indices == std::vector<int>{0, 1, 2});
    auto out_of_range = scene3d::occluder_mesh(occluder(
        R"({"shape": "array", "vertices": [[0, 0, 0], [1, 0, 0], [0, 1, 0]], "indices": [0, 1, 3]})"));
    REQUIRE_FALSE(out_of_range.ok());
    CHECK(out_of_range.error().message.find("out of range") != std::string::npos);
}

TEST_CASE("occluders block rays from either side") {
    scene3d::OccluderScene scene;
    CHECK(scene.cast({0, 0, 0}, {0, 0, -1}, 0.05F, 100.0F) == 100.0F);
    std::vector<std::array<Vec3, 3>> triangles;
    // A wall 2 across at z = -5, and many small ones far to the side, so the hierarchy splits.
    triangles.push_back({Vec3{-1, -1, -5}, Vec3{-1, 1, -5}, Vec3{1, 1, -5}});
    triangles.push_back({Vec3{-1, -1, -5}, Vec3{1, 1, -5}, Vec3{1, -1, -5}});
    for (int i = 0; i < 40; ++i) {
        const auto x = 10.0F + static_cast<float>(i);
        triangles.push_back({Vec3{x, 0, -3}, Vec3{x + 0.5F, 0, -3}, Vec3{x, 0.5F, -3}});
    }
    scene.build(triangles);
    CHECK(scene.size() == 42);
    CHECK(scene.cast({0, 0, 0}, {0, 0, -1}, 0.05F, 100.0F) == doctest::Approx(5.0F));
    CHECK(scene.cast({0, 0, -10}, {0, 0, 1}, 0.05F, 100.0F) == doctest::Approx(5.0F));
    CHECK(scene.cast({3, 0, 0}, {0, 0, -1}, 0.05F, 100.0F) == 100.0F);
    CHECK(scene.cast({20.1F, 0.1F, 0}, {0, 0, -1}, 0.05F, 100.0F) == doctest::Approx(3.0F));
    // Nearer than near, or further than far, isn't a hit.
    CHECK(scene.cast({0, 0, 0}, {0, 0, -1}, 6.0F, 100.0F) == 100.0F);
    CHECK(scene.cast({0, 0, 0}, {0, 0, -1}, 0.05F, 4.0F) == 4.0F);
}

TEST_CASE("the occlusion buffer is sized and leveled as Godot's") {
    using Size = std::array<int, 2>;
    // 512 rays for each of 8 threads, kept between one per 32x32 pixels and one per 2x2.
    CHECK(scene3d::OcclusionBuffer::size_for(1280, 720, 512) == Size{85, 48});
    CHECK(scene3d::OcclusionBuffer::size_for(64, 64, 512) == Size{32, 32});
    CHECK(scene3d::OcclusionBuffer::size_for(16, 16, 512) == Size{8, 8});
    CHECK(scene3d::OcclusionBuffer::size_for(0, 16, 512) == Size{0, 0});
    scene3d::OcclusionBuffer buffer;
    buffer.resize(85, 48);
    REQUIRE(buffer.levels() == 7);
    CHECK(buffer.size(1) == Size{42, 24});
    CHECK(buffer.size(5) == Size{2, 1});
    CHECK(buffer.size(6) == Size{1, 1});
    buffer.resize(1, 1);
    CHECK(buffer.levels() == 2);
    buffer.resize(0, 0);
    CHECK(buffer.empty());
}

TEST_CASE("boxes behind occluders are hidden, as Godot's HZBuffer finds them") {
    scene3d::OccluderScene scene;
    // A wall 4 across, 5 ahead.
    scene.build({{Vec3{-2, -2, -5}, Vec3{-2, 2, -5}, Vec3{2, 2, -5}},
                 {Vec3{-2, -2, -5}, Vec3{2, 2, -5}, Vec3{2, -2, -5}}});
    scene3d::OcclusionBuffer buffer;
    buffer.resize(32, 32);
    buffer.update(scene, camera(), 1, false);
    // The middle texel's ray hits the wall; a corner's goes on to the far plane and beyond.
    CHECK(buffer.depth(0, 16, 16) == doctest::Approx(5.0F).epsilon(0.01));
    CHECK(buffer.depth(0, 0, 0) == doctest::Approx(105.0F));
    // Each level keeps the farthest of the texels under it.
    CHECK(buffer.depth(buffer.levels() - 1, 0, 0) == doctest::Approx(105.0F));

    std::uint64_t timeout = 0;
    CHECK(buffer.occluded(cube({0, 0, -10}, 0.5F), timeout));
    // In front of the wall, beside it and around the camera, it shows.
    CHECK_FALSE(buffer.occluded(cube({0, 0, -3}, 0.5F), timeout));
    CHECK_FALSE(buffer.occluded(cube({6, 0, -10}, 0.5F), timeout));
    CHECK_FALSE(buffer.occluded(cube({0, 0, 0}, 0.5F), timeout));
    // Partly behind it is seen.
    CHECK_FALSE(buffer.occluded(cube({4, 0, -10}, 1.0F), timeout));
    CHECK(timeout == 0);
}

TEST_CASE("with jitter, a box once seen stays shown for nine frames") {
    scene3d::OccluderScene scene;
    scene.build({{Vec3{-2, -2, -5}, Vec3{-2, 2, -5}, Vec3{2, 2, -5}},
                 {Vec3{-2, -2, -5}, Vec3{2, 2, -5}, Vec3{2, -2, -5}}});
    scene3d::OcclusionBuffer buffer;
    buffer.resize(32, 32);
    std::uint64_t timeout = 0;
    // Seen at frame 1, behind the wall from frame 2: hidden only once frame 10 comes.
    buffer.update(scene, camera(), 1, true);
    CHECK_FALSE(buffer.occluded(cube({0, 0, -3}, 0.5F), timeout));
    CHECK(timeout == 10);
    for (std::uint64_t frame = 2; frame < 10; ++frame) {
        buffer.update(scene, camera(), frame, true);
        CHECK_FALSE(buffer.occluded(cube({0, 0, -10}, 0.5F), timeout));
    }
    buffer.update(scene, camera(), 10, true);
    CHECK(buffer.occluded(cube({0, 0, -10}, 0.5F), timeout));
    CHECK(timeout == 0);
}

TEST_CASE("meshes behind OccluderInstance3Ds aren't drawn, but still cast shadows") {
    Scene scene;
    scene.create("wall", R"({"Transform3D": {"position": [0, 0, -5]},
                              "OccluderInstance3D": {"shape": "quad", "size": [4, 4, 1]}})");
    scene.create("hidden", R"({"Transform3D": {"position": [0, 0, -10]},
                                "MeshInstance3D": {}, "BoxMesh": {}})");
    const entt::entity shown = scene.create(
        "shown",
        R"({"Transform3D": {"position": [0, 0, -3]}, "MeshInstance3D": {}, "BoxMesh": {"size": [0.5, 0.5, 0.5]}})");
    // Off by default, as Godot's rendering/occlusion_culling/use_occlusion_culling.
    canvas::Capture captured = scene.capture();
    CHECK(captured.meshes == 2);
    CHECK(captured.occluded == 0);
    scene.use_occlusion(true);
    captured = scene.capture();
    // The hidden box still casts, so it draws into shadow maps only.
    CHECK(captured.meshes == 2);
    CHECK(captured.occluded == 1);
    // One that casts no shadow isn't drawn at all.
    const entt::entity hidden = scene.world.find("hidden");
    scene.world.ecs().get<world::MeshInstance3D>(hidden).cast_shadow = "off";
    captured = scene.capture();
    CHECK(captured.meshes == 1);
    CHECK(captured.occluded == 1);
    // ignore_occlusion_culling keeps it, as Godot's.
    scene.world.ecs().get<world::MeshInstance3D>(hidden).ignore_occlusion_culling = true;
    captured = scene.capture();
    CHECK(captured.meshes == 2);
    CHECK(captured.occluded == 0);
    // Moved out from behind the wall, it shows.
    scene.world.ecs().get<world::MeshInstance3D>(hidden).ignore_occlusion_culling = false;
    scene.world.ecs().get<world::Transform3D>(hidden).position = {6.0F, 0.0F, -10.0F};
    captured = scene.capture();
    CHECK(captured.occluded == 0);
    // A hidden occluder hides nothing.
    scene.world.ecs().get<world::Transform3D>(hidden).position = {0.0F, 0.0F, -10.0F};
    scene.world.ecs().get<world::Transform3D>(scene.world.find("wall")).visible = false;
    captured = scene.capture();
    CHECK(captured.occluded == 0);
    CHECK(scene.world.valid(shown));
}

TEST_CASE("project.json's occlusion_culling is read as Godot's") {
    const auto read = [](std::string_view rendering) {
        return render::occlusion_culling(json::parse(rendering).value());
    };
    CHECK(read(R"({})") == render::OcclusionCulling{});
    CHECK(read(R"({"occlusion_culling": {"use_occlusion_culling": true}})").enabled);
    const render::OcclusionCulling set = read(R"({"occlusion_culling": {
        "use_occlusion_culling": true, "occlusion_rays_per_thread": 64,
        "jitter_projection": false}})");
    CHECK(set == render::OcclusionCulling{.enabled = true, .rays_per_thread = 64, .jitter = false});
    CHECK(read(R"({"occlusion_culling": {"occlusion_rays_per_thread": 0}})").rays_per_thread ==
          512);
}

TEST_CASE("occluders bake from the opaque meshes around them, as Godot's bake_scene") {
    Scene scene;
    const entt::entity baker = scene.create("baker", R"({"Transform3D": {"position": [0, 1, 0]},
                                  "OccluderInstance3D": {"bake_simplification_distance": 0}})");
    scene.create("crate", R"({"Transform3D": {"position": [3, 1, 0]},
                              "MeshInstance3D": {}, "BoxMesh": {}})");
    std::vector<scene3d::Problem> problems;
    scene3d::OccluderMesh baked = scene3d::bake_occluder(scene.world, baker, {}, problems);
    CHECK(problems.empty());
    // A box's 24 corners merge into 8, its 12 triangles kept, in the occluder's space.
    REQUIRE(baked.vertices.size() == 8);
    CHECK(baked.indices.size() == 36);
    for (const Vec3& p : baked.vertices) {
        CHECK(std::abs(p.x - 3.0F) == doctest::Approx(0.5F));
        CHECK(std::abs(p.y) == doctest::Approx(0.5F));
    }
    // Simplified, a box keeps its shape: it has nothing to lose.
    scene.world.ecs().get<world::OccluderInstance3D>(baker).bake_simplification_distance = 0.1F;
    baked = scene3d::bake_occluder(scene.world, baker, {}, problems);
    CHECK(baked.indices.size() == 36);
    // Transparent meshes, and those outside the bake mask, are left out.
    const entt::entity crate = scene.world.find("crate");
    scene.create("glass", R"({"Transform3D": {}, "MeshInstance3D": {}, "BoxMesh": {},
                              "StandardMaterial3D": {"transparency": "alpha"}})");
    scene.world.ecs().get<world::MeshInstance3D>(crate).layers = 2;
    scene.world.ecs().get<world::OccluderInstance3D>(baker).bake_mask = 1;
    baked = scene3d::bake_occluder(scene.world, baker, {}, problems);
    CHECK(baked.vertices.empty());
    CHECK(problems.empty());
}

TEST_CASE("scene checks find occluders that can't hide anything") {
    Scene scene;
    scene.create("line", R"({"OccluderInstance3D": {"shape": "polygon", "polygon": [[0, 0]]}})");
    scene.create("empty", R"({"OccluderInstance3D": {"shape": "array"}})");
    scene.create("bad", R"({"OccluderInstance3D": {"shape": "array",
        "vertices": [[0, 0, 0]], "indices": [0, 0, 1]}})");
    const std::vector<scene3d::Problem> problems = scene3d::check_occluders(scene.world);
    REQUIRE(problems.size() == 3);
    CHECK(problems[0].path == "/components/OccluderInstance3D/polygon");
    CHECK(problems[1].path == "/components/OccluderInstance3D/indices");
    CHECK(problems[1].message.find("occluder.bake") != std::string::npos);
    CHECK(problems[2].message.find("out of range") != std::string::npos);
}
