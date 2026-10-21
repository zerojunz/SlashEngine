#include "devices.hpp"

#include "slash/render/device.hpp"
#include "slash/render/particles.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <span>
#include <string>
#include <vector>

using slash::Vec2;
using slash::Vec4;
using slash::render::EmissionShape;
using slash::render::Particle;
using slash::render::ParticleCollision;
using slash::render::ParticleContact;
using slash::render::ParticleDraw;
using slash::render::ParticleOrder;
using slash::render::ParticleParam;
using slash::render::ParticleProcess;
using slash::render::ParticleShape;
using slash::render::ParticlesId;
using slash::render::ParticleStep;
using slash::render::RenderDevice;
using slash::render::Vertex2D;

namespace {

void set_param(ParticleProcess& process, ParticleParam param, float min, float max) {
    process.param_min.at(static_cast<std::size_t>(param)) = min;
    process.param_max.at(static_cast<std::size_t>(param)) = max;
}

// A process material that uses most of what one can do.
ParticleProcess busy_process() {
    ParticleProcess process;
    process.emission_shape = EmissionShape::Sphere;
    process.emission_sphere_radius = 6.0F;
    process.direction = {0.0F, -1.0F};
    process.spread = 30.0F;
    process.gravity = {0.0F, 40.0F};
    set_param(process, ParticleParam::InitialVelocity, 40.0F, 80.0F);
    set_param(process, ParticleParam::AngularVelocity, -90.0F, 90.0F);
    set_param(process, ParticleParam::OrbitVelocity, 0.1F, 0.2F);
    set_param(process, ParticleParam::LinearAccel, -5.0F, 5.0F);
    set_param(process, ParticleParam::RadialAccel, 2.0F, 4.0F);
    set_param(process, ParticleParam::TangentialAccel, -3.0F, 3.0F);
    set_param(process, ParticleParam::Damping, 1.0F, 3.0F);
    set_param(process, ParticleParam::Angle, 0.0F, 360.0F);
    set_param(process, ParticleParam::Scale, 0.5F, 1.5F);
    set_param(process, ParticleParam::HueVariation, -0.1F, 0.1F);
    set_param(process, ParticleParam::AnimSpeed, 1.0F, 2.0F);
    set_param(process, ParticleParam::RadialVelocity, 1.0F, 2.0F);
    process.param_curve.at(static_cast<std::size_t>(ParticleParam::Scale)) = {{0.0F, 1.0F},
                                                                              {1.0F, 0.2F}};
    process.alpha_curve = {{0.0F, 0.0F}, {0.2F, 1.0F}, {1.0F, 0.0F}};
    process.color = {1.0F, 0.8F, 0.4F, 1.0F};
    process.color_ramp = {{.offset = 0.0F, .color = {1.0F, 1.0F, 1.0F, 1.0F}},
                          {.offset = 1.0F, .color = {0.5F, 0.1F, 0.0F, 1.0F}}};
    process.lifetime_randomness = 0.3F;
    return process;
}

// Steps of a system emitting for a while at 30 per second, with a lifetime of a second.
std::vector<ParticleStep> run_steps(int count, float randomness, float explosiveness) {
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
        step.seed = 1234;
        step.randomness = randomness;
        step.explosiveness = explosiveness;
        step.emission = {1.0F, 0.0F, 0.0F, 1.0F, 50.0F, 60.0F};
        step.clear = i == 0;
        step.time = static_cast<float>(i + 1) * step.delta;
        steps.push_back(step);
    }
    return steps;
}

// A sub-emitter's steps beside a system's: the same ticks with its own lifetime and seed, not
// emitting, as GPUParticles2D runs a sub-emitter with its parent.
std::vector<ParticleStep> sub_steps(std::span<const ParticleStep> steps, float lifetime) {
    std::vector<ParticleStep> out(steps.begin(), steps.end());
    for (ParticleStep& step : out) {
        step.lifetime = lifetime;
        step.seed = 99;
        step.emitting = false;
    }
    return out;
}

std::size_t live_count(std::span<const Particle> particles) {
    return static_cast<std::size_t>(std::count_if(particles.begin(), particles.end(),
                                                  [](const Particle& p) { return p.active(); }));
}

std::string describe(const Particle& p) {
    return "flags " + std::to_string(p.flags) + " origin " + std::to_string(p.origin.x) + "," +
           std::to_string(p.origin.y) + " color " + std::to_string(p.color.x) + "," +
           std::to_string(p.color.w) + " custom " + std::to_string(p.custom.x) + "," +
           std::to_string(p.custom.y) + " velocity " + std::to_string(p.velocity.x) + "," +
           std::to_string(p.velocity.y) + " userdata " + std::to_string(p.userdata.x) + "," +
           std::to_string(p.userdata.y) + "," + std::to_string(p.userdata.z) + " basis " +
           std::to_string(p.basis.x) + "," + std::to_string(p.basis.y);
}

// Whether two particles agree to within what different float units give: each vector within 2%
// of its length (or of 1), so turbulence turning a fast particle may move its small components
// a little more than 2% of themselves.
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
    return near({a.basis.x, a.basis.y}, {b.basis.x, b.basis.y}) &&
           near({a.basis.z, a.basis.w}, {b.basis.z, b.basis.w}) &&
           near({a.origin.x, a.origin.y}, {b.origin.x, b.origin.y}) &&
           near({a.velocity.x, a.velocity.y}, {b.velocity.x, b.velocity.y}) &&
           near({a.color.x, a.color.y, a.color.z, a.color.w},
                {b.color.x, b.color.y, b.color.z, b.color.w}) &&
           near({a.custom.x}, {b.custom.x}) && near({a.custom.y}, {b.custom.y}) &&
           near({a.custom.z}, {b.custom.z}) && near({a.custom.w}, {b.custom.w}) &&
           near({a.userdata.x, a.userdata.y, a.userdata.z},
                {b.userdata.x, b.userdata.y, b.userdata.z}) &&
           near({a.userdata.w}, {b.userdata.w});
}

// Process materials that between them use every shape and option.
std::vector<ParticleProcess> varied_processes() {
    std::vector<ParticleProcess> processes{busy_process()};
    ParticleProcess box = busy_process();
    box.emission_shape = EmissionShape::Box;
    box.emission_box_extents = {20.0F, 4.0F};
    box.align_y = true;
    box.damping_as_friction = true;
    box.velocity_limit_curve = {{0.0F, 60.0F}, {1.0F, 10.0F}};
    box.emission_curve = {{0.0F, 0.5F}, {1.0F, 0.0F}};
    box.color_initial_ramp = {{.offset = 0.0F, .color = {1.0F, 0.0F, 0.0F, 1.0F}},
                              {.offset = 0.5F, .color = {0.0F, 1.0F, 0.0F, 1.0F}},
                              {.offset = 1.0F, .color = {0.0F, 0.0F, 1.0F, 0.5F}}};
    box.param_curve.at(static_cast<std::size_t>(ParticleParam::AngularVelocity)) = {{0.0F, 1.0F},
                                                                                    {1.0F, -1.0F}};
    processes.push_back(box);
    ParticleProcess points = busy_process();
    points.emission_shape = EmissionShape::DirectedPoints;
    points.emission_points = {{-10.0F, 0.0F}, {10.0F, 0.0F}, {0.0F, 8.0F}};
    points.emission_normals = {{1.0F, 0.0F}, {0.0F, 1.0F}};
    points.emission_colors = {{1.0F, 0.0F, 0.0F, 1.0F}};
    points.inherit_velocity_ratio = 0.5F;
    processes.push_back(points);
    ParticleProcess ring = busy_process();
    ring.emission_shape = EmissionShape::Ring;
    ring.emission_ring_radius = 12.0F;
    ring.emission_ring_inner_radius = 6.0F;
    ring.emission_shape_offset = {3.0F, -2.0F};
    ring.emission_shape_scale = {2.0F, 1.0F};
    ring.velocity_pivot = {5.0F, 5.0F};
    set_param(ring, ParticleParam::RadialVelocity, -20.0F, -10.0F);
    set_param(ring, ParticleParam::Scale, -1.0F, 1.0F);
    processes.push_back(ring);
    // Turbulence, with Godot's strength (a curl from close differences) and a stronger one. Strong
    // turbulence at a high frequency is chaotic, and an influence near a half all but stops a
    // particle the noise points against, so a GPU's rounding grows step by step there; these stay
    // where it doesn't.
    ParticleProcess turbulent = busy_process();
    turbulent.turbulence_enabled = true;
    turbulent.turbulence_noise_speed = {0.5F, -0.25F, 0.1F};
    set_param(turbulent, ParticleParam::TurbulenceInfluence, 0.1F, 0.3F);
    set_param(turbulent, ParticleParam::TurbulenceDisplacement, -4.0F, 4.0F);
    turbulent.param_curve.at(static_cast<std::size_t>(ParticleParam::TurbulenceInfluence)) = {
        {0.0F, 0.5F}, {1.0F, 1.0F}};
    processes.push_back(turbulent);
    ParticleProcess stormy = turbulent;
    stormy.turbulence_noise_strength = 12.0F;
    stormy.turbulence_noise_speed_random = 1.0F;
    stormy.velocity_limit_curve = {{0.0F, 70.0F}, {1.0F, 30.0F}};
    processes.push_back(stormy);
    return processes;
}

} // namespace

