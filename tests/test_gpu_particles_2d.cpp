#include "devices.hpp"

#include "slash/asset/assets.hpp"
#include "slash/canvas/canvas.hpp"
#include "slash/canvas/gpu_particles.hpp"
#include "slash/canvas/particles.hpp"
#include "slash/core/json.hpp"
#include "slash/render/device.hpp"
#include "slash/render/image.hpp"
#include "slash/render/particles.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/world.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace asset = slash::asset;
namespace canvas = slash::canvas;
namespace json = slash::json;
namespace render = slash::render;
namespace world = slash::world;
using slash::Vec2;

namespace {

constexpr double kTick = 1.0 / 60.0;

json::Value parse(std::string_view text) {
    auto value = json::parse(text);
    REQUIRE(value.ok());
    return std::move(value).value();
}

// Files in memory, for the renderer's assets.
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

// A world with the canvas installed and the events it emits.
struct Scene {
    MemoryFiles files;
    asset::Assets assets{files.source()};
    world::ComponentRegistry components;
    world::World world{components};
    std::vector<world::Event> events;

    Scene() {
        world::register_builtin_components(components);
        canvas::install(world, assets);
        world.set_event_listener([this](const world::Event& event) { events.push_back(event); });
    }

    entt::entity create(std::string_view id, std::string_view components_json) {
        const entt::entity entity = world.create(id).value();
        const json::Value values = parse(components_json);
        for (const auto& [name, fields] : values.as_object()) {
            std::string path;
            REQUIRE(components.find(name)->assign(world, entity, fields, path).ok());
        }
        return entity;
    }

    void step(int ticks = 1) {
        for (int i = 0; i < ticks; ++i) {
            world.step(kTick);
        }
    }

    world::GPUParticles2D& emitter(entt::entity entity) {
        return world.ecs().get<world::GPUParticles2D>(entity);
    }

    canvas::ParticleSteps steps(entt::entity entity) const {
        const std::optional<canvas::ParticleSteps> kept = canvas::particle_steps(world, entity);
        REQUIRE(kept.has_value());
        return *kept;
    }

    std::size_t count(std::string_view kind) const {
        return static_cast<std::size_t>(std::ranges::count_if(
            events, [&](const world::Event& event) { return event.kind == kind; }));
    }
};

// The particles' bytes, those of live ones only (a dead one keeps whatever it last had).
std::vector<std::uint8_t> live_bytes(const std::vector<render::Particle>& particles) {
    std::vector<std::uint8_t> out;
    for (const render::Particle& particle : particles) {
        out.push_back(particle.active() ? 1 : 0);
        if (particle.active()) {
            const auto* bytes = reinterpret_cast<const std::uint8_t*>(&particle);
            out.insert(out.end(), bytes, bytes + sizeof(render::Particle));
        }
    }
    return out;
}

constexpr std::string_view kSpray = R"({
    "format": 1,
    "direction": [0, -1],
    "spread": 0,
    "gravity": [0, 0],
    "initial_velocity_min": 60,
    "initial_velocity_max": 60
})";

constexpr std::string_view kBusy = R"({
    "format": 1,
    "emission_shape": "sphere",
    "emission_sphere_radius": 6,
    "direction": [0, -1],
    "spread": 30,
    "gravity": [0, 40],
    "initial_velocity_min": 40,
    "initial_velocity_max": 80,
    "angular_velocity_min": -90,
    "angular_velocity_max": 90,
    "damping_min": 1,
    "damping_max": 3,
    "scale_min": 0.5,
    "scale_max": 1.5,
    "scale_curve": [[0, 1], [1, 0.2]],
    "alpha_curve": [[0, 0], [0.2, 1], [1, 0]],
    "color_ramp": [{"offset": 0, "color": [1, 1, 1, 1]}, {"offset": 1, "color": [0.5, 0.1, 0, 1]}],
    "lifetime_randomness": 0.3,
    "inherit_velocity_ratio": 0.5
})";

// Rockets that fly up and burst into their sub-emitter's particles where they end.
constexpr std::string_view kRocket = R"({
    "format": 1,
    "direction": [0, -1],
    "spread": 20,
    "gravity": [0, 30],
    "initial_velocity_min": 50,
    "initial_velocity_max": 70,
    "lifetime_randomness": 0.2,
    "sub_emitter_mode": "at_end",
    "sub_emitter_amount_at_end": 6
})";

// Sparks that go every way from where they start.
constexpr std::string_view kSpark = R"({
    "format": 1,
    "spread": 180,
    "gravity": [0, 40],
    "initial_velocity_min": 20,
    "initial_velocity_max": 40,
    "scale_min": 2,
    "scale_max": 3,
    "color": [1, 0.8, 0.3, 1]
})";

std::string first_error(std::string_view text) {
    const auto errors = canvas::validate_process_material(parse(text));
    return errors.empty() ? "(valid)" : errors.front().path + ": " + errors.front().message;
}

} // namespace

