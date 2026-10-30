#include "slash/asset/assets.hpp"
#include "slash/canvas/canvas.hpp"
#include "slash/canvas/gpu_particles.hpp"
#include "slash/core/jobs.hpp"
#include "slash/core/json.hpp"
#include "slash/core/math.hpp"
#include "slash/render/device.hpp"
#include "slash/render/image.hpp"
#include "slash/render/particles3d.hpp"
#include "slash/scene3d/gpu_particles.hpp"
#include "slash/scene3d/particle_collision.hpp"
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

TEST_CASE("GPUParticles3D pairs with the attractors its box touches, as Godot's scene cull") {
    Scene scene;
    scene.files["still.process_material.json"] =
        R"({"format": 1, "gravity": [0, 0, 0], "emission_shape": "points",
            "emission_points": [[1, 0, 0]]})";
    const entt::entity emitter = scene.create("dust", R"({"Transform3D": {},
        "GPUParticles3D": {"amount": 1, "lifetime": 10, "explosiveness": 1,
                           "process_material": "still.process_material.json"},
        "QuadMesh": {}})");
    scene.create("pull", R"({"Transform3D": {}, "GPUParticlesAttractorSphere3D": {"radius": 2}})");
    // Another layer, too far and hidden: none of these act on it.
    scene.create("masked",
                 R"({"Transform3D": {}, "GPUParticlesAttractorBox3D": {"cull_mask": 2}})");
    scene.create(
        "far", R"({"Transform3D": {"position": [20, 0, 0]}, "GPUParticlesAttractorSphere3D": {}})");
    scene.create("hidden",
                 R"({"Transform3D": {"visible": false}, "GPUParticlesAttractorBox3D": {}})");
    // Scaled twice, its radius is twice as large; its bounds just reach into the box.
    scene.create("scaled", R"({"Transform3D": {"position": [0, 5.5, 0], "scale": [2, 2, 2]},
        "GPUParticlesAttractorSphere3D": {"strength": 0}})");
    scene.step();
    auto kept = scene3d::particle_steps_3d(scene.world, emitter);
    REQUIRE(kept.has_value());
    // In the order entities are stored; here by height.
    std::vector<render::ParticleAttractor3D> attractors = kept->steps.back().attractors;
    std::ranges::sort(attractors, {},
                      [](const render::ParticleAttractor3D& a) { return a.transform[10]; });
    REQUIRE(attractors.size() == 2);
    CHECK(attractors[0].shape == render::AttractorShape3D::Sphere);
    CHECK(attractors[0].extents.x == doctest::Approx(2.0));
    CHECK(attractors[1].extents.x == doctest::Approx(2.0));
    CHECK(attractors[1].transform[10] == doctest::Approx(5.5));
    // Its basis is made orthonormal.
    CHECK(attractors[1].transform[0] == doctest::Approx(1.0));

    // The sphere pulls the particle in toward its center.
    scene.step(30);
    auto live = scene.live(emitter);
    REQUIRE(live.size() == 1);
    CHECK(live[0].velocity.x < -0.1F);
    CHECK(live[0].origin.x < 1.0F);
    CHECK(live[0].origin.y == doctest::Approx(0.0));

    // With local_coords the attractors are in the emitter's space.
    auto& settings = scene.world.ecs().get<world::GPUParticles3D>(emitter);
    settings.local_coords = true;
    scene.world.ecs().get<world::Transform3D>(emitter).position = {1.0F, 0.0F, 0.0F};
    scene.step();
    kept = scene3d::particle_steps_3d(scene.world, emitter);
    REQUIRE(kept.has_value());
    REQUIRE(kept->steps.back().attractors.size() == 2);
    for (const render::ParticleAttractor3D& attractor : kept->steps.back().attractors) {
        CHECK(attractor.transform[9] == doctest::Approx(-1.0));
    }

    // Without attractor_interaction_enabled the particles go on as they were.
    scene.files["deaf.process_material.json"] =
        R"({"format": 1, "gravity": [0, 0, 0], "emission_shape": "points",
            "emission_points": [[1, 0, 0]], "attractor_interaction_enabled": false})";
    const entt::entity deaf = scene.create("deaf", R"({"Transform3D": {},
        "GPUParticles3D": {"amount": 1, "lifetime": 10, "explosiveness": 1,
                           "process_material": "deaf.process_material.json"},
        "QuadMesh": {}})");
    scene.step(30);
    live = scene.live(deaf);
    REQUIRE(live.size() == 1);
    CHECK(live[0].velocity.x == 0.0F);
    CHECK(live[0].origin.x == doctest::Approx(1.0));
}

