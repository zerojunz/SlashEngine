#include "slash/asset/assets.hpp"
#include "slash/canvas/canvas.hpp"
#include "slash/canvas/gpu_particles.hpp"
#include "slash/core/json.hpp"
#include "slash/core/math.hpp"
#include "slash/render/device.hpp"
#include "slash/render/image.hpp"
#include "slash/render/particles3d.hpp"
#include "slash/scene3d/gpu_particles.hpp"
#include "slash/scene3d/scene3d.hpp"
#include "slash/ui/ui.hpp"
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
#include <optional>
#include <span>
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
using slash::Vec4;

namespace {

constexpr double kTick = 1.0 / 60.0;

json::Value parse(std::string_view text) {
    auto value = json::parse(text);
    REQUIRE(value.ok());
    return std::move(value).value();
}

render::ParticleProcess3D process_3d(std::string_view text) {
    auto parsed = canvas::parse_process_material_3d(parse(text));
    REQUIRE_MESSAGE(parsed.ok(), (parsed.ok() ? "" : parsed.error().message));
    return std::move(parsed).value();
}

Vec3 origin(const render::Particle3D& particle) {
    return {particle.origin.x, particle.origin.y, particle.origin.z};
}

float length(Vec3 v) {
    return std::sqrt((v.x * v.x) + (v.y * v.y) + (v.z * v.z));
}

// Steps of a system emitting from the origin, every particle at once (explosiveness 1).
std::vector<render::ParticleStep3D> burst(int count, float delta, float lifetime = 1.0F) {
    std::vector<render::ParticleStep3D> steps;
    double phase = 0.0;
    for (int i = 0; i < count; ++i) {
        render::ParticleStep3D step;
        step.prev_system_phase = static_cast<float>(phase);
        phase = std::fmod(phase + (delta / lifetime), 1.0);
        step.system_phase = static_cast<float>(phase);
        step.delta = delta;
        step.lifetime = lifetime;
        step.explosiveness = 1.0F;
        step.time = delta * static_cast<float>(i + 1);
        step.seed = 7;
        step.clear = i == 0;
        steps.push_back(step);
    }
    return steps;
}

// A world with the canvas and 3D systems over files kept in memory.
struct Scene {
    std::map<std::string, std::string, std::less<>> files;
    asset::Assets assets{asset::FileSource{
        .read = [this](std::string_view path) -> slash::Result<std::string> {
            const auto found = files.find(path);
            if (found == files.end()) {
                return slash::make_error(slash::ErrorCode::NotFound, "no such file");
            }
            return found->second;
        },
        .stamp = [](std::string_view /*path*/) -> std::optional<asset::FileStamp> {
            return std::nullopt;
        },
    }};
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

    void step(int ticks = 1) {
        for (int i = 0; i < ticks; ++i) {
            world.step(kTick);
        }
    }

    [[nodiscard]] std::vector<render::Particle3D> live(entt::entity entity) {
        const auto& emitter = world.ecs().get<world::GPUParticles3D>(entity);
        render::ParticleProcess3D process;
        if (!emitter.process_material.empty()) {
            auto loaded = assets.load<canvas::ProcessMaterial>(emitter.process_material);
            REQUIRE(loaded.ok());
            process = loaded.value()->space;
        }
        return scene3d::gpu_particles_3d(world, entity, process);
    }

    [[nodiscard]] std::size_t count(std::string_view kind) const {
        return static_cast<std::size_t>(std::ranges::count_if(
            events, [&](const world::Event& event) { return event.kind == kind; }));
    }
};

using Rgba = std::array<std::uint8_t, 4>;

Rgba pixel(const render::Image& image, std::uint32_t x, std::uint32_t y) {
    const std::size_t at = ((std::size_t{y} * image.width) + x) * 4;
    return {image.pixels[at], image.pixels[at + 1], image.pixels[at + 2], image.pixels[at + 3]};
}

} // namespace