TEST_CASE("the GPU device runs particles as the CPU twin does") {
    auto device = slash::test::gpu_device();
    if (device == nullptr) {
        return;
    }
    constexpr std::uint32_t kAmount = 100;
    std::vector<ParticleStep> steps = run_steps(45, 0.5F, 0.1F);
    for (std::size_t i = 30; i < steps.size(); ++i) {
        // Turned, scaled and moving, with the last steps ending early.
        steps[i].emission = {0.0F, 2.0F, -1.0F, 0.0F, 70.0F, 40.0F};
        steps[i].emitter_velocity = {30.0F, -10.0F};
        steps[i].interp_to_end = i > 40 ? 0.25F : 0.0F;
        steps[i].amount_ratio = 0.7F;
    }
    int which = 0;
    for (const ParticleProcess& process : varied_processes()) {
        const int index = which++;
        CAPTURE(index);
        CAPTURE(static_cast<int>(process.emission_shape));
        std::vector<Particle> twin(kAmount);
        slash::render::process_particles(twin, process, steps);

        auto system = device->create_particles(kAmount);
        REQUIRE(system.ok());
        // In two calls, as a game's ticks would be.
        REQUIRE(
            device->process_particles(system.value(), process, std::span(steps).first(20)).ok());
        REQUIRE(
            device->process_particles(system.value(), process, std::span(steps).subspan(20)).ok());
        auto read = device->read_particles(system.value());
        REQUIRE(read.ok());
        REQUIRE(read.value().size() == kAmount);
        int live = 0;
        for (std::uint32_t i = 0; i < kAmount; ++i) {
            live += twin[i].active() ? 1 : 0;
            CHECK_MESSAGE(close(read.value()[i], twin[i]), "particle ", i, ": GPU ",
                          describe(read.value()[i]), ", twin ", describe(twin[i]));
        }
        CHECK(live > 20);
        device->destroy_particles(system.value());
    }
}

TEST_CASE("turbulence turns particles along Godot's curl noise") {
    using slash::render::process_particles;
    using slash::render::turbulence_frequency;
    // Godot's noise scale slider: a frequency of -4 at 0, 0 at 10, by fourth roots between.
    CHECK(turbulence_frequency(0.0F) == doctest::Approx(-4.0));
    CHECK(turbulence_frequency(10.0F) == doctest::Approx(0.0).epsilon(1e-6));
    CHECK(turbulence_frequency(9.0F) ==
          doctest::Approx((std::pow(9.0, 0.25) * 4.0 / std::pow(10.0, 0.25)) - 4.0));

    // Particles at 50 pixels a second to the right, with nothing else pulling on them.
    ParticleProcess process;
    process.direction = {1.0F, 0.0F};
    process.spread = 0.0F;
    process.gravity = {};
    set_param(process, ParticleParam::InitialVelocity, 50.0F, 50.0F);
    process.turbulence_enabled = true;
    process.turbulence_noise_speed = {0.0F, 1.0F, 0.0F};
    const std::vector<ParticleStep> steps = run_steps(20, 0.0F, 0.0F);
    const auto run = [&](const ParticleProcess& with, std::span<const ParticleStep> run_steps) {
        std::vector<Particle> particles(30);
        process_particles(particles, with, run_steps);
        return particles;
    };

    // With no influence they go straight.
    set_param(process, ParticleParam::TurbulenceInfluence, 0.0F, 0.0F);
    for (const Particle& particle : run(process, steps)) {
        if (particle.active()) {
            // (50 to 50 at random can round to 50.000004.)
            CHECK(particle.velocity.x == doctest::Approx(50.0));
            CHECK(particle.velocity.y == 0.0F);
            CHECK(particle.origin.y == 60.0F);
        }
    }
    // With all of it they go the noise's way at the same speed, counting the speed it turned
    // out of the screen, and the noise differs from place to place.
    set_param(process, ParticleParam::TurbulenceInfluence, 1.0F, 1.0F);
    const std::vector<Particle> turned = run(process, steps);
    int live = 0;
    float lowest = 0.0F;
    float highest = 0.0F;
    for (const Particle& particle : turned) {
        if (!particle.active()) {
            continue;
        }
        ++live;
        const Vec4 built = particle.userdata;
        CHECK(std::sqrt((built.x * built.x) + (built.y * built.y) + (built.z * built.z)) ==
              doctest::Approx(50.0).epsilon(1e-4));
        CHECK(particle.velocity == Vec2{built.x, built.y});
        lowest = std::min(lowest, particle.velocity.y);
        highest = std::max(highest, particle.velocity.y);
    }
    CHECK(live > 10);
    CHECK(highest - lowest > 10.0F);
    // The noise moves with the steps' time.
    std::vector<ParticleStep> later = steps;
    for (ParticleStep& step : later) {
        step.time += 100.0F;
    }
    const std::vector<Particle> moved = run(process, later);
    int differ = 0;
    for (std::size_t i = 0; i < turned.size(); ++i) {
        if (turned[i].active() && std::abs(moved[i].velocity.y - turned[i].velocity.y) > 1.0F) {
            ++differ;
        }
    }
    CHECK(differ > 5);
    // The same steps make the same particles.
    const std::vector<Particle> again = run(process, steps);
    for (std::size_t i = 0; i < turned.size(); ++i) {
        CHECK(again[i].flags == turned[i].flags);
        CHECK(again[i].origin == turned[i].origin);
        CHECK(again[i].userdata == turned[i].userdata);
    }

    // The displacement moves particles where they start, by up to the noise direction's length:
    // 0.9 at a strength of 9, as Godot's.
    set_param(process, ParticleParam::TurbulenceInfluence, 0.0F, 0.0F);
    set_param(process, ParticleParam::TurbulenceDisplacement, 10.0F, 10.0F);
    process.turbulence_noise_strength = 9.0F;
    int displaced = 0;
    for (const Particle& particle : run(process, steps)) {
        if (particle.active()) {
            CHECK(std::abs(particle.origin.y - 60.0F) <= 9.001F);
            displaced += std::abs(particle.origin.y - 60.0F) > 0.5F ? 1 : 0;
        }
    }
    CHECK(displaced > 5);
}