TEST_CASE("GPUParticles3D falls onto a collision box and bounces off a sphere, as Godot's") {
    Scene scene;
    scene.files["drop.process_material.json"] =
        R"({"format": 1, "gravity": [0, -9.8, 0], "emission_shape": "points",
            "emission_points": [[0, 2, 0]], "collision_mode": "rigid", "collision_bounce": 0.5})";
    const entt::entity emitter = scene.create("drops", R"({"Transform3D": {},
        "GPUParticles3D": {"amount": 1, "lifetime": 10, "explosiveness": 1,
                           "collision_base_size": 0.1,
                           "process_material": "drop.process_material.json"},
        "QuadMesh": {}})");
    scene.create("floor",
                 R"({"Transform3D": {"position": [0, -1, 0]},
                     "GPUParticlesCollisionBox3D": {"size": [6, 2, 6]}})");
    // Another layer, too far and hidden: none of these collide with it.
    scene.create("masked",
                 R"({"Transform3D": {}, "GPUParticlesCollisionSphere3D": {"cull_mask": 2}})");
    scene.create(
        "far", R"({"Transform3D": {"position": [20, 0, 0]}, "GPUParticlesCollisionSphere3D": {}})");
    scene.create("hidden",
                 R"({"Transform3D": {"visible": false}, "GPUParticlesCollisionBox3D": {}})");
    scene.step();
    auto kept = scene3d::particle_steps_3d(scene.world, emitter);
    REQUIRE(kept.has_value());
    const render::ParticleStep3D& step = kept->steps.back();
    REQUIRE(step.colliders.size() == 1);
    CHECK(step.colliders[0].shape == render::ColliderShape3D::Box);
    CHECK(step.colliders[0].extents.x == doctest::Approx(3.0));
    CHECK(step.colliders[0].transform[10] == doctest::Approx(-1.0));
    CHECK(step.collision_size == doctest::Approx(0.1));

    // The drop falls, bounces and comes to rest on the floor's top, its size above it.
    float highest_after_bounce = -1.0F;
    bool bounced = false;
    for (int i = 0; i < 120; ++i) {
        scene.step();
        auto live = scene.live(emitter);
        REQUIRE(live.size() == 1);
        if (live[0].velocity.y > 0.0F) {
            bounced = true;
        }
        if (bounced) {
            highest_after_bounce = std::max(highest_after_bounce, live[0].origin.y);
        }
    }
    CHECK(bounced);
    CHECK(highest_after_bounce > 0.2F);
    CHECK(highest_after_bounce < 2.0F);
    auto live = scene.live(emitter);
    REQUIRE(live.size() == 1);
    CHECK(live[0].origin.y == doctest::Approx(0.1).epsilon(0.05));
    CHECK(live[0].origin.x == doctest::Approx(0.0));

    // A ball of hide_on_contact hides what it touches.
    scene.files["hide.process_material.json"] =
        R"({"format": 1, "gravity": [0, -9.8, 0], "emission_shape": "points",
            "emission_points": [[4, 2, 0]], "collision_mode": "hide_on_contact"})";
    const entt::entity hiding = scene.create("hiding", R"({"Transform3D": {},
        "GPUParticles3D": {"amount": 1, "lifetime": 10, "explosiveness": 1,
                           "visibility_aabb_size": [12, 8, 8],
                           "process_material": "hide.process_material.json"},
        "QuadMesh": {}})");
    scene.create("ball", R"({"Transform3D": {"position": [4, 0, 0]},
        "GPUParticlesCollisionSphere3D": {"radius": 0.5}})");
    scene.step(5);
    CHECK(scene.live(hiding).size() == 1);
    scene.step(60);
    CHECK(scene.live(hiding).empty());
}

