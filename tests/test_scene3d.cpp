#include "devices.hpp"

#include "slash/asset/assets.hpp"
#include "slash/canvas/canvas.hpp"
#include "slash/core/json.hpp"
#include "slash/core/math.hpp"
#include "slash/render/device.hpp"
#include "slash/render/image.hpp"
#include "slash/render/mesh.hpp"
#include "slash/runtime/project.hpp"
#include "slash/scene3d/primitive.hpp"
#include "slash/scene3d/scene3d.hpp"
#include "slash/ui/ui.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/tree3d.hpp"
#include "slash/world/world.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace asset = slash::asset;
namespace canvas = slash::canvas;
namespace json = slash::json;
namespace render = slash::render;
namespace scene3d = slash::scene3d;
namespace world = slash::world;
using slash::Vec3;
using slash::test::drawing_devices;

namespace {

constexpr float kHalfPi = std::numbers::pi_v<float> / 2.0F;

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE(parsed.ok());
    return std::move(parsed).value();
}

// Files in memory, for textures.
struct MemoryFiles {
    std::map<std::string, std::string, std::less<>> files;

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
                return asset::FileStamp{.modified = 1, .size = it->second.size()};
            },
        };
    }
};

// A world with the built-in components and the canvas asset types, in a viewport of 64x64.
struct Scene {
    MemoryFiles files;
    asset::Assets assets{files.source()};
    world::ComponentRegistry components;
    world::World world{components};

    Scene() {
        world::register_builtin_components(components);
        canvas::install(world, assets);
        slash::ui::Settings ui;
        ui.viewport = {64.0F, 64.0F};
        slash::ui::set_settings(world, ui);
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

    world::Transform3D& transform(entt::entity entity) {
        return world.ecs().get<world::Transform3D>(entity);
    }
};

using Rgba = std::array<std::uint8_t, 4>;

Rgba pixel(const render::Image& image, std::uint32_t x, std::uint32_t y) {
    const std::size_t at = ((std::size_t{y} * image.width) + x) * 4;
    return {image.pixels[at], image.pixels[at + 1], image.pixels[at + 2], image.pixels[at + 3]};
}

float dot(Vec3 a, Vec3 b) {
    return (a.x * b.x) + (a.y * b.y) + (a.z * b.z);
}

Vec3 cross(Vec3 a, Vec3 b) {
    return {(a.y * b.z) - (a.z * b.y), (a.z * b.x) - (a.x * b.z), (a.x * b.y) - (a.y * b.x)};
}

Vec3 vec(const std::array<float, 3>& v) {
    return {v[0], v[1], v[2]};
}

bool near(Vec3 a, Vec3 b) {
    return std::abs(a.x - b.x) < 1e-5F && std::abs(a.y - b.y) < 1e-5F &&
           std::abs(a.z - b.z) < 1e-5F;
}

// How many of a mesh's triangles around the origin wind clockwise seen from outside, as Godot's
// front faces do, and how many the other way; triangles with no area count as neither.
std::pair<std::size_t, std::size_t> windings(const scene3d::MeshData& mesh) {
    std::size_t clockwise = 0;
    std::size_t counter = 0;
    for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        const Vec3 a = vec(mesh.vertices[mesh.indices[i]].position);
        const Vec3 b = vec(mesh.vertices[mesh.indices[i + 1]].position);
        const Vec3 c = vec(mesh.vertices[mesh.indices[i + 2]].position);
        const Vec3 facing = cross(b - a, c - a);
        const Vec3 middle{(a.x + b.x + c.x) / 3.0F, (a.y + b.y + c.y) / 3.0F,
                          (a.z + b.z + c.z) / 3.0F};
        const float side = dot(facing, middle);
        if (side < -1e-6F) {
            ++clockwise;
        } else if (side > 1e-6F) {
            ++counter;
        }
    }
    return {clockwise, counter};
}

} // namespace

TEST_CASE("primitive meshes have Godot's corners, normals, texture coordinates and winding") {
    const scene3d::MeshData box = scene3d::box_mesh({});
    CHECK(box.vertices.size() == 24);
    CHECK(box.indices.size() == 36);
    // Godot's first corners: the front's top left, then the back's.
    CHECK(near(vec(box.vertices[0].position), {-0.5F, 0.5F, 0.5F}));
    CHECK(near(vec(box.vertices[0].normal), {0.0F, 0.0F, 1.0F}));
    CHECK(box.vertices[0].tangent == std::array<float, 4>{1.0F, 0.0F, 0.0F, 1.0F});
    CHECK(box.vertices[0].uv == std::array<float, 2>{0.0F, 0.0F});
    CHECK(near(vec(box.vertices[1].position), {0.5F, 0.5F, -0.5F}));
    CHECK(near(vec(box.vertices[1].normal), {0.0F, 0.0F, -1.0F}));
    CHECK(box.vertices[1].uv[0] == doctest::Approx(2.0 / 3.0));
    CHECK(box.indices[0] == 0);
    CHECK(box.indices[1] == 2);
    CHECK(box.indices[2] == 4);

    const scene3d::MeshData sphere = scene3d::sphere_mesh({});
    CHECK(sphere.vertices.size() == 34 * 65);
    CHECK(sphere.indices.size() == 33 * 64 * 6);
    const scene3d::MeshData cylinder = scene3d::cylinder_mesh({});
    CHECK(cylinder.vertices.size() == (6 * 65) + (2 * 66));
    const scene3d::MeshData plane = scene3d::plane_mesh({});
    CHECK(plane.vertices.size() == 4);
    CHECK(plane.indices == std::vector<std::uint32_t>{0, 1, 2, 1, 3, 2});
    CHECK(near(vec(plane.vertices[0].position), {1.0F, 0.0F, 1.0F}));
    CHECK(plane.vertices[0].uv == std::array<float, 2>{1.0F, 1.0F});

    // Closed meshes around their centers face out: every triangle with an area winds clockwise
    // seen from outside, and its corners' normals point out.
    for (const auto& [name, mesh] :
         {std::pair{"box", box}, std::pair{"sphere", sphere}, std::pair{"cylinder", cylinder},
          std::pair{"capsule", scene3d::capsule_mesh({})},
          std::pair{"prism", scene3d::prism_mesh({})}}) {
        CAPTURE(name);
        const auto [clockwise, counter] = windings(mesh);
        CHECK(clockwise > 0);
        CHECK(counter == 0);
        for (const render::Vertex3D& corner : mesh.vertices) {
            CHECK(dot(vec(corner.normal), vec(corner.position)) >= -1e-6F);
        }
    }

    // flip_faces turns them inside out.
    const scene3d::MeshData inside = scene3d::sphere_mesh({.flip_faces = true});
    const auto [clockwise, counter] = windings(inside);
    CHECK(clockwise == 0);
    CHECK(counter > 0);
    CHECK(near(vec(inside.vertices[100].normal),
               {-sphere.vertices[100].normal[0], -sphere.vertices[100].normal[1],
                -sphere.vertices[100].normal[2]}));

    const auto torus = scene3d::make_mesh(world::TorusMesh{.inner_radius = 1.0F});
    REQUIRE_FALSE(torus.ok());
    CHECK(torus.error().message == "a TorusMesh's inner_radius and outer_radius can't be the same");
    CHECK(scene3d::make_mesh(world::TorusMesh{}).value().vertices.size() == 65 * 33);
}

TEST_CASE("Transform3D trees place, hide and order entities as Godot's Node3D") {
    Scene scene;
    const entt::entity child =
        scene.create("child", R"({"Transform3D": {"position": [0, 0, -1]}})");
    const entt::entity parent = scene.create("parent", R"({"Transform3D": {
        "position": [1, 0, 0], "rotation": [0, 1.5707964, 0], "children": ["child"]}})");
    const entt::entity other = scene.create("other", R"({"Transform3D": {}})");
    world::Tree3D tree(scene.world);
    CHECK(tree.parent(child) == parent);
    CHECK(tree.order() == std::vector<entt::entity>{parent, child, other});
    // Turned a quarter about y, the parent's -z is the world's -x.
    CHECK(near(tree.global(child).origin, {0.0F, 0.0F, 0.0F}));
    CHECK(near(tree.global(child).z, {1.0F, 0.0F, 0.0F}));

    scene.transform(child).top_level = true;
    CHECK(near(world::Tree3D(scene.world).global(child).origin, {0.0F, 0.0F, -1.0F}));

    scene.transform(parent).visible = false;
    CHECK_FALSE(world::Tree3D(scene.world).visible(child));
    CHECK(world::Tree3D(scene.world).visible(other));

    // A cycle is broken at the entity made first.
    scene.transform(child).children = {parent};
    world::Tree3D cycle(scene.world);
    CHECK(cycle.parent(child) == entt::entity{entt::null});
    CHECK(cycle.parent(parent) == child);

    // Euler angles turn z first, then x, then y: turned 90 degrees about x and y, the z axis
    // points down. The basis is the quaternion's.
    world::Transform3D turned;
    turned.rotation = {kHalfPi, kHalfPi, 0.0F};
    const world::Affine3D basis = world::affine(turned);
    const world::Affine3D from_quat = world::rotation_basis(world::quaternion(turned.rotation));
    CHECK(near(basis.x, from_quat.x));
    CHECK(near(basis.y, from_quat.y));
    CHECK(near(basis.z, from_quat.z));
    CHECK(near(basis.z, {0.0F, -1.0F, 0.0F}));
}

TEST_CASE("the current camera, its view and the WorldEnvironment") {
    Scene scene;
    const entt::entity first = scene.create("first", R"({"Transform3D": {}, "Camera3D": {}})");
    const entt::entity second = scene.create("second", R"({
        "Transform3D": {"position": [0, 0, 5]}, "Camera3D": {"current": true, "fov": 60}})");
    world::Tree3D tree(scene.world);
    CHECK(scene3d::current_camera(scene.world, tree) == second);
    scene.world.ecs().get<world::Camera3D>(second).current = false;
    CHECK(scene3d::current_camera(scene.world, tree) == first);

    // Perspective: fov across the height, and the camera's inverse as the view.
    const world::Camera3D lens{.fov = 60.0F};
    const scene3d::CameraView view =
        scene3d::camera_view(lens, world::Tree3D(scene.world).global(second), 2.0F);
    CHECK(view.projection == render::perspective(60.0F, 2.0F, 0.05F, 4000.0F));
    CHECK(view.view[14] == doctest::Approx(-5.0));
    // keep_aspect width: 60 degrees across a target twice as wide as high is about 32.2 high.
    const scene3d::CameraView wide =
        scene3d::camera_view({.keep_aspect = "width", .fov = 60.0F}, {}, 2.0F);
    const float fov_y = std::atan(std::tan(30.0F * std::numbers::pi_v<float> / 180.0F) / 2.0F) *
                        2.0F * 180.0F / std::numbers::pi_v<float>;
    const auto expected = render::perspective(fov_y, 2.0F, 0.05F, 4000.0F);
    for (std::size_t i = 0; i < 16; ++i) {
        CHECK(wide.projection[i] == doctest::Approx(expected[i]).epsilon(1e-5));
    }
    const scene3d::CameraView flat =
        scene3d::camera_view({.projection = "orthogonal", .size = 4.0F}, {}, 1.0F);
    CHECK(flat.projection == render::orthogonal(4.0F, 1.0F, 0.05F, 4000.0F));

    // No WorldEnvironment: the clear color and no ambient light.
    const render::Color clear{.r = 0.1F, .g = 0.2F, .b = 0.3F, .a = 1.0F};
    scene3d::Environment env = scene3d::environment(scene.world, clear);
    CHECK(env.background.g == doctest::Approx(0.2));
    CHECK(env.ambient == std::array<float, 3>{0.0F, 0.0F, 0.0F});
    // A color background lights what it surrounds, as Godot's ambient source "background".
    scene.create("env", R"({"WorldEnvironment": {"background_mode": "color",
        "background_color": [0, 0, 1, 1], "background_energy_multiplier": 0.5}})");
    env = scene3d::environment(scene.world, clear);
    CHECK(env.background.b == doctest::Approx(0.5));
    CHECK(env.ambient == std::array<float, 3>{0.0F, 0.0F, 1.0F});
    CHECK(env.ambient_energy == doctest::Approx(0.5));
}