TEST_CASE("particles collide with shapes as Godot's do with its distance field") {
    using slash::render::particle_contact;
    using slash::render::process_particles;
    // A solid box from (0, 0) to (100, 20).
    const std::vector<ParticleShape> box{
        {.points = {{0.0F, 0.0F}, {100.0F, 0.0F}, {100.0F, 20.0F}, {0.0F, 20.0F}}, .closed = true}};
    CHECK_FALSE(particle_contact({50.0F, -10.0F}, 4.0F, box).collided);
    // Nearer the top than its size: out upwards by the size less the distance.
    const ParticleContact above = particle_contact({50.0F, -3.0F}, 4.0F, box);
    CHECK(above.collided);
    CHECK(above.normal == Vec2{0.0F, -1.0F});
    CHECK(above.depth == doctest::Approx(1.0));
    // Inside, nearest the bottom: out through it by the distance and the size.
    const ParticleContact inside = particle_contact({30.0F, 15.0F}, 2.0F, box);
    CHECK(inside.collided);
    CHECK(inside.normal == Vec2{0.0F, 1.0F});
    CHECK(inside.depth == doctest::Approx(7.0));
    // Past a corner: out along the way from it.
    const ParticleContact corner = particle_contact({103.0F, -4.0F}, 6.0F, box);
    CHECK(corner.collided);
    CHECK(corner.normal.x == doctest::Approx(0.6));
    CHECK(corner.normal.y == doctest::Approx(-0.8));
    CHECK(corner.depth == doctest::Approx(1.0));
    // An open outline is a line, solid on neither side.
    const std::vector<ParticleShape> line{
        {.points = {{0.0F, 0.0F}, {100.0F, 0.0F}}, .closed = false}};
    const ParticleContact under = particle_contact({50.0F, 3.0F}, 4.0F, line);
    CHECK(under.collided);
    CHECK(under.normal == Vec2{0.0F, 1.0F});
    CHECK(under.depth == doctest::Approx(1.0));
    CHECK_FALSE(particle_contact({50.0F, 5.0F}, 4.0F, line).collided);
    // On it: out to the left of the way it runs.
    CHECK(particle_contact({50.0F, 0.0F}, 1.0F, line).normal == Vec2{0.0F, -1.0F});

    // Particles falling from (50, 60) onto a floor at 80.
    ParticleProcess process;
    process.direction = {0.0F, 1.0F};
    process.spread = 0.0F;
    process.gravity = {0.0F, 200.0F};
    set_param(process, ParticleParam::InitialVelocity, 50.0F, 50.0F);
    const std::vector<ParticleShape> floor{
        {.points = {{-1000.0F, 80.0F}, {1000.0F, 80.0F}, {1000.0F, 200.0F}, {-1000.0F, 200.0F}},
         .closed = true}};
    std::vector<ParticleStep> steps = run_steps(40, 0.0F, 0.0F);
    for (ParticleStep& step : steps) {
        step.shape_count = 1;
        step.collision_size = 2.0F;
    }
    const auto run = [&](const ParticleProcess& with, std::span<const ParticleStep> run_steps) {
        std::vector<Particle> particles(30);
        process_particles(particles, with, run_steps, floor);
        return particles;
    };
    // Without collision they fall through, and the shapes change nothing.
    int below = 0;
    for (const Particle& particle : run(process, steps)) {
        below += particle.active() && particle.origin.y > 100.0F ? 1 : 0;
    }
    CHECK(below > 5);
    // Rigid ones with no bounce come to rest two pixels above it, gravity pulling them back in by
    // a step's fall at most. One that has just reached it is in it by up to a step's fall at full
    // speed, as Godot's are until their next step pushes them out.
    const auto rests_on = [](const Particle& particle, float rest, float arriving) {
        return particle.origin.y < rest ||
               (particle.velocity.y > arriving && particle.origin.y < rest + (arriving / 10.0F));
    };
    process.collision_mode = ParticleCollision::Rigid;
    int resting = 0;
    for (const Particle& particle : run(process, steps)) {
        if (particle.active()) {
            CHECK(rests_on(particle, 78.3F, 90.0F));
            resting += particle.origin.y > 77.5F && particle.origin.y < 78.3F ? 1 : 0;
        }
    }
    CHECK(resting > 5);
    // Bouncing ones go back up.
    process.collision_bounce = 1.0F;
    int rising = 0;
    for (const Particle& particle : run(process, steps)) {
        rising += particle.active() && particle.velocity.y < -20.0F ? 1 : 0;
    }
    CHECK(rising > 3);
    // Those that hide die on touching it.
    process.collision_mode = ParticleCollision::HideOnContact;
    int live = 0;
    for (const Particle& particle : run(process, steps)) {
        if (particle.active()) {
            ++live;
            CHECK(rests_on(particle, 78.3F, 90.0F));
        }
    }
    CHECK(live > 3);
    CHECK(live < 15);

    // Particles in the emitter's coordinates reach the shapes through to_shapes: emitted at the
    // origin and moved by it to (50, 60), twice the size, they rest where a particle of size 1
    // in shape pixels is half a pixel from the floor in theirs.
    process.collision_mode = ParticleCollision::Rigid;
    process.collision_bounce = 0.0F;
    for (ParticleStep& step : steps) {
        step.emission = {1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F};
        step.to_shapes = {2.0F, 0.0F, 0.0F, 2.0F, 50.0F, 60.0F};
        step.from_shapes = {0.5F, 0.0F, 0.0F, 0.5F};
        step.collision_size = 1.0F;
    }
    resting = 0;
    for (const Particle& particle : run(process, steps)) {
        if (particle.active()) {
            CHECK(rests_on(particle, 9.3F, 40.0F));
            resting += particle.origin.y > 8.5F && particle.origin.y < 9.3F ? 1 : 0;
        }
    }
    CHECK(resting > 5);
}

TEST_CASE("the GPU device collides particles with shapes as the CPU twin does") {
    auto device = slash::test::gpu_device();
    if (device == nullptr) {
        return;
    }
    // A floor, a slope and a line, then the same shapes seen through a turned and scaled
    // transform, as for particles in an emitter's coordinates.
    const std::vector<ParticleShape> shapes{
        {.points = {{-100.0F, 90.0F}, {200.0F, 90.0F}, {200.0F, 120.0F}, {-100.0F, 120.0F}},
         .closed = true},
        {.points = {{20.0F, 30.0F}, {60.0F, 50.0F}, {20.0F, 50.0F}}, .closed = true},
        {.points = {{70.0F, 40.0F}, {90.0F, 30.0F}, {110.0F, 45.0F}}, .closed = false}};
    constexpr std::uint32_t kAmount = 100;
    std::vector<ParticleStep> steps = run_steps(45, 0.5F, 0.1F);
    for (std::size_t i = 0; i < steps.size(); ++i) {
        steps[i].shape_first = i < 30 ? 0 : 1;
        steps[i].shape_count = i < 30 ? 3 : 2;
        steps[i].collision_size = 3.0F;
        if (i >= 30) {
            steps[i].to_shapes = {0.0F, 2.0F, -2.0F, 0.0F, 10.0F, 5.0F};
            steps[i].from_shapes = {0.0F, -0.5F, 0.5F, 0.0F};
        }
    }
    std::vector<ParticleProcess> processes;
    ParticleProcess rigid = busy_process();
    rigid.direction = {0.0F, 1.0F};
    rigid.collision_mode = ParticleCollision::Rigid;
    rigid.collision_bounce = 0.5F;
    rigid.collision_friction = 0.2F;
    processes.push_back(rigid);
    ParticleProcess scaled = rigid;
    scaled.collision_use_scale = true;
    scaled.collision_bounce = 1.0F;
    processes.push_back(scaled);
    ParticleProcess hiding = busy_process();
    hiding.direction = {0.0F, 1.0F};
    hiding.collision_mode = ParticleCollision::HideOnContact;
    processes.push_back(hiding);
    int which = 0;
    for (const ParticleProcess& process : processes) {
        const int index = which++;
        CAPTURE(index);
        std::vector<Particle> twin(kAmount);
        slash::render::process_particles(twin, process, steps, shapes);
        auto system = device->create_particles(kAmount);
        REQUIRE(system.ok());
        REQUIRE(
            device->process_particles(system.value(), process, std::span(steps).first(20), shapes)
                .ok());
        REQUIRE(
            device->process_particles(system.value(), process, std::span(steps).subspan(20), shapes)
                .ok());
        auto read = device->read_particles(system.value());
        REQUIRE(read.ok());
        int live = 0;
        for (std::uint32_t i = 0; i < kAmount; ++i) {
            live += twin[i].active() ? 1 : 0;
            CHECK_MESSAGE(close(read.value()[i], twin[i]), "particle ", i, ": GPU ",
                          describe(read.value()[i]), ", twin ", describe(twin[i]));
        }
        CHECK(live > 10);
        device->destroy_particles(system.value());
    }
}

