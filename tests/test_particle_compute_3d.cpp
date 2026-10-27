#include "../engine/render/src/particle_layout_3d.hpp"
#include "devices.hpp"

#include "slash/core/math.hpp"
#include "slash/core/text.hpp"
#include "slash/render/device.hpp"
#include "slash/render/image.hpp"
#include "slash/render/mesh.hpp"
#include "slash/render/particles3d.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace render = slash::render;
using render::EmissionShape;
using render::Image;
using render::Particle3D;
using render::ParticleMeshDraw3D;
using render::ParticleParam;
using render::ParticleProcess3D;
using render::ParticleStep3D;
using render::RenderDevice;
using render::Vertex3D;
using slash::Vec3;
using slash::Vec4;

namespace {

template <typename T>
std::string error_of(const slash::Result<T>& result) {
    return result.ok() ? "(succeeded)" : result.error().message;
}

void set_range(ParticleProcess3D& process, ParticleParam param, float low, float high) {
    process.param_min.at(static_cast<std::size_t>(param)) = low;
    process.param_max.at(static_cast<std::size_t>(param)) = high;
}

render::ParticleCurve& curve(ParticleProcess3D& process, ParticleParam param) {
    return process.param_curve.at(static_cast<std::size_t>(param));
}

// A sphere of particles thrown out and pulled about by every force.
ParticleProcess3D busy_process() {
    ParticleProcess3D process;
    process.emission_shape = EmissionShape::Sphere;
    process.emission_sphere_radius = 0.5F;
    process.direction = {0.0F, 1.0F, 0.0F};
    process.spread = 60.0F;
    process.gravity = {0.0F, -2.0F, 0.0F};
    set_range(process, ParticleParam::InitialVelocity, 1.0F, 3.0F);
    set_range(process, ParticleParam::AngularVelocity, -90.0F, 90.0F);
    set_range(process, ParticleParam::LinearAccel, 0.5F, 1.0F);
    set_range(process, ParticleParam::RadialAccel, -1.0F, 1.0F);
    set_range(process, ParticleParam::TangentialAccel, 0.0F, 2.0F);
    set_range(process, ParticleParam::Damping, 0.2F, 0.6F);
    set_range(process, ParticleParam::Angle, 0.0F, 180.0F);
    set_range(process, ParticleParam::Scale, 0.5F, 1.5F);
    set_range(process, ParticleParam::HueVariation, -0.2F, 0.2F);
    set_range(process, ParticleParam::AnimSpeed, 1.0F, 2.0F);
    curve(process, ParticleParam::Scale) = {{0.0F, 1.0F}, {1.0F, 0.2F}};
    process.scale_curve.axes[1] = {{0.0F, 0.5F}, {1.0F, 1.5F}};
    process.alpha_curve = {{0.0F, 1.0F}, {0.7F, 0.6F}, {1.0F, 0.0F}};
    process.color_ramp = {{.offset = 0.0F, .color = {1.0F, 0.9F, 0.2F, 1.0F}},
                          {.offset = 1.0F, .color = {0.8F, 0.1F, 0.1F, 1.0F}}};
    process.lifetime_randomness = 0.3F;
    process.rotate_y = true;
    return process;
}

// Process materials that between them use every shape and option.
std::vector<ParticleProcess3D> varied_processes() {
    std::vector<ParticleProcess3D> processes{busy_process()};

    ParticleProcess3D box = busy_process();
    box.emission_shape = EmissionShape::Box;
    box.emission_box_extents = {1.0F, 0.2F, 0.5F};
    box.emission_shape_offset = {0.0F, 0.5F, 0.0F};
    box.emission_shape_scale = {2.0F, 1.0F, 1.0F};
    box.align_y = true;
    box.rotate_y = false;
    box.damping_as_friction = true;
    box.velocity_limit_curve = {{0.0F, 3.0F}, {1.0F, 0.5F}};
    box.emission_curve = {{0.0F, 0.5F}, {1.0F, 0.0F}};
    box.color_initial_ramp = {{.offset = 0.0F, .color = {1.0F, 0.0F, 0.0F, 1.0F}},
                              {.offset = 0.5F, .color = {0.0F, 1.0F, 0.0F, 1.0F}},
                              {.offset = 1.0F, .color = {0.0F, 0.0F, 1.0F, 0.5F}}};
    curve(box, ParticleParam::AngularVelocity) = {{0.0F, 1.0F}, {1.0F, -1.0F}};
    processes.push_back(box);

    ParticleProcess3D points = busy_process();
    points.emission_shape = EmissionShape::DirectedPoints;
    points.emission_points = {{1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}, {0.0F, 0.0F, -1.0F}};
    points.emission_normals = {{0.0F, 1.0F, 0.0F}, {1.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 1.0F}};
    points.emission_colors = {{1.0F, 0.0F, 0.0F, 1.0F}, {0.0F, 1.0F, 0.0F, 1.0F}};
    points.flatness = 0.5F;
    points.inherit_velocity_ratio = 0.5F;
    processes.push_back(points);

    ParticleProcess3D scattered = busy_process();
    scattered.emission_shape = EmissionShape::Points;
    scattered.emission_points = {{-1.0F, 0.0F, 0.0F}, {0.5F, 0.5F, 0.5F}};
    processes.push_back(scattered);

    ParticleProcess3D ring = busy_process();
    ring.emission_shape = EmissionShape::Ring;
    ring.emission_ring_axis = {1.0F, 1.0F, 0.0F};
    ring.emission_ring_radius = 1.0F;
    ring.emission_ring_inner_radius = 0.4F;
    ring.emission_ring_height = 0.5F;
    ring.emission_ring_cone_angle = 60.0F;
    ring.directional_velocity_min = 0.5F;
    ring.directional_velocity_max = 1.5F;
    ring.directional_velocity_curve.axes[0] = {{0.0F, 1.0F}, {1.0F, -1.0F}};
    ring.directional_velocity_curve.axes[2] = {{0.0F, 0.5F}, {1.0F, 0.5F}};
    ring.directional_velocity_global = true;
    processes.push_back(ring);

    ParticleProcess3D swirl = busy_process();
    swirl.emission_shape = EmissionShape::SphereSurface;
    swirl.velocity_pivot = {0.0F, 0.5F, 0.0F};
    set_range(swirl, ParticleParam::OrbitVelocity, 0.2F, 0.5F);
    swirl.orbit_velocity_curve.axes[1] = {{0.0F, 1.0F}, {1.0F, 0.0F}};
    set_range(swirl, ParticleParam::RadialVelocity, 0.5F, 1.0F);
    swirl.scale_over_velocity_min = 0.0F;
    swirl.scale_over_velocity_max = 4.0F;
    swirl.scale_over_velocity_curve.axes[0] = {{0.0F, 1.0F}, {1.0F, 2.0F}};
    processes.push_back(swirl);

    ParticleProcess3D flat = busy_process();
    flat.disable_z = true;
    flat.turbulence_enabled = true;
    flat.turbulence_noise_strength = 2.0F;
    flat.turbulence_noise_scale = 4.0F;
    flat.turbulence_noise_speed = {0.2F, 0.0F, 0.1F};
    set_range(flat, ParticleParam::TurbulenceInfluence, 0.1F, 0.3F);
    set_range(flat, ParticleParam::TurbulenceDisplacement, 0.0F, 0.2F);
    processes.push_back(flat);
    return processes;
}

// count steps of delta seconds over a system of lifetime seconds, emitting through its cycle.
std::vector<ParticleStep3D> run_steps(int count, float delta, float lifetime,
                                      float explosiveness = 0.0F) {
    std::vector<ParticleStep3D> steps;
    double phase = 0.0;
    for (int i = 0; i < count; ++i) {
        ParticleStep3D step;
        step.prev_system_phase = static_cast<float>(phase);
        phase += delta / lifetime;
        if (phase >= 1.0) {
            phase -= 1.0;
            ++step.cycle;
        }
        step.cycle += steps.empty() ? 0U : steps.back().cycle;
        step.system_phase = static_cast<float>(phase);
        step.delta = delta;
        step.lifetime = lifetime;
        step.explosiveness = explosiveness;
        step.randomness = 0.2F;
        step.time = delta * static_cast<float>(i + 1);
        step.seed = 11;
        step.clear = i == 0;
        steps.push_back(step);
    }
    return steps;
}

// Whether two particles agree to within what different float units give: each vector within 2%
// of its length (or of 1).
bool close(const Particle3D& a, const Particle3D& b) {
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
    const auto near4 = [&](const Vec4& p, const Vec4& q) {
        return near({p.x, p.y, p.z, p.w}, {q.x, q.y, q.z, q.w});
    };
    return near4(a.x, b.x) && near4(a.y, b.y) && near4(a.z, b.z) && near4(a.origin, b.origin) &&
           near({a.velocity.x, a.velocity.y, a.velocity.z},
                {b.velocity.x, b.velocity.y, b.velocity.z}) &&
           near4(a.color, b.color) && near({a.custom.x}, {b.custom.x}) &&
           near({a.custom.y}, {b.custom.y}) && near({a.custom.z}, {b.custom.z}) &&
           near({a.custom.w}, {b.custom.w});
}

std::string describe(const Particle3D& p) {
    return "flags " + std::to_string(p.flags) + " origin " + std::to_string(p.origin.x) + "," +
           std::to_string(p.origin.y) + "," + std::to_string(p.origin.z) + " velocity " +
           std::to_string(p.velocity.x) + "," + std::to_string(p.velocity.y) + "," +
           std::to_string(p.velocity.z) + " color " + std::to_string(p.color.x) + "," +
           std::to_string(p.color.w) + " custom " + std::to_string(p.custom.x) + "," +
           std::to_string(p.custom.y) + "," + std::to_string(p.custom.z);
}

// A card of side size facing +z, its corners colored white.
std::vector<Vertex3D> card(float size) {
    const float h = size * 0.5F;
    const auto corner = [&](float x, float y, float u, float v) {
        return Vertex3D{.position = {x, y, 0.0F},
                        .normal = {0.0F, 0.0F, 1.0F},
                        .tangent = {1.0F, 0.0F, 0.0F, 1.0F},
                        .uv = {u, v}};
    };
    return {corner(-h, h, 0, 0), corner(h, h, 1, 0),  corner(h, -h, 1, 1),
            corner(-h, h, 0, 0), corner(h, -h, 1, 1), corner(-h, -h, 0, 1)};
}

// A 4x4 texture of four colored quarters, frames of a 2x2 sheet.
std::vector<std::uint8_t> sheet() {
    const std::array<std::array<std::uint8_t, 4>, 4> colors{
        {{255, 64, 64, 255}, {64, 255, 64, 255}, {64, 64, 255, 255}, {255, 255, 64, 255}}};
    std::vector<std::uint8_t> texels;
    for (std::uint32_t y = 0; y < 4; ++y) {
        for (std::uint32_t x = 0; x < 4; ++x) {
            const auto& c = colors.at(((y / 2) * 2) + (x / 2));
            texels.insert(texels.end(), c.begin(), c.end());
        }
    }
    return texels;
}

// The draws the pixel test makes: each order, alignment and billboard.
std::vector<ParticleMeshDraw3D> varied_draws(const render::ParticleTransform& camera) {
    ParticleMeshDraw3D base;
    base.camera = camera;
    base.sort_direction = {0.0F, 0.0F, 1.0F};
    base.align_up = {0.0F, 1.0F, 0.0F};
    base.box_low = {-6.0F, -6.0F, -6.0F};
    base.box_high = {6.0F, 6.0F, 6.0F};
    std::vector<ParticleMeshDraw3D> draws;
    ParticleMeshDraw3D depth = base;
    depth.order = render::ParticleOrder3D::ViewDepth;
    depth.billboard = render::ParticleBillboard::Enabled;
    depth.keep_scale = true;
    draws.push_back(depth);
    ParticleMeshDraw3D frames = base;
    frames.order = render::ParticleOrder3D::Lifetime;
    frames.lifetime_split = 5;
    frames.billboard = render::ParticleBillboard::Particles;
    frames.h_frames = 2;
    frames.v_frames = 2;
    frames.anim_loop = true;
    draws.push_back(frames);
    ParticleMeshDraw3D velocity = base;
    velocity.order = render::ParticleOrder3D::ReverseLifetime;
    velocity.align = render::ParticleAlign::ZBillboardYToVelocity;
    velocity.remainder = 0.01F;
    draws.push_back(velocity);
    ParticleMeshDraw3D turned = base;
    turned.align = render::ParticleAlign::YToVelocity;
    turned.billboard = render::ParticleBillboard::FixedY;
    turned.transform = {0.8F, 0.0F, -0.6F, 0.0F, 1.0F, 0.0F, 0.6F, 0.0F, 0.8F, 0.3F, -0.2F, 0.0F};
    draws.push_back(turned);
    ParticleMeshDraw3D facing = base;
    facing.align = render::ParticleAlign::ZBillboard;
    facing.uv_scale = {2.0F, 2.0F};
    facing.uv_offset = {0.25F, 0.0F};
    draws.push_back(facing);
    return draws;
}

// Particles drawn as blended cards in each of varied_draws, side by side in one frame.
Image draw_particles(RenderDevice& device, std::uint32_t size) {
    constexpr std::uint32_t kAmount = 40;
    ParticleProcess3D process = busy_process();
    process.emission_sphere_radius = 1.5F;
    process.gravity = {0.0F, 0.0F, 0.0F};
    set_range(process, ParticleParam::InitialVelocity, 0.2F, 0.6F);
    set_range(process, ParticleParam::AnimOffset, 0.0F, 1.0F);
    set_range(process, ParticleParam::AnimSpeed, 0.0F, 0.0F);
    process.alpha_curve.clear();
    process.color_ramp.clear();
    process.color_initial_ramp = {{.offset = 0.0F, .color = {1.0F, 0.2F, 0.2F, 0.7F}},
                                  {.offset = 0.5F, .color = {0.2F, 1.0F, 0.2F, 0.7F}},
                                  {.offset = 1.0F, .color = {0.2F, 0.4F, 1.0F, 0.7F}}};
    const std::vector<ParticleStep3D> steps = run_steps(12, 1.0F / 30.0F, 4.0F, 1.0F);

    auto system = device.create_particles_3d(kAmount);
    REQUIRE_MESSAGE(system.ok(), error_of(system));
    REQUIRE(device.process_particles_3d(system.value(), process, steps).ok());
    auto target = device.create_texture({.width = size, .height = size, .render_target = true});
    REQUIRE(target.ok());
    auto texture = device.create_texture(
        {.width = 4, .height = 4, .filter = render::Filter::Nearest}, sheet());
    REQUIRE(texture.ok());

    const slash::Vec3 eye{0.0F, 0.0F, 6.0F};
    const render::ParticleTransform camera{1, 0, 0, 0, 1, 0, 0, 0, 1, eye.x, eye.y, eye.z};
    const std::vector<Vertex3D> source = card(0.5F);
    std::vector<render::MeshId> meshes;
    const std::vector<ParticleMeshDraw3D> draws = varied_draws(camera);
    for (const ParticleMeshDraw3D& draw : draws) {
        auto mesh = device.create_mesh(std::vector<Vertex3D>(source.size() * kAmount), {});
        REQUIRE(mesh.ok());
        const auto filled = device.fill_particle_mesh(system.value(), mesh.value(), source, draw);
        REQUIRE_MESSAGE(filled.ok(), error_of(filled));
        meshes.push_back(mesh.value());
    }
    render::Scene3D scene;
    scene.camera = render::look_at({eye.x, eye.y, eye.z}, {0.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F});
    scene.projection = render::perspective(70.0F, 1.0F, 0.1F, 100.0F);
    REQUIRE(device
                .begin_frame(
                    {.target = target.value(), .clear = {0.05F, 0.05F, 0.1F, 1.0F}, .scene = scene})
                .ok());
    // Each draw's particles in a column of their own.
    for (std::size_t i = 0; i < meshes.size(); ++i) {
        const float x = (static_cast<float>(i) - 2.0F) * 2.2F;
        const std::array<float, 16> placed{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, x, 0, 0, 1};
        render::Material3D material{.albedo = {1.0F, 1.0F, 1.0F, 1.0F},
                                    .vertex_color_use_as_albedo = true,
                                    .unshaded = true,
                                    .transparency = render::Transparency::Alpha,
                                    .cull = render::CullMode::Disabled};
        if (draws[i].billboard == render::ParticleBillboard::Particles) {
            material.albedo_texture = {.texture = texture.value()};
        }
        CHECK(device.draw_mesh(meshes[i], placed, material).ok());
    }
    REQUIRE(device.end_frame().ok());
    auto image = device.read_texture(target.value());
    REQUIRE_MESSAGE(image.ok(), error_of(image));
    for (const render::MeshId mesh : meshes) {
        device.destroy_mesh(mesh);
    }
    device.destroy_texture(texture.value());
    device.destroy_texture(target.value());
    device.destroy_particles_3d(system.value());
    return image.value();
}

std::array<int, 4> pixel(const Image& image, std::uint32_t x, std::uint32_t y) {
    const std::size_t at = (std::size_t{y} * image.width + x) * 4;
    return {image.pixels.at(at), image.pixels.at(at + 1), image.pixels.at(at + 2),
            image.pixels.at(at + 3)};
}

} // namespace

