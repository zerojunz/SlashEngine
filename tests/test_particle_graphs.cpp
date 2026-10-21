#include "devices.hpp"

#include "slash/canvas/gpu_particles.hpp"
#include "slash/core/json.hpp"
#include "slash/core/math.hpp"
#include "slash/render/device.hpp"
#include "slash/render/material.hpp"
#include "slash/render/particles.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace canvas = slash::canvas;
namespace json = slash::json;
namespace render = slash::render;
using render::Particle;
using render::ParticleProcess;
using render::ParticleStep;
using slash::Vec2;

namespace {

json::Value parse(std::string_view text) {
    auto value = json::parse(text);
    REQUIRE(value.ok());
    return std::move(value).value();
}

ParticleProcess process_of(std::string_view text) {
    auto parsed = canvas::parse_process_material(parse(text));
    REQUIRE_MESSAGE(parsed.ok(), (parsed.ok() ? std::string() : parsed.error().message));
    return std::move(parsed).value();
}

// "path: message" for each of a process material's mistakes.
std::vector<std::string> errors_of(std::string_view text) {
    std::vector<std::string> out;
    for (const json::ValidationError& error : canvas::validate_process_material(parse(text))) {
        out.push_back(error.path + ": " + error.message);
    }
    return out;
}

// Steps of a system emitting 30 particles a second with a lifetime of a second, from (50, 60).
std::vector<ParticleStep> run_steps(int count) {
    std::vector<ParticleStep> steps;
    double phase = 0.0;
    std::uint32_t cycle = 0;
    for (int i = 0; i < count; ++i) {
        ParticleStep step;
        step.delta = 1.0F / 30.0F;
        step.lifetime = 1.0F;
        step.prev_system_phase = static_cast<float>(phase);
        phase += step.delta / step.lifetime;
        if (phase >= 1.0) {
            phase -= 1.0;
            ++cycle;
        }
        step.system_phase = static_cast<float>(phase);
        step.cycle = cycle;
        step.seed = 77;
        step.emission = {1.0F, 0.0F, 0.0F, 1.0F, 50.0F, 60.0F};
        step.clear = i == 0;
        step.time = static_cast<float>(i + 1) * step.delta;
        steps.push_back(step);
    }
    return steps;
}

std::vector<Particle> run(const ParticleProcess& process, int steps, std::uint32_t amount = 30) {
    std::vector<Particle> particles(amount);
    render::process_particles(particles, process, run_steps(steps));
    return particles;
}

float age_of(const Particle& particle) {
    return particle.custom.y; // the steps' lifetime is a second
}

// Whether two particles agree to within what different float units give.
bool close(const Particle& a, const Particle& b) {
    if (a.flags != b.flags) {
        return false;
    }
    if (!a.active()) {
        return true;
    }
    const auto near = [](std::initializer_list<float> x, std::initializer_list<float> y) {
        float apart = 0.0F;
        float size = 0.0F;
        for (auto i = x.begin(), j = y.begin(); i != x.end(); ++i, ++j) {
            apart += (*i - *j) * (*i - *j);
            size += *i * *i;
        }
        return std::sqrt(apart) <= 0.02F * std::max(1.0F, std::sqrt(size));
    };
    return near({a.basis.x, a.basis.y, a.basis.z, a.basis.w},
                {b.basis.x, b.basis.y, b.basis.z, b.basis.w}) &&
           near({a.origin.x, a.origin.y}, {b.origin.x, b.origin.y}) &&
           near({a.velocity.x, a.velocity.y}, {b.velocity.x, b.velocity.y}) &&
           near({a.color.x, a.color.y, a.color.z, a.color.w},
                {b.color.x, b.color.y, b.color.z, b.color.w}) &&
           near({a.custom.x, a.custom.z}, {b.custom.x, b.custom.z});
}

} // namespace

TEST_CASE("portable exp and ln agree with the C library's to the last few bits") {
    const auto within = [](float ours, double exact) {
        return std::abs(static_cast<double>(ours) - exact) <=
               4.0 * std::numeric_limits<float>::epsilon() * std::abs(exact);
    };
    for (float x = -87.0F; x <= 88.0F; x += 0.37F) {
        CAPTURE(x);
        CHECK(within(slash::exp(x), std::exp(static_cast<double>(x))));
    }
    for (float x = 1.0e-30F; x < 1.0e30F; x *= 3.7F) {
        CAPTURE(x);
        CHECK(within(slash::ln(x), std::log(static_cast<double>(x))));
    }
    CHECK(slash::exp(0.0F) == 1.0F);
    CHECK(slash::ln(1.0F) == 0.0F);
    CHECK(std::isinf(slash::exp(100.0F)));
    CHECK(slash::exp(-200.0F) == 0.0F);
    CHECK(slash::ln(0.0F) == -std::numeric_limits<float>::infinity());
    CHECK(std::isnan(slash::ln(-1.0F)));
    CHECK(std::isnan(slash::exp(std::numeric_limits<float>::quiet_NaN())));
}

