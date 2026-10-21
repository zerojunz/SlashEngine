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
    CHECK(problems[0].message.starts_with("has no mesh: add a BoxMesh"));
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
        REQUIRE(drawn.ok());
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
