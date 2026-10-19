#include "devices.hpp"

#include "slash/render/device.hpp"
#include "slash/render/particles.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using slash::Vec2;
using slash::Vec4;
using slash::render::EmissionShape;
using slash::render::Particle;
using slash::render::ParticleDraw;
using slash::render::ParticleOrder;
using slash::render::ParticleParam;
using slash::render::ParticleProcess;
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
        steps.push_back(step);
    }
    return steps;
}

std::string describe(const Particle& p) {
    return "flags " + std::to_string(p.flags) + " origin " + std::to_string(p.origin.x) + "," +
           std::to_string(p.origin.y) + " color " + std::to_string(p.color.x) + "," +
           std::to_string(p.color.w) + " custom " + std::to_string(p.custom.x) + "," +
           std::to_string(p.custom.y);
}

// Whether two particles agree to within what different float units give.
bool close(const Particle& a, const Particle& b) {
    if (a.flags != b.flags) {
        return false;
    }
    if (!a.active()) {
        return true;
    }
    const auto near = [](float x, float y) {
        return std::abs(x - y) <= 0.02F * std::max(1.0F, std::abs(x));
    };
    const std::array<float, 20> fa{a.basis.x,    a.basis.y,    a.basis.z,    a.basis.w,
                                   a.origin.x,   a.origin.y,   a.velocity.x, a.velocity.y,
                                   a.color.x,    a.color.y,    a.color.z,    a.color.w,
                                   a.custom.x,   a.custom.y,   a.custom.z,   a.custom.w,
                                   a.userdata.x, a.userdata.y, a.userdata.z, a.userdata.w};
    const std::array<float, 20> fb{b.basis.x,    b.basis.y,    b.basis.z,    b.basis.w,
                                   b.origin.x,   b.origin.y,   b.velocity.x, b.velocity.y,
                                   b.color.x,    b.color.y,    b.color.z,    b.color.w,
                                   b.custom.x,   b.custom.y,   b.custom.z,   b.custom.w,
                                   b.userdata.x, b.userdata.y, b.userdata.z, b.userdata.w};
    for (std::size_t i = 0; i < fa.size(); ++i) {
        if (!near(fa.at(i), fb.at(i))) {
            return false;
        }
    }
    return true;
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
    for (const ParticleProcess& process : varied_processes()) {
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
        CHECK(error_of(
                  device->draw_particles(system.value(), slash::render::TextureId::None, {}, {})) ==
              "draw_particles needs a frame; call begin_frame first");

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
        auto big = device->create_particles(700000);
        REQUIRE(big.ok());
        CHECK(
            error_of(device->draw_particles(big.value(), slash::render::TextureId::None, {}, {})) ==
            "a frame draws up to 4194304 vertices, six a particle; these 700000 particles "
            "would make 4200000");
        REQUIRE(device->end_frame().ok());
        device->destroy_particles(big.value());
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