TEST_CASE("a process material reads in 3D: vectors with z and curves per axis") {
    const render::ParticleProcess3D defaults = process_3d(R"({"format": 1})");
    CHECK(defaults == render::ParticleProcess3D{});
    CHECK(defaults.gravity.y == doctest::Approx(-9.8));

    const render::ParticleProcess3D read = process_3d(R"({
        "format": 1,
        "direction": [0, 1, 0],
        "gravity": [0, 0],
        "emission_box_extents": [2, 3],
        "emission_shape": "points",
        "emission_points": [[1, 2, 3], [4, 5]],
        "emission_ring_axis": [0, 1, 0],
        "emission_ring_cone_angle": 45,
        "flatness": 0.5,
        "particle_flag_rotate_y": true,
        "particle_flag_disable_z": true,
        "directional_velocity_min": 2,
        "directional_velocity_max": 3,
        "directional_velocity_curve": {"x": [[0, 1]], "y": [[0, 0]], "z": [[0, -1], [1, 1]]},
        "scale_curve": [[0, 1], [1, 0]],
        "scale_over_velocity_max": 5,
        "scale_over_velocity_curve": {"x": [[0, 1]], "y": [[0, 2]], "z": [[0, 3]]}
    })");
    CHECK(read.direction == Vec3{0.0F, 1.0F, 0.0F});
    CHECK(read.gravity == Vec3{0.0F, 0.0F, 0.0F});
    // A vector of two keeps the 3D default's z.
    CHECK(read.emission_box_extents == Vec3{2.0F, 3.0F, 1.0F});
    REQUIRE(read.emission_points.size() == 2);
    CHECK(read.emission_points[0] == Vec3{1.0F, 2.0F, 3.0F});
    CHECK(read.emission_points[1] == Vec3{4.0F, 5.0F, 0.0F});
    CHECK(read.emission_ring_axis == Vec3{0.0F, 1.0F, 0.0F});
    CHECK(read.emission_ring_cone_angle == 45.0F);
    CHECK(read.flatness == 0.5F);
    CHECK(read.rotate_y);
    CHECK(read.disable_z);
    CHECK(read.directional_velocity_min == 2.0F);
    CHECK(read.directional_velocity_curve.axes[2].size() == 2);
    // One curve serves every axis, as Godot's CurveTexture.
    CHECK(read.scale_curve.axes[0] == read.scale_curve.axes[2]);
    CHECK(read.scale_over_velocity_curve.axes[1].at(0).y == 2.0F);

    // 2D takes an XYZ curve's x.
    auto flat = canvas::parse_process_material(
        parse(R"({"format": 1, "scale_curve": {"x": [[0, 2]], "y": [[0, 3]], "z": [[0, 4]]}})"));
    REQUIRE(flat.ok());
    CHECK(flat.value()
              .param_curve.at(static_cast<std::size_t>(render::ParticleParam::Scale))
              .at(0)
              .y == 2.0F);

    // Each axis's offsets go in order, and vectors take two or three numbers.
    const auto unordered = canvas::validate_process_material(parse(R"({
        "format": 1, "orbit_velocity_curve": {"x": [], "y": [[0.5, 1], [0.25, 1]], "z": []}
    })"));
    REQUIRE(unordered.size() == 1);
    CHECK(unordered[0].path == "/orbit_velocity_curve/y/1/0");
    const auto too_long =
        canvas::validate_process_material(parse(R"({"format": 1, "direction": [1, 2, 3, 4]})"));
    REQUIRE(too_long.size() == 1);
    CHECK(too_long[0].path == "/direction");
}

TEST_CASE("3D particles start along direction and fall with gravity, as Godot's") {
    const render::ParticleProcess3D process = process_3d(R"({
        "format": 1, "direction": [0, 0, 1], "spread": 0,
        "initial_velocity_min": 2, "initial_velocity_max": 2, "gravity": [0, -10, 0]
    })");
    std::vector<render::Particle3D> particles(4);
    const auto steps = burst(30, 1.0F / 60.0F);
    render::process_particles_3d(particles, process, steps);
    for (const render::Particle3D& particle : particles) {
        REQUIRE(particle.active());
        // Half a second at 2 m/s along z, falling under gravity.
        CHECK(particle.origin.z == doctest::Approx(1.0).epsilon(0.05));
        CHECK(particle.origin.y < -0.5F);
        CHECK(particle.velocity.y == doctest::Approx(-5.0).epsilon(0.05));
    }
    // The same steps make the same particles.
    std::vector<render::Particle3D> again(4);
    render::process_particles_3d(again, process, steps);
    CHECK(std::ranges::equal(particles, again, [](const auto& a, const auto& b) {
        return origin(a) == origin(b) && a.velocity == b.velocity;
    }));
    // They die at the end of their life.
    // With emission stopped, so the next cycle starts none.
    auto later = burst(70, 1.0F / 60.0F);
    for (render::ParticleStep3D& step : later) {
        step.emitting = false;
    }
    render::process_particles_3d(particles, process, std::span(later).subspan(30));
    CHECK(std::ranges::none_of(particles, [](const auto& p) { return p.active(); }));
}