TEST_CASE("GPUParticles3D rests on a baked SDF of the meshes around it, as Godot's") {
    Scene scene;
    scene.files["drop.process_material.json"] =
        R"({"format": 1, "gravity": [0, -9.8, 0], "emission_shape": "points",
            "emission_points": [[0, 1, 0]], "collision_mode": "rigid"})";
    const entt::entity emitter = scene.create("drops", R"({"Transform3D": {},
        "GPUParticles3D": {"amount": 1, "lifetime": 10, "explosiveness": 1,
                           "collision_base_size": 0.1,
                           "process_material": "drop.process_material.json"},
        "QuadMesh": {}})");
    // A slab whose top is at y 0, and a ball on another layer the bake leaves out.
    scene.create("slab", R"({"Transform3D": {"position": [0, -0.25, 0]}, "MeshInstance3D": {},
        "BoxMesh": {"size": [3, 0.5, 3]}})");
    scene.create("ball", R"({"Transform3D": {"position": [0, 1, 0]},
        "MeshInstance3D": {"layers": 2}, "SphereMesh": {}})");
    const entt::entity sdf = scene.create("sdf", R"({"Transform3D": {},
        "GPUParticlesCollisionSDF3D": {"size": [4, 2, 4], "resolution": "32", "thickness": 2,
                                       "bake_mask": 1}})");

    // Godot's cells: 32 along the longest side, the others as many whole cells as fit.
    CHECK(scene3d::particle_sdf_cells({4.0F, 2.0F, 4.0F}, "32") ==
          std::array<std::uint32_t, 3>{32, 16, 32});
    CHECK(scene3d::particle_sdf_cells({4.0F, 2.0F, 4.0F}, "33") ==
          std::array<std::uint32_t, 3>{0, 0, 0});
    const scene3d::ModelLoader no_models;
    auto baked = scene3d::bake_particle_sdf(scene.world, sdf, no_models);
    REQUIRE(baked.ok());
    CHECK(baked.value().meshes == 1);
    CHECK(baked.value().triangles == 12);
    // The same distances on a pool with threads.
    slash::jobs::Pool pool(3);
    auto threaded = scene3d::bake_particle_sdf(scene.world, sdf, no_models, &pool);
    REQUIRE(threaded.ok());
    CHECK(threaded.value().sdf == baked.value().sdf);
    // The file reads back as the distances.
    auto encoded = scene3d::encode_particle_sdf(baked.value().sdf);
    REQUIRE(encoded.ok());
    const render::Gray16 texture = render::decode_gray16(encoded.value()).value();
    CHECK(scene3d::particle_sdf(texture, 1, 32).value() == baked.value().sdf);
    CHECK_FALSE(scene3d::particle_sdf(texture, 64, 1).ok());

    // Without a texture it collides with nothing: a field of 1s, as Godot's white.
    scene.step();
    auto kept = scene3d::particle_steps_3d(scene.world, emitter);
    REQUIRE(kept.has_value());
    REQUIRE(kept->steps.back().colliders.size() == 1);
    CHECK(kept->steps.back().colliders[0].shape == render::ColliderShape3D::Sdf);
    CHECK(kept->steps.back().colliders[0].sdf->distances == std::vector<std::uint16_t>{0x3C00});

    scene.files["level.sdf.png"] = encoded.value();
    scene.world.ecs().patch<world::GPUParticlesCollisionSDF3D>(sdf, [](auto& collider) {
        collider.texture = "level.sdf.png";
        collider.slices_vertical = 32;
    });
    scene.step(150);
    auto live = scene.live(emitter);
    REQUIRE(live.size() == 1);
    CHECK(live[0].origin.y == doctest::Approx(0.1).epsilon(0.05));
    CHECK(scene3d::check_meshes(scene.world).empty());

    // A texture that isn't an .sdf.png file, or can't be cut into its slices, is reported.
    scene.world.set_file_reader([&](std::string_view path) -> slash::Result<std::string> {
        const auto found = scene.files.find(path);
        if (found == scene.files.end()) {
            return slash::make_error(slash::ErrorCode::NotFound, "no such file");
        }
        return found->second;
    });
    scene.create("wrong", R"({"Transform3D": {}, "GPUParticlesCollisionSDF3D": {
        "texture": "level.png"}})");
    scene.create("uncut", R"({"Transform3D": {}, "GPUParticlesCollisionSDF3D": {
        "texture": "level.sdf.png", "slices_horizontal": 64}})");
    const auto problems = scene3d::check_meshes(scene.world);
    REQUIRE(problems.size() == 2);
    CHECK(problems[0].path == "/components/GPUParticlesCollisionSDF3D/texture");
    CHECK(problems[0].message == "\"level.png\" isn't a .sdf.png file");
    CHECK(problems[1].message.find("level.sdf.png") != std::string::npos);
}

