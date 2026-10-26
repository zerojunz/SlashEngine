#include "devices.hpp"

#include "slash/asset/assets.hpp"
#include "slash/canvas/canvas.hpp"
#include "slash/core/jobs.hpp"
#include "slash/core/json.hpp"
#include "slash/core/math.hpp"
#include "slash/render/device.hpp"
#include "slash/render/image.hpp"
#include "slash/scene3d/particles.hpp"
#include "slash/scene3d/scene3d.hpp"
#include "slash/ui/ui.hpp"
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
#include <map>
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
using slash::Vec2;
using slash::Vec3;
using slash::Vec4;

namespace {

constexpr double kTick = 1.0 / 60.0;

json::Value parse(std::string_view text) {
    auto value = json::parse(text);
    REQUIRE(value.ok());
    return std::move(value).value();
}

// No files: particles here draw primitive meshes.
asset::FileSource no_files() {
    return {
        .read = [](std::string_view /*path*/) -> slash::Result<std::string> {
            return slash::make_error(slash::ErrorCode::NotFound, "no such file");
        },
        .stamp = [](std::string_view /*path*/) -> std::optional<asset::FileStamp> {
            return std::nullopt;
        },
    };
}

// A world with the canvas and 3D systems, the 3D particles among them, and the events it emits.
struct Scene {
    asset::Assets assets{no_files()};
    world::ComponentRegistry components;
    world::World world{components};
    std::vector<world::Event> events;

    Scene() {
        world::register_builtin_components(components);
        canvas::install(world, assets);
        slash::ui::Settings ui;
        ui.viewport = {32.0F, 32.0F};
        slash::ui::set_settings(world, ui);
        world.set_event_listener([this](const world::Event& event) { events.push_back(event); });
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

    // An emitter at a position with a CPUParticles3D whose fields the JSON sets.
    entt::entity add(std::string_view id, Vec3 position, std::string_view fields) {
        const entt::entity entity = world.create(id).value();
        world.ecs().emplace<world::Transform3D>(entity).position = position;
        std::string path;
        REQUIRE(components.find("CPUParticles3D")->assign(world, entity, parse(fields), path).ok());
        return entity;
    }

    void step(int ticks = 1) {
        for (int i = 0; i < ticks; ++i) {
            world.step(kTick);
        }
    }

    [[nodiscard]] std::vector<scene3d::Particle3D> live(entt::entity entity) const {
        return scene3d::particles_3d(world, entity);
    }

    world::CPUParticles3D& emitter(entt::entity entity) {
        return world.ecs().get<world::CPUParticles3D>(entity);
    }

    [[nodiscard]] std::size_t count(std::string_view kind) const {
        return static_cast<std::size_t>(std::ranges::count_if(
            events, [&](const world::Event& event) { return event.kind == kind; }));
    }
};

float length(Vec3 v) {
    return std::sqrt((v.x * v.x) + (v.y * v.y) + (v.z * v.z));
}

float dot(Vec3 a, Vec3 b) {
    return (a.x * b.x) + (a.y * b.y) + (a.z * b.z);
}

bool near(Vec3 a, Vec3 b, float tolerance = 1e-5F) {
    return std::abs(a.x - b.x) <= tolerance && std::abs(a.y - b.y) <= tolerance &&
           std::abs(a.z - b.z) <= tolerance;
}

using Rgba = std::array<std::uint8_t, 4>;

Rgba pixel(const render::Image& image, std::uint32_t x, std::uint32_t y) {
    const std::size_t at = ((std::size_t{y} * image.width) + x) * 4;
    return {image.pixels[at], image.pixels[at + 1], image.pixels[at + 2], image.pixels[at + 3]};
}

} // namespace

TEST_CASE("3D particles emit evenly over their lifetime, move and die at its end") {
    Scene scene;
    const entt::entity fountain =
        scene.add("fountain", {1.0F, 2.0F, 3.0F},
                  R"({"amount": 4, "lifetime": 1, "spread": 0, "gravity": [0, 0, 0],
                      "direction": [0, 1, 0], "initial_velocity_min": 2,
                      "initial_velocity_max": 2})");
    scene.step();
    REQUIRE(scene.live(fountain).size() == 1);
    // A quarter of the cycle in, the second slot emits.
    scene.step(15);
    const std::vector<scene3d::Particle3D> live = scene.live(fountain);
    REQUIRE(live.size() == 2);
    // Straight up from the emitter at 2 meters a second.
    CHECK(near(live[0].velocity, {0.0F, 2.0F, 0.0F}));
    CHECK(live[0].transform.origin.x == doctest::Approx(1.0));
    CHECK(live[0].transform.origin.z == doctest::Approx(3.0));
    CHECK(live[0].transform.origin.y == doctest::Approx(2.0 + (2.0 * live[0].age)).epsilon(1e-4));
    CHECK(live[1].age < live[0].age);
    // A full cycle on, the first has died and been emitted again.
    scene.step(45);
    CHECK(scene.live(fountain).size() == 4);
    for (const scene3d::Particle3D& particle : scene.live(fountain)) {
        CHECK(particle.age < particle.lifetime);
    }
}