TEST_CASE("sub-emitters start particles where their parents' particles are, as Godot's") {
    using slash::render::ParticleChainLink;
    using slash::render::SubEmitterMode;
    // Parents fly right at 50 pixels a second from (50, 60), one a step; the particles their
    // sub-emitter starts stay where they start, unless they keep their parent's velocity.
    ParticleProcess parent;
    parent.direction = {1.0F, 0.0F};
    parent.spread = 0.0F;
    parent.gravity = {};
    set_param(parent, ParticleParam::InitialVelocity, 50.0F, 50.0F);
    ParticleProcess sub;
    sub.gravity = {};
    const std::vector<ParticleStep> steps = run_steps(45, 0.0F, 0.0F);
    std::vector<ParticleStep> subs = sub_steps(steps, 5.0F);
    std::vector<Particle> parents;
    std::vector<Particle> children;
    const auto run = [&](std::size_t count, std::size_t sub_amount) {
        parents.assign(30, Particle{});
        children.assign(sub_amount, Particle{});
        const std::array<ParticleChainLink, 2> chain{
            ParticleChainLink{.particles = parents,
                              .process = &parent,
                              .steps = std::span(steps).first(count),
                              .shapes = {}},
            ParticleChainLink{.particles = children,
                              .process = &sub,
                              .steps = std::span(subs).first(count),
                              .shapes = {}}};
        slash::render::process_particle_chain(chain);
    };

    // A sub-emitter starts nothing of its own, and nothing while its parent's mode is disabled.
    run(45, 100);
    CHECK(live_count(parents) > 25);
    CHECK(live_count(children) == 0);

    // At start: as many as the amount where each parent is after its first step.
    parent.sub_emitter_mode = SubEmitterMode::AtStart;
    parent.sub_emitter_amount_at_start = 2;
    run(10, 100);
    CHECK(live_count(parents) > 5);
    CHECK(live_count(children) == 2 * live_count(parents));
    for (const Particle& child : children) {
        if (child.active()) {
            CHECK(child.origin.x >= 50.0F);
            CHECK(child.origin.x <= 50.0F + (50.0F / 30.0F) + 0.01F);
            CHECK(child.origin.y == doctest::Approx(60.0));
            CHECK(child.velocity == Vec2{});
        }
    }
    // Kept velocity carries on as the parent went.
    parent.sub_emitter_keep_velocity = true;
    run(10, 100);
    int moving = 0;
    for (const Particle& child : children) {
        if (child.active()) {
            CHECK(child.velocity.x == doctest::Approx(50.0));
            moving += child.origin.x > 55.0F ? 1 : 0;
        }
    }
    CHECK(moving > 3);
    parent.sub_emitter_keep_velocity = false;

    // At end: only where parents die, a second and 50 pixels on, in their last steps.
    parent.sub_emitter_mode = SubEmitterMode::AtEnd;
    parent.sub_emitter_amount_at_end = 3;
    run(26, 100);
    CHECK(live_count(children) == 0);
    run(45, 100);
    CHECK(live_count(children) > 10);
    CHECK(live_count(children) % 3 == 0);
    for (const Particle& child : children) {
        if (child.active()) {
            CHECK(child.origin.x > 96.0F);
            CHECK(child.origin.x < 102.0F);
        }
    }

    // Constant: one each tenth of a second of a parent's life, five pixels apart along its way
    // (where it got to in the step that took it there).
    parent.sub_emitter_mode = SubEmitterMode::Constant;
    parent.sub_emitter_frequency = 10.0F;
    run(30, 400);
    CHECK(live_count(children) > 100);
    for (const Particle& child : children) {
        if (child.active()) {
            CHECK(std::fmod(child.origin.x - 50.0F + 0.01F, 5.0F) < 1.7F);
            CHECK(child.origin.x > 54.9F);
        }
    }

    // At collision: where parents falling on a floor at 80 touch it.
    parent.sub_emitter_mode = SubEmitterMode::AtCollision;
    parent.sub_emitter_amount_at_collision = 1;
    parent.direction = {0.0F, 1.0F};
    parent.collision_mode = ParticleCollision::Rigid;
    const std::vector<ParticleShape> floor{
        {.points = {{-1000.0F, 80.0F}, {1000.0F, 80.0F}, {1000.0F, 200.0F}, {-1000.0F, 200.0F}},
         .closed = true}};
    std::vector<ParticleStep> falling = steps;
    for (ParticleStep& step : falling) {
        step.shape_count = 1;
    }
    parents.assign(30, Particle{});
    children.assign(300, Particle{});
    const std::array<ParticleChainLink, 2> chain{
        ParticleChainLink{
            .particles = parents, .process = &parent, .steps = falling, .shapes = floor},
        ParticleChainLink{.particles = children, .process = &sub, .steps = subs, .shapes = {}}};
    slash::render::process_particle_chain(chain);
    CHECK(live_count(children) > 10);
    for (const Particle& child : children) {
        if (child.active()) {
            CHECK(child.origin.y > 76.0F);
            CHECK(child.origin.y < 82.0F);
        }
    }
}

TEST_CASE("a sub-emitter takes particles in turn, alternating ends, the newest over live ones") {
    using slash::render::ParticleChainLink;
    using slash::render::SubEmitterMode;
    ParticleProcess parent;
    parent.sub_emitter_mode = SubEmitterMode::AtStart;
    parent.sub_emitter_amount_at_start = 3;
    ParticleProcess sub;
    const std::vector<ParticleStep> steps = run_steps(3, 0.0F, 0.0F);
    std::vector<ParticleStep> subs = sub_steps(steps, 5.0F);
    std::vector<Particle> parents;
    std::vector<Particle> children;
    const auto run = [&](std::size_t count) {
        parents.assign(30, Particle{});
        children.assign(4, Particle{});
        const std::array<ParticleChainLink, 2> chain{
            ParticleChainLink{.particles = parents,
                              .process = &parent,
                              .steps = std::span(steps).first(count),
                              .shapes = {}},
            ParticleChainLink{.particles = children,
                              .process = &sub,
                              .steps = std::span(subs).first(count),
                              .shapes = {}}};
        slash::render::process_particle_chain(chain);
    };
    const auto active = [&] {
        std::string out;
        for (const Particle& child : children) {
            out += child.active() ? '1' : '0';
        }
        return out;
    };
    // One parent starts in the first step: its three take the first slots, and the first particle
    // keeps the count and the cycle (plus 1).
    run(1);
    CHECK(live_count(parents) == 1);
    CHECK(active() == "1110");
    CHECK(children[0].spare[0] == 3);
    CHECK(children[0].spare[1] == 1);
    // The next ones go on from there and round onto live ones; the count goes round with them.
    run(2);
    CHECK(active() == "1111");
    CHECK(children[0].spare[0] == (3 + (3 * (live_count(parents) - 1))) % 4);
    // An odd emission cycle takes them from the last slot down.
    subs[0].cycle = 1;
    run(1);
    CHECK(active() == "0111");
    CHECK(children[0].spare[1] == 2);
    subs[0].cycle = 0;
    // More than the slots in one step: all are taken, the count back where it started.
    parent.sub_emitter_amount_at_start = 6;
    run(1);
    CHECK(active() == "1111");
    CHECK(children[0].spare[0] == 0);
}