TEST_CASE("process material files are Godot's ParticleProcessMaterial in 2D") {
    auto process = canvas::parse_process_material(parse(R"({
        "format": 1,
        "emission_shape": "ring",
        "emission_ring_radius": 10,
        "emission_ring_inner_radius": 4,
        "direction": [0, -1],
        "spread": 20,
        "initial_velocity_min": 60,
        "initial_velocity_max": 90,
        "radial_velocity_min": 2,
        "radial_velocity_max": 3,
        "scale_curve": [[0, 1], [1, 0]],
        "color_ramp": [{"offset": 0, "color": [1, 0.8, 0.2, 1]}, {"offset": 1, "color": [1, 0.2, 0, 0]}],
        "particle_flag_align_y": true,
        "turbulence_enabled": true,
        "turbulence_noise_strength": 3,
        "turbulence_noise_scale": 4.5,
        "turbulence_noise_speed": [1, 0.5, 0],
        "turbulence_noise_speed_random": 0.5,
        "turbulence_influence_min": 0.2,
        "turbulence_influence_max": 0.6,
        "turbulence_influence_over_life": [[0, 0], [1, 1]],
        "turbulence_initial_displacement_min": -4,
        "turbulence_initial_displacement_max": 4
    })"));
    REQUIRE(process.ok());
    const render::ParticleProcess& p = process.value();
    CHECK(p.emission_shape == render::EmissionShape::Ring);
    CHECK(p.emission_ring_radius == 10.0F);
    CHECK(p.emission_ring_inner_radius == 4.0F);
    CHECK(p.direction == Vec2{0.0F, -1.0F});
    CHECK(p.spread == 20.0F);
    CHECK(p.param_min.at(0) == 60.0F);
    CHECK(p.param_max.at(0) == 90.0F);
    CHECK(p.param_max.at(static_cast<std::size_t>(render::ParticleParam::RadialVelocity)) == 3.0F);
    CHECK(p.param_curve.at(static_cast<std::size_t>(render::ParticleParam::Scale)) ==
          render::ParticleCurve{{0.0F, 1.0F}, {1.0F, 0.0F}});
    CHECK(p.color_ramp.size() == 2);
    CHECK(p.color_ramp[1].color.w == 0.0F);
    CHECK(p.align_y);
    CHECK(p.turbulence_enabled);
    CHECK(p.turbulence_noise_strength == 3.0F);
    CHECK(p.turbulence_noise_scale == 4.5F);
    CHECK(p.turbulence_noise_speed == slash::Vec3{1.0F, 0.5F, 0.0F});
    CHECK(p.turbulence_noise_speed_random == 0.5F);
    const auto turbulence = static_cast<std::size_t>(render::ParticleParam::TurbulenceInfluence);
    CHECK(p.param_min.at(turbulence) == 0.2F);
    CHECK(p.param_max.at(turbulence) == 0.6F);
    CHECK(p.param_curve.at(turbulence) == render::ParticleCurve{{0.0F, 0.0F}, {1.0F, 1.0F}});
    const auto displacement =
        static_cast<std::size_t>(render::ParticleParam::TurbulenceDisplacement);
    CHECK(p.param_min.at(displacement) == -4.0F);
    CHECK(p.param_max.at(displacement) == 4.0F);
    // Left out, gravity is a 2D ParticleProcessMaterial's, and scale 1.
    CHECK(p.gravity == Vec2{0.0F, 98.0F});
    CHECK(p.param_min.at(static_cast<std::size_t>(render::ParticleParam::Scale)) == 1.0F);
    CHECK_FALSE(render::ParticleProcess{}.turbulence_enabled);
    CHECK(render::ParticleProcess{}.param_min.at(turbulence) == 0.1F);
    CHECK(canvas::parse_process_material(parse(R"({"format": 1})")).value() ==
          render::ParticleProcess{});
    auto collides = canvas::parse_process_material(
        parse(R"({"format": 1, "collision_mode": "hide_on_contact", "collision_friction": 0.3,
                  "collision_bounce": 0.6, "collision_use_scale": true})"));
    REQUIRE(collides.ok());
    CHECK(collides.value().collision_mode == render::ParticleCollision::HideOnContact);
    CHECK(collides.value().collision_friction == 0.3F);
    CHECK(collides.value().collision_bounce == 0.6F);
    CHECK(collides.value().collision_use_scale);
    CHECK(render::ParticleProcess{}.collision_mode == render::ParticleCollision::Disabled);
    CHECK(
        first_error(R"({"format": 1, "collision_mode": "solid"})").starts_with("/collision_mode"));
    CHECK(first_error(R"({"format": 1, "collision_bounce": 2})").starts_with("/collision_bounce"));

    CHECK(first_error(R"({"format": 1, "emission_shape": "box"})") == "(valid)");
    CHECK(first_error(R"({"emission_shape": "box"})").starts_with(": "));
    CHECK(first_error(R"({"format": 1, "emission_shape": "sphear"})")
              .starts_with("/emission_shape: "));
    CHECK(first_error(R"({"format": 1, "scale_amount_min": 2})").starts_with("/scale_amount_min"));
    CHECK(first_error(R"({"format": 1, "scale_curve": [[0, 1], [0.6, 2], [0.5, 3]]})") ==
          "/scale_curve/2/0: offsets go in order: 0.5 comes after 0.6");
    CHECK(first_error(R"({"format": 1, "color_ramp": [{"offset": 1, "color": [1, 1, 1, 1]},
                                                      {"offset": 0, "color": [1, 1, 1, 1]}]})") ==
          "/color_ramp/1/offset: offsets go in order: 0 comes after 1");
    CHECK(first_error(R"({"format": 1, "turbulence_influence_over_life": [[0.5, 1], [0.2, 0]]})") ==
          "/turbulence_influence_over_life/1/0: offsets go in order: 0.2 comes after 0.5");
    CHECK(first_error(R"({"format": 1, "turbulence_noise_strength": 25})")
              .starts_with("/turbulence_noise_strength"));
    CHECK(first_error(R"({"format": 1, "turbulence_noise_speed": [1, 0]})")
              .starts_with("/turbulence_noise_speed"));
    CHECK(first_error(R"({"format": 1, "turbulence_initial_displacement_curve": []})")
              .starts_with("/turbulence_initial_displacement_curve"));
    CHECK(first_error(R"({"format": 1, "scale_curve": [[2, 1]]})").starts_with("/scale_curve/0/0"));
    CHECK(first_error(R"({"format": 1, "emission_shape": "points"})") ==
          "/emission_shape: \"points\" needs emission_points to start particles at");
    CHECK(first_error(R"({"format": 1, "emission_shape": "directed_points",
                          "emission_points": [[0, 0], [4, 0]], "emission_normals": [[1, 0]]})") ==
          "/emission_normals: has 1 items for 2 emission_points; give none, or one per point");
    std::string long_curve = R"({"format": 1, "alpha_curve": [)";
    for (int i = 0; i < 33; ++i) {
        long_curve += (i > 0 ? ", [" : "[") + std::to_string(i / 32.0) + ", 1]";
    }
    CHECK(first_error(long_curve + "]}").starts_with("/alpha_curve"));
    CHECK(canvas::process_material_schema().find("title")->as_string() ==
          "SlashEngine particle process material");
}