TEST_CASE("lights shine from their entities down their -z, in tree order, and frames interpolate") {
    Scene scene;
    scene.create("sun", R"({"Transform3D": {"rotation": [-1.5707964, 0, 0]},
                            "DirectionalLight3D": {"light_energy": 2}})");
    const entt::entity lamp = scene.create(
        "lamp", R"({"Transform3D": {}, "OmniLight3D": {"omni_range": 3, "light_negative": true}})");
    scene.create("hidden", R"({"Transform3D": {"visible": false}, "SpotLight3D": {}})");
    world::Tree3D tree(scene.world);
    const auto lights = scene3d::lights(scene.world, tree,
                                        [&](entt::entity entity) { return tree.global(entity); });
    REQUIRE(lights.size() == 2);
    CHECK(lights[0].type == render::LightType::Directional);
    CHECK(lights[0].energy == 2.0F);
    CHECK(near(vec(lights[0].direction), {0.0F, -1.0F, 0.0F}));
    CHECK(lights[1].type == render::LightType::Omni);
    CHECK(lights[1].range == 3.0F);
    CHECK(lights[1].energy == -1.0F);

    // Halfway between ticks, the lamp is halfway along its move.
    const auto device = render::create_null_device();
    scene3d::Renderer renderer(*device, nullptr);
    scene.create("camera", R"({"Transform3D": {}, "Camera3D": {}})");
    renderer.before_tick(scene.world);
    scene.transform(lamp).position = {2.0F, 0.0F, 0.0F};
    const auto frame = renderer.prepare(scene.world, 64.0F, 64.0F, 0.5, {});
    REQUIRE(frame.has_value());
    CHECK(frame->scene.lights[1].position[0] == doctest::Approx(1.0));
    CHECK(renderer.lights() == 2);
}

TEST_CASE("a frame takes 8 directional lights first, then 512 omni and spot lights") {
    Scene scene;
    scene.create("lamp", R"({"Transform3D": {}, "OmniLight3D": {"omni_range": 2}})");
    for (int i = 0; i < 9; ++i) {
        scene.create("sun" + std::to_string(i),
                     R"({"Transform3D": {}, "DirectionalLight3D": {"light_energy": )" +
                         std::to_string(i) + "}}");
    }
    for (int i = 0; i < 520; ++i) {
        scene.create("candle" + std::to_string(i), R"({"Transform3D": {}, "SpotLight3D": {}})");
    }
    world::Tree3D tree(scene.world);
    const auto lights = scene3d::lights(scene.world, tree,
                                        [&](entt::entity entity) { return tree.global(entity); });
    REQUIRE(lights.size() == render::kMaxDirectionalLights3D + render::kMaxClusteredLights3D);
    // The first 8 suns, so Compatibility's first 64 lights keep them all, then the lamp and
    // the first 511 candles.
    for (std::size_t i = 0; i < render::kMaxDirectionalLights3D; ++i) {
        CHECK(lights[i].type == render::LightType::Directional);
        CHECK(lights[i].energy == static_cast<float>(i));
    }
    CHECK(lights[8].type == render::LightType::Omni);
    CHECK(lights[9].type == render::LightType::Spot);
    CHECK(lights.back().type == render::LightType::Spot);
}

TEST_CASE("meshes of equal settings share one on the device, and unused meshes go") {
    Scene scene;
    const auto device = render::create_null_device();
    scene3d::Renderer renderer(*device, nullptr);
    CHECK_FALSE(renderer.prepare(scene.world, 64.0F, 64.0F, 1.0, {}).has_value());

    scene.create("camera", R"({"Transform3D": {"position": [0, 0, 3]}, "Camera3D": {}})");
    const entt::entity a =
        scene.create("a", R"({"Transform3D": {}, "MeshInstance3D": {}, "BoxMesh": {}})");
    scene.create("b", R"({"Transform3D": {}, "MeshInstance3D": {}, "BoxMesh": {}})");
    REQUIRE(renderer.prepare(scene.world, 64.0F, 64.0F, 1.0, {}).has_value());
    CHECK(renderer.meshes() == 2);
    CHECK(renderer.meshes_kept() == 1);

    scene.world.ecs().get<world::BoxMesh>(a).size = {2.0F, 1.0F, 1.0F};
    (void)renderer.prepare(scene.world, 64.0F, 64.0F, 1.0, {});
    CHECK(renderer.meshes_kept() == 2);

    // Hidden, or on a layer the camera doesn't see and casting no shadow: not drawn, and its
    // mesh goes.
    scene.transform(a).visible = false;
    (void)renderer.prepare(scene.world, 64.0F, 64.0F, 1.0, {});
    CHECK(renderer.meshes() == 1);
    CHECK(renderer.meshes_kept() == 1);
    scene.transform(a).visible = true;
    auto& instance = scene.world.ecs().get<world::MeshInstance3D>(a);
    instance.layers = 1U << 21U;
    instance.cast_shadow = "off";
    (void)renderer.prepare(scene.world, 64.0F, 64.0F, 1.0, {});
    CHECK(renderer.meshes() == 1);
    CHECK(renderer.meshes_kept() == 1);
}

TEST_CASE("lights cast shadows and meshes cast them as their fields say") {
    Scene scene;
    scene.create("sun", R"({"Transform3D": {"rotation": [-0.7853982, 0, 0]},
                            "DirectionalLight3D": {"shadow_enabled": true, "shadow_opacity": 0.5,
                                "shadow_caster_mask": 3, "directional_shadow_mode": "orthogonal",
                                "directional_shadow_split_1": 0.2,
                                "directional_shadow_max_distance": 40,
                                "directional_shadow_blend_splits": true}})");
    scene.create("lamp", R"({"Transform3D": {}, "OmniLight3D": {"shadow_enabled": true,
                             "shadow_reverse_cull_face": true}})");
    scene.create("spot", R"({"Transform3D": {}, "SpotLight3D": {}})");
    world::Tree3D tree(scene.world);
    const auto lights = scene3d::lights(scene.world, tree,
                                        [&](entt::entity entity) { return tree.global(entity); });
    REQUIRE(lights.size() == 3);
    CHECK(lights[0].shadow);
    CHECK(lights[0].shadow_opacity == 0.5F);
    CHECK(lights[0].shadow_caster_mask == 3);
    CHECK(lights[0].shadow_normal_bias == 2.0F);
    CHECK(lights[0].shadow_mode == render::DirectionalShadowMode::Orthogonal);
    CHECK(lights[0].shadow_splits[0] == 0.2F);
    CHECK(lights[0].shadow_max_distance == 40.0F);
    CHECK(lights[0].shadow_blend_splits);
    // Turned down by 45 degrees, its +Y leans the other way.
    CHECK(near(vec(lights[0].up), {0.0F, 0.70710677F, -0.70710677F}));
    CHECK(lights[1].shadow);
    CHECK(lights[1].shadow_reverse_cull_face);
    CHECK(lights[1].shadow_bias == doctest::Approx(0.1));
    CHECK_FALSE(lights[2].shadow);
    CHECK(lights[2].shadow_bias == doctest::Approx(0.03));

    // Meshes the camera doesn't see still cast, as shadows-only draws; ones that cast nothing
    // aren't drawn.
    scene.create("camera", R"({"Transform3D": {"position": [0, 0, 3]},
                               "Camera3D": {"cull_mask": 1}})");
    scene.create("seen", R"({"Transform3D": {}, "MeshInstance3D": {}, "BoxMesh": {}})");
    scene.create("unseen", R"({"Transform3D": {}, "MeshInstance3D": {"layers": 2},
                               "BoxMesh": {}})");
    scene.create("ghost", R"({"Transform3D": {}, "MeshInstance3D": {"layers": 2,
                              "cast_shadow": "off"}, "BoxMesh": {}})");
    const auto device = render::create_software_device();
    scene3d::Renderer renderer(*device, nullptr);
    const render::ShadowSettings shadows{.directional_size = 1024};
    const auto frame = renderer.prepare(scene.world, 16.0F, 16.0F, 1.0, {}, shadows);
    REQUIRE(frame.has_value());
    CHECK(frame->scene.shadows == shadows);
    CHECK(renderer.meshes() == 2);
    auto target = device->create_texture({.width = 16, .height = 16, .render_target = true});
    REQUIRE(target.ok());
    REQUIRE(
        device->begin_frame({.target = target.value(), .clear = {}, .scene = frame->scene}).ok());
    REQUIRE(renderer.draw().ok());
    REQUIRE(device->end_frame().ok());
    // Both draws reach the device, which counts them.
    CHECK(device->stats().draws == 2);
    device->destroy_texture(target.value());
}

TEST_CASE("meshes that can't draw are reported, in the world and in scene files") {
    Scene scene;
    scene.create("camera", R"({"Transform3D": {}, "Camera3D": {}})");
    scene.create("none", R"({"Transform3D": {}, "MeshInstance3D": {}})");
    scene.create("two", R"({"Transform3D": {}, "MeshInstance3D": {}, "BoxMesh": {},
                            "SphereMesh": {}})");
    scene.create("ring", R"({"Transform3D": {}, "MeshInstance3D": {},
                             "TorusMesh": {"inner_radius": 1}})");
    scene.create("textured", R"({"Transform3D": {}, "MeshInstance3D": {}, "QuadMesh": {},
                                 "StandardMaterial3D": {"albedo_texture": "missing.png"}})");
    scene.create("loose", R"({"PrismMesh": {}, "PlaneMesh": {}})");

    const auto problems = scene3d::check_meshes(scene.world);
    REQUIRE(problems.size() == 4);
    CHECK(problems[0].path == "/components/MeshInstance3D");
    CHECK(problems[0].message.starts_with(
        "has no mesh: set mesh to a .gltf, .glb, .obj, .fbx, .blend or .model.json file"));
    CHECK(problems[1].path == "/components");
    CHECK(problems[1].message == "an entity draws one mesh, but it has BoxMesh and SphereMesh");
    CHECK(problems[2].path == "/components/TorusMesh");
    CHECK(problems[3].message == "an entity draws one mesh, but it has PlaneMesh and PrismMesh");

    // Drawing also reports the missing texture, and draws the quad without it.
    const auto device = render::create_null_device();
    canvas::CanvasRenderer renderer(*device, scene.assets);
    canvas::FrameContents contents;
    canvas::CanvasOptions options;
    options.contents = &contents;
    REQUIRE(renderer.draw(scene.world, options).ok());
    CHECK(renderer.meshes_drawn() == 1);
    REQUIRE(contents.problems.size() == 4);
    CHECK(contents.problems[3].path == "/components/StandardMaterial3D/albedo_texture");

    const auto errors = scene3d::check_meshes(scene.components, parse(R"({"format": 1,
        "entities": {"cube": {"components": {"Transform3D": {}, "MeshInstance3D": {}}}}})"),
                                              {});
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].path == "/entities/cube/components/MeshInstance3D");
}