TEST_CASE("the 3D particle shaders' sort and groups cover every slot") {
    using render::detail::bitonic_steps;
    using render::detail::mesh_groups;
    using render::detail::sort_size;
    CHECK(sort_size(1) == 1);
    CHECK(sort_size(5) == 8);
    CHECK(sort_size(1024) == 1024);
    // A bitonic sort of 2^n slots takes n(n+1)/2 steps, each comparing slots j apart in runs of k.
    const auto steps = bitonic_steps(8);
    REQUIRE(steps.size() == 6);
    CHECK(steps.front() == std::array<std::uint32_t, 2>{1, 2});
    CHECK(steps.back() == std::array<std::uint32_t, 2>{1, 8});
    CHECK(bitonic_steps(1).empty());
    // Rows of at most 65535 groups.
    CHECK(mesh_groups(0) == std::array<std::uint32_t, 2>{1, 1});
    CHECK(mesh_groups(65) == std::array<std::uint32_t, 2>{2, 1});
    CHECK(mesh_groups(64 * 70000) == std::array<std::uint32_t, 2>{65535, 2});
}

TEST_CASE("every device steps 3D particles as the CPU twin does") {
    constexpr std::uint32_t kAmount = 100;
    std::vector<ParticleStep3D> steps = run_steps(45, 0.02F, 0.6F);
    for (std::size_t i = 30; i < steps.size(); ++i) {
        // Turned, moved and moving, with the last steps ending early.
        steps[i].emission = {0.8F, 0.6F, 0.0F, -0.6F, 0.8F, 0.0F,
                             0.0F, 0.0F, 1.0F, 1.0F,  2.0F, -0.5F};
        steps[i].emitter_velocity = {1.0F, -0.5F, 0.25F};
        steps[i].interp_to_end = i > 40 ? 0.25F : 0.0F;
        steps[i].amount_ratio = 0.7F;
    }
    std::vector<std::unique_ptr<RenderDevice>> devices = slash::test::hardware_devices();
    devices.push_back(render::create_software_device());
    for (auto& device : devices) {
        CAPTURE(render::backend_name(device->backend()));
        int which = 0;
        for (const ParticleProcess3D& process : varied_processes()) {
            const int index = which++;
            CAPTURE(index);
            std::vector<Particle3D> twin(kAmount);
            render::process_particles_3d(twin, process, steps);

            auto system = device->create_particles_3d(kAmount);
            REQUIRE_MESSAGE(system.ok(), error_of(system));
            // In two calls, as a game's ticks would be.
            REQUIRE(
                device->process_particles_3d(system.value(), process, std::span(steps).first(20))
                    .ok());
            REQUIRE(
                device->process_particles_3d(system.value(), process, std::span(steps).subspan(20))
                    .ok());
            auto read = device->read_particles_3d(system.value());
            REQUIRE_MESSAGE(read.ok(), error_of(read));
            REQUIRE(read.value().size() == kAmount);
            int live = 0;
            for (std::uint32_t i = 0; i < kAmount; ++i) {
                live += twin[i].active() ? 1 : 0;
                CHECK_MESSAGE(close(read.value()[i], twin[i]), "particle ", i, ": device ",
                              describe(read.value()[i]), ", twin ", describe(twin[i]));
            }
            CHECK(live > 20);
            device->destroy_particles_3d(system.value());
        }
    }
}

