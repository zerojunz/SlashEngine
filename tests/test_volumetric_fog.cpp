#include "../engine/render/src/mesh_shading.hpp"
#include "../engine/render/src/volumetric_fog.hpp"
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
#include <string>
#include <utility>
#include <vector>

namespace render = slash::render;
namespace detail = render::detail;
using render::Image;
using render::Light3D;
using render::LightType;
using render::RenderDevice;
using render::Vertex3D;
using Float3 = std::array<float, 3>;
using Mat4 = std::array<float, 16>;

namespace {

template <typename T>
std::string error_of(const slash::Result<T>& result) {
    return result.ok() ? "(succeeded)" : result.error().message;
}

slash::json::Value parse(const char* text) {
    auto parsed = slash::json::parse(text);
    REQUIRE(parsed.ok());
    return std::move(parsed).value();
}

// The devices that draw Forward+: the software device and the GPU device. The OpenGL devices
// always draw as Compatibility, which has no volumetric fog.
std::vector<std::unique_ptr<RenderDevice>> forward_devices() {
    std::vector<std::unique_ptr<RenderDevice>> devices;
    devices.push_back(render::create_software_device());
    if (auto gpu = slash::test::gpu_device(); gpu != nullptr) {
        devices.push_back(std::move(gpu));
    }
    return devices;
}

// A square of side size around center, facing normal, its corners clockwise from the front.
std::vector<Vertex3D> quad(const Float3& center, const Float3& u, const Float3& up,
                           const Float3& normal, float size) {
    const auto corner = [&](float s, float t) {
        const float a = s * size * 0.5F;
        const float b = t * size * 0.5F;
        return Vertex3D{.position = {center[0] + (u[0] * a) + (up[0] * b),
                                     center[1] + (u[1] * a) + (up[1] * b),
                                     center[2] + (u[2] * a) + (up[2] * b)},
                        .normal = normal,
                        .tangent = {u[0], u[1], u[2], 1.0F},
                        .uv = {(s + 1.0F) * 0.5F, (1.0F - t) * 0.5F}};
    };
    return {corner(-1, 1), corner(1, 1),  corner(1, -1),
            corner(-1, 1), corner(1, -1), corner(-1, -1)};
}

constexpr Mat4 kIdentity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

// What draw_fog draws.
struct Foggy {
    bool fog = true;
    render::RenderMethod method = render::RenderMethod::ForwardPlus;
    std::uint32_t frames = 1; // drawn in a row on one target
    bool reproject = true;    // the fog's temporal reprojection
    bool filter = true;       // the project's use_filter
    bool shadow = true;       // the sun casts a shadow
    bool lamp = true;         // an omni light with a shadow hangs over the floor
    float anisotropy = 0.2F;
    float turn = 0.0F; // how far the camera moves right each frame
    float sky_affect = 1.0F;
    bool disable_fog = false; // the floor's material
};

// A gray floor seen at a slant under a wall that shades the sun, lit by the sun from behind it
// and a lamp, in fog, over a blue sky, 48 x 32 pixels.
Image draw_fog(RenderDevice& device, const Foggy& foggy) {
    auto target = device.create_texture({.width = 48, .height = 32, .render_target = true});
    REQUIRE(target.ok());
    auto floor = device.create_mesh(quad({0, 0, 0}, {1, 0, 0}, {0, 0, -1}, {0, 1, 0}, 16.0F), {});
    REQUIRE(floor.ok());
    auto wall = device.create_mesh(quad({0, 2.5F, -3}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, 3.0F), {});
    REQUIRE(wall.ok());
    for (std::uint32_t frame = 0; frame < foggy.frames; ++frame) {
        render::Scene3D scene;
        scene.method = foggy.method;
        const float eye = foggy.turn * static_cast<float>(frame);
        scene.camera = render::look_at({eye, 2.0F, 5.0F}, {eye, 0.5F, -2.0F}, {0, 1, 0});
        scene.projection = render::perspective(60.0F, 1.5F, 0.1F, 40.0F);
        scene.ambient = {0.2F, 0.2F, 0.25F};
        scene.sky.kind = render::SkyKind::Color;
        scene.sky.color = {0.3F, 0.5F, 0.9F};
        Light3D sun{.type = LightType::Directional,
                    .color = {1.0F, 0.9F, 0.7F},
                    .energy = 1.5F,
                    .direction = {0.0F, -0.5F, 0.8660254F},
                    .up = {0.0F, 0.8660254F, 0.5F},
                    .shadow = foggy.shadow};
        Light3D lamp{.type = LightType::Omni,
                     .color = {1.0F, 0.6F, 0.3F},
                     .energy = 3.0F,
                     .position = {2.0F, 1.5F, 0.0F},
                     .range = 4.0F,
                     .shadow = foggy.shadow};
        lamp.volumetric_fog_energy = 2.0F;
        scene.lights = {sun};
        if (foggy.lamp) {
            scene.lights.push_back(lamp);
        }
        scene.shadows = {.directional_size = 1024, .positional_size = 1024};
        scene.volumetric_fog = {
            .enabled = foggy.fog,
            .density = 0.08F,
            .albedo = {0.9F, 0.95F, 1.0F},
            .emission = {0.05F, 0.02F, 0.0F},
            .emission_energy = 1.0F,
            .anisotropy = foggy.anisotropy,
            .length = 24.0F,
            .detail_spread = 2.0F,
            .ambient_inject = 0.5F,
            .sky_affect = foggy.sky_affect,
            .temporal_reprojection = foggy.reproject,
            .temporal_reprojection_amount = 0.9F,
            .settings = {.volume_size = 32, .volume_depth = 32, .use_filter = foggy.filter}};
        const auto begun =
            device.begin_frame({.target = target.value(), .clear = {}, .scene = scene});
        REQUIRE_MESSAGE(begun.ok(), error_of(begun));
        const auto floor_drawn = device.draw_mesh(
            floor.value(), kIdentity,
            {.albedo = {0.7F, 0.7F, 0.7F, 1.0F}, .disable_fog = foggy.disable_fog});
        REQUIRE_MESSAGE(floor_drawn.ok(), error_of(floor_drawn));
        const auto wall_drawn = device.draw_mesh(
            wall.value(), kIdentity,
            {.albedo = {0.5F, 0.3F, 0.2F, 1.0F}, .cull = render::CullMode::Disabled});
        REQUIRE_MESSAGE(wall_drawn.ok(), error_of(wall_drawn));
        const auto ended = device.end_frame();
        REQUIRE_MESSAGE(ended.ok(), error_of(ended));
    }
    auto image = device.read_texture(target.value());
    REQUIRE_MESSAGE(image.ok(), error_of(image));
    device.destroy_mesh(floor.value());
    device.destroy_mesh(wall.value());
    device.destroy_texture(target.value());
    return image.value();
}

int difference(const Image& a, const Image& b) {
    int worst = 0;
    for (std::size_t i = 0; i < a.pixels.size(); ++i) {
        worst = std::max(worst, std::abs(int{a.pixels[i]} - int{b.pixels[i]}));
    }
    return worst;
}

// How many of the channels of a and b differ by more than by.
std::size_t differing(const Image& a, const Image& b, int by) {
    std::size_t count = 0;
    for (std::size_t i = 0; i < a.pixels.size(); ++i) {
        count += std::abs(int{a.pixels[i]} - int{b.pixels[i]}) > by ? 1U : 0U;
    }
    return count;
}

std::array<int, 4> pixel(const Image& image, std::uint32_t x, std::uint32_t y) {
    const std::size_t at = (std::size_t{y} * image.width + x) * 4;
    return {image.pixels.at(at), image.pixels.at(at + 1), image.pixels.at(at + 2),
            image.pixels.at(at + 3)};
}

// A Forward+ scene with fog of density and emission only, seen straight ahead.
render::Scene3D emitting(float density) {
    render::Scene3D scene;
    scene.camera = render::look_at({0, 0, 0}, {0, 0, -1}, {0, 1, 0});
    scene.projection = render::perspective(60.0F, 1.0F, 0.1F, 100.0F);
    scene.volumetric_fog = {.enabled = true,
                            .density = density,
                            .emission = {1.0F, 1.0F, 1.0F},
                            .length = 64.0F,
                            .temporal_reprojection = false,
                            .settings = {.volume_size = 16, .volume_depth = 32}};
    return scene;
}

} // namespace