TEST_CASE("the GPU device runs chains of sub-emitters as the CPU twin does") {
    using slash::render::ParticleChainLink;
    using slash::render::ParticleRun;
    using slash::render::SubEmitterMode;
    auto device = slash::test::gpu_device();
    if (device == nullptr) {
        return;
    }
    // Sparks that each start two embers where they die, which trail smoke as they fall.
    ParticleProcess sparks = busy_process();
    sparks.sub_emitter_mode = SubEmitterMode::AtEnd;
    sparks.sub_emitter_amount_at_end = 2;
    ParticleProcess embers = busy_process();
    embers.sub_emitter_mode = SubEmitterMode::Constant;
    embers.sub_emitter_frequency = 7.0F;
    embers.sub_emitter_keep_velocity = true;
    ParticleProcess smoke = busy_process();
    smoke.emission_shape = EmissionShape::Point;
    const std::array<ParticleProcess*, 3> processes{&sparks, &embers, &smoke};
    const std::array<std::uint32_t, 3> amounts{40, 120, 300};
    std::vector<ParticleStep> steps = run_steps(60, 0.5F, 0.1F);
    for (std::size_t i = 30; i < steps.size(); ++i) {
        steps[i].emission = {0.0F, 2.0F, -1.0F, 0.0F, 70.0F, 40.0F};
        steps[i].emitter_velocity = {30.0F, -10.0F};
    }
    std::array<std::vector<ParticleStep>, 3> link_steps{steps, sub_steps(steps, 0.7F),
                                                        sub_steps(steps, 0.4F)};
    // An emission cycle of the embers and smoke goes past every so often, as theirs would.
    for (std::size_t k = 1; k < 3; ++k) {
        for (std::size_t i = 0; i < steps.size(); ++i) {
            link_steps[k][i].cycle = static_cast<std::uint32_t>(i / (10 * k));
        }
    }

    std::array<std::vector<Particle>, 3> twin;
    std::array<ParticleChainLink, 3> links;
    std::array<ParticleRun, 3> runs;
    std::array<ParticlesId, 3> systems{};
    for (std::size_t k = 0; k < 3; ++k) {
        twin[k].resize(amounts[k]);
        links[k] = {
            .particles = twin[k], .process = processes[k], .steps = link_steps[k], .shapes = {}};
        auto system = device->create_particles(amounts[k]);
        REQUIRE(system.ok());
        systems[k] = system.value();
    }
    slash::render::process_particle_chain(links);
    // In two calls, as a game's ticks would be.
    for (const auto& [from, count] : {std::pair<std::size_t, std::size_t>{0, 20}, {20, 40}}) {
        for (std::size_t k = 0; k < 3; ++k) {
            runs[k] = {.particles = systems[k],
                       .process = processes[k],
                       .steps = std::span(link_steps[k]).subspan(from, count),
                       .shapes = {}};
        }
        REQUIRE(device->process_particle_chain(runs).ok());
    }
    for (std::size_t k = 0; k < 3; ++k) {
        CAPTURE(k);
        auto read = device->read_particles(systems[k]);
        REQUIRE(read.ok());
        REQUIRE(read.value().size() == amounts[k]);
        CHECK(live_count(twin[k]) > 10);
        for (std::uint32_t i = 0; i < amounts[k]; ++i) {
            CHECK_MESSAGE(close(read.value()[i], twin[k][i]), "particle ", i, ": GPU ",
                          describe(read.value()[i]), ", twin ", describe(twin[k][i]));
        }
        CHECK(read.value()[0].spare == twin[k][0].spare);
        device->destroy_particles(systems[k]);
    }
}

namespace {

// Whether two particles are the same to the bit, as the CPU twin makes them from the same steps.
bool same(const Particle& a, const Particle& b) {
    return std::memcmp(&a, &b, sizeof(Particle)) == 0;
}

} // namespace

TEST_CASE("trail copies follow their particle's path, as Godot's") {
    using slash::render::kParticleCycleShift;
    using slash::render::kParticleStarted;
    using slash::render::kParticleTrailed;
    using slash::render::ParticleChainLink;
    // Particles thrown up and pulled down, so each step's place differs from the last.
    ParticleProcess process;
    process.direction = {1.0F, -1.0F};
    process.spread = 10.0F;
    process.gravity = {0.0F, 60.0F};
    set_param(process, ParticleParam::InitialVelocity, 40.0F, 60.0F);
    set_param(process, ParticleParam::AngularVelocity, -90.0F, 90.0F);
    constexpr std::uint32_t kAmount = 6;
    constexpr std::uint32_t kTrail = 5; // four copies
    constexpr std::uint32_t kSpan = 8;  // copy j runs 8 * j / 5 steps behind: 1, 3, 4 and 6
    std::vector<ParticleStep> steps = run_steps(80, 0.2F, 0.0F);
    for (std::size_t i = 0; i < steps.size(); ++i) {
        steps[i].trail_steps = kSpan;
        steps[i].emitting = i < 30; // then the particles go, a second later
    }
    const auto copy_of = [](std::span<const Particle> all, std::uint32_t index, std::uint32_t j) {
        return all[kAmount + (index * (kTrail - 1)) + j - 1];
    };

    // Step by step, each with the steps before it its copies need, as a renderer's frames run
    // them, keeping what each step left.
    std::vector<Particle> trailed(kAmount * kTrail);
    std::vector<std::vector<Particle>> after;
    for (std::size_t i = 0; i < steps.size(); ++i) {
        const std::size_t history = std::min<std::size_t>(i, kSpan - 1);
        const ParticleChainLink link{.particles = trailed,
                                     .process = &process,
                                     .steps = std::span(steps).subspan(i - history, history + 1),
                                     .shapes = {},
                                     .trail = kTrail,
                                     .history = history};
        slash::render::process_particle_chain({&link, 1});
        after.push_back(trailed);
    }
    // All at once makes the same, and the particles are those a system without trails has.
    std::vector<Particle> once(kAmount * kTrail);
    const ParticleChainLink all{.particles = once,
                                .process = &process,
                                .steps = steps,
                                .shapes = {},
                                .trail = kTrail,
                                .history = 0};
    slash::render::process_particle_chain({&all, 1});
    for (std::size_t i = 0; i < once.size(); ++i) {
        CAPTURE(i);
        CHECK(same(once[i], trailed[i]));
    }
    std::vector<Particle> alone(kAmount);
    slash::render::process_particles(alone, process, std::span(steps).first(40));
    for (std::uint32_t i = 0; i < kAmount; ++i) {
        CAPTURE(i);
        CHECK(same(alone[i], after[39][i]));
    }

    // Each copy waits where its particle started, then takes its place lag steps behind.
    std::size_t waited = 0;
    std::size_t followed = 0;
    for (std::size_t i = 0; i < steps.size(); ++i) {
        for (std::uint32_t index = 0; index < kAmount; ++index) {
            const Particle& head = after[i][index];
            for (std::uint32_t j = 1; j < kTrail; ++j) {
                CAPTURE(i);
                CAPTURE(index);
                CAPTURE(j);
                const std::uint32_t lag = (j * kSpan) / kTrail;
                const Particle& copy = copy_of(after[i], index, j);
                if ((head.flags & kParticleStarted) != 0) {
                    CHECK(copy.flags ==
                          (kParticleTrailed |
                           (head.flags >> kParticleCycleShift << kParticleCycleShift)));
                    CHECK(copy.spare[0] == lag);
                    CHECK(copy.origin.x == head.origin.x);
                    CHECK(copy.origin.y == head.origin.y);
                    ++waited;
                }
                if (!copy.active() || i < lag) {
                    continue;
                }
                const Particle& then = after[i - lag][index];
                if ((then.flags >> kParticleCycleShift) != (copy.flags >> kParticleCycleShift)) {
                    continue;
                }
                CHECK(then.active());
                CHECK(copy.origin.x == then.origin.x);
                CHECK(copy.origin.y == then.origin.y);
                CHECK(copy.velocity.x == then.velocity.x);
                CHECK(copy.custom.y == then.custom.y);
                ++followed;
            }
        }
    }
    CHECK(waited > 20);
    CHECK(followed > 300);

    // Once the particles stop starting, each dies of age and its copies run on to the end of
    // the same life, the last six steps after it.
    std::size_t last = 0;
    for (std::size_t i = 0; i < steps.size(); ++i) {
        if (live_count(std::span(after[i]).first(kAmount)) > 0) {
            last = i;
        }
    }
    REQUIRE(last + 7 < steps.size());
    CHECK(live_count(std::span(after[last + 1]).subspan(kAmount)) > 0);
    CHECK(live_count(std::span(after[last + 6]).subspan(kAmount)) > 0);
    CHECK(live_count(std::span(after[last + 7]).subspan(kAmount)) == 0);
}