TEST_CASE("3D particles spread in a cone about direction, flattened by flatness") {
    Scene scene;
    const entt::entity cone =
        scene.add("cone", {}, R"({"amount": 64, "lifetime": 1, "explosiveness": 1, "spread": 30,
                        "gravity": [0, 0, 0], "initial_velocity_min": 1,
                        "initial_velocity_max": 1})");
    const entt::entity fan =
        scene.add("fan", {}, R"({"amount": 64, "lifetime": 1, "explosiveness": 1, "spread": 30,
                       "flatness": 1, "gravity": [0, 0, 0], "initial_velocity_min": 1,
                       "initial_velocity_max": 1})");
    scene.step();
    float cone_y = 0.0F;
    for (const scene3d::Particle3D& particle : scene.live(cone)) {
        // Within 30 degrees of +x each way across, as Godot's spread.
        const float most = std::cos(30.0F * std::numbers::pi_v<float> / 180.0F);
        CHECK(length(particle.velocity) == doctest::Approx(1.0));
        CHECK(particle.velocity.x >= (most * most) - 1e-5F);
        cone_y = std::max(cone_y, std::abs(particle.velocity.y));
    }
    CHECK(cone_y > 0.1F);
    for (const scene3d::Particle3D& particle : scene.live(fan)) {
        // Flat across y.
        CHECK(std::abs(particle.velocity.y) < 1e-6F);
    }
}