TEST_CASE("volumetric fog settings come from project.json's rendering.environment") {
    const render::VolumetricFogSettings read = render::volumetric_fog_settings_from_json(
        parse(R"({"volume_size": 96, "volume_depth": 128, "use_filter": false})"));
    CHECK(read.volume_size == 96);
    CHECK(read.volume_depth == 128);
    CHECK_FALSE(read.use_filter);
    // Out of Godot's 16 to 512, fractions and other types keep the defaults.
    const render::VolumetricFogSettings kept = render::volumetric_fog_settings_from_json(
        parse(R"({"volume_size": 8, "volume_depth": 64.5, "use_filter": 0})"));
    CHECK(kept == render::VolumetricFogSettings{});
    CHECK(render::volumetric_fog_settings_from_json(parse("[]")) ==
          render::VolumetricFogSettings{});
}

TEST_CASE("the fog's volume is sized as Godot sizes it for the frame's shape") {
    const render::VolumetricFogSettings settings;
    // Godot: width = size * ratio, height = size / ratio, ratio = w / ((w + h) / 2).
    CHECK(detail::fog_volume_size(settings, 1280, 720) == std::array<std::uint32_t, 3>{81, 50, 64});
    CHECK(detail::fog_volume_size(settings, 512, 512) == std::array<std::uint32_t, 3>{64, 64, 64});
    CHECK(detail::fog_volume_size({.volume_size = 512, .volume_depth = 512}, 4096, 4096) ==
          std::array<std::uint32_t, 3>{512, 512, 32});
}

