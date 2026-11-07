#include "../engine/render/src/subsurface.hpp"
#include "devices.hpp"

#include "slash/core/json.hpp"
#include "slash/render/device.hpp"
#include "slash/render/image.hpp"
#include "slash/render/mesh.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <numbers>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace render = slash::render;
namespace detail = render::detail;
namespace json = slash::json;
using render::Image;
using render::Material3D;
using render::RenderDevice;
using render::Vertex3D;
using Float3 = std::array<float, 3>;
using Mat4 = std::array<float, 16>;

namespace {

// The devices that draw Forward+: the software device and the GPU device. The OpenGL devices
// always draw as Compatibility, which has no subsurface scattering.
std::vector<std::unique_ptr<RenderDevice>> forward_devices() {
    std::vector<std::unique_ptr<RenderDevice>> devices;
    devices.push_back(render::create_software_device());
    if (auto gpu = slash::test::gpu_device(); gpu != nullptr) {
        devices.push_back(std::move(gpu));
    }
    return devices;
}

std::array<int, 4> pixel(const Image& image, std::uint32_t x, std::uint32_t y) {
    const std::size_t at = (std::size_t{y} * image.width + x) * 4;
    return {image.pixels.at(at), image.pixels.at(at + 1), image.pixels.at(at + 2),
            image.pixels.at(at + 3)};
}

// How many pixels of a and b differ by more than by in a channel, and the most any does.
std::pair<int, int> differing(const Image& a, const Image& b, int by) {
    int count = 0;
    int worst = 0;
    for (std::uint32_t y = 0; y < a.height; ++y) {
        for (std::uint32_t x = 0; x < a.width; ++x) {
            const std::array<int, 4> p = pixel(a, x, y);
            const std::array<int, 4> q = pixel(b, x, y);
            int most = 0;
            for (std::size_t c = 0; c < 4; ++c) {
                most = std::max(most, std::abs(p.at(c) - q.at(c)));
            }
            count += most > by ? 1 : 0;
            worst = std::max(worst, most);
        }
    }
    return {count, worst};
}

// A sphere of radius around the origin.
std::vector<Vertex3D> sphere(float radius) {
    constexpr int kRings = 16;
    constexpr int kSegments = 24;
    const auto corner = [&](int ring, int segment) {
        const double theta = std::numbers::pi * ring / kRings;
        const double phi = 2.0 * std::numbers::pi * segment / kSegments;
        const Float3 n{static_cast<float>(std::sin(theta) * std::cos(phi)),
                       static_cast<float>(std::cos(theta)),
                       static_cast<float>(std::sin(theta) * std::sin(phi))};
        return Vertex3D{
            .position = {n[0] * radius, n[1] * radius, n[2] * radius},
            .normal = n,
            .tangent = {static_cast<float>(-std::sin(phi)), 0.0F, static_cast<float>(std::cos(phi)),
                        1.0F},
            .uv = {static_cast<float>(segment) / kSegments, static_cast<float>(ring) / kRings}};
    };
    std::vector<Vertex3D> out;
    for (int ring = 0; ring < kRings; ++ring) {
        for (int segment = 0; segment < kSegments; ++segment) {
            const Vertex3D a = corner(ring, segment);
            const Vertex3D b = corner(ring, segment + 1);
            const Vertex3D c = corner(ring + 1, segment + 1);
            const Vertex3D d = corner(ring + 1, segment);
            out.insert(out.end(), {a, b, c, a, c, d});
        }
    }
    return out;
}

// A cube of side 1 around the origin.
std::vector<Vertex3D> cube() {
    struct Face {
        Float3 n;
        Float3 u;
        Float3 up;
    };
    const std::array<Face, 6> faces{{
        {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}},
        {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}},
        {{0, 1, 0}, {1, 0, 0}, {0, 0, -1}},
        {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}},
        {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}},
        {{0, 0, -1}, {-1, 0, 0}, {0, 1, 0}},
    }};
    std::vector<Vertex3D> out;
    for (const Face& f : faces) {
        const auto corner = [&](float s, float t) {
            return Vertex3D{.position = {(f.n[0] * 0.5F) + ((f.u[0] * s) + (f.up[0] * t)) * 0.5F,
                                         (f.n[1] * 0.5F) + ((f.u[1] * s) + (f.up[1] * t)) * 0.5F,
                                         (f.n[2] * 0.5F) + ((f.u[2] * s) + (f.up[2] * t)) * 0.5F},
                            .normal = f.n,
                            .tangent = {f.u[0], f.u[1], f.u[2], 1.0F},
                            .uv = {(s + 1.0F) * 0.5F, (1.0F - t) * 0.5F}};
        };
        const Vertex3D a = corner(-1, 1);
        const Vertex3D b = corner(1, 1);
        const Vertex3D c = corner(1, -1);
        const Vertex3D d = corner(-1, -1);
        out.insert(out.end(), {a, b, c, a, c, d});
    }
    return out;
}