TEST_CASE("a GPUParticles2D steps its cycle as Godot's particles storage does") {
    Scene scene;
    const entt::entity fountain =
        scene.create("fountain", R"({"Transform2D": {"position": [10, 20]},
                        "GPUParticles2D": {"amount": 4, "lifetime": 0.5}})");
    // Nothing until the first tick.
    CHECK_FALSE(canvas::particle_steps(scene.world, fountain).has_value());
    scene.step();
    canvas::ParticleSteps kept = scene.steps(fountain);
    CHECK(kept.amount == 4);
    CHECK(kept.first == 0);
    // The first step clears the system and runs, though the tick is shorter than a step.
    REQUIRE(kept.steps.size() == 1);
    const render::ParticleStep& first = kept.steps.front();
    CHECK(first.clear);
    CHECK(first.emitting);
    CHECK(first.delta == doctest::Approx(1.0 / 30.0));
    CHECK(first.prev_system_phase == 0.0F);
    CHECK(first.system_phase == doctest::Approx(1.0 / 15.0));
    CHECK(first.lifetime == 0.5F);
    CHECK(first.cycle == 0);
    CHECK(first.emission == std::array<float, 6>{1.0F, 0.0F, 0.0F, 1.0F, 10.0F, 20.0F});
    CHECK(first.seed == canvas::particle_seed("fountain", 0));
    CHECK(first.time == doctest::Approx(1.0 / 30.0));
    CHECK(kept.remainder == doctest::Approx(-1.0 / 60.0));

    // Then a step of 1/30 s each time 1/30 s of ticks has built up; the cycle counts up as the
    // phase comes round.
    scene.step(59);
    kept = scene.steps(fountain);
    const std::uint64_t made = kept.first + kept.steps.size();
    CHECK(made >= 30);
    CHECK(made <= 31);
    std::uint32_t cycle = 0;
    for (std::size_t i = 1; i < kept.steps.size(); ++i) {
        const render::ParticleStep& step = kept.steps[i];
        CHECK_FALSE(step.clear);
        CHECK(step.delta == doctest::Approx(1.0 / 30.0));
        CHECK(step.prev_system_phase == kept.steps[i - 1].system_phase);
        CHECK(step.time == doctest::Approx(kept.steps[i - 1].time + step.delta));
        if (step.system_phase < step.prev_system_phase) {
            ++cycle;
        }
        CHECK(step.cycle == kept.steps.front().cycle + cycle);
    }
    CHECK(kept.steps.back().cycle >= 1);
    // Only the steps that can still have made a live particle are kept: a bit more than a
    // lifetime of them.
    CHECK(kept.steps.size() >= 16);
    CHECK(kept.steps.size() <= 18);

    // fixed_fps 0 steps once a tick, by the tick times speed_scale.
    scene.emitter(fountain).fixed_fps = 0;
    scene.emitter(fountain).speed_scale = 2.0F;
    scene.step();
    kept = scene.steps(fountain);
    CHECK(kept.steps.back().delta == doctest::Approx(2.0 / 60.0));
    CHECK(kept.remainder == 0.0F);

    // A new amount starts over from nothing.
    scene.emitter(fountain).amount = 6;
    scene.step();
    kept = scene.steps(fountain);
    CHECK(kept.amount == 6);
    REQUIRE(kept.steps.size() == 1);
    CHECK(kept.steps.front().clear);
    CHECK(kept.steps.front().cycle == 0);
    CHECK(kept.steps.front().time == doctest::Approx(kept.steps.front().delta));

    // In local coordinates particles start in the emitter's own space.
    scene.emitter(fountain).local_coords = true;
    scene.step();
    CHECK(scene.steps(fountain).steps.back().emission ==
          std::array<float, 6>{1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F});
    // The emitter's velocity goes with each step.
    scene.world.ecs().get<world::Transform2D>(fountain).position.x += 3.0F;
    scene.step();
    CHECK(scene.steps(fountain).steps.back().emitter_velocity.x == doctest::Approx(180.0));
}

TEST_CASE("preprocess runs ahead in steps when a GPUParticles2D starts") {
    Scene scene;
    const entt::entity smoke = scene.create("smoke", R"({"Transform2D": {},
                                  "GPUParticles2D": {"preprocess": 0.25, "fixed_fps": 20}})");
    scene.step();
    const canvas::ParticleSteps kept = scene.steps(smoke);
    // Steps of 1/20 s, the first clearing, up to 0.25 s (and, as Godot's, a last one of what
    // the float rounding left over); the tick's own 1/60 s isn't a step.
    REQUIRE(kept.steps.size() >= 5);
    CHECK(kept.steps.front().clear);
    for (std::size_t i = 0; i < 5; ++i) {
        CHECK(kept.steps[i].delta == doctest::Approx(0.05));
    }
    CHECK(kept.steps.back().system_phase == doctest::Approx(0.25));
}

TEST_CASE("a one_shot GPUParticles2D stops emitting, finishes and restarts as Godot's") {
    Scene scene;
    const entt::entity burst = scene.create("burst", R"({"Transform2D": {},
                                  "GPUParticles2D": {"one_shot": true, "lifetime": 0.5,
                                                     "explosiveness": 0.5, "amount": 3}})");
    // Emitting until a lifetime has passed, then finished at lifetime times 2 - explosiveness.
    scene.step(29);
    CHECK(scene.emitter(burst).emitting);
    scene.step(3);
    CHECK_FALSE(scene.emitter(burst).emitting);
    scene.step(12);
    CHECK(scene.count("finished") == 0);
    scene.step(3);
    CHECK(scene.count("finished") == 1);
    // The storage stopped emitting when its cycle came round.
    const canvas::ParticleSteps kept = scene.steps(burst);
    CHECK_FALSE(kept.steps.back().emitting);
    const std::uint32_t seed = kept.steps.back().seed;
    scene.step(60);
    CHECK(scene.count("finished") == 1);

    // Emitting again starts another cycle with another seed, from a cleared system.
    scene.emitter(burst).emitting = true;
    scene.step();
    const canvas::ParticleSteps again = scene.steps(burst);
    CHECK(again.steps.back().emitting);
    CHECK(again.steps.front().clear);
    CHECK(again.steps.back().seed != seed);
    CHECK(again.steps.back().seed == canvas::gpu_particle_seed(scene.emitter(burst), "burst", 1));
    scene.step(60);
    CHECK(scene.count("finished") == 2);

    // With use_fixed_seed, the seed is the seed field every time.
    scene.emitter(burst).use_fixed_seed = true;
    scene.emitter(burst).seed = 77;
    scene.emitter(burst).emitting = true;
    scene.step(2);
    CHECK(scene.steps(burst).steps.back().seed == 77U);

    // Turning one_shot off while it emits restarts it, emitting for good.
    scene.emitter(burst).one_shot = false;
    scene.step();
    CHECK(scene.steps(burst).steps.front().clear);
    scene.step(120);
    CHECK(scene.emitter(burst).emitting);
    CHECK(scene.steps(burst).steps.back().emitting);

    // A burst set off again as it finishes bursts at once, though the last cycle's phase hasn't
    // come round (Godot's storage would wait for it, and then stop emitting).
    const entt::entity pop = scene.create("pop", R"({"Transform2D": {},
                                  "GPUParticles2D": {"one_shot": true, "lifetime": 0.5,
                                                     "explosiveness": 1, "amount": 4}})");
    const auto live = [&] {
        const canvas::ParticleSteps steps = scene.steps(pop);
        const auto particles = canvas::run_steps(steps.amount, {}, steps.steps);
        return std::ranges::count_if(particles,
                                     [](const render::Particle& p) { return p.active(); });
    };
    scene.step(2);
    CHECK(live() == 4);
    const auto finished = scene.count("finished");
    while (scene.count("finished") == finished) {
        scene.step();
    }
    scene.emitter(pop).emitting = true;
    scene.step(2);
    CHECK(scene.steps(pop).steps.front().clear);
    CHECK(live() == 4);
}