TEST_CASE("only Forward+ frames, not reflection probes' captures, have volumetric fog") {
    render::Scene3D scene;
    CHECK_FALSE(detail::uses_volumetric_fog(scene));
    scene.volumetric_fog.enabled = true;
    CHECK(detail::uses_volumetric_fog(scene));
    scene.method = render::RenderMethod::Compatibility;
    CHECK_FALSE(detail::uses_volumetric_fog(scene));
    scene.method = render::RenderMethod::ForwardPlus;
    scene.capture = render::ProbeCapture{};
    CHECK_FALSE(detail::uses_volumetric_fog(scene));
}

TEST_CASE("emitting fog gathers its light and lets through what Beer's law says") {
    for (const float density : {0.01F, 0.05F, 0.2F}) {
        CAPTURE(density);
        const render::Scene3D scene = emitting(density);
        const detail::MeshSceneUniforms lights = detail::mesh_scene_uniforms(scene);
        detail::FogClock clock;
        const detail::FogUniforms uniforms =
            detail::volumetric_fog_uniforms(scene, lights, 64, 64, clock);
        CHECK(clock.frames == 1);
        for (const bool filter : {false, true}) {
            const detail::FogVolume volume = detail::run_volumetric_fog(uniforms, {}, {}, filter);
            const std::array<std::uint32_t, 3> size{16, 16, 32};
            detail::MeshSceneUniforms fogged = lights;
            detail::set_volumetric_fog(fogged, scene, size);
            // Through the whole fog, to its last slice's center: 64 * (31.5 / 32)^2 meters.
            const float depth = 64.0F * (31.5F / 32.0F) * (31.5F / 32.0F);
            const float through = std::exp(-density * depth);
            const detail::Float4 seen = detail::sample_fog_volume(
                volume.fog, size, fogged.volumetric_fog, {0.5F, 0.5F}, 100.0F);
            CHECK(seen[3] == doctest::Approx(through).epsilon(0.001));
            // The light it adds is its emission times what it hides.
            CHECK(seen[0] == doctest::Approx(1.0F - through).epsilon(0.001));
            // Nearer points are seen through less of it.
            const detail::Float4 near = detail::sample_fog_volume(
                volume.fog, size, fogged.volumetric_fog, {0.5F, 0.5F}, 4.0F);
            CHECK(near[3] > seen[3]);
            CHECK(near[0] < seen[0]);
        }
    }
}