// The blur's uniforms for an orthogonal camera over a width x height frame, its step along the
// pass's axis scale * strength / 3 of the frame at each kernel offset.
detail::SubsurfaceUniforms uniforms(std::uint32_t width, std::uint32_t height, bool down,
                                    float scale, std::uint32_t samples = 6) {
    detail::SubsurfaceUniforms out;
    out.size = {width, height, down ? 1U : 0U, 1U};
    out.params = {0.0F, scale, 0.0F, static_cast<float>(samples)};
    out.projection = {1.0F, 0.0F, 0.0F, 1.0F};
    return out;
}

// A row of width pixels, all of color and strength, and its depth.
std::vector<float> row(std::size_t width, const std::array<float, 4>& color) {
    std::vector<float> out;
    for (std::size_t i = 0; i < width; ++i) {
        out.insert(out.end(), color.begin(), color.end());
    }
    return out;
}

// What draw_ball draws.
struct Ball {
    render::RenderMethod method = render::RenderMethod::ForwardPlus;
    render::SubsurfaceQuality quality = render::SubsurfaceQuality::Medium;
    float subsurface = 1.0F;
    bool skin = false;
    std::uint32_t size = 48;
};

// A pale ball lit from the side, so its lit half meets its dark half down the middle, before a
// box behind it that takes no part in the scattering.
Image draw_ball(RenderDevice& device, const Ball& ball) {
    auto target =
        device.create_texture({.width = ball.size, .height = ball.size, .render_target = true});
    REQUIRE(target.ok());
    auto mesh = device.create_mesh(sphere(0.8F), {});
    REQUIRE(mesh.ok());
    auto box = device.create_mesh(cube(), {});
    REQUIRE(box.ok());
    render::Scene3D scene;
    scene.method = ball.method;
    scene.camera = render::look_at({0.0F, 0.0F, 3.0F}, {0.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F});
    scene.projection = render::perspective(50.0F, 1.0F, 0.1F, 20.0F);
    scene.ambient = {0.05F, 0.05F, 0.05F};
    scene.lights = {{.type = render::LightType::Directional,
                     .color = {1.0F, 0.95F, 0.9F},
                     .direction = {-1.0F, 0.0F, 0.0F}}};
    scene.subsurface = {.quality = ball.quality, .scale = 0.2F, .depth_scale = 0.0F};
    REQUIRE(
        device.begin_frame({.target = target.value(), .clear = {0, 0, 0, 1}, .scene = scene}).ok());
    constexpr Mat4 kBehind{2, 0, 0, 0, 0, 2, 0, 0, 0, 0, 0.2F, 0, 0, 0, -2, 1};
    REQUIRE(device.draw_mesh(box.value(), kBehind, {.albedo = {0.2F, 0.5F, 0.3F, 1.0F}}).ok());
    constexpr Mat4 kHere{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    REQUIRE(device
                .draw_mesh(mesh.value(), kHere,
                           {.albedo = {0.9F, 0.75F, 0.65F, 1.0F},
                            .roughness = 0.8F,
                            .subsurface = ball.subsurface,
                            .subsurface_skin = ball.skin})
                .ok());
    REQUIRE(device.end_frame().ok());
    auto image = device.read_texture(target.value());
    REQUIRE(image.ok());
    device.destroy_mesh(mesh.value());
    device.destroy_mesh(box.value());
    device.destroy_texture(target.value());
    return image.value();
}

// A white slab, thickness meters deep, facing the camera, lit only from behind by a directional
// light, with the transmittance on or off: the middle pixel of its front face. The light is
// turned a little so that no pixel looks straight into it, where Godot's half vector is 0.
std::array<int, 4> through_slab(RenderDevice& device, float thickness, bool transmittance,
                                bool shadow) {
    auto target = device.create_texture({.width = 15, .height = 15, .render_target = true});
    REQUIRE(target.ok());
    auto box = device.create_mesh(cube(), {});
    REQUIRE(box.ok());
    render::Scene3D scene;
    scene.camera = render::look_at({0.0F, 0.0F, 3.0F}, {0.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F});
    scene.projection = render::perspective(40.0F, 1.0F, 0.1F, 20.0F);
    scene.lights = {{.type = render::LightType::Directional,
                     .direction = {0.0F, 0.0998F, 0.995F},
                     .up = {0.0F, 1.0F, 0.0F},
                     .shadow = shadow,
                     .shadow_normal_bias = 0.0F}};
    scene.shadows = {.directional_size = 2048, .positional_size = 2048};
    REQUIRE(device.begin_frame({.target = target.value(), .clear = {}, .scene = scene}).ok());
    const Mat4 slab{2, 0, 0, 0, 0, 2, 0, 0, 0, 0, thickness, 0, 0, 0, 0, 1};
    const auto drawn = device.draw_mesh(box.value(), slab,
                                        {.albedo = {1.0F, 1.0F, 1.0F, 1.0F},
                                         .subsurface = 1.0F,
                                         .transmittance = transmittance,
                                         .transmittance_color = {1.0F, 1.0F, 1.0F, 1.0F},
                                         .transmittance_depth = 1.0F});
    REQUIRE(drawn.ok());
    REQUIRE(device.end_frame().ok());
    auto image = device.read_texture(target.value());
    REQUIRE(image.ok());
    device.destroy_mesh(box.value());
    device.destroy_texture(target.value());
    return pixel(image.value(), 7, 7);
}

} // namespace