TEST_CASE("GPUParticles3D rests on a height field drawn from above, which follows the camera") {
    Scene scene;
    scene.files["drop.process_material.json"] =
        R"({"format": 1, "gravity": [0, -9.8, 0], "emission_shape": "points",
            "emission_points": [[1, 3, 0]], "collision_mode": "rigid"})";
    const entt::entity emitter = scene.create("drops", R"({"Transform3D": {},
        "GPUParticles3D": {"amount": 1, "lifetime": 10, "explosiveness": 1,
                           "collision_base_size": 0.1, "visibility_aabb_size": [8, 8, 8],
                           "process_material": "drop.process_material.json"},
        "QuadMesh": {}})");
    // Ground at y 0.5, and a roof above it on a layer the height field leaves out.
    scene.create("ground", R"({"Transform3D": {"position": [0, 0.5, 0]}, "MeshInstance3D": {},
        "PlaneMesh": {"size": [4, 4]}})");
    scene.create("roof", R"({"Transform3D": {"position": [0, 1.5, 0]},
        "MeshInstance3D": {"layers": 2}, "PlaneMesh": {"size": [4, 4]}})");
    const entt::entity field = scene.create("field", R"({"Transform3D": {},
        "GPUParticlesCollisionHeightField3D": {"size": [6, 4, 6], "resolution": "256",
                                               "heightfield_mask": 1}})");
    // A second height field: Godot's particles take only the first.
    scene.create("second", R"({"Transform3D": {},
        "GPUParticlesCollisionHeightField3D": {"size": [6, 4, 6], "resolution": "256",
                                               "heightfield_mask": 1}})");

    CHECK(scene3d::particle_height_field_size({6.0F, 4.0F, 3.0F}, "256") ==
          std::array<std::uint32_t, 2>{256, 128});
    CHECK(scene3d::particle_height_field_size({6.0F, 4.0F, 3.0F}, "64") ==
          std::array<std::uint32_t, 2>{0, 0});
    std::vector<scene3d::Problem> problems;
    const scene3d::ModelLoader no_models;
    const render::ParticleHeightField3D heights = scene3d::particle_height_field(
        scene.world, world::Tree3D(scene.world), field, no_models, problems);
    CHECK(problems.empty());
    REQUIRE(heights.width == 256);
    REQUIRE(heights.height == 256);
    // The ground's height (0.5 in a box from -2 to 2) in the middle, nothing at the edge.
    CHECK(heights.heights[(128 * 256) + 128] == doctest::Approx(0.625));
    CHECK(heights.heights[0] == 0.0F);

    scene.step();
    auto kept = scene3d::particle_steps_3d(scene.world, emitter);
    REQUIRE(kept.has_value());
    REQUIRE(kept->steps.back().colliders.size() == 1);
    CHECK(kept->steps.back().colliders[0].shape == render::ColliderShape3D::HeightField);
    scene.step(150);
    auto live = scene.live(emitter);
    REQUIRE(live.size() == 1);
    CHECK(live[0].origin.y == doctest::Approx(0.6).epsilon(0.05));
    CHECK(live[0].origin.x == doctest::Approx(1.0));
    CHECK(scene3d::check_meshes(scene.world).empty());

    // With follow_camera_enabled it steps after the current camera a meter at a time.
    scene.create("eye", R"({"Transform3D": {"position": [10.5, 2, -3.2]},
        "Camera3D": {"current": true}})");
    scene.world.ecs().patch<world::GPUParticlesCollisionHeightField3D>(
        field, [](auto& c) { c.follow_camera_enabled = true; });
    scene.step();
    const Vec3 moved = scene.world.ecs().get<world::Transform3D>(field).position;
    CHECK(moved.x == doctest::Approx(10.0));
    CHECK(moved.y == doctest::Approx(0.0));
    CHECK(moved.z == doctest::Approx(-3.0));
}