TEST_CASE("the fog reprojects the frame before's density from its second frame on") {
    render::Scene3D scene = emitting(0.05F);
    scene.volumetric_fog.temporal_reprojection = true;
    const detail::MeshSceneUniforms lights = detail::mesh_scene_uniforms(scene);
    detail::FogClock clock;
    const detail::FogUniforms first = detail::volumetric_fog_uniforms(scene, lights, 64, 64, clock);
    CHECK(first.emission[3] == 0.0F);
    const detail::FogUniforms second =
        detail::volumetric_fog_uniforms(scene, lights, 64, 64, clock);
    CHECK(second.emission[3] == 1.0F);
    CHECK(second.jitter[3] == doctest::Approx(0.9F));
    // Another size starts it again.
    const detail::FogUniforms resized =
        detail::volumetric_fog_uniforms(scene, lights, 128, 64, clock);
    CHECK(resized.emission[3] == 0.0F);
    // A still camera reprojects the density where it was: a uniform fog stays as it is.
    const detail::FogVolume before = detail::run_volumetric_fog(first, {}, {}, false);
    const detail::FogVolume after = detail::run_volumetric_fog(second, {}, before.density, false);
    for (std::size_t i = 0; i < before.density.size(); i += 97) {
        CHECK(after.density[i][3] == doctest::Approx(before.density[i][3]).epsilon(0.0001));
    }
}

TEST_CASE("Forward+ volumetric fog lights the view, shaded by the lights' shadows") {
    for (auto& device : forward_devices()) {
        CAPTURE(render::backend_name(device->backend()));
        const Image clear = draw_fog(*device, {.fog = false});
        const Image fogged = draw_fog(*device, {});
        // The fog changes the floor, the wall and the sky.
        CHECK(differing(fogged, clear, 8) > fogged.pixels.size() / 2);
        // The sky is seen through it all, and the far floor through more than the near.
        const std::array<int, 4> sky = pixel(fogged, 24, 1);
        CHECK(sky != pixel(clear, 24, 1));
        CHECK(sky[3] == 255);
        // Without its effect on the sky, the sky is as it is.
        const Image clear_sky = draw_fog(*device, {.sky_affect = 0.0F});
        CHECK(pixel(clear_sky, 24, 1) == pixel(clear, 24, 1));
        // A material that disables fog leaves the floor clear of it.
        const Image disabled = draw_fog(*device, {.disable_fog = true});
        CHECK(pixel(disabled, 24, 30) == pixel(clear, 24, 30));
        // The shadows darken the fog in them.
        const Image unshaded = draw_fog(*device, {.shadow = false});
        CHECK(differing(unshaded, fogged, 2) > 0);
        // Godot's Compatibility renderer has no volumetric fog.
        const auto compatibility = render::RenderMethod::Compatibility;
        CHECK(difference(draw_fog(*device, {.method = compatibility}),
                         draw_fog(*device, {.fog = false, .method = compatibility})) == 0);
    }
}

TEST_CASE("GPUs draw volumetric fog as the software device does") {
    auto gpu = slash::test::gpu_device();
    if (gpu == nullptr) {
        MESSAGE("no GPU backend; skipped");
        return;
    }
    const auto software = render::create_software_device();
    const std::vector<Foggy> scenes{
        {},
        {.filter = false},
        {.frames = 4},
        {.frames = 4, .turn = 0.2F},
        {.lamp = false, .anisotropy = -0.5F},
        {.shadow = false, .anisotropy = 0.8F},
    };
    for (std::size_t i = 0; i < scenes.size(); ++i) {
        CAPTURE(i);
        const Image expected = draw_fog(*software, scenes[i]);
        const Image drawn = draw_fog(*gpu, scenes[i]);
        // Floats round differently on each GPU, and shadow edges move a texel now and then.
        CHECK(differing(drawn, expected, 3) <= drawn.pixels.size() / 50);
        CHECK(difference(drawn, expected) <= 24);
    }
}