TEST_CASE("the steps kept make the particles that running every step makes") {
    Scene scene;
    const auto process = canvas::parse_process_material(parse(kBusy));
    REQUIRE(process.ok());
    const entt::entity torch = scene.create("torch", R"({"Transform2D": {"position": [50, 50]},
                                  "GPUParticles2D": {"amount": 24, "lifetime": 0.8,
                                                     "randomness": 0.5, "explosiveness": 0.2,
                                                     "interp_to_end": 0.1}})");
    std::vector<render::Particle> every(24);
    std::uint64_t next = 0;
    std::uint64_t serial = 0;
    for (int tick = 0; tick < 300; ++tick) {
        // It moves, stops emitting for a while and speeds up.
        scene.world.ecs().get<world::Transform2D>(torch).position.x =
            50.0F + static_cast<float>(tick % 37);
        if (tick == 100) {
            scene.emitter(torch).emitting = false;
        }
        if (tick == 130) {
            scene.emitter(torch).emitting = true;
        }
        if (tick == 200) {
            scene.emitter(torch).speed_scale = 1.7F;
        }
        scene.step();
        const canvas::ParticleSteps kept = scene.steps(torch);
        if (kept.serial != serial || next < kept.first) {
            // Started over: only at the start, and after it stopped for more than a lifetime.
            CHECK((tick == 0 || tick > 100));
            every = canvas::run_steps(kept.amount, process.value(), kept.steps);
            serial = kept.serial;
        } else {
            render::process_particles(
                every, process.value(),
                kept.steps.subspan(static_cast<std::size_t>(next - kept.first)));
        }
        next = kept.first + kept.steps.size();
        if (tick % 25 == 24) {
            CAPTURE(tick);
            const std::vector<render::Particle> replayed =
                canvas::run_steps(kept.amount, process.value(), kept.steps);
            CHECK(live_bytes(replayed) == live_bytes(every));
            CHECK(std::ranges::any_of(every, [](const render::Particle& p) {
                      return p.active();
                  }) == (tick < 100 || tick > 140));
        }
    }
    // gpu_particles runs them the same way, in draw order.
    scene.files.files["busy.process_material.json"] = std::string(kBusy);
    scene.emitter(torch).process_material = "busy.process_material.json";
    auto live = canvas::gpu_particles(scene.world, torch, scene.assets);
    REQUIRE(live.ok());
    CHECK(live.value().size() == static_cast<std::size_t>(std::ranges::count_if(
                                     every, [](const render::Particle& p) { return p.active(); })));
    scene.emitter(torch).process_material = "missing.process_material.json";
    CHECK_FALSE(canvas::gpu_particles(scene.world, torch, scene.assets).ok());
}

TEST_CASE("simulate runs a GPUParticles2D apart from the world as stepping would") {
    Scene scene;
    const auto process = canvas::parse_process_material(parse(kSpray));
    REQUIRE(process.ok());
    scene.files.files["spray.process_material.json"] = std::string(kSpray);
    const entt::entity spray = scene.create("spray", R"({"Transform2D": {"position": [5, 5]},
                                  "GPUParticles2D": {"amount": 10, "process_material":
                                                     "spray.process_material.json"}})");
    scene.step(45);
    auto stepped = canvas::gpu_particles(scene.world, spray, scene.assets);
    REQUIRE(stepped.ok());
    const std::vector<render::Particle> simulated =
        canvas::simulate(scene.emitter(spray), process.value(),
                         scene.world.ecs().get<world::Transform2D>(spray), "spray", 0.75, kTick);
    CHECK(live_bytes(simulated) == live_bytes(stepped.value()));
    CHECK(simulated.size() >= 7);
    // Lifetime order: the oldest first, so the youngest draw on top.
    for (std::size_t i = 1; i < simulated.size(); ++i) {
        CHECK(simulated[i - 1].custom.y >= simulated[i].custom.y);
    }
    // Each moves straight up at 60 pixels a second from the emitter.
    for (const render::Particle& particle : simulated) {
        CHECK(particle.origin.x == doctest::Approx(5.0));
        CHECK(particle.velocity.y == doctest::Approx(-60.0));
    }
}