TEST_CASE("3D emission shapes and disable_z place particles as Godot's") {
    SUBCASE("a sphere") {
        std::vector<render::Particle3D> particles(64);
        render::process_particles_3d(
            particles,
            process_3d(R"({"format": 1, "emission_shape": "sphere", "emission_sphere_radius": 2,
                           "gravity": [0, 0, 0]})"),
            burst(1, 1.0F / 60.0F));
        for (const render::Particle3D& particle : particles) {
            CHECK(length(origin(particle)) <= 2.0F + 1e-4F);
        }
    }
    SUBCASE("a ring about y") {
        std::vector<render::Particle3D> particles(64);
        render::process_particles_3d(
            particles,
            process_3d(R"({"format": 1, "emission_shape": "ring", "emission_ring_axis": [0, 1, 0],
                           "emission_ring_radius": 3, "emission_ring_inner_radius": 2,
                           "emission_ring_height": 0, "gravity": [0, 0, 0]})"),
            burst(1, 1.0F / 60.0F));
        for (const render::Particle3D& particle : particles) {
            const Vec3 p = origin(particle);
            const float r = std::sqrt((p.x * p.x) + (p.z * p.z));
            CHECK(r >= 2.0F - 1e-4F);
            CHECK(r <= 3.0F + 1e-4F);
            CHECK(std::abs(p.y) < 1e-4F);
        }
    }
    SUBCASE("disable_z keeps particles in the xy plane") {
        std::vector<render::Particle3D> particles(32);
        render::process_particles_3d(
            particles,
            process_3d(R"({"format": 1, "emission_shape": "box", "emission_box_extents": [1, 1, 1],
                           "spread": 180, "initial_velocity_min": 1, "initial_velocity_max": 2,
                           "particle_flag_disable_z": true})"),
            burst(20, 1.0F / 60.0F));
        for (const render::Particle3D& particle : particles) {
            CHECK(particle.origin.z == 0.0F);
            CHECK(particle.velocity.z == 0.0F);
        }
    }
}

TEST_CASE("a 3D particle system runs and fills meshes on every device as on the CPU") {
    const render::ParticleProcess3D process = process_3d(R"({
        "format": 1, "direction": [1, 0, 0], "spread": 30,
        "initial_velocity_min": 1, "initial_velocity_max": 3, "color": [1, 0.5, 0.25, 1]
    })");
    const auto steps = burst(10, 1.0F / 60.0F);
    std::vector<render::Particle3D> expected(8);
    render::process_particles_3d(expected, process, steps);

    const auto device = render::create_software_device();
    auto made = device->create_particles_3d(8);
    REQUIRE(made.ok());
    REQUIRE(device->process_particles_3d(made.value(), process, steps).ok());
    const auto read = device->read_particles_3d(made.value());
    REQUIRE(read.ok());
    REQUIRE(read.value().size() == 8);
    for (std::size_t i = 0; i < 8; ++i) {
        CHECK(origin(read.value()[i]) == origin(expected[i]));
    }

    // A one-corner draw pass lands on each particle, tinted by its color.
    render::Vertex3D corner;
    corner.rgba = {255, 255, 255, 255};
    const std::vector<render::Vertex3D> source(3, corner);
    std::vector<render::Vertex3D> blank(3 * 8);
    auto mesh = device->create_mesh(blank, {});
    REQUIRE(mesh.ok());
    REQUIRE(device->fill_particle_mesh(made.value(), mesh.value(), source, {}).ok());
    std::vector<render::Vertex3D> placed(3 * 8);
    render::particle_mesh_vertices_3d(expected, source, {}, placed);
    CHECK(placed[0].position[0] == expected[0].origin.x);
    CHECK(placed[3].position[1] == expected[1].origin.y);
    CHECK(placed[0].rgba[0] == 255);
    CHECK(std::abs(placed[0].rgba[1] - 128) <= 1);
    CHECK(std::abs(placed[0].rgba[2] - 64) <= 1);
    // A mesh of the wrong size is refused.
    auto small = device->create_mesh(std::vector<render::Vertex3D>(3), {});
    REQUIRE(small.ok());
    CHECK_FALSE(device->fill_particle_mesh(made.value(), small.value(), source, {}).ok());
    device->destroy_particles_3d(made.value());
    CHECK_FALSE(device->read_particles_3d(made.value()).ok());

    // The null device checks the same calls and runs nothing.
    const auto null_device = render::create_null_device();
    auto none = null_device->create_particles_3d(8);
    REQUIRE(none.ok());
    REQUIRE(null_device->process_particles_3d(none.value(), process, steps).ok());
    const auto still = null_device->read_particles_3d(none.value());
    REQUIRE(still.ok());
    CHECK(std::ranges::none_of(still.value(), [](const auto& p) { return p.active(); }));
    CHECK_FALSE(null_device->create_particles_3d(0).ok());
}