TEST_CASE("subsurface scattering settings read project.json's as Godot's") {
    const render::SubsurfaceSettings defaults;
    CHECK(defaults.quality == render::SubsurfaceQuality::Low);
    CHECK(defaults.scale == 0.05F);
    CHECK(defaults.depth_scale == 0.01F);
    auto parsed = json::parse(R"({"quality": "high", "scale": 0.2, "depth_scale": 0.5})");
    REQUIRE(parsed.ok());
    const render::SubsurfaceSettings read = render::subsurface_settings_from_json(parsed.value());
    CHECK(read.quality == render::SubsurfaceQuality::High);
    CHECK(read.scale == 0.2F);
    CHECK(read.depth_scale == 0.5F);
    // Values out of range or of the wrong kind keep their defaults.
    auto bad = json::parse(R"({"quality": "ultra", "scale": 3, "depth_scale": "x"})");
    REQUIRE(bad.ok());
    CHECK(render::subsurface_settings_from_json(bad.value()) == defaults);
    CHECK(detail::subsurface_samples(render::SubsurfaceQuality::Low) == 6);
    CHECK(detail::subsurface_samples(render::SubsurfaceQuality::Medium) == 9);
    CHECK(detail::subsurface_samples(render::SubsurfaceQuality::High) == 13);
}