TEST_CASE("the GPU device runs trails as the CPU twin does") {
    using slash::render::ParticleChainLink;
    using slash::render::ParticleRun;
    auto device = slash::test::gpu_device();
    if (device == nullptr) {
        return;
    }
    constexpr std::uint32_t kAmount = 40;
    constexpr std::uint32_t kTrail = 9;
    std::vector<ParticleStep> steps = run_steps(50, 0.5F, 0.1F);
    for (std::size_t i = 0; i < steps.size(); ++i) {
        steps[i].trail_steps = 12;
        if (i >= 30) {
            steps[i].emission = {0.0F, 2.0F, -1.0F, 0.0F, 70.0F, 40.0F};
            steps[i].emitter_velocity = {30.0F, -10.0F};
        }
    }
    int which = 0;
    for (const ParticleProcess& process : varied_processes()) {
        const int index = which++;
        CAPTURE(index);
        std::vector<Particle> twin(kAmount * kTrail);
        const ParticleChainLink link{.particles = twin,
                                     .process = &process,
                                     .steps = steps,
                                     .shapes = {},
                                     .trail = kTrail,
                                     .history = 0};
        slash::render::process_particle_chain({&link, 1});
        auto system = device->create_particles(kAmount, kTrail);
        REQUIRE(system.ok());
        // In two calls, as a game's ticks would be: the second starts with the steps before it
        // that its copies run behind.
        REQUIRE(
            device->process_particles(system.value(), process, std::span(steps).first(20)).ok());
        const std::array<ParticleRun, 1> rest{ParticleRun{.particles = system.value(),
                                                          .process = &process,
                                                          .steps = std::span(steps).subspan(9),
                                                          .shapes = {},
                                                          .history = 11}};
        REQUIRE(device->process_particle_chain(rest).ok());
        auto read = device->read_particles(system.value());
        REQUIRE(read.ok());
        REQUIRE(read.value().size() == twin.size());
        for (std::size_t i = 0; i < twin.size(); ++i) {
            CHECK_MESSAGE(close(read.value()[i], twin[i]), "particle ", i, ": GPU ",
                          describe(read.value()[i]), ", twin ", describe(twin[i]));
            CHECK(read.value()[i].spare == twin[i].spare);
        }
        CHECK(live_count(std::span(twin).subspan(kAmount)) > 100);
        device->destroy_particles(system.value());
    }
}

TEST_CASE("trails draw as Godot's ribbon through a particle and its copies") {
    using slash::render::kParticleActive;
    using slash::render::kParticleTrailed;
    // A particle moving right with its x axis down, so the ribbon is as wide as a frame across
    // its path; a copy behind it, and one waiting further back.
    std::vector<Particle> particles(3);
    const std::array<Vec2, 3> places{{{20.0F, 0.0F}, {10.0F, 0.0F}, {0.0F, 0.0F}}};
    const std::array<Vec4, 3> colors{
        {{1.0F, 0.0F, 0.0F, 1.0F}, {0.0F, 1.0F, 0.0F, 1.0F}, {0.0F, 0.0F, 1.0F, 1.0F}}};
    for (std::size_t i = 0; i < 3; ++i) {
        particles[i].flags = i == 2 ? kParticleTrailed : kParticleActive;
        particles[i].origin = places.at(i);
        particles[i].velocity = {10.0F, 0.0F};
        particles[i].basis = {0.0F, 1.0F, -1.0F, 0.0F};
        particles[i].color = colors.at(i);
    }
    ParticleDraw look;
    look.size = {4.0F, 4.0F};
    look.trail_subdivisions = 2;
    // Copy j of 3 is 4 * j / 3 steps behind, so copy 1 rounded off a third of a step and copy
    // 2 two thirds: each draws back that much along its velocity.
    look.trail_steps = 4;
    look.step_delta = 0.1F;
    CHECK(slash::render::particle_vertex_count(1, look) == 6);
    CHECK(slash::render::particle_vertex_count(3, look) == 24);
    std::vector<Vertex2D> vertices;
    slash::render::particle_vertices(particles, look, vertices, 3);
    REQUIRE(vertices.size() == 24);
    // Rings down the ribbon: at the particle, halfway to copy 1, at copy 1, halfway to copy 2,
    // at copy 2. Segment s is rings s and s + 1: top left, top right, bottom left, then top
    // right, bottom right, bottom left.
    const std::array<float, 5> ring_x{20.0F, 14.8333F, 9.6667F, 4.5F, -0.6667F};
    const std::array<std::array<std::uint8_t, 4>, 5> ring_rgba{{{255, 0, 0, 255},
                                                                {128, 128, 0, 255},
                                                                {0, 255, 0, 255},
                                                                {0, 128, 128, 255},
                                                                {0, 0, 255, 255}}};
    for (std::size_t segment = 0; segment < 4; ++segment) {
        CAPTURE(segment);
        const auto quad = std::span(vertices).subspan(segment * 6, 6);
        const std::array<std::size_t, 6> ring{segment, segment,     segment + 1,
                                              segment, segment + 1, segment + 1};
        const std::array<float, 6> side{-2.0F, 2.0F, -2.0F, 2.0F, 2.0F, -2.0F};
        for (std::size_t v = 0; v < 6; ++v) {
            CAPTURE(v);
            CHECK(quad[v].x == doctest::Approx(ring_x.at(ring.at(v))).epsilon(1e-4));
            CHECK(quad[v].y == doctest::Approx(side.at(v)));
            CHECK(quad[v].u == doctest::Approx(side.at(v) < 0.0F ? 0.0F : 1.0F));
            CHECK(quad[v].v == doctest::Approx(static_cast<float>(ring.at(v)) / 4.0F));
            CHECK(quad[v].rgba == ring_rgba.at(ring.at(v)));
        }
    }
    // When the particle goes, the segments it bends have no area and the copies' part draws on;
    // a segment through a copy that isn't live has none either.
    particles[0].flags = 0;
    vertices.clear();
    slash::render::particle_vertices(particles, look, vertices, 3);
    const auto empty = [&](std::size_t segment) {
        return std::ranges::all_of(std::span(vertices).subspan(segment * 6, 6),
                                   [](const Vertex2D& v) { return v.rgba[3] == 0; });
    };
    CHECK(empty(0));
    CHECK(empty(1));
    CHECK_FALSE(empty(2));
    CHECK_FALSE(empty(3));
    particles[2].flags = 0;
    vertices.clear();
    slash::render::particle_vertices(particles, look, vertices, 3);
    CHECK(empty(2));
    CHECK(empty(3));
}