TEST_CASE("every device places 3D particles' meshes as the software device does") {
    const auto software = render::create_software_device();
    const Image expected = draw_particles(*software, 128);
    // Something of each column drew.
    int lit = 0;
    for (std::uint32_t y = 0; y < expected.height; ++y) {
        for (std::uint32_t x = 0; x < expected.width; ++x) {
            lit += pixel(expected, x, y)[0] > 40 ? 1 : 0;
        }
    }
    CHECK(lit > 400);
    for (auto& device : slash::test::hardware_devices()) {
        CAPTURE(render::backend_name(device->backend()));
        const bool opengl = device->backend() == render::Backend::OpenGl ||
                            device->backend() == render::Backend::OpenGlEs;
        if (opengl) {
            continue; // OpenGL blends as Compatibility; it places them on the CPU, as tested above
        }
        const Image image = draw_particles(*device, 128);
        int differing = 0;
        int worst = 0;
        for (std::uint32_t y = 0; y < image.height; ++y) {
            for (std::uint32_t x = 0; x < image.width; ++x) {
                const std::array<int, 4> a = pixel(image, x, y);
                const std::array<int, 4> b = pixel(expected, x, y);
                int most = 0;
                for (std::size_t c = 0; c < 4; ++c) {
                    most = std::max(most, std::abs(a.at(c) - b.at(c)));
                }
                differing += most > 2 ? 1 : 0;
                worst = std::max(worst, most);
            }
        }
        CAPTURE(worst);
        CHECK(differing <= 40);
        if (differing > 40) {
            auto drawn = render::encode_png(image);
            auto wanted = render::encode_png(expected);
            REQUIRE(drawn.ok());
            REQUIRE(wanted.ok());
            MESSAGE("drawn, as a base64 PNG: " << slash::base64_encode(drawn.value()));
            MESSAGE("expected, as a base64 PNG: " << slash::base64_encode(wanted.value()));
        }
    }
}