TEST_CASE("the canvas runs and draws GPU particles the same on every device") {
    Scene scene;
    scene.files.files["spray.process_material.json"] = std::string(kSpray);
    scene.files.files["busy.process_material.json"] = std::string(kBusy);
    scene.create("spray", R"({"Transform2D": {"position": [10, 40]},
                              "GPUParticles2D": {"amount": 8, "explosiveness": 1,
                                                 "process_material": "spray.process_material.json"}})");
    scene.create("torch", R"({"Transform2D": {"position": [32, 40]},
                              "GPUParticles2D": {"amount": 32, "draw_order": "reverse_lifetime",
                                                 "process_material": "busy.process_material.json",
                                                 "local_coords": true}})");
    scene.create("hidden", R"({"Transform2D": {}, "GPUParticles2D": {"amount": 5,
                               "visible": false}})");
    scene.create("broken", R"({"Transform2D": {}, "GPUParticles2D": {"amount": 6,
                               "process_material": "gone.process_material.json"}})");
    constexpr std::uint32_t kSize = 48;
    const auto draw_with = [&](canvas::CanvasRenderer& renderer, render::RenderDevice& device,
                               canvas::FrameContents* contents) {
        auto target = device.create_texture({.width = kSize,
                                             .height = kSize,
                                             .filter = render::Filter::Nearest,
                                             .render_target = true});
        REQUIRE(target.ok());
        canvas::CanvasOptions options;
        options.target = target.value();
        options.width = static_cast<float>(kSize);
        options.height = static_cast<float>(kSize);
        options.contents = contents;
        REQUIRE(renderer.draw(scene.world, options).ok());
        auto image = device.read_texture(target.value());
        REQUIRE(image.ok());
        device.destroy_texture(target.value());
        return std::move(image).value();
    };

    // One renderer draws every tick; another, made at the end, catches up from the steps kept.
    auto software = render::create_software_device();
    canvas::CanvasRenderer steady(*software, scene.assets);
    for (int i = 0; i < 40; ++i) {
        scene.step();
        (void)draw_with(steady, *software, nullptr);
    }
    canvas::FrameContents contents;
    const render::Image expected = draw_with(steady, *software, &contents);
    CHECK(steady.particles_drawn() == 40);
    std::size_t lit = 0;
    for (std::size_t i = 0; i < expected.pixels.size(); i += 4) {
        lit += expected.pixels[i] > 60 ? 1U : 0U;
    }
    CHECK(lit > 20);
    // The frame's contents have each emitter's live particles, run on the CPU.
    std::map<std::string, std::size_t> counts;
    for (const canvas::FrameItem& item : contents.items) {
        CHECK(item.kind == canvas::FrameItem::Kind::GpuParticles);
        counts[std::string(scene.world.id_of(item.entity))] = item.count;
    }
    CHECK(counts.size() == 2);
    CHECK(counts["spray"] == 8);
    CHECK(counts["torch"] > 10);
    REQUIRE(contents.problems.size() == 1);
    CHECK(contents.problems.front().path == "/components/GPUParticles2D/process_material");

    auto late_device = render::create_software_device();
    canvas::CanvasRenderer late(*late_device, scene.assets);
    CHECK(draw_with(late, *late_device, nullptr).pixels == expected.pixels);

    // The null device counts them; hardware devices draw what the software device does.
    auto null = render::create_null_device();
    canvas::CanvasRenderer counted(*null, scene.assets);
    REQUIRE(counted.draw(scene.world, {}).ok());
    CHECK(counted.particles_drawn() == 40);
    for (auto& device : slash::test::hardware_devices()) {
        CAPTURE(render::backend_name(device->backend()));
        canvas::CanvasRenderer renderer(*device, scene.assets);
        const render::Image image = draw_with(renderer, *device, nullptr);
        std::size_t different = 0;
        for (std::size_t i = 0; i < image.pixels.size(); ++i) {
            different += std::abs(int{image.pixels[i]} - int{expected.pixels[i]}) > 12 ? 1U : 0U;
        }
        CHECK(different < (lit / 4) + 8);
    }
}