TEST_CASE("every device refuses the same particle mistakes") {
    std::vector<std::unique_ptr<RenderDevice>> devices = slash::test::drawing_devices();
    devices.push_back(slash::render::create_null_device());
    for (auto& device : devices) {
        CAPTURE(slash::render::backend_name(device->backend()));
        const auto error_of = [](const auto& result) {
            return result.ok() ? std::string("(succeeded)") : result.error().message;
        };
        CHECK(error_of(device->create_particles(0)) ==
              "a particle system has from 1 to 1000000 particles, not 0");
        CHECK(error_of(device->create_particles(1000001)) ==
              "a particle system has from 1 to 1000000 particles, not 1000001");
        const auto missing = static_cast<ParticlesId>(99);
        CHECK(error_of(device->process_particles(missing, {}, {})) == "no particle system 99");
        CHECK(error_of(device->read_particles(missing)) == "no particle system 99");
        auto system = device->create_particles(4);
        REQUIRE(system.ok());
        ParticleProcess process;
        process.alpha_curve.resize(33);
        CHECK(error_of(device->process_particles(system.value(), process, {})) ==
              "alpha_curve has more than 32 points");
        process.alpha_curve = {{0.5F, 1.0F}, {0.2F, 0.0F}};
        CHECK(error_of(device->process_particles(system.value(), process, {})) ==
              "alpha_curve's offsets must be numbers in order");
        ParticleStep step;
        step.lifetime = 0.0F;
        CHECK(error_of(device->process_particles(system.value(), {}, std::span(&step, 1))) ==
              "a particle step's lifetime is more than 0 and its delta 0 or more");
        step = ParticleStep{};
        step.shape_first = 1;
        step.shape_count = 2;
        const std::vector<ParticleShape> one{{.points = {{0.0F, 0.0F}}, .closed = true}};
        CHECK(error_of(device->process_particles(system.value(), {}, std::span(&step, 1), one)) ==
              "a particle step names shapes 1 to 2 of 1");
        step.shape_first = 0;
        step.shape_count = 1;
        std::vector<ParticleShape> many{{.points = std::vector<Vec2>(1025), .closed = true}};
        CHECK(error_of(device->process_particles(system.value(), {}, std::span(&step, 1), many)) ==
              "a particle step's shapes have at most 1024 points");
        many[0].points = {{0.0F, std::nanf("")}};
        CHECK(error_of(device->process_particles(system.value(), {}, std::span(&step, 1), many)) ==
              "a particle shape's points are numbers");
        many[0].points = {{0.0F, 0.0F}};
        step.collision_size = -1.0F;
        CHECK(error_of(device->process_particles(system.value(), {}, std::span(&step, 1), many)) ==
              "a particle step's collision size is 0 or more");
        CHECK(error_of(
                  device->draw_particles(system.value(), slash::render::TextureId::None, {}, {})) ==
              "draw_particles needs a frame; call begin_frame first");
        using slash::render::ParticleRun;
        CHECK(error_of(device->process_particle_chain({})) ==
              "a particle chain has from 1 to 8 systems, not 0");
        const ParticleProcess still;
        const std::vector<ParticleRun> nine(
            9,
            ParticleRun{.particles = system.value(), .process = &still, .steps = {}, .shapes = {}});
        CHECK(error_of(device->process_particle_chain(nine)) ==
              "a particle chain has from 1 to 8 systems, not 9");
        CHECK(error_of(device->process_particle_chain(std::span(nine).first(2))) ==
              "a particle chain names system " +
                  std::to_string(static_cast<std::uint32_t>(system.value())) + " twice");
        auto other = device->create_particles(4);
        REQUIRE(other.ok());
        std::array<ParticleRun, 2> pair{
            ParticleRun{.particles = system.value(), .process = &still, .steps = {}, .shapes = {}},
            ParticleRun{
                .particles = other.value(), .process = &still, .steps = {&step, 1}, .shapes = {}}};
        CHECK(error_of(device->process_particle_chain(pair)) ==
              "a particle chain's systems run as many steps each");
        pair[1] = {.particles = other.value(), .process = nullptr, .steps = {}, .shapes = {}};
        CHECK(error_of(device->process_particle_chain(pair)) ==
              "a particle run has a process material");
        pair[1] = {.particles = missing, .process = &still, .steps = {}, .shapes = {}};
        CHECK(error_of(device->process_particle_chain(pair)) == "no particle system 99");
        device->destroy_particles(other.value());
        // Trails.
        CHECK(error_of(device->create_particles(4, 0)) ==
              "a particle trail has from 2 to 129 particles, or 1 for none, not 0");
        CHECK(error_of(device->create_particles(4, 130)) ==
              "a particle trail has from 2 to 129 particles, or 1 for none, not 130");
        CHECK(error_of(device->create_particles(10000, 101)) ==
              "a particle system with trails has at most 1000000 particles with its trails; "
              "10000 with trails of 101 would make 1010000");
        step = ParticleStep{};
        step.trail_steps = 0;
        CHECK(error_of(device->process_particles(system.value(), {}, std::span(&step, 1))) ==
              "a particle step's trail_steps is from 1 to 65536");
        step.trail_steps = 1;
        const std::array<ParticleRun, 1> behind{ParticleRun{.particles = system.value(),
                                                            .process = &still,
                                                            .steps = {&step, 1},
                                                            .shapes = {},
                                                            .history = 2}};
        CHECK(error_of(device->process_particle_chain(behind)) ==
              "a particle run's history is 2 of its 1 steps");
        auto trailed = device->create_particles(4, 9);
        REQUIRE(trailed.ok());
        auto read_trailed = device->read_particles(trailed.value());
        REQUIRE(read_trailed.ok());
        CHECK(read_trailed.value().size() == 36);

        auto target = device->create_texture({.width = 4,
                                              .height = 4,
                                              .filter = slash::render::Filter::Linear,
                                              .render_target = true});
        REQUIRE(target.ok());
        REQUIRE(device->begin_frame({.target = target.value(), .clear = {}, .view = {}}).ok());
        CHECK(error_of(device->read_particles(system.value())) ==
              "read_particles can't run during a frame; call end_frame first");
        CHECK(error_of(device->draw_particles(missing, slash::render::TextureId::None, {}, {})) ==
              "no particle system 99");
        ParticleDraw look;
        look.hframes = 0;
        CHECK(error_of(device->draw_particles(system.value(), slash::render::TextureId::None, look,
                                              {})) ==
              "particles' hframes and vframes are at least 1");
        CHECK(error_of(device->draw_particles(system.value(), target.value(), {}, {})) ==
              "texture 1 is this frame's target, so it can't be drawn with until the frame ends");
        look = ParticleDraw{};
        look.trail_subdivisions = 8;
        auto long_trails = device->create_particles(1000, 129);
        REQUIRE(long_trails.ok());
        CHECK(error_of(device->draw_particles(long_trails.value(), slash::render::TextureId::None,
                                              look, {})) ==
              "a frame draws up to 4194304 vertices, 6144 a particle; these 1000 particles "
              "would make 6144000");
        look.trail_subdivisions = 0;
        CHECK(error_of(device->draw_particles(trailed.value(), slash::render::TextureId::None, look,
                                              {})) ==
              "particle trails have from 1 to 1024 subdivisions, trail_steps from 1 to 65536 "
              "and a step_delta of 0 or more");
        auto big = device->create_particles(700000);
        REQUIRE(big.ok());
        CHECK(
            error_of(device->draw_particles(big.value(), slash::render::TextureId::None, {}, {})) ==
            "a frame draws up to 4194304 vertices, 6 a particle; these 700000 particles "
            "would make 4200000");
        // A system without trails has none to divide.
        CHECK(error_of(device->draw_particles(system.value(), slash::render::TextureId::None, look,
                                              {})) == "(succeeded)");
        REQUIRE(device->end_frame().ok());
        device->destroy_particles(big.value());
        device->destroy_particles(long_trails.value());
        device->destroy_particles(trailed.value());
        device->destroy_particles(system.value());
        CHECK(error_of(device->read_particles(system.value())) == "no particle system 1");
    }
}