TEST_CASE("a process graph changes the step's velocity and color, and the change carries on") {
    // Pushed right at 30 pixels per second squared, with nothing else moving them.
    const ParticleProcess process = process_of(R"({
      "format": 1,
      "gravity": [0, 0],
      "process": {
        "velocity": {"op": "add", "args": ["velocity", {"op": "mul", "args": [[30, 0], "delta"]}]},
        "color": [1, 0, 0, 1]
      }
    })");
    CHECK(process.process_program.outputs ==
          (render::kParticleOutVelocity | render::kParticleOutColor));
    CHECK(process.start_program.outputs == 0);
    int live = 0;
    for (const Particle& particle : run(process, 20)) {
        if (!particle.active()) {
            continue;
        }
        ++live;
        CAPTURE(age_of(particle));
        CHECK(particle.velocity.x == doctest::Approx(30.0F * age_of(particle)).epsilon(1e-3));
        CHECK(particle.velocity.y == 0.0F);
        CHECK(particle.userdata.x == doctest::Approx(particle.velocity.x));
        // Moved by each step's velocity, which grows a step at a time.
        CHECK(particle.origin.x > 50.0F);
        CHECK(particle.origin.x < 50.0F + (15.0F * age_of(particle) * age_of(particle)) + 1.0F);
        CHECK(particle.origin.y == 60.0F);
        CHECK(particle.color.x == 1.0F);
        CHECK(particle.color.y == 0.0F);
        CHECK(particle.color.w == 1.0F);
    }
    CHECK(live == 20);
}

TEST_CASE("a process graph's position places the particle instead of its velocity moving it") {
    const ParticleProcess process = process_of(R"({
      "format": 1,
      "gravity": [0, 0],
      "spread": 0,
      "initial_velocity_min": 100,
      "initial_velocity_max": 100,
      "process": {
        "position": {"op": "add", "args": ["emitter_position",
                                           {"op": "vec2", "args": [{"op": "mul", "args": ["age", 10]}, 0]}]}
      }
    })");
    for (const Particle& particle : run(process, 20)) {
        if (!particle.active()) {
            continue;
        }
        CHECK(particle.origin.x == doctest::Approx(50.0F + (10.0F * age_of(particle))));
        CHECK(particle.origin.y == 60.0F);
        CHECK(particle.velocity.x == doctest::Approx(100.0F));
    }
}

TEST_CASE("a process graph turns, sizes, animates and ends particles") {
    const std::string text = R"({
      "format": 1,
      "gravity": [0, 0],
      "particle_flag_align_y": true,
      "initial_velocity_min": 20,
      "initial_velocity_max": 20,
      "process": {
        "angle": 0.5,
        "scale": {"op": "mul", "args": ["scale", 2]},
        "frame": "life",
        "active": {"op": "step", "args": ["age", 0.5]}
      }
    })";
    const ParticleProcess process = process_of(text);
    int live = 0;
    for (const Particle& particle : run(process, 40)) {
        if (!particle.active()) {
            continue;
        }
        ++live;
        CHECK(age_of(particle) <= 0.5F);
        CHECK(particle.custom.x == 0.5F);
        CHECK(particle.custom.z == doctest::Approx(age_of(particle) / particle.custom.w));
        // The angle given turns it, not its velocity, though it aligns along it.
        const Vec2 turn = slash::cos_sin(0.5F);
        CHECK(particle.basis.x == doctest::Approx(turn.x * 2.0F));
        CHECK(particle.basis.y == doctest::Approx(-turn.y * 2.0F));
        CHECK(particle.basis.z == doctest::Approx(turn.y * 2.0F));
    }
    // Those older than half a second have gone.
    CHECK(live >= 14);
    CHECK(live <= 16);
}