TEST_CASE("3D particles draw in lifetime and view depth order, aligned as Godot's") {
    std::vector<render::Particle3D> particles(3);
    for (std::size_t i = 0; i < particles.size(); ++i) {
        particles[i].flags = render::kParticleActive;
        particles[i].origin = {0.0F, 0.0F, static_cast<float>(i), 0.0F};
        particles[i].velocity = {0.0F, 0.0F, 1.0F};
    }
    render::Vertex3D corner;
    const std::vector<render::Vertex3D> source{corner};
    std::vector<render::Vertex3D> out(3);
    render::ParticleMeshDraw3D look;
    look.order = render::ParticleOrder3D::ViewDepth;
    look.sort_direction = {0.0F, 0.0F, 1.0F}; // the camera looks down -z from +z
    render::particle_mesh_vertices_3d(particles, source, look, out);
    // The farthest from the camera (lowest z) first.
    CHECK(out[0].position[2] == 0.0F);
    CHECK(out[2].position[2] == 2.0F);
    look.sort_direction = {0.0F, 0.0F, -1.0F};
    render::particle_mesh_vertices_3d(particles, source, look, out);
    CHECK(out[0].position[2] == 2.0F);

    // Lifetime order starts at the oldest slot, after the split.
    CHECK(render::particle_draw_index_3d(0, 3, render::ParticleOrder3D::Lifetime, 1) == 1);
    CHECK(render::particle_draw_index_3d(2, 3, render::ParticleOrder3D::Lifetime, 1) == 0);
    CHECK(render::particle_draw_index_3d(0, 3, render::ParticleOrder3D::ReverseLifetime, 1) == 0);

    // y_to_velocity turns each particle's y axis along its velocity.
    render::Vertex3D up;
    up.position = {0.0F, 1.0F, 0.0F};
    std::vector<render::Vertex3D> turned(3);
    render::ParticleMeshDraw3D aligned;
    aligned.align = render::ParticleAlign::YToVelocity;
    render::particle_mesh_vertices_3d(particles, std::vector<render::Vertex3D>{up}, aligned,
                                      turned);
    CHECK(turned[0].position[2] == doctest::Approx(1.0));
    CHECK(turned[0].position[1] == doctest::Approx(0.0));
    // remainder moves them on along their velocity.
    aligned.align = render::ParticleAlign::Disabled;
    aligned.remainder = 0.5F;
    render::particle_mesh_vertices_3d(particles, source, aligned, turned);
    CHECK(turned[0].position[2] == doctest::Approx(0.5));
}