TEST_CASE("the 3D world draws under the canvas, on the software device and every GPU") {
    Scene scene;
    scene.create("camera", R"({"Transform3D": {"position": [0, 0, 3]}, "Camera3D": {}})");
    scene.create("env", R"({"WorldEnvironment": {"background_mode": "color",
                                                 "background_color": [0, 0, 1, 1]}})");
    scene.create("cube", R"({"Transform3D": {}, "MeshInstance3D": {}, "BoxMesh": {},
        "StandardMaterial3D": {"shading_mode": "unshaded", "albedo_color": [1, 0, 0, 1]}})");
    scene.create("lit", R"({"Transform3D": {"position": [1.2, 0, 0], "scale": [0.5, 0.5, 0.5]},
                            "MeshInstance3D": {}, "BoxMesh": {}})");
    scene.create("sun", R"({"Transform3D": {}, "DirectionalLight3D": {}})");
    scene.create("badge", R"({"Control": {"offset_right": 8, "offset_bottom": 8},
                              "ColorRect": {"color": [0, 1, 0, 1]}})");

    const auto captured = canvas::capture(scene.world, scene.assets);
    REQUIRE(captured.ok());
    const render::Image& image = captured.value().image;
    CHECK(captured.value().meshes == 2);
    CHECK(pixel(image, 32, 32) == Rgba{255, 0, 0, 255}); // the red cube
    CHECK(pixel(image, 32, 4) == Rgba{0, 0, 255, 255});  // the background
    CHECK(pixel(image, 4, 4) == Rgba{0, 255, 0, 255});   // the control, over it all
    // The small cube faces the sun, lit white, plus the blue background's ambient light.
    const Rgba lit = pixel(image, 50, 32);
    CHECK(lit[0] > 200);
    CHECK(lit[2] >= lit[0]);

    for (const auto& device : drawing_devices()) {
        CAPTURE(render::backend_name(device->backend()));
        canvas::CanvasRenderer renderer(*device, scene.assets);
        auto target = device->create_texture(
            {.width = 64, .height = 64, .filter = render::Filter::Linear, .render_target = true});
        REQUIRE(target.ok());
        canvas::CanvasOptions options;
        options.target = target.value();
        options.width = 64.0F;
        options.height = 64.0F;
        REQUIRE(renderer.draw(scene.world, options).ok());
        CHECK(renderer.meshes_drawn() == 2);
        CHECK(renderer.lights_3d_drawn() == 1);
        const auto drawn = device->read_texture(target.value());
        REQUIRE_MESSAGE(drawn.ok(), (drawn.ok() ? std::string() : drawn.error().message));
        for (const auto& [x, y] :
             {std::pair{32U, 32U}, std::pair{32U, 4U}, std::pair{4U, 4U}, std::pair{50U, 32U}}) {
            CAPTURE(x);
            CAPTURE(y);
            const Rgba want = pixel(image, x, y);
            const Rgba got = pixel(drawn.value(), x, y);
            for (std::size_t c = 0; c < 4; ++c) {
                CHECK(std::abs(int{got[c]} - int{want[c]}) <= 2);
            }
        }
        device->destroy_texture(target.value());
    }
}

TEST_CASE("the WorldEnvironment's tonemap, glow and adjustments post-process the 3D world") {
    Scene scene;
    scene.create("camera", R"({"Transform3D": {"position": [0, 0, 3]}, "Camera3D": {}})");
    scene.create("cube", R"({"Transform3D": {}, "MeshInstance3D": {}, "BoxMesh": {},
        "StandardMaterial3D": {"shading_mode": "unshaded", "albedo_color": [1, 0, 0, 1]}})");
    const entt::entity env = scene.create("env", R"({"WorldEnvironment": {
        "tonemap_mode": "aces", "tonemap_exposure": 2, "tonemap_white": 4, "glow_enabled": true,
        "glow_bloom": 0.25, "glow_hdr_threshold": 0.5, "adjustment_enabled": true,
        "adjustment_saturation": 0, "adjustment_color_correction": "grade.png"}})");
    const scene3d::Environment read = scene3d::environment(scene.world, {});
    CHECK(read.entity == env);
    CHECK(read.post == render::PostProcess{.tonemapper = render::Tonemapper::Aces,
                                           .exposure = 2.0F,
                                           .white = 4.0F,
                                           .glow = true,
                                           .glow_bloom = 0.25F,
                                           .glow_hdr_threshold = 0.5F,
                                           .adjustments = true,
                                           .saturation = 0.0F});
    CHECK(read.color_correction == "grade.png");

    // Saturation 0 turns the red cube gray. The texture is missing, so it is reported and left
    // out.
    auto& settings = scene.world.ecs().get<world::WorldEnvironment>(env);
    settings.tonemap_mode = "linear";
    settings.tonemap_exposure = 1.0F;
    settings.glow_enabled = false;
    const auto device = render::create_software_device();
    const auto draw = [&](canvas::FrameContents& contents) {
        canvas::CanvasRenderer renderer(*device, scene.assets);
        auto target = device->create_texture({.width = 64, .height = 64, .render_target = true});
        REQUIRE(target.ok());
        canvas::CanvasOptions options;
        options.target = target.value();
        options.width = 64.0F;
        options.height = 64.0F;
        options.contents = &contents;
        REQUIRE(renderer.draw(scene.world, options).ok());
        auto image = device->read_texture(target.value());
        REQUIRE(image.ok());
        device->destroy_texture(target.value());
        return pixel(image.value(), 32, 32);
    };
    canvas::FrameContents contents;
    Rgba middle = draw(contents);
    CHECK(std::abs(int{middle[0]} - 85) <= 2);
    CHECK(middle[1] == middle[0]);
    REQUIRE(contents.problems.size() == 1);
    CHECK(contents.problems[0].path == "/components/WorldEnvironment/adjustment_color_correction");

    // A texture of the wrong shape is reported too; a 1D one that maps everything to blue is used.
    const render::Image square{
        .width = 2, .height = 2, .pixels = std::vector<std::uint8_t>(16, 255)};
    scene.files.files["square.png"] = render::encode_png(square).value();
    settings.adjustment_color_correction = "square.png";
    contents = {};
    draw(contents);
    REQUIRE(contents.problems.size() == 1);
    CHECK(contents.problems[0].message.find("not 2x2") != std::string::npos);
    const render::Image blue{.width = 2, .height = 1, .pixels = {0, 0, 255, 255, 0, 0, 255, 255}};
    scene.files.files["blue.png"] = render::encode_png(blue).value();
    settings.adjustment_color_correction = "blue.png";
    contents = {};
    CHECK(draw(contents) == Rgba{0, 0, 255, 255});
    CHECK(contents.problems.empty());
}