TEST_CASE("a start graph places particles, sets their velocity and leaves some out") {
    const ParticleProcess process = process_of(R"({
      "format": 1,
      "gravity": [0, 0],
      "initial_velocity_min": 50,
      "initial_velocity_max": 80,
      "start": {
        "position": {"op": "add", "args": ["emitter_position",
                                           {"op": "vec2", "args": [{"op": "mul", "args": ["index", 4]}, 0]}]},
        "velocity": [0, -20],
        "active": {"op": "step", "args": [0.5, "random.x"]}
      },
      "process": {"color": {"op": "vec4", "args": ["random.xyz", 1]}}
    })");
    CHECK(process.start_program.outputs == render::kParticleStartOutputs);
    std::vector<Particle> early = run(process, 20, 20);
    std::vector<Particle> later = run(process, 25, 20);
    int live = 0;
    for (std::size_t i = 0; i < early.size(); ++i) {
        if (!early[i].active()) {
            continue;
        }
        ++live;
        CAPTURE(i);
        CHECK(early[i].origin.x == 50.0F + (4.0F * static_cast<float>(i)));
        CHECK(early[i].origin.y == doctest::Approx(60.0F - (20.0F * age_of(early[i]))));
        CHECK(early[i].velocity.x == 0.0F);
        CHECK(early[i].velocity.y == -20.0F);
        // Its random numbers stay the same for its life.
        if (later[i].active() && age_of(later[i]) > age_of(early[i])) {
            CHECK(later[i].color.x == early[i].color.x);
            CHECK(later[i].color.z == early[i].color.z);
        }
    }
    // About half are left out.
    CHECK(live >= 4);
    CHECK(live <= 16);
}

TEST_CASE("the GPU device runs particle graphs as the CPU twin does") {
    auto device = slash::test::gpu_device();
    if (device == nullptr) {
        return;
    }
    // Every kind of op, on what each stage reads.
    const ParticleProcess process = process_of(R"({
      "format": 1,
      "emission_shape": "sphere",
      "emission_sphere_radius": 8,
      "initial_velocity_min": 20,
      "initial_velocity_max": 60,
      "spread": 60,
      "angle_max": 90,
      "params": {"swirl": {"type": "float", "value": 3}},
      "nodes": {
        "out": {"op": "sub", "args": ["position", "emitter_position"]},
        "around": {"op": "vec2", "args": [{"op": "negate", "args": ["out.y"]}, "out.x"]},
        "wave": {"op": "sin", "args": [{"op": "add", "args": [{"op": "mul", "args": ["time", 4]}, "index"]}]}
      },
      "start": {
        "velocity": {"op": "add", "args": ["velocity", {"op": "mul", "args": [{"op": "sub", "args": ["random.xy", 0.5]}, 30]}]},
        "active": {"op": "step", "args": [0.1, "random.w"]}
      },
      "process": {
        "velocity": {"op": "add", "args": ["velocity",
          {"op": "mul", "args": [{"op": "normalize", "args": ["around"]}, {"op": "mul", "args": ["swirl", "delta"]}]}]},
        "color": {"op": "vec4", "args": [
          {"op": "hsv_to_rgb", "args": [{"op": "vec3", "args": [{"op": "fract", "args": [{"op": "mul", "args": ["life", 0.7]}]}, 0.8, 1]}]},
          {"op": "smoothstep", "args": [1, 0.6, "life"]}]},
        "angle": {"op": "atan", "args": ["velocity.y", "velocity.x"]},
        "scale": {"op": "mix", "args": [1, {"op": "pow", "args": [{"op": "exp", "args": [{"op": "negate", "args": ["age"]}]}, 2]}, 0.5]},
        "frame": {"op": "clamp", "args": [{"op": "add", "args": [{"op": "mul", "args": ["wave", 0.5]}, 0.5]}, 0, 1]},
        "active": {"op": "step", "args": [{"op": "length", "args": ["out"]}, 200]}
      }
    })");
    std::vector<ParticleStep> steps = run_steps(45);
    for (std::size_t i = 30; i < steps.size(); ++i) {
        steps[i].emission = {0.0F, 2.0F, -1.0F, 0.0F, 70.0F, 40.0F};
        steps[i].emitter_velocity = {30.0F, -10.0F};
    }
    constexpr std::uint32_t kAmount = 60;
    std::vector<Particle> twin(kAmount);
    render::process_particles(twin, process, steps);
    auto system = device->create_particles(kAmount);
    REQUIRE(system.ok());
    REQUIRE(device->process_particles(system.value(), process, std::span(steps).first(20)).ok());
    REQUIRE(device->process_particles(system.value(), process, std::span(steps).subspan(20)).ok());
    auto read = device->read_particles(system.value());
    REQUIRE(read.ok());
    int live = 0;
    for (std::uint32_t i = 0; i < kAmount; ++i) {
        live += twin[i].active() ? 1 : 0;
        CHECK_MESSAGE(close(read.value()[i], twin[i]), "particle ", i);
    }
    CHECK(live > 20);
    device->destroy_particles(system.value());
}