TEST_CASE("only lit, opaque materials in Forward+ frames scatter under their surface") {
    render::Scene3D scene;
    scene.method = render::RenderMethod::ForwardPlus;
    CHECK(detail::uses_subsurface(scene));
    scene.subsurface.quality = render::SubsurfaceQuality::Disabled;
    CHECK(!detail::uses_subsurface(scene));
    scene.subsurface.quality = render::SubsurfaceQuality::High;
    scene.method = render::RenderMethod::Compatibility;
    CHECK(!detail::uses_subsurface(scene));
    scene.method = render::RenderMethod::ForwardPlus;
    scene.path_trace = render::PathTrace3D{};
    CHECK(!detail::uses_subsurface(scene));

    Material3D material{.subsurface = 0.5F};
    CHECK(detail::draws_subsurface(material));
    material.transparency = render::Transparency::Alpha;
    CHECK(!detail::draws_subsurface(material));
    material.transparency = render::Transparency::AlphaScissor;
    CHECK(detail::draws_subsurface(material));
    material.unshaded = true;
    CHECK(!detail::draws_subsurface(material));
    CHECK(!detail::draws_subsurface({}));
}

TEST_CASE("the subsurface blur spreads light by Jimenez's kernels, as Godot's") {
    constexpr std::uint32_t kWidth = 32;
    const std::vector<float> depth(kWidth, 0.5F);

    // Light that is the same everywhere stays as it is: the weights are divided out.
    const std::vector<float> even = row(kWidth, {0.4F, 0.3F, 0.2F, 0.6F});
    const std::vector<float> across =
        detail::run_subsurface_pass(uniforms(kWidth, 1, false, 0.4F), even, depth, even);
    for (std::size_t i = 0; i < across.size(); ++i) {
        CHECK(across[i] == doctest::Approx(even[i]).epsilon(1e-5));
    }
    // The pass down gives the change, none here, with alpha 0.
    const std::vector<float> change =
        detail::run_subsurface_pass(uniforms(1, kWidth, true, 0.4F), even, depth, even);
    for (const float value : change) {
        CHECK(value == doctest::Approx(0.0F).epsilon(1e-5));
    }

    // A bright pixel in a dark row spreads to its neighbors and loses some of its own light.
    std::vector<float> spot = row(kWidth, {0.0F, 0.0F, 0.0F, 1.0F});
    std::ranges::fill(std::span(spot).subspan(16 * 4, 3), 1.0F);
    const std::vector<float> spread =
        detail::run_subsurface_pass(uniforms(kWidth, 1, false, 0.4F), spot, depth, spot);
    CHECK(spread[16 * 4] < 0.9F);
    CHECK(spread[17 * 4] > 0.01F);
    CHECK(spread[15 * 4] > 0.01F);
    CHECK(spread[17 * 4] == doctest::Approx(spread[15 * 4]));
    CHECK(spread[17 * 4 + 3] == 1.0F);

    // Skin's profile keeps less of red at the center than of blue, so red spreads further.
    std::vector<float> skin = spot;
    for (std::size_t i = 0; i < kWidth; ++i) {
        skin[(i * 4) + 3] = -1.0F;
    }
    const std::vector<float> skinned =
        detail::run_subsurface_pass(uniforms(kWidth, 1, false, 0.4F), skin, depth, skin);
    CHECK(skinned[17 * 4] > skinned[(17 * 4) + 2] * 1.2F);
    CHECK(skinned[16 * 4] < skinned[(16 * 4) + 2]);

    // A pixel of no strength keeps its light, and the pass down changes nothing there.
    std::vector<float> walled = spot;
    walled[(18 * 4) + 3] = 0.0F;
    walled[(18 * 4)] = 5.0F;
    const std::vector<float> stopped =
        detail::run_subsurface_pass(uniforms(kWidth, 1, false, 0.4F), walled, depth, walled);
    CHECK(stopped[18 * 4] == 5.0F);
    // The pass down gives no change where there is no strength.
    const std::vector<float> nothing =
        detail::run_subsurface_pass(uniforms(1, kWidth, true, 0.4F), walled, depth, walled);
    CHECK(nothing[18 * 4] == 0.0F);

    // More samples reach further: high quality's kernel spans 3 steps, low's 2. A step is
    // 0.4 / 3 of the row here, 4.3 pixels.
    const std::vector<float> wide =
        detail::run_subsurface_pass(uniforms(kWidth, 1, false, 0.4F, 13), spot, depth, spot);
    CHECK(wide[(16 + 11) * 4] > 0.0F);
    CHECK(spread[(16 + 11) * 4] == 0.0F);
}