TEST_CASE("the WorldEnvironment's sky and fog, and the suns its sky shows") {
    Scene scene;
    const render::Color clear{.r = 0.1F, .g = 0.2F, .b = 0.3F, .a = 1.0F};
    // A sky background with no sky material is Godot's default procedural sky. Ambient light from
    // it is ambient_light_color, mixed with the sky's light.
    const entt::entity env = scene.create("env", R"({"WorldEnvironment": {
        "background_mode": "sky", "background_energy_multiplier": 2, "sky_custom_fov": 50,
        "sky_rotation": [0, 1.5707964, 0], "ambient_light_color": [0.2, 0.2, 0.2, 1],
        "ambient_light_energy": 3}})");
    scene3d::Environment read = scene3d::environment(scene.world, clear);
    CHECK(read.sky.kind == render::SkyKind::Procedural);
    CHECK(read.sky.procedural == render::ProceduralSky{});
    CHECK(read.sky.energy == 2.0F);
    CHECK(read.sky.custom_fov == 50.0F);
    CHECK(read.ambient == std::array<float, 3>{0.2F, 0.2F, 0.2F});
    CHECK(read.ambient_energy == 3.0F);
    CHECK(read.sky_texture_field == "/components/ProceduralSkyMaterial/sky_cover");
    CHECK(read.problems.empty());
    // Turned a quarter about y, the sky's +x is where the world's -z is.
    const auto& m = read.sky.orientation;
    const Vec3 forward{-m[8], -m[9], -m[10]};
    CHECK(near(forward, {1.0F, 0.0F, 0.0F}));

    // The material on the entity, with its texture by path.
    scene.create("unused", R"({"ProceduralSkyMaterial": {"sky_curve": 0.5}})");
    world::ProceduralSkyMaterial procedural;
    procedural.sky_top_color = {0.0F, 0.0F, 1.0F, 1.0F};
    procedural.sun_curve = 0.3F;
    procedural.sky_cover = "clouds.png";
    procedural.use_debanding = false;
    scene.world.ecs().emplace<world::ProceduralSkyMaterial>(env, procedural);
    read = scene3d::environment(scene.world, clear);
    CHECK(read.sky.procedural.sky_top_color == std::array<float, 3>{0.0F, 0.0F, 1.0F});
    CHECK(read.sky.procedural.sun_curve == 0.3F);
    CHECK(read.sky.procedural.sky_curve == 0.15F);
    CHECK_FALSE(read.sky.procedural.use_debanding);
    CHECK(read.sky_texture == "clouds.png");
    CHECK(read.sky_filter == render::Filter::Linear);

    // A second material draws nothing and is reported.
    scene.world.ecs().emplace<world::PhysicalSkyMaterial>(
        env, world::PhysicalSkyMaterial{.turbidity = 4.0F, .night_sky = "stars.png"});
    read = scene3d::environment(scene.world, clear);
    CHECK(read.sky.kind == render::SkyKind::Procedural);
    REQUIRE(read.problems.size() == 1);
    CHECK(read.problems[0].path == "/components/PhysicalSkyMaterial");
    scene.world.ecs().remove<world::ProceduralSkyMaterial>(env);
    read = scene3d::environment(scene.world, clear);
    CHECK(read.problems.empty());
    CHECK(read.sky.kind == render::SkyKind::Physical);
    CHECK(read.sky.physical.turbidity == 4.0F);
    CHECK(read.sky_texture == "stars.png");
    CHECK(read.sky_texture_field == "/components/PhysicalSkyMaterial/night_sky");
    scene.world.ecs().remove<world::PhysicalSkyMaterial>(env);
    scene.world.ecs().emplace<world::PanoramaSkyMaterial>(
        env, world::PanoramaSkyMaterial{
                 .panorama = "view.png", .filter = false, .energy_multiplier = 0.5F});
    read = scene3d::environment(scene.world, clear);
    CHECK(read.sky.kind == render::SkyKind::Panorama);
    CHECK(read.sky.panorama.energy_multiplier == 0.5F);
    CHECK(read.sky_filter == render::Filter::Nearest);
    CHECK(read.sky_texture_field == "/components/PanoramaSkyMaterial/panorama");

    // Fog, as its fields say. Over a color background it is a sky of that color, which the
    // background's energy multiplies twice, as Godot's.
    auto& settings = scene.world.ecs().get<world::WorldEnvironment>(env);
    CHECK_FALSE(read.fog.enabled);
    settings.fog_enabled = true;
    settings.fog_mode = "depth";
    settings.fog_light_color = {1.0F, 0.0F, 0.0F, 1.0F};
    settings.fog_density = 0.5F;
    settings.fog_depth_end = 0.0F;
    settings.fog_sky_affect = 0.25F;
    read = scene3d::environment(scene.world, clear);
    CHECK(read.fog == render::Fog3D{.enabled = true,
                                    .depth = true,
                                    .light_color = {1.0F, 0.0F, 0.0F},
                                    .density = 0.5F,
                                    .sky_affect = 0.25F,
                                    .depth_end = 0.0F});
    settings.background_mode = "color";
    settings.background_color = {0.0F, 0.5F, 0.0F, 1.0F};
    read = scene3d::environment(scene.world, clear);
    CHECK(read.sky.kind == render::SkyKind::Color);
    CHECK(read.sky.color == std::array<float, 3>{0.0F, 1.0F, 0.0F});
    CHECK(read.sky.energy == 2.0F);
    CHECK(read.sky_texture.empty());
    settings.fog_enabled = false;
    CHECK(scene3d::environment(scene.world, clear).sky.kind == render::SkyKind::None);

    // A material's disable_fog.
    std::vector<scene3d::Problem> problems;
    world::StandardMaterial3D clear_of_fog;
    clear_of_fog.disable_fog = true;
    CHECK(scene3d::material(clear_of_fog, nullptr, problems).disable_fog);

    // Suns: directional lights whose sky_mode isn't "light_only", in tree order, at most four.
    // "sky_only" ones light no meshes.
    Scene suns;
    suns.create("sun", R"({"Transform3D": {"rotation": [-1.5707964, 0, 0]},
        "DirectionalLight3D": {"light_color": [1, 0.5, 0, 1], "light_energy": 3,
                               "light_angular_distance": 2, "sky_mode": "sky_only"}})");
    suns.create("lamp", R"({"Transform3D": {}, "DirectionalLight3D": {"sky_mode": "light_only"}})");
    suns.create("hidden", R"({"Transform3D": {"visible": false}, "DirectionalLight3D": {}})");
    for (int i = 0; i < 4; ++i) {
        suns.create("moon" + std::to_string(i),
                    R"({"Transform3D": {}, "DirectionalLight3D": {"light_negative": true}})");
    }
    world::Tree3D tree(suns.world);
    const auto global = [&](entt::entity entity) { return tree.global(entity); };
    const auto shown = scene3d::sky_lights(suns.world, tree, global);
    REQUIRE(shown.size() == render::kMaxSkyLights);
    CHECK(near(vec(shown[0].direction), {0.0F, -1.0F, 0.0F}));
    CHECK(shown[0].color == std::array<float, 3>{1.0F, 0.5F, 0.0F});
    CHECK(shown[0].energy == 3.0F);
    CHECK(shown[0].angular_distance == 2.0F);
    CHECK(shown[1].energy == -1.0F);
    const auto lit = scene3d::lights(suns.world, tree, global);
    REQUIRE(lit.size() == 5);
    CHECK(lit[0].direction == std::array<float, 3>{0.0F, 0.0F, -1.0F});
}

TEST_CASE("camera attributes expose the 3D world, as Godot's CameraAttributesPractical") {
    Scene scene;
    const render::Color clear{.r = 0.1F, .g = 0.2F, .b = 0.3F, .a = 1.0F};
    const entt::entity camera = scene.create("camera", R"({"Transform3D": {}, "Camera3D": {}})");
    CHECK((scene3d::camera_attributes(scene.world, camera) == entt::null));
    scene3d::Environment read = scene3d::environment(scene.world, clear, camera);
    CHECK((read.attributes == entt::null));
    CHECK(read.exposure == 1.0F);
    CHECK_FALSE(read.post.auto_exposure);

    // A camera with none uses the WorldEnvironment's. exposure_multiplier brightens the
    // background and the sky, but not the ambient light from the background.
    const entt::entity env = scene.create("env", R"({"WorldEnvironment": {
        "background_mode": "color", "background_color": [0.5, 0.25, 1, 1],
        "background_energy_multiplier": 2},
        "CameraAttributesPractical": {"exposure_multiplier": 0.5, "auto_exposure_enabled": true,
                                      "auto_exposure_scale": 0.2,
                                      "auto_exposure_max_sensitivity": 1600}})");
    CHECK((scene3d::camera_attributes(scene.world, camera) == env));
    read = scene3d::environment(scene.world, clear, camera);
    CHECK((read.attributes == env));
    CHECK(read.exposure == 0.5F);
    CHECK(read.background.r == 0.5F);
    CHECK(read.background.g == 0.25F);
    CHECK(read.background.b == 1.0F);
    CHECK(read.sky.energy == 1.0F);
    CHECK(read.ambient == std::array<float, 3>{0.5F, 0.25F, 1.0F});
    CHECK(read.ambient_energy == 2.0F);
    // Auto exposure's sensitivities over the camera's, ISO 100, as luminances.
    CHECK(read.post.auto_exposure);
    CHECK(read.post.auto_exposure_scale == 0.2F);
    CHECK(read.post.auto_exposure_min == 0.0F);
    CHECK(read.post.auto_exposure_max == 2.0F);
    CHECK((scene3d::environment(scene.world, clear).attributes == env));
    // Fog over the background is a sky the multiplier brightens twice, as the background's
    // energy.
    auto& settings = scene.world.ecs().get<world::WorldEnvironment>(env);
    settings.fog_enabled = true;
    read = scene3d::environment(scene.world, clear, camera);
    CHECK(read.sky.kind == render::SkyKind::Color);
    CHECK(read.sky.color == std::array<float, 3>{0.5F, 0.25F, 1.0F});
    CHECK(read.sky.energy == 1.0F);
    settings.fog_enabled = false;

    // The camera's own come first.
    scene.world.ecs().emplace<world::CameraAttributesPractical>(
        camera, world::CameraAttributesPractical{.exposure_sensitivity = 200.0F,
                                                 .exposure_multiplier = 2.0F,
                                                 .auto_exposure_enabled = true});
    CHECK((scene3d::camera_attributes(scene.world, camera) == camera));
    read = scene3d::environment(scene.world, clear, camera);
    CHECK(read.background.r == 2.0F);
    CHECK(read.post.auto_exposure_max == 0.5F);
    CHECK(read.problems.empty());
    scene.world.ecs().get<world::CameraAttributesPractical>(camera).exposure_sensitivity = 0.0F;
    read = scene3d::environment(scene.world, clear, camera);
    CHECK_FALSE(read.post.auto_exposure);
    REQUIRE(read.problems.size() == 1);
    CHECK(read.problems[0].path == "/components/CameraAttributesPractical/exposure_sensitivity");

    // Lights and the sky's suns take the multiplier too.
    scene.world.ecs().get<world::CameraAttributesPractical>(camera).exposure_sensitivity = 100.0F;
    scene.create("lamp", R"({"Transform3D": {}, "OmniLight3D": {"light_energy": 3}})");
    const auto device = render::create_null_device();
    scene3d::Renderer renderer(*device, nullptr);
    auto frame = renderer.prepare(scene.world, 64.0F, 64.0F, 1.0, {}, {}, 1.0);
    REQUIRE(frame.has_value());
    REQUIRE(frame->scene.lights.size() == 1);
    CHECK(frame->scene.lights[0].energy == 6.0F);

    // Auto exposure moves auto_exposure_speed (0.5) of the way per second of game time, and starts
    // over when time goes back, when it follows other attributes and when it turns on again.
    CHECK(frame->scene.post.auto_exposure_adjust == 0.0F);
    const std::uint64_t first = frame->scene.post.auto_exposure_version;
    CHECK(first != 0);
    frame = renderer.prepare(scene.world, 64.0F, 64.0F, 1.0, {}, {}, 1.5);
    CHECK(frame->scene.post.auto_exposure_version == first);
    CHECK(frame->scene.post.auto_exposure_adjust == 0.25F);
    frame = renderer.prepare(scene.world, 64.0F, 64.0F, 1.0, {}, {}, 1.0);
    const std::uint64_t back = frame->scene.post.auto_exposure_version;
    CHECK(back != first);
    CHECK(frame->scene.post.auto_exposure_adjust == 0.0F);
    scene.world.ecs().remove<world::CameraAttributesPractical>(camera);
    frame = renderer.prepare(scene.world, 64.0F, 64.0F, 1.0, {}, {}, 2.0);
    CHECK(frame->scene.post.auto_exposure_version != back);
    CHECK(frame->scene.post.auto_exposure_max == 2.0F);
    const std::uint64_t environment = frame->scene.post.auto_exposure_version;
    scene.world.ecs().get<world::CameraAttributesPractical>(env).auto_exposure_enabled = false;
    frame = renderer.prepare(scene.world, 64.0F, 64.0F, 1.0, {}, {}, 3.0);
    CHECK_FALSE(frame->scene.post.auto_exposure);
    scene.world.ecs().get<world::CameraAttributesPractical>(env).auto_exposure_enabled = true;
    frame = renderer.prepare(scene.world, 64.0F, 64.0F, 1.0, {}, {}, 4.0);
    CHECK(frame->scene.post.auto_exposure_version != environment);
}

TEST_CASE("camera attributes blur near and far, as Godot's depth of field") {
    Scene scene;
    const entt::entity camera = scene.create("camera", R"({"Transform3D": {}, "Camera3D": {},
        "CameraAttributesPractical": {"dof_blur_far_enabled": true, "dof_blur_far_distance": 6,
            "dof_blur_far_transition": -1, "dof_blur_near_enabled": true,
            "dof_blur_near_distance": 1.5, "dof_blur_near_transition": 0.5,
            "dof_blur_amount": 0.3}})");
    const scene3d::Environment read = scene3d::environment(scene.world, {}, camera);
    CHECK(read.post.dof_far);
    CHECK(read.post.dof_far_distance == 6.0F);
    CHECK(read.post.dof_far_transition == -1.0F);
    CHECK(read.post.dof_near);
    CHECK(read.post.dof_near_distance == 1.5F);
    CHECK(read.post.dof_near_transition == 0.5F);
    CHECK(read.post.dof_amount == 0.3F);
    CHECK_FALSE(read.post.auto_exposure);
    // The project's settings shape the blur, and with jitter the game time seeds it.
    const auto device = render::create_null_device();
    scene3d::Renderer renderer(*device, nullptr);
    const render::DepthOfFieldSettings settings{.bokeh_shape = render::BokehShape::Circle,
                                                .bokeh_quality = render::BokehQuality::High,
                                                .use_jitter = true};
    auto frame = renderer.prepare(scene.world, 64.0F, 64.0F, 1.0, {}, {}, 3.25, settings);
    REQUIRE(frame.has_value());
    CHECK(frame->scene.post.depth_of_field == settings);
    CHECK(frame->scene.post.dof_jitter_seed == 250.0F);
    frame = renderer.prepare(scene.world, 64.0F, 64.0F, 1.0, {}, {}, 3.25);
    CHECK(frame->scene.post.depth_of_field == render::DepthOfFieldSettings{});
    CHECK(frame->scene.post.dof_jitter_seed == 0.0F);
}