TEST_CASE("every device refuses particle graphs it can't run") {
    std::vector<std::unique_ptr<render::RenderDevice>> devices = slash::test::drawing_devices();
    devices.push_back(render::create_null_device());
    for (auto& device : devices) {
        CAPTURE(render::backend_name(device->backend()));
        const auto error_of = [](const auto& result) {
            return result.ok() ? std::string("(succeeded)") : result.error().message;
        };
        auto system = device->create_particles(4);
        REQUIRE(system.ok());
        ParticleProcess process;
        process.start_program.outputs = render::kParticleOutColor;
        CHECK(error_of(device->process_particles(system.value(), process, {})) ==
              "the start graph changes only position, velocity and active");
        process.start_program = {};
        process.process_program.outputs = render::kParticleOutColor;
        process.process_program.program.code = {
            render::instruction(render::Op::Sample, render::kParticleOutRegister + 1,
                                render::operand(render::kParticleMotionRegister))};
        CHECK(error_of(device->process_particles(system.value(), process, {})) ==
              "the process graph: instruction 0 samples a texture, which this program has none "
              "of");
        process.process_program.program.code = {
            render::instruction(render::Op::Mov, render::kParticleOutRegister + 1,
                                render::operand(render::kParticleFirstFreeRegister))};
        CHECK(error_of(device->process_particles(system.value(), process, {}))
                  .starts_with("the process graph: instruction 0 reads register 11"));
        device->destroy_particles(system.value());
    }
}

TEST_CASE("process material graphs name Godot's particle built-ins and report each mistake") {
    // A graph's stage names what it changes; params are numbers it reads.
    const ParticleProcess process = process_of(R"({
      "format": 1,
      "params": {"push": {"type": "vec2", "value": [10, 0], "doc": "Wind."}},
      "process": {"velocity": {"op": "add", "args": ["velocity", "push"]}}
    })");
    REQUIRE(process.process_program.outputs == render::kParticleOutVelocity);
    REQUIRE(render::check_program(process.process_program.program,
                                  render::kParticleFirstFreeRegister, false)
                .ok());

    CHECK(errors_of(R"({"format": 1, "process": {"velocity": "velocty"}})") ==
          std::vector<std::string>{
              R"(/process/velocity: no node, param or input "velocty"; did you mean "velocity"?)"});
    CHECK(
        errors_of(R"({"format": 1, "process": {"position": "age"}})") ==
        std::vector<std::string>{
            R"(/process/position: position is a vec2, not a float; make one, as {"op": "vec2", "args": [x]})"});
    // What start() runs before is refused there, where a node uses it too.
    CHECK(errors_of(R"({"format": 1,
                        "nodes": {"hit": {"op": "mul", "args": ["collision_normal", 5]}},
                        "start": {"velocity": {"op": "vec2", "args": ["age", 0]}},
                        "process": {"velocity": "hit"}})") ==
          std::vector<std::string>{
              R"(/start/velocity/args/0: "age" is only known in process, not start)"});
    CHECK(errors_of(R"({"format": 1,
                        "nodes": {"hit": {"op": "mul", "args": ["collision_normal", 5]}},
                        "start": {"velocity": "hit"}})") ==
          std::vector<std::string>{
              R"(/nodes/hit/args/0: "collision_normal" is only known in process, not start)"});
    // Unused nodes are checked too.
    CHECK(errors_of(R"({"format": 1, "nodes": {"spare": {"op": "sine", "args": [1]}}})") ==
          std::vector<std::string>{R"(/nodes/spare/op: no op "sine"; did you mean "sin"?)"});
    // A graph has no textures.
    CHECK_FALSE(errors_of(R"({"format": 1, "params": {"t": {"type": "texture"}}})").empty());
    CHECK(
        errors_of(
            R"({"format": 1, "process": {"color": {"op": "texture", "args": ["texture", "position"]}}})") ==
        std::vector<std::string>{R"(/process/color/args/0: no node, param or input "texture")"});
    // Start changes only where a particle is, how it moves and whether it lives.
    const std::vector<std::string> color_at_start =
        errors_of(R"({"format": 1, "start": {"color": [1, 0, 0, 1]}})");
    REQUIRE(color_at_start.size() == 1);
    CHECK(color_at_start.front().starts_with("/start/color:"));
}

TEST_CASE("process material schemas describe particle graphs") {
    const json::Value schema = canvas::process_material_schema();
    const json::Value* start = schema.find("properties")->find("start");
    REQUIRE(start != nullptr);
    CHECK(start->find("properties")->as_object().size() == 3);
    CHECK(schema.find("properties")->find("process")->find("properties")->as_object().size() == 7);
    // The ops are listed for whoever writes a graph.
    const json::Value* op =
        schema.find("$defs")->find("node")->find("properties")->find("op")->find("enum");
    REQUIRE(op != nullptr);
    CHECK(op->as_array().size() == 40);
}