TEST_CASE("3D emission shapes place new particles as Godot's") {
    Scene scene;
    const auto shape = [&](std::string_view id, std::string_view fields) {
        return scene.add(id, {10.0F, 0.0F, 0.0F},
                         std::string(R"({"amount": 48, "lifetime": 1, "explosiveness": 1,
                                         "gravity": [0, 0, 0], )") +
                             std::string(fields) + "}");
    };
    const entt::entity surface =
        shape("surface", R"("emission_shape": "sphere_surface", "emission_sphere_radius": 2)");
    const entt::entity ball = shape("ball", R"("emission_shape": "sphere",
                                               "emission_sphere_radius": 2)");
    const entt::entity box =
        shape("box", R"("emission_shape": "box", "emission_box_extents": [1, 2, 3])");
    const entt::entity ring =
        shape("ring", R"("emission_shape": "ring", "emission_ring_axis": [0, 1, 0],
                         "emission_ring_radius": 2, "emission_ring_inner_radius": 2,
                         "emission_ring_height": 4)");
    const entt::entity cone_ring =
        shape("cone_ring", R"("emission_shape": "ring", "emission_ring_axis": [0, 1, 0],
                              "emission_ring_radius": 2, "emission_ring_inner_radius": 2,
                              "emission_ring_height": 1, "emission_ring_cone_angle": 45)");
    const entt::entity points =
        shape("points", R"("emission_shape": "directed_points", "direction": [0, 0, 1],
                           "spread": 0, "initial_velocity_min": 3, "initial_velocity_max": 3,
                           "emission_points": [[0, 1, 0], [0, -1, 0]],
                           "emission_normals": [[0, 1, 0], [0, -1, 0]],
                           "emission_colors": [[1, 0, 0, 1], [0, 0, 1, 1]])");
    scene.step();
    const Vec3 center{10.0F, 0.0F, 0.0F};
    for (const scene3d::Particle3D& p : scene.live(surface)) {
        CHECK(length(p.transform.origin - center) == doctest::Approx(2.0).epsilon(1e-4));
    }
    float farthest = 0.0F;
    for (const scene3d::Particle3D& p : scene.live(ball)) {
        const float d = length(p.transform.origin - center);
        CHECK(d <= 2.0001F);
        farthest = std::max(farthest, d);
    }
    CHECK(farthest > 1.0F);
    for (const scene3d::Particle3D& p : scene.live(box)) {
        const Vec3 d = p.transform.origin - center;
        CHECK(std::abs(d.x) <= 1.0F);
        CHECK(std::abs(d.y) <= 2.0F);
        CHECK(std::abs(d.z) <= 3.0F);
    }
    for (const scene3d::Particle3D& p : scene.live(ring)) {
        // A cylinder's side round y, 4 meters tall.
        const Vec3 d = p.transform.origin - center;
        CHECK(std::sqrt((d.x * d.x) + (d.z * d.z)) == doctest::Approx(2.0).epsilon(1e-4));
        CHECK(std::abs(d.y) <= 2.0001F);
    }
    for (const scene3d::Particle3D& p : scene.live(cone_ring)) {
        // The side leans in by a meter over its meter of height: 2 meters across at the bottom,
        // 1 at the top.
        const Vec3 d = p.transform.origin - center;
        const float across = std::sqrt((d.x * d.x) + (d.z * d.z));
        CHECK(across == doctest::Approx(2.0 - (d.y + 0.5)).epsilon(1e-3));
    }
    for (const scene3d::Particle3D& p : scene.live(points)) {
        // Each point emits along its normal, tinted by its color.
        const bool up = p.transform.origin.y > 0.0F;
        CHECK(near(p.velocity, {0.0F, up ? 3.0F : -3.0F, 0.0F}));
        CHECK(p.color.x == (up ? 1.0F : 0.0F));
        CHECK(p.color.z == (up ? 0.0F : 1.0F));
    }
}

TEST_CASE("3D particle forces, damping and flags move and turn particles as Godot's") {
    Scene scene;
    const entt::entity falling =
        scene.add("falling", {}, R"({"amount": 1, "lifetime": 10, "explosiveness": 1, "spread": 0,
                           "initial_velocity_min": 1, "initial_velocity_max": 1})");
    const entt::entity damped =
        scene.add("damped", {}, R"({"amount": 1, "lifetime": 10, "explosiveness": 1, "spread": 0,
                          "gravity": [0, 0, 0], "initial_velocity_min": 1,
                          "initial_velocity_max": 1, "damping_min": 0.5,
                          "damping_max": 0.5})");
    const entt::entity aligned =
        scene.add("aligned", {}, R"({"amount": 1, "lifetime": 10, "explosiveness": 1, "spread": 0,
                           "direction": [0, 0, 1], "gravity": [0, 0, 0],
                           "initial_velocity_min": 1, "initial_velocity_max": 1,
                           "particle_flag_align_y": true})");
    const entt::entity flat =
        scene.add("flat", {}, R"({"amount": 1, "lifetime": 10, "explosiveness": 1, "spread": 0,
                        "gravity": [0, 0, 0], "direction": [1, 0, 1],
                        "initial_velocity_min": 1, "initial_velocity_max": 1,
                        "particle_flag_disable_z": true, "angle_min": 90, "angle_max": 90})");
    const entt::entity spinning =
        scene.add("spinning", {}, R"({"amount": 1, "lifetime": 10, "explosiveness": 1,
                            "gravity": [0, 0, 0], "particle_flag_rotate_y": true,
                            "angle_min": 90, "angle_max": 90, "scale_amount_min": 2,
                            "scale_amount_max": 2})");
    scene.step(60);
    const scene3d::Particle3D fell = scene.live(falling).at(0);
    CHECK(fell.velocity.y == doctest::Approx(-9.8).epsilon(1e-3));
    CHECK(fell.velocity.x == doctest::Approx(1.0));
    const scene3d::Particle3D slowed = scene.live(damped).at(0);
    CHECK(length(slowed.velocity) == doctest::Approx(0.5).epsilon(1e-3));
    // Its y axis along its velocity, +z.
    const scene3d::Particle3D along = scene.live(aligned).at(0);
    CHECK(near(along.transform.y, {0.0F, 0.0F, 1.0F}));
    CHECK(std::abs(dot(along.transform.x, along.transform.y)) < 1e-5F);
    // In the xy plane, turned about z by its angle as Godot's columns.
    const scene3d::Particle3D in_plane = scene.live(flat).at(0);
    CHECK(in_plane.transform.origin.z == 0.0F);
    CHECK(in_plane.velocity.z == 0.0F);
    CHECK(near(in_plane.transform.x, {0.0F, -1.0F, 0.0F}));
    // Turned about y and scaled.
    const scene3d::Particle3D turned = scene.live(spinning).at(0);
    CHECK(near(turned.transform.x, {0.0F, 0.0F, -2.0F}));
    CHECK(near(turned.transform.y, {0.0F, 2.0F, 0.0F}));
}