TEST_CASE("a captured frame blurs what is past dof_blur_far_distance") {
    Scene scene;
    scene.create("camera", R"({"Transform3D": {"position": [0, 0, 3]}, "Camera3D": {}})");
    const entt::entity env = scene.create("env", R"({"WorldEnvironment": {
        "background_mode": "color", "background_color": [0, 0, 0, 1]}})");
    scene.create("cube", R"({"Transform3D": {}, "MeshInstance3D": {}, "BoxMesh": {},
        "StandardMaterial3D": {"albedo_color": [0, 0, 0, 1], "emission_enabled": true,
        "emission": [1, 0, 0, 1]}})");
    // The red across the middle row of the frame. The cube's front is 2.5 meters away, and the
    // black background is at the far plane.
    const auto row = [&] {
        const auto captured = canvas::capture(scene.world, scene.assets, {});
        REQUIRE(captured.ok());
        std::array<int, 64> red{};
        for (std::uint32_t x = 0; x < red.size(); ++x) {
            red.at(x) = pixel(captured.value().image, x, 32)[0];
        }
        return red;
    };
    const std::array<int, 64> sharp = row();
    std::size_t edge = 32; // the first pixel of background right of the cube
    while (edge < 63 && sharp.at(edge) > 0) {
        ++edge;
    }
    REQUIRE(sharp.at(edge - 1) == 255);
    REQUIRE(sharp.at(edge + 2) == 0);
    // Far blur from 1 meter blurs the cube into the background.
    scene.world.ecs().emplace<world::CameraAttributesPractical>(
        env, world::CameraAttributesPractical{.dof_blur_far_enabled = true,
                                              .dof_blur_far_distance = 1.0F,
                                              .dof_blur_far_transition = 1.0F});
    const std::array<int, 64> blurred = row();
    CHECK(blurred.at(edge - 1) < 255);
    CHECK(blurred.at(edge + 2) > 0);
    // From 5 meters it blurs only the background: the cube stays sharp, and as in Godot the
    // background beside it takes a trace of its light.
    scene.world.ecs().get<world::CameraAttributesPractical>(env).dof_blur_far_distance = 5.0F;
    const std::array<int, 64> behind = row();
    for (std::size_t x = 32; x < edge; ++x) {
        CHECK(behind.at(x) == sharp.at(x));
    }
    CHECK(behind.at(edge + 2) < blurred.at(edge + 2));
}

TEST_CASE("the WorldEnvironment's SSAO fields and the project's settings reach the frame") {
    Scene scene;
    scene.create("camera", R"({"Transform3D": {}, "Camera3D": {}})");
    scene.create("env", R"({"WorldEnvironment": {"ssao_enabled": true, "ssao_radius": 2,
        "ssao_intensity": 3, "ssao_power": 2, "ssao_detail": 1, "ssao_horizon": 0.1,
        "ssao_sharpness": 0.5, "ssao_light_affect": 0.25, "ssao_ao_channel_affect": 0.5}})");
    const scene3d::Environment read = scene3d::environment(scene.world, {});
    CHECK(read.ssao == render::Ssao3D{.enabled = true,
                                      .radius = 2.0F,
                                      .intensity = 3.0F,
                                      .power = 2.0F,
                                      .detail = 1.0F,
                                      .horizon = 0.1F,
                                      .sharpness = 0.5F,
                                      .light_affect = 0.25F,
                                      .ao_channel_affect = 0.5F});
    const auto device = render::create_null_device();
    scene3d::Renderer renderer(*device, nullptr);
    const render::SsaoSettings settings{
        .quality = render::SsaoQuality::Ultra, .half_size = false, .blur_passes = 4};
    auto frame = renderer.prepare(scene.world, 64.0F, 64.0F, 1.0, {}, {}, 0.0, {}, settings);
    REQUIRE(frame.has_value());
    CHECK(frame->scene.ssao.enabled);
    CHECK(frame->scene.ssao.radius == 2.0F);
    CHECK(frame->scene.ssao.settings == settings);
    // With ssao_enabled off, the frame has none.
    scene.world.ecs().get<world::WorldEnvironment>(scene.world.find("env")).ssao_enabled = false;
    frame = renderer.prepare(scene.world, 64.0F, 64.0F, 1.0, {}, {}, 0.0, {}, settings);
    CHECK_FALSE(frame->scene.ssao.enabled);
}

TEST_CASE("a captured frame darkens a wall where a slab stands against it, with SSAO") {
    Scene scene;
    scene.create("camera", R"({"Transform3D": {"position": [0, 0, 3]}, "Camera3D": {}})");
    const entt::entity env = scene.create("env", R"({"WorldEnvironment": {
        "background_mode": "color", "background_color": [0, 0, 0, 1],
        "ambient_light_source": "color", "ambient_light_color": [1, 1, 1, 1],
        "ambient_light_energy": 0.5}})");
    scene.create("wall", R"({"Transform3D": {"position": [0, 0, -1]}, "MeshInstance3D": {},
        "BoxMesh": {"size": [8, 8, 0.2]}, "StandardMaterial3D": {}})");
    // A slab 0.3 meters deep, standing against the wall.
    scene.create("slab", R"({"Transform3D": {"position": [0, 0, -0.75]}, "MeshInstance3D": {},
        "BoxMesh": {"size": [1, 1, 0.3]},
        "StandardMaterial3D": {"albedo_color": [0.5, 0.5, 0.5, 1]}})");
    const auto row = [&] {
        const auto captured = canvas::capture(scene.world, scene.assets, {});
        REQUIRE(captured.ok());
        std::array<int, 64> red{};
        for (std::uint32_t x = 0; x < red.size(); ++x) {
            red.at(x) = pixel(captured.value().image, x, 32)[0];
        }
        return red;
    };
    const std::array<int, 64> plain = row();
    std::size_t edge = 32; // the first pixel of wall right of the slab
    while (edge < 60 && plain.at(edge) == plain.at(32)) {
        ++edge;
    }
    REQUIRE(plain.at(edge) > plain.at(32));
    scene.world.ecs().get<world::WorldEnvironment>(env).ssao_enabled = true;
    const std::array<int, 64> occluded = row();
    CHECK(occluded.at(edge + 1) < plain.at(edge + 1) - 4);
    // Far from the slab the wall keeps its light.
    CHECK(occluded.at(62) >= plain.at(62) - 2);
}

TEST_CASE("a path-traced capture shades a wall darker where a slab stands against it") {
    Scene scene;
    scene.create("camera", R"({"Transform3D": {"position": [0, 0, 3]}, "Camera3D": {}})");
    scene.create("env", R"({"WorldEnvironment": {
        "background_mode": "color", "background_color": [0, 0, 0, 1],
        "ambient_light_source": "color", "ambient_light_color": [1, 1, 1, 1],
        "ambient_light_energy": 0.5}})");
    scene.create("wall", R"({"Transform3D": {"position": [0, 0, -1]}, "MeshInstance3D": {},
        "BoxMesh": {"size": [8, 8, 0.2]}, "StandardMaterial3D": {}})");
    scene.create("slab", R"({"Transform3D": {"position": [0, 0, -0.75]}, "MeshInstance3D": {},
        "BoxMesh": {"size": [1, 1, 0.3]},
        "StandardMaterial3D": {"albedo_color": [0.5, 0.5, 0.5, 1]}})");
    canvas::CaptureOptions options;
    options.path_trace = render::PathTrace3D{.samples = 32, .bounces = 2};
    const auto captured = canvas::capture(scene.world, scene.assets, options);
    REQUIRE(captured.ok());
    CHECK(captured.value().path_samples == 32);
    const auto red = [&](std::uint32_t x) { return pixel(captured.value().image, x, 32)[0]; };
    // Light from the sky reaches the wall beside the slab only from the side away from it.
    std::uint32_t edge = 32; // the first pixel of wall right of the slab
    while (edge < 60 && std::abs(red(edge) - red(32)) < 8) {
        ++edge;
    }
    REQUIRE(edge < 60);
    CHECK(red(edge + 1) + 10 < red(63));
    // A capture without paths has none.
    const auto drawn = canvas::capture(scene.world, scene.assets, {});
    REQUIRE(drawn.ok());
    CHECK(drawn.value().path_samples == 0);
}

TEST_CASE("the WorldEnvironment's SSR fields and the project's settings reach the frame") {
    Scene scene;
    scene.create("camera", R"({"Transform3D": {}, "Camera3D": {}})");
    scene.create("env", R"({"WorldEnvironment": {"ssr_enabled": true, "ssr_max_steps": 32,
        "ssr_fade_in": 0.5, "ssr_fade_out": 3, "ssr_depth_tolerance": 0.25}})");
    const scene3d::Environment read = scene3d::environment(scene.world, {});
    CHECK(read.ssr == render::Ssr3D{.enabled = true,
                                    .max_steps = 32,
                                    .fade_in = 0.5F,
                                    .fade_out = 3.0F,
                                    .depth_tolerance = 0.25F});
    const auto device = render::create_null_device();
    scene3d::Renderer renderer(*device, nullptr);
    const render::SsrSettings settings{.half_size = false};
    auto frame = renderer.prepare(scene.world, 64.0F, 64.0F, 1.0, {}, {}, 0.0, {}, {}, settings);
    REQUIRE(frame.has_value());
    CHECK(frame->scene.ssr.enabled);
    CHECK(frame->scene.ssr.max_steps == 32);
    CHECK(frame->scene.ssr.settings == settings);
    // With ssr_enabled off, the frame has none.
    scene.world.ecs().get<world::WorldEnvironment>(scene.world.find("env")).ssr_enabled = false;
    frame = renderer.prepare(scene.world, 64.0F, 64.0F, 1.0, {}, {}, 0.0, {}, {}, settings);
    CHECK_FALSE(frame->scene.ssr.enabled);
}