TEST_CASE("a vector field attractor cuts its texture into slices, as Godot's Texture3D importer") {
    // Four 1x2 slices side by side, two across and two down, each texel its own red.
    render::Image image;
    image.width = 2;
    image.height = 4;
    for (std::uint8_t i = 0; i < 8; ++i) {
        image.pixels.insert(image.pixels.end(), {static_cast<std::uint8_t>(i * 30), 128, 255, 255});
    }
    auto field = scene3d::particle_field(image, 2, 2);
    REQUIRE(field.ok());
    CHECK(field.value().width == 1);
    CHECK(field.value().height == 2);
    CHECK(field.value().depth == 4);
    // Slice by slice: left then right, top then bottom.
    const std::array<int, 8> reds{0, 60, 30, 90, 120, 180, 150, 210};
    REQUIRE(field.value().texels.size() == reds.size());
    for (std::size_t i = 0; i < reds.size(); ++i) {
        CHECK(field.value().texels[i].x == doctest::Approx(reds.at(i) / 255.0));
    }
    CHECK(field.value().texels[0].z == 1.0F);
    CHECK_FALSE(scene3d::particle_field(image, 3, 1).ok());
    CHECK_FALSE(scene3d::particle_field(image, 0, 1).ok());

    Scene scene;
    auto png = render::encode_png(image);
    REQUIRE(png.ok());
    scene.files["wind.png"] = png.value();
    scene.world.set_file_reader([&](std::string_view path) -> slash::Result<std::string> {
        const auto found = scene.files.find(path);
        if (found == scene.files.end()) {
            return slash::make_error(slash::ErrorCode::NotFound, "no such file");
        }
        return found->second;
    });
    const entt::entity emitter =
        scene.create("leaves", R"({"Transform3D": {}, "GPUParticles3D": {}, "QuadMesh": {}})");
    scene.create("wind", R"({"Transform3D": {}, "GPUParticlesAttractorVectorField3D": {
        "texture": "wind.png", "slices_horizontal": 2, "slices_vertical": 2}})");
    // Without a texture, white.
    scene.create("blank", R"({"Transform3D": {}, "GPUParticlesAttractorVectorField3D": {}})");
    scene.step();
    auto kept = scene3d::particle_steps_3d(scene.world, emitter);
    REQUIRE(kept.has_value());
    // The cut field, and white without a texture.
    std::vector<render::ParticleAttractor3D> attractors = kept->steps.back().attractors;
    REQUIRE(attractors.size() == 2);
    REQUIRE(attractors[0].field != nullptr);
    REQUIRE(attractors[1].field != nullptr);
    if (attractors[0].field->depth == 1) {
        std::swap(attractors[0], attractors[1]);
    }
    CHECK(*attractors[0].field == field.value());
    CHECK(attractors[0].extents.x == doctest::Approx(1.0));
    CHECK(attractors[1].field->texels == std::vector<Vec4>{{1.0F, 1.0F, 1.0F, 1.0F}});
    CHECK(scene3d::check_meshes(scene.world).empty());

    // A texture that can't be read or cut is reported.
    scene.create("missing", R"({"Transform3D": {}, "GPUParticlesAttractorVectorField3D": {
        "texture": "gone.png"}})");
    scene.create("thin", R"({"Transform3D": {}, "GPUParticlesAttractorVectorField3D": {
        "texture": "wind.png", "slices_horizontal": 3}})");
    const auto problems = scene3d::check_meshes(scene.world);
    REQUIRE(problems.size() == 2);
    CHECK(problems[0].path == "/components/GPUParticlesAttractorVectorField3D/texture");
    CHECK(problems[0].message.find("could not be read") != std::string::npos);
    CHECK(problems[1].message.find("can't be cut into 3x8 slices") != std::string::npos);
}