TEST_CASE("3D particles in local coordinates move with their emitter; others stay behind") {
    Scene scene;
    const char* const fields = R"({"amount": 1, "lifetime": 10, "explosiveness": 1,
                                   "gravity": [0, 0, 0]})";
    const entt::entity local =
        scene.add("local", {}, std::string(fields).insert(1, R"("local_coords": true, )"));
    const entt::entity loose = scene.add("loose", {}, fields);
    scene.step();
    for (const entt::entity entity : {local, loose}) {
        scene.world.ecs().get<world::Transform3D>(entity).position = {5.0F, 0.0F, 0.0F};
    }
    scene.step();
    CHECK(scene.live(local).at(0).transform.origin == Vec3{});
    CHECK(scene.live(loose).at(0).transform.origin == Vec3{});
    // particles.eval-style placing: the local one is where its emitter is now.
    const world::Affine3D placed = world::Tree3D(scene.world).global(local);
    CHECK((placed * scene.live(local).at(0).transform).origin.x == 5.0F);
}

TEST_CASE("a 3D one_shot emits one cycle, stops, says it finished and restarts") {
    Scene scene;
    const entt::entity burst = scene.add(
        "burst", {}, R"({"amount": 5, "lifetime": 0.5, "one_shot": true, "explosiveness": 1})");
    scene.step(31);
    CHECK_FALSE(scene.emitter(burst).emitting);
    scene.step(31);
    CHECK(scene.live(burst).empty());
    CHECK(scene.count("finished") == 1);
    scene.emitter(burst).emitting = true;
    scene.step();
    CHECK(scene.live(burst).size() == 5);
}

TEST_CASE("3D particles repeat, step the same on threads, and simulate as the system runs") {
    const auto run = [](int threads) {
        slash::jobs::Pool pool(threads);
        Scene scene;
        scene.world.set_pool(pool);
        std::vector<entt::entity> entities;
        for (int i = 0; i < 8; ++i) {
            entities.push_back(
                scene.add("spray" + std::to_string(i), {static_cast<float>(i), 0.0F, 0.0F},
                          R"({"amount": 40, "lifetime": 0.8, "randomness": 1, "spread": 60,
                    "emission_shape": "ring", "emission_ring_axis": [1, 1, 0],
                    "emission_ring_cone_angle": 60, "initial_velocity_min": 1,
                    "initial_velocity_max": 4, "tangential_accel_min": 2,
                    "tangential_accel_max": 3, "radial_accel_min": -1, "radial_accel_max": 1,
                    "hue_variation_min": -0.2, "hue_variation_max": 0.3,
                    "particle_flag_align_y": true, "scale_amount_min": 0.5})"));
        }
        scene.step(50);
        std::vector<std::string> out;
        for (const entt::entity entity : entities) {
            for (const scene3d::Particle3D& p : scene.live(entity)) {
                const Vec3 o = p.transform.origin;
                out.push_back(std::to_string(o.x) + " " + std::to_string(o.y) + " " +
                              std::to_string(o.z) + " " + std::to_string(p.color.x));
            }
        }
        return out;
    };
    const std::vector<std::string> alone = run(0);
    CHECK_FALSE(alone.empty());
    CHECK(run(3) == alone);

    Scene scene;
    const entt::entity spray = scene.add("spray", {1.0F, 2.0F, 3.0F},
                                         R"({"amount": 12, "lifetime": 1, "preprocess": 0.5,
                                             "spread": 90, "initial_velocity_max": 3})");
    scene.step(20);
    const std::vector<scene3d::Particle3D> ran = scene.live(spray);
    const std::vector<scene3d::Particle3D> simulated =
        scene3d::simulate(scene.emitter(spray), world::Tree3D(scene.world).global(spray),
                          scene3d::particle_seed("spray", 0), 20.0 * kTick, kTick);
    REQUIRE(ran.size() == simulated.size());
    for (std::size_t i = 0; i < ran.size(); ++i) {
        CHECK(ran[i].transform.origin == simulated[i].transform.origin);
    }
}