TEST_CASE("a captured frame reflects a glowing cube in a polished floor, with SSR") {
    Scene scene;
    scene.create("camera", R"({"Transform3D": {"position": [0, 1, 3], "rotation": [-0.3, 0, 0]},
        "Camera3D": {}})");
    const entt::entity env = scene.create("env", R"({"WorldEnvironment": {
        "background_mode": "color", "background_color": [0, 0, 0, 1],
        "ambient_light_source": "color", "ambient_light_color": [1, 1, 1, 1],
        "ambient_light_energy": 0.3}})");
    scene.create("floor", R"({"Transform3D": {"position": [0, -0.1, 0]}, "MeshInstance3D": {},
        "BoxMesh": {"size": [8, 0.2, 8]},
        "StandardMaterial3D": {"metallic": 1, "roughness": 0.05}})");
    scene.create("cube", R"({"Transform3D": {"position": [0, 0.5, -1]}, "MeshInstance3D": {},
        "BoxMesh": {}, "StandardMaterial3D": {"albedo_color": [0, 0, 0, 1],
        "emission_enabled": true, "emission": [1, 0, 0, 1]}})");
    const auto column = [&] {
        const auto captured = canvas::capture(scene.world, scene.assets, {});
        REQUIRE(captured.ok());
        std::array<int, 64> red{};
        for (std::uint32_t y = 0; y < red.size(); ++y) {
            red.at(y) = pixel(captured.value().image, 32, y)[0];
        }
        return red;
    };
    const std::array<int, 64> plain = column();
    // The cube's bottom edge: the last red pixel down the middle column.
    std::size_t edge = 63;
    while (edge > 0 && plain.at(edge) < 200) {
        --edge;
    }
    REQUIRE(edge > 16);
    REQUIRE(edge < 56);
    scene.world.ecs().get<world::WorldEnvironment>(env).ssr_enabled = true;
    const std::array<int, 64> reflected = column();
    // Below the cube the floor shows its reflection, and the cube itself is as it was.
    CHECK(reflected.at(edge + 3) > plain.at(edge + 3) + 40);
    CHECK(reflected.at(edge - 2) == plain.at(edge - 2));
}

TEST_CASE("ReflectionProbes reach the frame largest first, with cubes in one atlas") {
    Scene scene;
    scene.create("camera", R"({"Transform3D": {}, "Camera3D": {}})");
    scene.create("small", R"({"Transform3D": {"position": [1, 2, 3]},
        "ReflectionProbe": {"size": [2, 2, 2], "intensity": 0.5, "box_projection": true,
        "ambient_mode": "color", "ambient_color": [1, 0, 0, 1], "ambient_color_energy": 2}})");
    scene.create("large", R"({"Transform3D": {"rotation": [0, 1.5708, 0], "scale": [2, 2, 2]},
        "ReflectionProbe": {"size": [10, 4, 10], "origin_offset": [0, 1, 0],
        "blend_distance": 2, "reflection_mask": 3}})");
    const entt::entity hidden =
        scene.create("hidden", R"({"Transform3D": {"visible": false}, "ReflectionProbe": {}})");
    const auto device = render::create_null_device();
    scene3d::Renderer renderer(*device, nullptr);
    auto frame = renderer.prepare(scene.world, 64.0F, 64.0F, 1.0, {});
    REQUIRE(frame.has_value());
    CHECK(renderer.problems().empty());
    const render::Reflections3D& reflections = frame->scene.reflections;
    CHECK(reflections.atlas != render::TextureId::None);
    REQUIRE(reflections.probes.size() == 2);
    const render::ReflectionProbe3D& large = reflections.probes.at(0);
    const render::ReflectionProbe3D& small = reflections.probes.at(1);
    // The size is the probe's own: its node's scale is left out, as Godot's.
    CHECK(large.size == std::array{10.0F, 4.0F, 10.0F});
    CHECK(large.origin_offset == std::array{0.0F, 1.0F, 0.0F});
    CHECK(large.blend_distance == 2.0F);
    CHECK(large.reflection_mask == 3U);
    CHECK(large.transform.at(0) == doctest::Approx(0.0F).epsilon(0.0001));
    CHECK(large.transform.at(2) == doctest::Approx(-1.0F).epsilon(0.0001));
    CHECK(small.transform.at(12) == 1.0F);
    CHECK(small.transform.at(13) == 2.0F);
    CHECK(small.transform.at(14) == 3.0F);
    CHECK(small.intensity == 0.5F);
    CHECK(small.box_projection);
    CHECK(small.ambient_mode == render::ReflectionAmbient::Color);
    CHECK(small.ambient_color == std::array{1.0F, 0.0F, 0.0F});
    CHECK(small.ambient_color_energy == 2.0F);
    CHECK(large.cube != small.cube);
    // Compatibility draws no probes yet, and an atlas of one cube keeps only the largest.
    frame = renderer.prepare(scene.world, 64.0F, 64.0F, 1.0, {}, {}, 0.0, {}, {}, {}, {},
                             render::RenderMethod::Compatibility);
    CHECK(frame->scene.reflections.probes.empty());
    frame = renderer.prepare(scene.world, 64.0F, 64.0F, 1.0, {}, {}, 0.0, {}, {}, {},
                             {.reflection_size = 64, .reflection_count = 1});
    REQUIRE(frame->scene.reflections.probes.size() == 1);
    CHECK(frame->scene.reflections.probes.at(0).size == std::array{10.0F, 4.0F, 10.0F});
    // A shown probe takes part again.
    scene.world.ecs().get<world::Transform3D>(hidden).visible = true;
    frame = renderer.prepare(scene.world, 64.0F, 64.0F, 1.0, {});
    CHECK(frame->scene.reflections.probes.size() == 3);
}

TEST_CASE("Decals reach the frame in Forward+, their textures by path") {
    Scene scene;
    std::map<std::string, std::shared_ptr<const render::Image>, std::less<>> images;
    images["red.png"] = std::make_shared<const render::Image>(
        render::Image{.width = 1, .height = 1, .pixels = {255, 0, 0, 255}});
    images["flat.png"] = std::make_shared<const render::Image>(
        render::Image{.width = 1, .height = 1, .pixels = {128, 128, 255, 255}});
    const scene3d::ImageSource source =
        [&images](const std::string& path) -> slash::Result<std::shared_ptr<const render::Image>> {
        const auto it = images.find(path);
        if (it == images.end()) {
            return slash::make_error(slash::ErrorCode::NotFound, "no such file");
        }
        return it->second;
    };
    scene.create("camera", R"({"Transform3D": {}, "Camera3D": {"cull_mask": 3}})");
    scene.create("decal", R"({"Transform3D": {"position": [1, 2, -5]},
        "Decal": {"size": [4, 2, 4], "texture_albedo": "red.png", "texture_normal": "flat.png",
        "emission_energy": 2, "modulate": [1, 0.5, 0.5, 1], "albedo_mix": 0.5,
        "normal_fade": 2, "upper_fade": -1, "lower_fade": 0.5, "distance_fade_enabled": true,
        "distance_fade_begin": 20, "distance_fade_length": 5, "cull_mask": 3, "layers": 2,
        "sorting_offset": 1.5}})");
    const entt::entity hidden = scene.create(
        "hidden", R"({"Transform3D": {"visible": false}, "Decal": {"texture_albedo": "red.png"}})");
    scene.create("masked", R"({"Transform3D": {}, "Decal": {"texture_albedo": "red.png",
        "layers": 4}})");
    const entt::entity missing =
        scene.create("missing", R"({"Transform3D": {}, "Decal": {"texture_albedo": "gone.png"}})");
    const auto device = render::create_null_device();
    scene3d::Renderer renderer(*device, nullptr, {}, {}, {}, source);
    auto frame = renderer.prepare(scene.world, 64.0F, 64.0F, 1.0, {}, {}, 0.0, {}, {}, {}, {},
                                  render::RenderMethod::ForwardPlus, false, false, std::nullopt,
                                  render::DecalFilter::Nearest);
    REQUIRE(frame.has_value());
    // The missing texture is reported and its decal left out; the hidden decal and the one on a
    // layer the camera doesn't see are left out quietly.
    REQUIRE(renderer.problems().size() == 1);
    CHECK(renderer.problems()[0].entity == missing);
    CHECK(renderer.problems()[0].path == "/components/Decal/texture_albedo");
    CHECK(renderer.problems()[0].message == "no such file");
    const render::Decals3D& decals = frame->scene.decals;
    CHECK(decals.filter == render::DecalFilter::Nearest);
    REQUIRE(decals.decals.size() == 1);
    const render::Decal3D& decal = decals.decals[0];
    CHECK(decal.transform.at(12) == 1.0F);
    CHECK(decal.transform.at(13) == 2.0F);
    CHECK(decal.transform.at(14) == -5.0F);
    CHECK(decal.size == std::array{4.0F, 2.0F, 4.0F});
    CHECK(decal.albedo == images["red.png"]);
    CHECK(decal.normal == images["flat.png"]);
    CHECK(decal.orm == nullptr);
    CHECK(decal.emission == nullptr);
    CHECK(decal.emission_energy == 2.0F);
    CHECK(decal.modulate == std::array{1.0F, 0.5F, 0.5F, 1.0F});
    CHECK(decal.albedo_mix == 0.5F);
    // Fades are kept in Godot's ranges.
    CHECK(decal.normal_fade == 1.0F);
    CHECK(decal.upper_fade == 0.0F);
    CHECK(decal.lower_fade == 0.5F);
    CHECK(decal.distance_fade);
    CHECK(decal.distance_fade_begin == 20.0F);
    CHECK(decal.distance_fade_length == 5.0F);
    CHECK(decal.cull_mask == 3U);
    CHECK(decal.sorting_offset == 1.5F);
    // Compatibility draws no decals, as Godot's.
    frame = renderer.prepare(scene.world, 64.0F, 64.0F, 1.0, {}, {}, 0.0, {}, {}, {}, {},
                             render::RenderMethod::Compatibility);
    CHECK(frame->scene.decals.decals.empty());
    // A shown decal takes part again.
    scene.world.ecs().get<world::Transform3D>(hidden).visible = true;
    frame = renderer.prepare(scene.world, 64.0F, 64.0F, 1.0, {});
    CHECK(frame->scene.decals.decals.size() == 2);
    CHECK(frame->scene.decals.filter == render::DecalFilter::LinearMipmaps);
}

TEST_CASE("decals whose textures overflow the atlas are reported, and those before them kept") {
    Scene scene;
    std::map<std::string, std::shared_ptr<const render::Image>, std::less<>> images;
    const scene3d::ImageSource source =
        [&images](const std::string& path) -> slash::Result<std::shared_ptr<const render::Image>> {
        const auto it = images.find(path);
        if (it == images.end()) {
            return slash::make_error(slash::ErrorCode::NotFound, "no such file");
        }
        return it->second;
    };
    scene.create("camera", R"({"Transform3D": {}, "Camera3D": {}})");
    // Ten decals of four strips 16384 texels across each need more than an atlas holds.
    constexpr std::size_t kDecals = 10;
    constexpr std::uint32_t kWide = 16384;
    for (std::size_t i = 0; i < kDecals; ++i) {
        std::string fields;
        for (const char* field : {"albedo", "normal", "orm", "emission"}) {
            const std::string path = std::to_string(i) + field + ".png";
            images[path] = std::make_shared<const render::Image>(render::Image{
                .width = kWide, .height = 1, .pixels = std::vector<std::uint8_t>(kWide * 4)});
            fields += std::string(fields.empty() ? "" : ", ") + "\"texture_" + field + "\": \"" +
                      path + "\"";
        }
        scene.create("decal" + std::to_string(i),
                     R"({"Transform3D": {}, "Decal": {)" + fields + "}}");
    }
    const auto device = render::create_null_device();
    scene3d::Renderer renderer(*device, nullptr, {}, {}, {}, source);
    const auto frame = renderer.prepare(scene.world, 64.0F, 64.0F, 1.0, {});
    REQUIRE(frame.has_value());
    const std::size_t kept = frame->scene.decals.decals.size();
    CHECK(kept > 0);
    CHECK(kept < kDecals);
    CHECK(render::check_decals(frame->scene.decals).ok());
    REQUIRE(renderer.problems().size() == kDecals - kept);
    for (const auto& problem : renderer.problems()) {
        CHECK(problem.path == "/components/Decal");
        CHECK(problem.message.find("past the 33554432 it holds") != std::string::npos);
    }
}