TEST_CASE("a GPUParticles2D's sub_emitter takes the particles it starts, stepping with it") {
    auto process = canvas::parse_process_material(
        parse(R"({"format": 1, "sub_emitter_mode": "at_end", "sub_emitter_frequency": 6,
                  "sub_emitter_amount_at_end": 5, "sub_emitter_amount_at_collision": 2,
                  "sub_emitter_amount_at_start": 3, "sub_emitter_keep_velocity": true})"));
    REQUIRE(process.ok());
    CHECK(process.value().sub_emitter_mode == render::SubEmitterMode::AtEnd);
    CHECK(process.value().sub_emitter_frequency == 6.0F);
    CHECK(process.value().sub_emitter_amount_at_end == 5);
    CHECK(process.value().sub_emitter_amount_at_collision == 2);
    CHECK(process.value().sub_emitter_amount_at_start == 3);
    CHECK(process.value().sub_emitter_keep_velocity);
    CHECK(render::ParticleProcess{}.sub_emitter_mode == render::SubEmitterMode::Disabled);
    CHECK(first_error(R"({"format": 1, "sub_emitter_mode": "sometimes"})")
              .starts_with("/sub_emitter_mode"));
    CHECK(first_error(R"({"format": 1, "sub_emitter_frequency": 0})")
              .starts_with("/sub_emitter_frequency"));
    CHECK(first_error(R"({"format": 1, "sub_emitter_amount_at_end": 0})")
              .starts_with("/sub_emitter_amount_at_end"));

    Scene scene;
    const entt::entity sparks = scene.create("sparks", R"({"Transform2D": {"position": [500, 0]},
        "GPUParticles2D": {"amount": 64, "lifetime": 0.5, "speed_scale": 2,
                           "one_shot": true}})");
    const entt::entity rockets = scene.create("rockets", R"({"Transform2D": {"position": [50, 50]},
        "GPUParticles2D": {"amount": 8, "lifetime": 1, "sub_emitter": "sparks"}})");
    // Another naming the same sub-emitter after the first, by ID, runs on its own.
    const entt::entity zebra = scene.create("zebra", R"({"Transform2D": {},
        "GPUParticles2D": {"amount": 4, "sub_emitter": "sparks"}})");
    scene.step(3);
    const std::vector<canvas::ParticleChainMember> chain =
        canvas::particle_chain(scene.world, sparks);
    REQUIRE(chain.size() == 2);
    CHECK(chain[0].entity == rockets);
    CHECK(chain[1].entity == sparks);
    CHECK(chain[0].steps.link == 0);
    CHECK(chain[1].steps.link == 1);
    CHECK(chain[1].steps.links == 2);
    CHECK(scene.steps(sparks).links == 2);
    CHECK(scene.steps(zebra).links == 1);
    CHECK(canvas::particle_chain(scene.world, zebra).empty());
    REQUIRE(chain[0].steps.steps.size() == chain[1].steps.steps.size());
    CHECK(chain[0].steps.first == chain[1].steps.first);
    // A step of the rockets is one of the sparks, at its speed against theirs, emitting only
    // what the rockets start: its own emitting and one_shot go unused.
    for (std::size_t i = 0; i < chain[0].steps.steps.size(); ++i) {
        const render::ParticleStep& rocket = chain[0].steps.steps[i];
        const render::ParticleStep& spark = chain[1].steps.steps[i];
        CHECK(spark.delta == doctest::Approx(rocket.delta * 2.0));
        CHECK(spark.lifetime == 0.5F);
        CHECK(spark.clear == rocket.clear);
        CHECK_FALSE(spark.emitting);
    }
    scene.step(120);
    CHECK(scene.count("finished") == 0);
    CHECK(scene.steps(sparks).steps.back().delta ==
          doctest::Approx(2.0 * scene.steps(rockets).steps.back().delta));

    // The link that would close a loop is left out.
    const entt::entity loop_a = scene.create("loop_a", R"({"GPUParticles2D": {}})");
    const entt::entity loop_b =
        scene.create("loop_b", R"({"GPUParticles2D": {"sub_emitter": "loop_a"}})");
    scene.emitter(loop_a).sub_emitter = loop_b;
    // Eight in a chain at most: the ninth runs on its own.
    std::vector<entt::entity> line;
    for (int i = 9; i >= 1; --i) {
        const std::string id = "line" + std::to_string(i);
        const std::string sub =
            i == 9 ? "" : R"("sub_emitter": "line)" + std::to_string(i + 1) + "\"";
        line.insert(line.begin(), scene.create(id, R"({"GPUParticles2D": {)" + sub + "}}"));
    }
    scene.step();
    CHECK(canvas::particle_chain(scene.world, loop_b).size() == 2);
    CHECK(scene.steps(loop_a).link == 0);
    CHECK(scene.steps(loop_b).link == 1);
    CHECK(scene.steps(line[0]).links == 8);
    CHECK(scene.steps(line[7]).link == 7);
    CHECK(scene.steps(line[8]).links == 1);

    // Each sub_emitter left out, and why.
    const entt::entity self = scene.create("self", R"({"GPUParticles2D": {}})");
    scene.emitter(self).sub_emitter = self;
    scene.create("rock", R"({"Transform2D": {}})");
    scene.create("pebbles", R"({"GPUParticles2D": {"sub_emitter": "rock"}})");
    const std::vector<canvas::SubEmitterProblem> problems = canvas::check_sub_emitters(scene.world);
    REQUIRE(problems.size() == 5);
    CHECK(problems[0].id == "line8");
    CHECK(problems[0].message == "a chain of sub-emitters has at most 8 emitters");
    CHECK(problems[1].id == "loop_b");
    CHECK(problems[1].message == "\"loop_a\" leads back to this emitter through its sub-emitters");
    CHECK(problems[2].id == "pebbles");
    CHECK(problems[2].message == "\"rock\" has no GPUParticles2D");
    CHECK(problems[3].id == "self");
    CHECK(problems[3].message == "an emitter can't be its own sub-emitter");
    CHECK(problems[4].id == "zebra");
    CHECK(problems[4].message == "\"sparks\" is already the sub-emitter of \"rockets\"");

    // In a scene file, at the field.
    const json::Value file = parse(R"({"format": 1, "entities": {
        "a": {"components": {"GPUParticles2D": {"sub_emitter": "c"}}},
        "b": {"components": {"GPUParticles2D": {"sub_emitter": "c"}}},
        "c": {"components": {"GPUParticles2D": {}}}}})");
    const std::vector<json::ValidationError> errors =
        canvas::check_sub_emitters(scene.components, file, nullptr);
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].path == "/entities/b/components/GPUParticles2D/sub_emitter");
    CHECK(errors[0].message == "\"c\" is already the sub-emitter of \"a\"");
    CHECK(canvas::check_sub_emitters(scene.components, parse(R"({"format": 1, "entities": {}})"),
                                     nullptr)
              .empty());
}