TEST_CASE("billboards face the camera as Godot's billboard modes") {
    world::Affine3D camera;
    camera.x = {0.0F, 0.0F, -1.0F}; // a camera on the +x side, looking back toward -x
    camera.z = {1.0F, 0.0F, 0.0F};
    camera.origin = {10.0F, 0.0F, 0.0F};
    world::Affine3D model;
    model.x = {2.0F, 0.0F, 0.0F};
    model.y = {0.0F, 3.0F, 0.0F};
    model.origin = {1.0F, 2.0F, 3.0F};

    const world::Affine3D facing =
        scene3d::billboard(model, camera, scene3d::Billboard::Enabled, false, 0.0F);
    CHECK(facing.x == camera.x);
    CHECK(facing.z == camera.z);
    CHECK(facing.origin == model.origin);
    const world::Affine3D kept =
        scene3d::billboard(model, camera, scene3d::Billboard::Enabled, true, 0.0F);
    CHECK(near(kept.x, {0.0F, 0.0F, -2.0F}));
    CHECK(near(kept.y, {0.0F, 3.0F, 0.0F}));

    world::Affine3D tilted = camera;
    tilted.y = {0.6F, 0.8F, 0.0F};
    tilted.z = {0.8F, -0.6F, 0.0F};
    const world::Affine3D upright =
        scene3d::billboard(model, tilted, scene3d::Billboard::FixedY, false, 0.0F);
    CHECK(upright.y == Vec3{0.0F, 1.0F, 0.0F});
    CHECK(near(upright.x, {0.0F, 0.0F, -1.0F}));

    // Particles turn about the view by their angle: a quarter turn puts x along the camera's -y.
    const world::Affine3D particle = scene3d::billboard(
        model, camera, scene3d::Billboard::Particles, false, std::numbers::pi_v<float> / 2.0F);
    CHECK(near(particle.x, {0.0F, -1.0F, 0.0F}));
    CHECK(near(particle.y, {0.0F, 0.0F, -1.0F}));

    CHECK(scene3d::billboard_of("particles") == scene3d::Billboard::Particles);
    CHECK(scene3d::billboard_of("sideways") == scene3d::Billboard::Disabled);
    CHECK(scene3d::billboard(model, camera, scene3d::Billboard::Disabled, true, 1.0F) == model);
}

TEST_CASE("particle frames move texture coordinates as Godot's particle animation") {
    // 2 by 2 frames: halfway through is the third frame, bottom left.
    CHECK(scene3d::particle_frame_uv({1.0F, 1.0F}, 0.5F, 2, 2, false) == Vec2{0.5F, 1.0F});
    CHECK(scene3d::particle_frame_uv({0.0F, 0.0F}, 0.75F, 2, 2, false) == Vec2{0.5F, 0.5F});
    // Past the end it stays on the last, or loops round.
    CHECK(scene3d::particle_frame_uv({0.0F, 0.0F}, 1.5F, 2, 2, false) == Vec2{0.5F, 0.5F});
    CHECK(scene3d::particle_frame_uv({0.0F, 0.0F}, 1.25F, 2, 2, true) == Vec2{0.5F, 0.0F});
}