TEST_CASE("every Forward+ device scatters light under a material's surface") {
    for (auto& device : forward_devices()) {
        CAPTURE(render::backend_name(device->backend()));
        const Image plain = draw_ball(*device, {.subsurface = 0.0F});
        const Image scattered = draw_ball(*device, {});
        // The dark half takes light from the lit half where they meet, and nothing else changes.
        const auto [count, worst] = differing(plain, scattered, 2);
        CAPTURE(worst);
        CHECK(count > 10);
        const std::array<int, 4> before = pixel(plain, 22, 24);
        const std::array<int, 4> after = pixel(scattered, 22, 24);
        CAPTURE(before);
        CAPTURE(after);
        CHECK(after[0] > before[0]);
        CHECK(pixel(scattered, 2, 2) == pixel(plain, 2, 2));
        // Skin's profile spreads red as the plain kernel does, and blue less.
        const std::array<int, 4> skin = pixel(draw_ball(*device, {.skin = true}), 22, 24);
        CAPTURE(skin);
        CHECK(std::abs(skin[0] - after[0]) <= 1);
        CHECK(skin[2] - before[2] < after[2] - before[2]);
        // Disabled, it draws as if no material scattered; in Compatibility, the same.
        CHECK(differing(draw_ball(*device, {.quality = render::SubsurfaceQuality::Disabled}), plain,
                        0)
                  .first == 0);
        CHECK(differing(draw_ball(*device, {.method = render::RenderMethod::Compatibility}),
                        draw_ball(*device, {.method = render::RenderMethod::Compatibility,
                                            .subsurface = 0.0F}),
                        0)
                  .first == 0);
    }
}

TEST_CASE("light passes through thin meshes by their transmittance, measured in shadow maps") {
    for (auto& device : forward_devices()) {
        CAPTURE(render::backend_name(device->backend()));
        // A tenth of the transmittance depth through: about half the light (e^-(8.25 * 0.1)^2,
        // the light's pi over pi), 188 in sRGB.
        const std::array<int, 4> thin = through_slab(*device, 0.1F, true, true);
        const std::array<int, 4> thick = through_slab(*device, 1.0F, true, true);
        const std::array<int, 4> off = through_slab(*device, 0.1F, false, true);
        const std::array<int, 4> unshadowed = through_slab(*device, 0.1F, true, false);
        CAPTURE(thin);
        CAPTURE(thick);
        CHECK(thin[0] > 150);
        CHECK(thin[0] < 225);
        CHECK(thick[0] < 10);
        CHECK(off[0] == 0);
        // Without a shadow map, the light is taken to go through the whole depth.
        CHECK(unshadowed[0] < 2);
    }
}

TEST_CASE("GPUs scatter light under a surface as the software device does") {
    const auto software = render::create_software_device();
    const Image expected = draw_ball(*software, {.quality = render::SubsurfaceQuality::High});
    const Image skin = draw_ball(*software, {.skin = true});
    for (auto& device : slash::test::hardware_devices()) {
        CAPTURE(render::backend_name(device->backend()));
        if (device->backend() == render::Backend::OpenGl ||
            device->backend() == render::Backend::OpenGlEs) {
            continue;
        }
        const auto [count, worst] = differing(
            draw_ball(*device, {.quality = render::SubsurfaceQuality::High}), expected, 2);
        CAPTURE(worst);
        CHECK(count <= 20);
        const auto [skin_count, skin_worst] =
            differing(draw_ball(*device, {.skin = true}), skin, 2);
        CAPTURE(skin_worst);
        CHECK(skin_count <= 20);
    }
}