TEST_CASE("the steps a chain of sub-emitters keeps make what running every step makes") {
    Scene scene;
    const auto rocket = canvas::parse_process_material(parse(kRocket));
    const auto spark = canvas::parse_process_material(parse(kBusy));
    REQUIRE(rocket.ok());
    REQUIRE(spark.ok());
    const entt::entity sparks = scene.create("sparks", R"({"Transform2D": {},
        "GPUParticles2D": {"amount": 40, "lifetime": 0.4, "speed_scale": 1.3,
                           "randomness": 0.3}})");
    const entt::entity rockets = scene.create("rockets", R"({"Transform2D": {"position": [50, 50]},
        "GPUParticles2D": {"amount": 6, "lifetime": 0.9, "randomness": 0.5,
                           "sub_emitter": "sparks"}})");
    const std::array<const render::ParticleProcess*, 2> processes{&rocket.value(), &spark.value()};
    std::array<std::vector<render::Particle>, 2> every;
    std::uint64_t next = 0;
    std::uint64_t serial = 0;
    for (int tick = 0; tick < 300; ++tick) {
        // It moves, stops emitting for a while and speeds up, and so do its sparks.
        scene.world.ecs().get<world::Transform2D>(rockets).position.x =
            50.0F + static_cast<float>(tick % 37);
        if (tick == 100) {
            scene.emitter(rockets).emitting = false;
        }
        if (tick == 130) {
            scene.emitter(rockets).emitting = true;
        }
        if (tick == 200) {
            scene.emitter(rockets).speed_scale = 1.7F;
            scene.emitter(sparks).speed_scale = 0.8F;
        }
        scene.step();
        const std::vector<canvas::ParticleChainMember> chain =
            canvas::particle_chain(scene.world, rockets);
        REQUIRE(chain.size() == 2);
        const canvas::ParticleSteps& root = chain[0].steps;
        std::array<canvas::ChainRun, 2> runs;
        for (std::size_t k = 0; k < 2; ++k) {
            runs[k] = {.amount = chain[k].steps.amount,
                       .process = processes[k],
                       .steps = chain[k].steps.steps,
                       .shapes = {}};
        }
        if (root.serial != serial || next < root.first) {
            CHECK((tick == 0 || tick > 100));
            auto all = canvas::run_chain(runs);
            every[0] = std::move(all[0]);
            every[1] = std::move(all[1]);
            serial = root.serial;
        } else {
            std::array<render::ParticleChainLink, 2> links;
            for (std::size_t k = 0; k < 2; ++k) {
                links[k] = {.particles = every[k],
                            .process = processes[k],
                            .steps = chain[k].steps.steps.subspan(
                                static_cast<std::size_t>(next - root.first)),
                            .shapes = {}};
            }
            render::process_particle_chain(links);
        }
        next = root.first + root.steps.size();
        if (tick % 25 == 24) {
            CAPTURE(tick);
            const auto replayed = canvas::run_chain(runs);
            CHECK(live_bytes(replayed[0]) == live_bytes(every[0]));
            CHECK(live_bytes(replayed[1]) == live_bytes(every[1]));
        }
    }
    CHECK(std::ranges::any_of(every[1], [](const render::Particle& p) { return p.active(); }));
    // gpu_particles runs a sub-emitter with its chain.
    scene.files.files["rocket.process_material.json"] = std::string(kRocket);
    scene.files.files["busy.process_material.json"] = std::string(kBusy);
    scene.emitter(rockets).process_material = "rocket.process_material.json";
    scene.emitter(sparks).process_material = "busy.process_material.json";
    auto live = canvas::gpu_particles(scene.world, sparks, scene.assets);
    REQUIRE(live.ok());
    CHECK(live.value().size() ==
          static_cast<std::size_t>(std::ranges::count_if(
              every[1], [](const render::Particle& p) { return p.active(); })));
    scene.emitter(rockets).process_material = "missing.process_material.json";
    CHECK_FALSE(canvas::gpu_particles(scene.world, sparks, scene.assets).ok());
}

TEST_CASE("the canvas draws chains of sub-emitters the same however late it starts") {
    Scene scene;
    scene.files.files["rocket.process_material.json"] = std::string(kRocket);
    scene.files.files["spark.process_material.json"] = std::string(kSpark);
    scene.create("sparks", R"({"Transform2D": {},
                               "GPUParticles2D": {"amount": 48, "lifetime": 0.5,
                                                  "process_material": "spark.process_material.json"}})");
    scene.create("rockets", R"({"Transform2D": {"position": [24, 44]},
                                "GPUParticles2D": {"amount": 4, "lifetime": 0.6,
                                                   "sub_emitter": "sparks",
                                                   "process_material": "rocket.process_material.json"}})");
    constexpr std::uint32_t kSize = 48;
    const auto draw_with = [&](canvas::CanvasRenderer& renderer, render::RenderDevice& device,
                               canvas::FrameContents* contents) {
        auto target = device.create_texture({.width = kSize,
                                             .height = kSize,
                                             .filter = render::Filter::Nearest,
                                             .render_target = true});
        REQUIRE(target.ok());
        canvas::CanvasOptions options;
        options.target = target.value();
        options.width = static_cast<float>(kSize);
        options.height = static_cast<float>(kSize);
        options.contents = contents;
        REQUIRE(renderer.draw(scene.world, options).ok());
        auto image = device.read_texture(target.value());
        REQUIRE(image.ok());
        device.destroy_texture(target.value());
        return std::move(image).value();
    };
    auto software = render::create_software_device();
    canvas::CanvasRenderer steady(*software, scene.assets);
    for (int i = 0; i < 70; ++i) {
        scene.step();
        (void)draw_with(steady, *software, nullptr);
    }
    canvas::FrameContents contents;
    const render::Image expected = draw_with(steady, *software, &contents);
    std::map<std::string, std::size_t> counts;
    for (const canvas::FrameItem& item : contents.items) {
        counts[std::string(scene.world.id_of(item.entity))] = item.count;
    }
    CHECK(counts["rockets"] > 0);
    CHECK(counts["sparks"] > 6);
    CHECK(steady.particles_drawn() == 52);
    auto late_device = render::create_software_device();
    canvas::CanvasRenderer late(*late_device, scene.assets);
    CHECK(draw_with(late, *late_device, nullptr).pixels == expected.pixels);

    // Hidden, the sparks still take the rockets' particles, and show them once shown again.
    scene.emitter(scene.world.find("sparks")).visible = false;
    for (int i = 0; i < 20; ++i) {
        scene.step();
        (void)draw_with(steady, *software, nullptr);
    }
    scene.emitter(scene.world.find("sparks")).visible = true;
    const render::Image shown = draw_with(steady, *software, nullptr);
    auto fresh_device = render::create_software_device();
    canvas::CanvasRenderer fresh(*fresh_device, scene.assets);
    CHECK(draw_with(fresh, *fresh_device, nullptr).pixels == shown.pixels);

    for (auto& device : slash::test::hardware_devices()) {
        CAPTURE(render::backend_name(device->backend()));
        canvas::CanvasRenderer renderer(*device, scene.assets);
        const render::Image image = draw_with(renderer, *device, nullptr);
        std::size_t different = 0;
        std::size_t lit = 0;
        for (std::size_t i = 0; i < image.pixels.size(); ++i) {
            different += std::abs(int{image.pixels[i]} - int{shown.pixels[i]}) > 12 ? 1U : 0U;
            lit += shown.pixels[i] > 60 ? 1U : 0U;
        }
        CHECK(different < (lit / 4) + 8);
    }
}