TEST_CASE("3D particles draw their mesh at each particle, colored and facing the camera") {
    Scene scene;
    scene.create("camera", R"({"Transform3D": {"position": [0, 0, 4]}, "Camera3D": {}})");
    // Unshaded quads turned edge on by their emitters, which billboards turn back to the camera.
    const auto sparks = [&](std::string_view id, std::string_view point, std::string_view color) {
        return scene.create(id, std::string(R"({"Transform3D": {"rotation": [0, 1.5707964, 0]},
                      "CPUParticles3D": {"amount": 1, "lifetime": 10, "explosiveness": 1,
                                         "gravity": [0, 0, 0], "emission_shape": "points",
                                         "emission_points": [)") +
                                    std::string(point) + R"(], "emission_colors": [)" +
                                    std::string(color) +
                                    R"(]},
                      "QuadMesh": {"size": [0.5, 0.5]},
                      "StandardMaterial3D": {"shading_mode": "unshaded",
                                             "vertex_color_use_as_albedo": true,
                                             "billboard_mode": "particles"}})");
    };
    const entt::entity red = sparks("red", "[-1, 0, 0]", "[1, 0, 0, 1]");
    const entt::entity blue = sparks("blue", "[1, 0, 0]", "[0, 0, 1, 1]");
    scene.step();
    REQUIRE(scene.live(red).size() == 1);
    CHECK(scene.live(red).at(0).transform.origin.z == doctest::Approx(1.0));
    CHECK(scene.live(blue).at(0).transform.origin.z == doctest::Approx(-1.0));
    CHECK(scene3d::check_meshes(scene.world).empty());

    const auto device = render::create_software_device();
    canvas::CanvasRenderer renderer(*device, scene.assets);
    auto target = device->create_texture({.width = 32, .height = 32, .render_target = true});
    REQUIRE(target.ok());
    canvas::CanvasOptions options;
    options.target = target.value();
    options.width = 32.0F;
    options.height = 32.0F;
    REQUIRE(renderer.draw(scene.world, options).ok());
    CHECK(renderer.meshes_drawn() == 2);
    const auto drawn = device->read_texture(target.value());
    REQUIRE(drawn.ok());
    // The emitter turned the points onto the z axis: one in front of the other, the nearer
    // (red, at z = 1) drawn over the blue one in the middle of the frame.
    const Rgba middle = pixel(drawn.value(), 16, 16);
    CHECK(middle[0] > 200);
    CHECK(middle[2] < 50);
    device->destroy_texture(target.value());

    // Without the billboard, the quads stay edge on to the camera and show nothing.
    for (const entt::entity entity : {red, blue}) {
        scene.world.ecs().get<world::StandardMaterial3D>(entity).billboard_mode = "disabled";
    }
    auto edge_target = device->create_texture({.width = 32, .height = 32, .render_target = true});
    REQUIRE(edge_target.ok());
    options.target = edge_target.value();
    REQUIRE(renderer.draw(scene.world, options).ok());
    const auto edge = device->read_texture(edge_target.value());
    REQUIRE(edge.ok());
    CHECK(pixel(edge.value(), 16, 16) != middle);
    device->destroy_texture(edge_target.value());
}

TEST_CASE("CPUParticles3D without a mesh is reported") {
    Scene scene;
    scene.create("empty", R"({"Transform3D": {}, "CPUParticles3D": {}})");
    scene.create("model", R"({"Transform3D": {}, "CPUParticles3D": {"mesh": "rock.png"}})");
    const auto problems = scene3d::check_meshes(scene.world);
    REQUIRE(problems.size() == 2);
    CHECK(problems[0].path == "/components/CPUParticles3D/mesh");
    CHECK(problems[0].message.starts_with("has no mesh to draw at its particles"));
    CHECK(problems[1].message == "expected a .gltf, .glb, .obj, .fbx, .blend or .model.json file");
}