TEST_CASE("every device refuses the same 3D particle mistakes") {
    std::vector<std::unique_ptr<RenderDevice>> devices = slash::test::hardware_devices();
    devices.push_back(render::create_software_device());
    devices.push_back(render::create_null_device());
    const std::vector<ParticleStep3D> steps = run_steps(3, 0.02F, 1.0F);
    for (auto& device : devices) {
        CAPTURE(render::backend_name(device->backend()));
        CHECK_FALSE(device->create_particles_3d(0).ok());
        CHECK_FALSE(device->create_particles_3d(render::kMaxParticles3D + 1).ok());
        auto system = device->create_particles_3d(4);
        REQUIRE(system.ok());

        ParticleProcess3D long_curve;
        for (int i = 0; i < 33; ++i) {
            long_curve.alpha_curve.push_back({static_cast<float>(i) / 32.0F, 1.0F});
        }
        CHECK_FALSE(device->process_particles_3d(system.value(), long_curve, steps).ok());
        ParticleProcess3D backwards;
        backwards.scale_curve.axes[2] = {{0.5F, 1.0F}, {0.2F, 1.0F}};
        CHECK_FALSE(device->process_particles_3d(system.value(), backwards, steps).ok());
        std::vector<ParticleStep3D> bad = steps;
        bad[1].lifetime = 0.0F;
        CHECK_FALSE(device->process_particles_3d(system.value(), {}, bad).ok());
        bad = steps;
        bad[2].delta = -1.0F;
        CHECK_FALSE(device->process_particles_3d(system.value(), {}, bad).ok());
        CHECK_FALSE(device->process_particles_3d(render::Particles3DId{999}, {}, steps).ok());
        CHECK(device->process_particles_3d(system.value(), {}, steps).ok());

        const std::vector<Vertex3D> source = card(1.0F);
        auto small = device->create_mesh(std::vector<Vertex3D>(source.size()), {});
        REQUIRE(small.ok());
        CHECK_FALSE(device->fill_particle_mesh(system.value(), small.value(), source, {}).ok());
        auto mesh = device->create_mesh(std::vector<Vertex3D>(source.size() * 4), {});
        REQUIRE(mesh.ok());
        CHECK_FALSE(device->fill_particle_mesh(system.value(), mesh.value(), {}, {}).ok());
        CHECK_FALSE(
            device->fill_particle_mesh(system.value(), render::MeshId{999}, source, {}).ok());
        CHECK_FALSE(
            device->fill_particle_mesh(render::Particles3DId{999}, mesh.value(), source, {}).ok());
        CHECK(device->fill_particle_mesh(system.value(), mesh.value(), source, {}).ok());
        CHECK_FALSE(device->read_particles_3d(render::Particles3DId{999}).ok());

        // Not while a frame is drawn.
        auto target = device->create_texture({.width = 4, .height = 4, .render_target = true});
        REQUIRE(target.ok());
        REQUIRE(device->begin_frame({.target = target.value(), .clear = {}}).ok());
        CHECK_FALSE(device->fill_particle_mesh(system.value(), mesh.value(), source, {}).ok());
        REQUIRE(device->end_frame().ok());

        device->destroy_mesh(small.value());
        device->destroy_mesh(mesh.value());
        device->destroy_texture(target.value());
        device->destroy_particles_3d(system.value());
        CHECK_FALSE(device->read_particles_3d(system.value()).ok());
    }
}