TEST_CASE("GPU particles collide with LightOccluder2D outlines and tile occluders") {
    Scene scene;
    scene.files.files["rain.process_material.json"] =
        R"({"format": 1, "direction": [0, 1], "spread": 0, "gravity": [0, 200],
            "initial_velocity_min": 50, "initial_velocity_max": 50, "collision_mode": "rigid"})";
    scene.files.files["drift.process_material.json"] =
        R"({"format": 1, "direction": [0, 1], "spread": 0, "gravity": [0, 200],
            "initial_velocity_min": 50, "initial_velocity_max": 50})";
    render::Image drop;
    drop.width = 6;
    drop.height = 10;
    drop.pixels.assign(std::size_t{6} * 10 * 4, 255);
    scene.files.files["drop.png"] = render::encode_png(drop).value();
    const entt::entity floor = scene.create("floor", R"({"Transform2D": {"position": [0, 80]},
        "LightOccluder2D": {"polygon": [[-100, 0], [100, 0], [100, 40], [-100, 40]]}})");
    const entt::entity rain = scene.create("rain", R"({"Transform2D": {},
        "GPUParticles2D": {"amount": 30, "collision_base_size": 2,
                           "process_material": "rain.process_material.json"}})");
    const entt::entity drift = scene.create("drift", R"({"Transform2D": {},
        "GPUParticles2D": {"amount": 30, "process_material": "drift.process_material.json"}})");
    const entt::entity spout = scene.create("spout", R"({"Transform2D": {"position": [10, 20],
        "scale": [2, 2]}, "GPUParticles2D": {"amount": 30, "texture": "drop.png",
        "local_coords": true, "process_material": "rain.process_material.json"}})");
    scene.step(60);

    // The floor where it is in the world, the same set for every step, and the emitter's size.
    const canvas::ParticleSteps kept = scene.steps(rain);
    REQUIRE(kept.shapes.size() == 1);
    CHECK(
        kept.shapes[0].points ==
        std::vector<Vec2>{{-100.0F, 80.0F}, {100.0F, 80.0F}, {100.0F, 120.0F}, {-100.0F, 120.0F}});
    CHECK(kept.shapes[0].closed);
    for (const render::ParticleStep& step : kept.steps) {
        CHECK(step.shape_first == 0);
        CHECK(step.shape_count == 1);
        CHECK(step.collision_size == 2.0F);
        CHECK(step.to_shapes == std::array<float, 6>{1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F});
    }
    // Its particles come to rest on the floor, a collision size above it, those reaching it in it
    // by up to a step's fall until their next step pushes them out.
    const auto rests_on = [](const render::Particle& particle, float rest, float arriving) {
        return particle.origin.y < rest ||
               (particle.velocity.y > arriving && particle.origin.y < rest + (arriving / 10.0F));
    };
    auto live = canvas::gpu_particles(scene.world, rain, scene.assets);
    REQUIRE(live.ok());
    int resting = 0;
    for (const render::Particle& particle : live.value()) {
        CHECK(rests_on(particle, 78.3F, 100.0F));
        resting += particle.origin.y > 77.5F && particle.origin.y < 78.3F ? 1 : 0;
    }
    CHECK(resting > 5);
    // A process material that doesn't collide carries no shapes, and falls through.
    CHECK(scene.steps(drift).shapes.empty());
    CHECK(std::ranges::all_of(scene.steps(drift).steps, [](const render::ParticleStep& step) {
        return step.shape_count == 0;
    }));
    live = canvas::gpu_particles(scene.world, drift, scene.assets);
    REQUIRE(live.ok());
    CHECK(std::ranges::any_of(
        live.value(), [](const render::Particle& particle) { return particle.origin.y > 90; }));
    // Particles in the emitter's coordinates reach the floor through its transform, and a
    // texture's half average side scales the size: 1 times (6 + 10) / 4.
    const canvas::ParticleSteps spouted = scene.steps(spout);
    const render::ParticleStep& local = spouted.steps.back();
    CHECK(local.to_shapes == std::array<float, 6>{2.0F, 0.0F, 0.0F, 2.0F, 10.0F, 20.0F});
    CHECK(local.from_shapes == std::array<float, 4>{0.5F, 0.0F, 0.0F, 0.5F});
    CHECK(local.collision_size == 4.0F);
    live = canvas::gpu_particles(scene.world, spout, scene.assets);
    REQUIRE(live.ok());
    for (const render::Particle& particle : live.value()) {
        // The floor is 30 of their pixels down, less 4 of size.
        CHECK(rests_on(particle, 26.3F, 50.0F));
    }

    // A moving floor adds a set of shapes each tick, and the old ones go with the steps that
    // named them.
    for (int i = 0; i < 120; ++i) {
        scene.world.ecs().get<world::Transform2D>(floor).position.y =
            80.0F + static_cast<float>(i % 7);
        scene.step();
    }
    CHECK(scene.steps(rain).shapes.size() <= 64);
    // With sdf_collision off, the floor is no shape.
    scene.world.ecs().get<world::LightOccluder2D>(floor).sdf_collision = false;
    scene.step(2); // a step every other tick, at 30 steps a second
    CHECK(scene.steps(rain).steps.back().shape_count == 0);

    // Tile occluders collide where their occlusion layer has sdf_collision.
    render::Image white;
    white.width = 16;
    white.height = 16;
    white.pixels.assign(std::size_t{16} * 16 * 4, 255);
    scene.files.files["white.png"] = render::encode_png(white).value();
    scene.files.files["walls.tileset.json"] = R"({"format": 1,
        "occlusion_layers": [{"sdf_collision": true}, {}],
        "sources": {"white": {"texture": "white.png", "tiles": {
            "0,0": {"name": "wall",
                    "occlusion": [{"points": [[-8, -8], [8, -8], [8, 8], [-8, 8]]},
                                  {"layer": 1, "points": [[-8, 0], [8, 0]], "closed": false}]}}}}})";
    scene.create("walls", R"({"Transform2D": {"position": [0, 100]},
        "TileMapLayer": {"tile_set": "walls.tileset.json", "legend": {"w": "wall"},
                         "rows": ["w.w"]}})");
    scene.step(2);
    const canvas::ParticleSteps tiled = scene.steps(rain);
    const render::ParticleStep& last = tiled.steps.back();
    REQUIRE(last.shape_count == 2);
    CHECK(tiled.shapes[last.shape_first].points ==
          std::vector<Vec2>{{0.0F, 100.0F}, {16.0F, 100.0F}, {16.0F, 116.0F}, {0.0F, 116.0F}});
    CHECK(tiled.shapes[last.shape_first + 1].points ==
          std::vector<Vec2>{{32.0F, 100.0F}, {48.0F, 100.0F}, {48.0F, 116.0F}, {32.0F, 116.0F}});
}