TEST_CASE("particles draw as quads in lifetime order, live ones only") {
    std::vector<Particle> particles(3);
    for (std::size_t i = 0; i < particles.size(); ++i) {
        particles[i].flags = i == 1 ? 0U : slash::render::kParticleActive;
        particles[i].origin = {10.0F * static_cast<float>(i), 0.0F};
        particles[i].velocity = {0.0F, 10.0F};
        particles[i].color = {1.0F, 0.5F, 0.0F, 1.0F};
        particles[i].custom.z = 0.6F; // the frame 60% through the sheet
    }
    ParticleDraw look;
    look.size = {2.0F, 4.0F};
    look.remainder = 0.1F;
    look.hframes = 2;
    look.vframes = 2;
    look.modulate = {1.0F, 1.0F, 1.0F, 0.5F};
    look.transform = {1.0F, 0.0F, 0.0F, 1.0F, 100.0F, 0.0F};
    std::vector<Vertex2D> vertices;
    slash::render::particle_vertices(particles, look, vertices);
    REQUIRE(vertices.size() == 18);
    // Frame 2 of 4: the sheet's bottom left; moved on 0.1 s and over by the transform.
    CHECK(vertices[0].x == doctest::Approx(99.0F));
    CHECK(vertices[0].y == doctest::Approx(-1.0F));
    CHECK(vertices[0].u == doctest::Approx(0.0F));
    CHECK(vertices[0].v == doctest::Approx(0.5F));
    CHECK(vertices[4].x == doctest::Approx(101.0F));
    CHECK(vertices[4].y == doctest::Approx(3.0F));
    CHECK(vertices[4].u == doctest::Approx(0.5F));
    CHECK(vertices[4].v == doctest::Approx(1.0F));
    CHECK(vertices[0].rgba == std::array<std::uint8_t, 4>{255, 128, 0, 128});
    // The particle that isn't live has no area.
    CHECK(vertices[6].rgba == std::array<std::uint8_t, 4>{0, 0, 0, 0});
    CHECK(vertices[6].x == vertices[7].x);

    // In lifetime order from slot 2, the oldest: slots 2, 0, 1; reversed, 1, 0, 2.
    look.order = ParticleOrder::Lifetime;
    look.lifetime_split = 2;
    vertices.clear();
    slash::render::particle_vertices(particles, look, vertices);
    CHECK(vertices[0].x == doctest::Approx(119.0F));
    CHECK(vertices[6].x == doctest::Approx(99.0F));
    look.order = ParticleOrder::ReverseLifetime;
    vertices.clear();
    slash::render::particle_vertices(particles, look, vertices);
    CHECK(vertices[0].rgba == std::array<std::uint8_t, 4>{0, 0, 0, 0});
    CHECK(vertices[12].x == doctest::Approx(119.0F));

    CHECK(slash::render::particle_lifetime_split(8, 0.0) == 1);
    CHECK(slash::render::particle_lifetime_split(8, 0.5) == 5);
    CHECK(slash::render::particle_lifetime_split(8, 0.99) == 0);
}

TEST_CASE("every device draws trails as the software device does") {
    using slash::render::ParticleRun;
    ParticleProcess process = busy_process();
    process.align_y = true;
    std::vector<ParticleStep> steps = run_steps(40, 0.3F, 0.0F);
    for (ParticleStep& step : steps) {
        step.trail_steps = 9;
    }
    constexpr std::uint32_t kSize = 96;
    constexpr std::uint32_t kAmount = 32;
    constexpr std::uint32_t kTrail = 6;
    ParticleDraw look;
    look.size = {3.0F, 3.0F};
    look.order = ParticleOrder::Lifetime;
    look.lifetime_split =
        slash::render::particle_lifetime_split(kAmount, steps.back().system_phase);
    look.remainder = 0.01F;
    look.trail_subdivisions = 3;
    look.trail_steps = 9;
    look.step_delta = steps.back().delta;
    const auto draw = [&](RenderDevice& device) {
        auto system = device.create_particles(kAmount, kTrail);
        REQUIRE(system.ok());
        REQUIRE(device.process_particles(system.value(), process, steps).ok());
        auto target = device.create_texture({.width = kSize,
                                             .height = kSize,
                                             .filter = slash::render::Filter::Linear,
                                             .render_target = true});
        REQUIRE(target.ok());
        REQUIRE(device
                    .begin_frame(
                        {.target = target.value(), .clear = {0.0F, 0.0F, 0.0F, 1.0F}, .view = {}})
                    .ok());
        slash::render::Material additive;
        additive.blend = slash::render::BlendMode::Add;
        REQUIRE(
            device.draw_particles(system.value(), slash::render::TextureId::None, look, additive)
                .ok());
        REQUIRE(device.end_frame().ok());
        // Five sections of three segments, two triangles each, for every particle.
        CHECK(device.stats().triangles == kAmount * 30);
        auto image = device.read_texture(target.value());
        REQUIRE(image.ok());
        return std::move(image).value();
    };
    auto software = slash::render::create_software_device();
    const slash::render::Image expected = draw(*software);
    std::size_t lit = 0;
    for (std::size_t i = 0; i < expected.pixels.size(); i += 4) {
        lit += expected.pixels[i] > 0 ? 1U : 0U;
    }
    CHECK(lit > 300);
    // The null device checks and counts the same.
    auto null = slash::render::create_null_device();
    (void)draw(*null);
    for (auto& device : slash::test::hardware_devices()) {
        CAPTURE(slash::render::backend_name(device->backend()));
        const slash::render::Image image = draw(*device);
        std::size_t different = 0;
        for (std::size_t i = 0; i < image.pixels.size(); ++i) {
            different += std::abs(int{image.pixels[i]} - int{expected.pixels[i]}) > 12 ? 1U : 0U;
        }
        CHECK(different < lit / 10);
    }
}

TEST_CASE("every device draws particles as the software device does") {
    const ParticleProcess process = busy_process();
    const std::vector<ParticleStep> steps = run_steps(40, 0.3F, 0.0F);
    constexpr std::uint32_t kSize = 96;
    ParticleDraw look;
    look.size = {6.0F, 6.0F};
    look.order = ParticleOrder::Lifetime;
    look.lifetime_split = slash::render::particle_lifetime_split(64, steps.back().system_phase);
    look.remainder = 0.01F;
    const auto draw = [&](RenderDevice& device) {
        auto system = device.create_particles(64);
        REQUIRE(system.ok());
        REQUIRE(device.process_particles(system.value(), process, steps).ok());
        auto target = device.create_texture({.width = kSize,
                                             .height = kSize,
                                             .filter = slash::render::Filter::Linear,
                                             .render_target = true});
        REQUIRE(target.ok());
        REQUIRE(device
                    .begin_frame(
                        {.target = target.value(), .clear = {0.0F, 0.0F, 0.0F, 1.0F}, .view = {}})
                    .ok());
        slash::render::Material additive;
        additive.blend = slash::render::BlendMode::Add;
        REQUIRE(
            device.draw_particles(system.value(), slash::render::TextureId::None, look, additive)
                .ok());
        REQUIRE(device.end_frame().ok());
        const slash::render::Stats stats = device.stats();
        CHECK(stats.draws == 1);
        CHECK(stats.triangles == 128);
        auto image = device.read_texture(target.value());
        REQUIRE(image.ok());
        return std::move(image).value();
    };
    auto software = slash::render::create_software_device();
    const slash::render::Image expected = draw(*software);
    std::size_t lit = 0;
    for (std::size_t i = 0; i < expected.pixels.size(); i += 4) {
        lit += expected.pixels[i] > 0 ? 1U : 0U;
    }
    CHECK(lit > 200);
    for (auto& device : slash::test::hardware_devices()) {
        CAPTURE(slash::render::backend_name(device->backend()));
        const slash::render::Image image = draw(*device);
        // Float units differ in their last bits, which moves a few edges by a pixel.
        std::size_t different = 0;
        for (std::size_t i = 0; i < image.pixels.size(); ++i) {
            different += std::abs(int{image.pixels[i]} - int{expected.pixels[i]}) > 12 ? 1U : 0U;
        }
        CHECK(different < lit / 10);
    }
}