TEST_CASE("GPUParticles3D steps its emission each tick and a one_shot finishes as Godot's") {
    Scene scene;
    scene.files["spark.process_material.json"] = R"({"format": 1, "gravity": [0, 0, 0],
        "direction": [0, 1, 0], "spread": 0, "initial_velocity_min": 1, "initial_velocity_max": 1})";
    const entt::entity fountain =
        scene.create("fountain", R"({"Transform3D": {"position": [1, 0, 0]},
        "GPUParticles3D": {"amount": 6, "lifetime": 1, "process_material": "spark.process_material.json"},
        "QuadMesh": {}})");
    scene.step();
    REQUIRE(scene3d::particle_steps_3d(scene.world, fountain).has_value());
    scene.step(59);
    // A lifetime on, every slot has emitted, from where the emitter is, moving up.
    const auto live = scene.live(fountain);
    CHECK(live.size() == 6);
    for (const render::Particle3D& particle : live) {
        CHECK(particle.origin.x == doctest::Approx(1.0));
        CHECK(particle.velocity.y == doctest::Approx(1.0));
    }
    // simulate runs the same settings apart from the world.
    const auto& settings = scene.world.ecs().get<world::GPUParticles3D>(fountain);
    auto loaded = scene.assets.load<canvas::ProcessMaterial>(settings.process_material);
    REQUIRE(loaded.ok());
    const auto simulated = scene3d::simulate_gpu(settings, {.origin = {1.0F, 0.0F, 0.0F}},
                                                 loaded.value()->space, "fountain", 1.0, kTick);
    REQUIRE(simulated.size() == live.size());
    for (std::size_t i = 0; i < live.size(); ++i) {
        CHECK(origin(simulated[i]) == origin(live[i]));
    }

    const entt::entity burst_emitter = scene.create("burst", R"({"Transform3D": {},
        "GPUParticles3D": {"amount": 4, "lifetime": 0.5, "one_shot": true},
        "QuadMesh": {}})");
    scene.step(31);
    CHECK_FALSE(scene.world.ecs().get<world::GPUParticles3D>(burst_emitter).emitting);
    CHECK(scene.count("finished") == 0);
    // lifetime times 2 - explosiveness after it started.
    scene.step(30);
    CHECK(scene.count("finished") == 1);
    CHECK(scene3d::check_meshes(scene.world).empty());
}

TEST_CASE("GPUParticles3D draws its draw passes at its particles, culled by its visibility box") {
    Scene scene;
    scene.files["still.process_material.json"] =
        R"({"format": 1, "gravity": [0, 0], "emission_shape": "points",
            "emission_points": [[0, 0, 0]], "color": [1, 0, 0, 1]})";
    scene.create("camera", R"({"Transform3D": {"position": [0, 0, 4]}, "Camera3D": {}})");
    const entt::entity emitter = scene.create("dots", R"({"Transform3D": {},
        "GPUParticles3D": {"amount": 1, "lifetime": 10, "explosiveness": 1,
                           "process_material": "still.process_material.json"},
        "QuadMesh": {"size": [1, 1]},
        "StandardMaterial3D": {"shading_mode": "unshaded", "vertex_color_use_as_albedo": true,
                               "billboard_mode": "particles"}})");
    scene.step();
    CHECK(scene3d::check_meshes(scene.world).empty());

    const auto device = render::create_software_device();
    canvas::CanvasRenderer renderer(*device, scene.assets);
    const auto draw = [&]() {
        auto target = device->create_texture({.width = 32, .height = 32, .render_target = true});
        REQUIRE(target.ok());
        canvas::CanvasOptions options;
        options.target = target.value();
        options.width = 32.0F;
        options.height = 32.0F;
        REQUIRE(renderer.draw(scene.world, options).ok());
        auto image = device->read_texture(target.value());
        REQUIRE(image.ok());
        device->destroy_texture(target.value());
        return std::move(image).value();
    };
    const render::Image drawn = draw();
    CHECK(renderer.meshes_drawn() == 1);
    const Rgba middle = pixel(drawn, 16, 16);
    CHECK(middle[0] > 200);
    CHECK(middle[1] < 50);

    // A second pass with no mesh draws nothing more.
    auto& settings = scene.world.ecs().get<world::GPUParticles3D>(emitter);
    settings.draw_passes = 2;
    (void)draw();
    CHECK(renderer.meshes_drawn() == 1);

    // Out of view, its box draws nothing.
    settings.visibility_aabb_position = {-4.0F, -4.0F, 10.0F};
    const render::Image culled = draw();
    CHECK(renderer.meshes_drawn() == 0);
    CHECK(pixel(culled, 16, 16) != middle);
}

TEST_CASE("GPUParticles3D draw passes without a mesh are reported") {
    Scene scene;
    scene.create("empty", R"({"Transform3D": {}, "GPUParticles3D": {}})");
    scene.create("model", R"({"Transform3D": {}, "GPUParticles3D": {"draw_passes": 2,
        "draw_pass_2": "rock.png"}, "QuadMesh": {}})");
    const auto problems = scene3d::check_meshes(scene.world);
    REQUIRE(problems.size() == 2);
    CHECK(problems[0].path == "/components/GPUParticles3D/draw_pass_1");
    CHECK(problems[0].message.starts_with("has no mesh to draw at its particles"));
    CHECK(problems[1].path == "/components/GPUParticles3D/draw_pass_2");
}