TEST_CASE("FogVolumes reach the frame's volumetric fog in Forward+, with their FogMaterials") {
    Scene scene;
    scene.create("camera", R"({"Transform3D": {}, "Camera3D": {"cull_mask": 3}})");
    scene.create("env", R"({"WorldEnvironment": {"volumetric_fog_enabled": true}})");
    scene.create("box", R"({"Transform3D": {"position": [1, 2, -5]},
        "FogVolume": {"size": [4, -1, 2], "layers": 2}})");
    scene.create("glow", R"({"Transform3D": {"position": [0, 0, -8]},
        "FogVolume": {"shape": "ellipsoid"}, "FogMaterial": {"density": -0.5,
        "albedo": [1, 0.5, 0.25, 1], "emission": [0, 1, 0, 1], "height_falloff": -2,
        "edge_fade": 0.5}})");
    const entt::entity hidden = scene.create(
        "hidden", R"({"Transform3D": {"visible": false}, "FogVolume": {"shape": "world"}})");
    scene.create("masked", R"({"Transform3D": {}, "FogVolume": {"layers": 4}})");
    const auto device = render::create_null_device();
    scene3d::Renderer renderer(*device, nullptr);
    auto frame = renderer.prepare(scene.world, 64.0F, 64.0F, 1.0, {});
    REQUIRE(frame.has_value());
    CHECK(renderer.problems().empty());
    // The hidden volume and the one on a layer the camera doesn't see are left out.
    const std::vector<render::FogVolume3D>& volumes = frame->scene.volumetric_fog.volumes;
    REQUIRE(volumes.size() == 2);
    CHECK(volumes[0].shape == render::FogVolumeShape::Box);
    CHECK(volumes[0].transform.at(12) == 1.0F);
    CHECK(volumes[0].transform.at(14) == -5.0F);
    // Sizes and the material's falloff and fade are kept at 0 or more, as Godot's setters keep
    // them; with no FogMaterial, Godot's default one fills the volume.
    CHECK(volumes[0].size == std::array{4.0F, 0.0F, 2.0F});
    CHECK_FALSE(volumes[0].material);
    CHECK(volumes[1].shape == render::FogVolumeShape::Ellipsoid);
    CHECK(volumes[1].material);
    CHECK(volumes[1].density == -0.5F);
    CHECK(volumes[1].albedo == std::array{1.0F, 0.5F, 0.25F});
    CHECK(volumes[1].emission == std::array{0.0F, 1.0F, 0.0F});
    CHECK(volumes[1].height_falloff == 0.0F);
    CHECK(volumes[1].edge_fade == 0.5F);
    // Compatibility draws no volumetric fog, as Godot's.
    frame = renderer.prepare(scene.world, 64.0F, 64.0F, 1.0, {}, {}, 0.0, {}, {}, {}, {},
                             render::RenderMethod::Compatibility);
    CHECK(frame->scene.volumetric_fog.volumes.empty());
    // Past 32 volumes, the rest are reported.
    scene.world.ecs().get<world::Transform3D>(hidden).visible = true;
    for (int i = 0; i < 31; ++i) {
        scene.create("more" + std::to_string(i), R"({"Transform3D": {}, "FogVolume": {}})");
    }
    frame = renderer.prepare(scene.world, 64.0F, 64.0F, 1.0, {});
    CHECK(frame->scene.volumetric_fog.volumes.size() == render::kMaxFogVolumes);
    REQUIRE(renderer.problems().size() == 2);
    CHECK(renderer.problems()[0].path == "/components/FogVolume");
    CHECK(renderer.problems()[0].message ==
          "a frame draws at most 32 fog volumes; this one is left out");
}

TEST_CASE("a captured frame reflects a glowing cube in a polished floor, with a ReflectionProbe") {
    Scene scene;
    scene.create("camera", R"({"Transform3D": {"position": [0, 1, 3], "rotation": [-0.3, 0, 0]},
        "Camera3D": {}})");
    scene.create("env", R"({"WorldEnvironment": {
        "background_mode": "color", "background_color": [0, 0, 0, 1],
        "ambient_light_source": "color", "ambient_light_color": [1, 1, 1, 1],
        "ambient_light_energy": 0.3}})");
    scene.create("floor", R"({"Transform3D": {"position": [0, -0.1, 0]}, "MeshInstance3D": {},
        "BoxMesh": {"size": [8, 0.2, 8]},
        "StandardMaterial3D": {"metallic": 1, "roughness": 0.05}})");
    scene.create("cube", R"({"Transform3D": {"position": [0, 0.5, -1]}, "MeshInstance3D": {},
        "BoxMesh": {}, "StandardMaterial3D": {"albedo_color": [0, 0, 0, 1],
        "emission_enabled": true, "emission": [1, 0, 0, 1]}})");
    const auto column = [&] {
        const auto captured = canvas::capture(scene.world, scene.assets, {});
        REQUIRE(captured.ok());
        std::array<int, 64> red{};
        for (std::uint32_t y = 0; y < red.size(); ++y) {
            red.at(y) = pixel(captured.value().image, 32, y)[0];
        }
        return red;
    };
    const std::array<int, 64> plain = column();
    std::size_t edge = 63;
    while (edge > 0 && plain.at(edge) < 200) {
        --edge;
    }
    REQUIRE(edge > 16);
    REQUIRE(edge < 56);
    // Drawn from halfway up the cube, which the floor's reflections in front of it look toward.
    const entt::entity probe = scene.create("probe", R"({"Transform3D":
        {"position": [0, 0.5, 0]}, "ReflectionProbe": {"size": [8, 4, 8]}})");
    const std::array<int, 64> reflected = column();
    // Below the cube the floor shows its reflection, and the cube itself is as it was.
    CHECK(reflected.at(edge + 3) > plain.at(edge + 3) + 20);
    CHECK(std::abs(reflected.at(edge - 2) - plain.at(edge - 2)) <= 2);
    // A floor on a layer the probe's reflection_mask leaves out reflects only the sky.
    scene.world.ecs().get<world::ReflectionProbe>(probe).reflection_mask = 2;
    CHECK(column() == plain);
}

TEST_CASE("a captured frame adapts to the 3D world's brightness over game time") {
    Scene scene;
    scene.create("camera", R"({"Transform3D": {"position": [0, 0, 3]}, "Camera3D": {}})");
    const entt::entity env = scene.create("env", R"({"WorldEnvironment": {
        "background_mode": "color", "background_color": [0, 0, 0, 1]}})");
    const entt::entity cube = scene.create("cube", R"({"Transform3D": {}, "MeshInstance3D": {},
        "BoxMesh": {}, "StandardMaterial3D": {"albedo_color": [0, 0, 0, 1],
        "emission_enabled": true, "emission": [1, 0, 0, 1],
        "emission_energy_multiplier": 0.2}})");
    const auto red = [&](double time) {
        const auto captured = canvas::capture(scene.world, scene.assets, {.time = time});
        REQUIRE(captured.ok());
        return int{pixel(captured.value().image, 32, 32)[0]};
    };
    // Emission of 0.2 shows as sRGB 124, and twice as bright with an exposure multiplier of 2.
    CHECK(std::abs(red(0.0) - 124) <= 1);
    scene.world.ecs().emplace<world::CameraAttributesPractical>(
        env, world::CameraAttributesPractical{.exposure_multiplier = 2.0F});
    CHECK(std::abs(red(0.0) - 170) <= 1);

    // With auto exposure, the first capture is exposed by the frame's own brightness.
    auto& attributes = scene.world.ecs().get<world::CameraAttributesPractical>(env);
    attributes.auto_exposure_enabled = true;
    attributes.auto_exposure_scale = 0.02F;
    attributes.auto_exposure_max_sensitivity = 80000.0F;
    const int adapted = red(0.0);
    CHECK(adapted != 170);
    // A cube ten times as bright saturates until game time passes: at a speed of 0.5 the
    // brightness moves most of the way in 1.8 seconds, and all of it in the next 2.
    scene.world.ecs().get<world::StandardMaterial3D>(cube).emission_energy_multiplier = 2.0F;
    CHECK(red(0.0) == 255);
    const int most = red(1.8);
    CHECK(most < 255);
    CHECK(most > adapted);
    CHECK(std::abs(red(3.8) - adapted) <= 1);
}

TEST_CASE("the sky lights and is reflected in meshes by the WorldEnvironment's sources") {
    Scene scene;
    const render::Color clear{.r = 0.1F, .g = 0.2F, .b = 0.3F, .a = 1.0F};
    // A sky background lights and is reflected, as Godot's "background" sources.
    const entt::entity env =
        scene.create("env", R"({"WorldEnvironment": {"background_mode": "sky"}})");
    scene3d::Environment read = scene3d::environment(scene.world, clear);
    CHECK(read.radiance == render::Radiance3D{.kind = render::SkyKind::Procedural,
                                              .ambient = true,
                                              .reflections = true});
    CHECK(read.sky_material == render::SkyKind::Procedural);
    auto& settings = scene.world.ecs().get<world::WorldEnvironment>(env);
    settings.ambient_light_source = "color";
    read = scene3d::environment(scene.world, clear);
    CHECK(read.radiance ==
          render::Radiance3D{.kind = render::SkyKind::Procedural, .reflections = true});
    settings.ambient_light_source = "disabled";
    settings.reflected_light_source = "disabled";
    read = scene3d::environment(scene.world, clear);
    CHECK(read.radiance == render::Radiance3D{});
    CHECK(read.ambient == std::array<float, 3>{0.0F, 0.0F, 0.0F});
    CHECK(read.sky.kind == render::SkyKind::Procedural);

    // The sky's light from the entity's material when the background isn't the sky, which then
    // draws nothing behind the meshes.
    settings.background_mode = "color";
    settings.ambient_light_source = "sky";
    settings.ambient_light_color = {1.0F, 0.0F, 0.0F, 1.0F};
    settings.ambient_light_sky_contribution = 0.25F;
    settings.sky_radiance_size = 64;
    scene.world.ecs().emplace<world::PhysicalSkyMaterial>(
        env, world::PhysicalSkyMaterial{.night_sky = "stars.png"});
    read = scene3d::environment(scene.world, clear);
    CHECK(read.sky.kind == render::SkyKind::None);
    CHECK(read.sky_material == render::SkyKind::Physical);
    CHECK(read.radiance == render::Radiance3D{.kind = render::SkyKind::Physical,
                                              .size = 64,
                                              .ambient = true,
                                              .ambient_mix = 0.25F});
    CHECK(read.ambient == std::array<float, 3>{1.0F, 0.0F, 0.0F});
    CHECK(read.sky_texture == "stars.png");
    CHECK(read.problems.empty());
    settings.fog_enabled = true;
    settings.reflected_light_source = "sky";
    read = scene3d::environment(scene.world, clear);
    CHECK(read.sky.kind == render::SkyKind::Color);
    CHECK(read.radiance.reflections);

    // A size that isn't one of Godot's is reported, and drawn at 256.
    settings.sky_radiance_size = 100;
    read = scene3d::environment(scene.world, clear);
    REQUIRE(read.problems.size() == 1);
    CHECK(read.problems[0].path == "/components/WorldEnvironment/sky_radiance_size");
    CHECK(read.radiance.size == 256);
}

TEST_CASE("a sky behind nothing lights the meshes it surrounds, its texture by path") {
    Scene scene;
    scene.create("camera", R"({"Transform3D": {"position": [0, 0, 3]}, "Camera3D": {}})");
    scene.create("box", R"({"Transform3D": {}, "MeshInstance3D": {}, "BoxMesh": {}})");
    scene.create("env", R"({"WorldEnvironment": {"background_mode": "color",
        "ambient_light_source": "sky"}, "PanoramaSkyMaterial": {"panorama": "view.png"}})");
    const auto device = render::create_software_device();
    const auto draw = [&](canvas::FrameContents& contents) {
        canvas::CanvasRenderer renderer(*device, scene.assets);
        auto target = device->create_texture({.width = 64, .height = 64, .render_target = true});
        REQUIRE(target.ok());
        canvas::CanvasOptions options;
        options.target = target.value();
        options.width = 64.0F;
        options.height = 64.0F;
        options.contents = &contents;
        REQUIRE(renderer.draw(scene.world, options).ok());
        auto image = device->read_texture(target.value());
        REQUIRE(image.ok());
        device->destroy_texture(target.value());
        return std::pair{pixel(image.value(), 32, 32), pixel(image.value(), 2, 2)};
    };
    // A missing panorama is reported, and its light is black.
    canvas::FrameContents contents;
    auto [box, background] = draw(contents);
    CHECK(box == Rgba{0, 0, 0, 255});
    REQUIRE(contents.problems.size() == 1);
    CHECK(contents.problems[0].path == "/components/PanoramaSkyMaterial/panorama");
    // A green one lights the box green, behind a black background.
    const render::Image green{.width = 2, .height = 1, .pixels = {0, 255, 0, 255, 0, 255, 0, 255}};
    scene.files.files["view.png"] = render::encode_png(green).value();
    contents = {};
    std::tie(box, background) = draw(contents);
    CHECK(contents.problems.empty());
    CHECK(box[0] == 0);
    CHECK(box[1] > 200);
    CHECK(box[2] == 0);
    CHECK(background == Rgba{0, 0, 0, 255});
}

TEST_CASE("skies and fog draw behind and over the 3D world, their textures by path") {
    Scene scene;
    scene.create("camera", R"({"Transform3D": {"position": [0, 0, 3]}, "Camera3D": {}})");
    const entt::entity env = scene.create("env", R"({"WorldEnvironment": {
        "background_mode": "color", "background_color": [0, 0, 1, 1], "fog_enabled": true}})");
    const auto device = render::create_software_device();
    const auto draw = [&](canvas::FrameContents& contents) {
        canvas::CanvasRenderer renderer(*device, scene.assets);
        auto target = device->create_texture({.width = 64, .height = 64, .render_target = true});
        REQUIRE(target.ok());
        canvas::CanvasOptions options;
        options.target = target.value();
        options.width = 64.0F;
        options.height = 64.0F;
        options.contents = &contents;
        REQUIRE(renderer.draw(scene.world, options).ok());
        auto image = device->read_texture(target.value());
        REQUIRE(image.ok());
        device->destroy_texture(target.value());
        return pixel(image.value(), 32, 32);
    };
    // With no meshes, the fogged background is all fog: its light color.
    canvas::FrameContents contents;
    Rgba middle = draw(contents);
    CHECK(std::abs(int{middle[0]} - 132) <= 1);
    CHECK(std::abs(int{middle[1]} - 141) <= 1);
    CHECK(std::abs(int{middle[2]} - 155) <= 1);
    CHECK(contents.problems.empty());

    // A panorama that is missing is reported, and the sky is black; a green one is green.
    auto& settings = scene.world.ecs().get<world::WorldEnvironment>(env);
    settings.background_mode = "sky";
    settings.fog_enabled = false;
    scene.world.ecs().emplace<world::PanoramaSkyMaterial>(
        env, world::PanoramaSkyMaterial{.panorama = "view.png"});
    contents = {};
    CHECK(draw(contents) == Rgba{0, 0, 0, 255});
    REQUIRE(contents.problems.size() == 1);
    CHECK(contents.problems[0].path == "/components/PanoramaSkyMaterial/panorama");
    const render::Image green{.width = 2, .height = 1, .pixels = {0, 255, 0, 255, 0, 255, 0, 255}};
    scene.files.files["view.png"] = render::encode_png(green).value();
    contents = {};
    CHECK(draw(contents) == Rgba{0, 255, 0, 255});
    CHECK(contents.problems.empty());
}

TEST_CASE("project.json's rendering.rendering_method picks how 3D meshes are lit") {
    json::Value project = parse(R"({"format": 1, "name": "Lamps", "version": "1.0.0",
        "main_scene": "main.scene.json", "rendering": {"rendering_method": "gl_compatibility"}})");
    CHECK(slash::runtime::validate_project(project).empty());
    CHECK(slash::runtime::parse_project(project).value().rendering_method ==
          render::RenderMethod::Compatibility);
    project["rendering"] = json::Object{};
    CHECK(slash::runtime::parse_project(project).value().rendering_method ==
          render::RenderMethod::ForwardPlus);
    project["rendering"]["rendering_method"] = "mobile";
    const auto errors = slash::runtime::validate_project(project);
    REQUIRE(errors.size() == 1);
    CHECK(errors.front().path == "/rendering/rendering_method");
    CHECK(render::parse_render_method("forward_plus") == render::RenderMethod::ForwardPlus);
    CHECK_FALSE(render::parse_render_method("forward").has_value());
}

TEST_CASE("project.json's rendering.lights_and_shadows is read and checked") {
    json::Value project = parse(R"({"format": 1, "name": "Shadows", "version": "1.0.0",
        "main_scene": "main.scene.json",
        "rendering": {"lights_and_shadows": {
            "directional_shadow": {"size": 2048, "soft_shadow_filter_quality": "soft_high"},
            "positional_shadow": {"atlas_size": 8192}}}})");
    CHECK(slash::runtime::validate_project(project).empty());
    const slash::runtime::Project parsed = slash::runtime::parse_project(project).value();
    CHECK(parsed.shadows ==
          render::ShadowSettings{.directional_size = 2048,
                                 .directional_quality = render::ShadowQuality::SoftHigh,
                                 .positional_size = 8192});
    const auto first_error = [&](std::string_view settings) {
        project["rendering"]["lights_and_shadows"] = parse(settings);
        const auto errors = slash::runtime::validate_project(project);
        REQUIRE_FALSE(errors.empty());
        return errors.front().path;
    };
    CHECK(first_error(R"({"directional_shadow": {"size": 1000}})") ==
          "/rendering/lights_and_shadows/directional_shadow/size");
    CHECK(first_error(R"({"positional_shadow": {"atlas_size": 128}})") ==
          "/rendering/lights_and_shadows/positional_shadow/atlas_size");
    CHECK(first_error(R"({"positional_shadow": {"soft_shadow_filter_quality": "soft"}})") ==
          "/rendering/lights_and_shadows/positional_shadow/soft_shadow_filter_quality");
    CHECK(first_error(R"({"omni_shadow": {}})") == "/rendering/lights_and_shadows/omni_shadow");
}

TEST_CASE("project.json's rendering.environment.ssao is read and checked") {
    json::Value project = parse(R"({"format": 1, "name": "Corners", "version": "1.0.0",
        "main_scene": "main.scene.json",
        "rendering": {"environment": {"ssao": {"quality": "ultra", "half_size": false,
            "adaptive_target": 0.75, "blur_passes": 3, "fadeout_from": 10,
            "fadeout_to": 20}}}})");
    CHECK(slash::runtime::validate_project(project).empty());
    CHECK(slash::runtime::parse_project(project).value().ssao ==
          render::SsaoSettings{.quality = render::SsaoQuality::Ultra,
                               .half_size = false,
                               .adaptive_target = 0.75F,
                               .blur_passes = 3,
                               .fadeout_from = 10.0F,
                               .fadeout_to = 20.0F});
    CHECK(render::ssao_settings_from_json(parse(R"({"quality": "extreme"})")) ==
          render::SsaoSettings{});
    project["rendering"]["environment"]["ssao"]["blur_passes"] = 7;
    const auto errors = slash::runtime::validate_project(project);
    REQUIRE(errors.size() == 1);
    CHECK(errors.front().path == "/rendering/environment/ssao/blur_passes");
}

TEST_CASE("project.json's rendering.environment.screen_space_reflection is read and checked") {
    json::Value project = parse(R"({"format": 1, "name": "Mirror", "version": "1.0.0",
        "main_scene": "main.scene.json",
        "rendering": {"environment": {"screen_space_reflection": {"half_size": false}}}})");
    CHECK(slash::runtime::validate_project(project).empty());
    CHECK(slash::runtime::parse_project(project).value().ssr ==
          render::SsrSettings{.half_size = false});
    project["rendering"]["environment"]["screen_space_reflection"]["half_size"] = 1;
    const auto errors = slash::runtime::validate_project(project);
    REQUIRE(errors.size() == 1);
    CHECK(errors.front().path == "/rendering/environment/screen_space_reflection/half_size");
}

TEST_CASE("project.json's rendering.camera.depth_of_field is read and checked") {
    json::Value project = parse(R"({"format": 1, "name": "Focus", "version": "1.0.0",
        "main_scene": "main.scene.json",
        "rendering": {"camera": {"depth_of_field": {"depth_of_field_bokeh_shape": "box",
            "depth_of_field_bokeh_quality": "very_low", "depth_of_field_use_jitter": true}}}})");
    CHECK(slash::runtime::validate_project(project).empty());
    CHECK(slash::runtime::parse_project(project).value().depth_of_field ==
          render::DepthOfFieldSettings{.bokeh_shape = render::BokehShape::Box,
                                       .bokeh_quality = render::BokehQuality::VeryLow,
                                       .use_jitter = true});
    CHECK(render::depth_of_field_settings_from_json(parse(R"({"depth_of_field_bokeh_shape":
        "star"})")) == render::DepthOfFieldSettings{});
    project["rendering"]["camera"]["depth_of_field"]["depth_of_field_bokeh_quality"] = "ultra";
    const auto errors = slash::runtime::validate_project(project);
    REQUIRE(errors.size() == 1);
    CHECK(errors.front().path == "/rendering/camera/depth_of_field/depth_of_field_bokeh_quality");
}
