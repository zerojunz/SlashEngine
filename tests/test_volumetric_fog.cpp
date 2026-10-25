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
#include <cstring>
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
    std::vector<render::FogVolume3D> volumes{};
    bool voxel_gi = false;  // a VoxelGI glows over the floor
    float gi_inject = 1.0F; // how much of its light the fog takes
};

// A world turned by none, at x, y, z.
Mat4 placed(float x, float y, float z) {
    Mat4 out = kIdentity;
    out[12] = x;
    out[13] = y;
    out[14] = z;
    return out;
}

// A glowing box that thins with height, a default ellipsoid, cone and cylinder, and a world of
// thinner, bluer fog, about the floor.
std::vector<render::FogVolume3D> some_volumes() {
    return {
        {.transform = placed(-1.5F, 1.0F, 0.0F),
         .size = {1.5F, 2.0F, 1.5F},
         .shape = render::FogVolumeShape::Box,
         .material = true,
         .density = 1.5F,
         .albedo = {1.0F, 0.8F, 0.6F},
         .emission = {1.0F, 0.4F, 0.1F},
         .height_falloff = 0.5F,
         .edge_fade = 0.3F},
        {.transform = placed(1.5F, 1.0F, -1.0F), .shape = render::FogVolumeShape::Ellipsoid},
        {.transform = placed(0.0F, 1.0F, -4.0F),
         .size = {2.0F, 3.0F, 2.0F},
         .shape = render::FogVolumeShape::Cone},
        {.transform = placed(3.0F, 1.0F, -3.0F),
         .size = {1.0F, 2.0F, 1.0F},
         .shape = render::FogVolumeShape::Cylinder},
        {.shape = render::FogVolumeShape::World,
         .material = true,
         .density = -0.03F,
         .albedo = {0.5F, 0.6F, 1.0F}},
    };
}

// A VoxelGI 4 x 2 x 4 meters about the floor, its 4 x 4 x 4 cells of two levels glowing warm
// on the left, green deeper in and blue up high, in a 16 x 6 atlas: level 0's slices side by side
// on top, level 1's (2 x 2 x 2) under them.
struct FogVoxelGI {
    Image atlas;
    render::VoxelGIProbe probe;
};
FogVoxelGI fog_voxel_gi() {
    FogVoxelGI out{.atlas = {.width = 16, .height = 6, .pixels = {}}, .probe = {}};
    out.atlas.pixels.resize(std::size_t{16} * 6 * 4);
    const auto put = [&](std::uint32_t x, std::uint32_t y, std::array<std::uint8_t, 4> texel) {
        std::copy(texel.begin(), texel.end(),
                  out.atlas.pixels.begin() + static_cast<std::ptrdiff_t>((y * 16 + x) * 4));
    };
    for (std::uint32_t z = 0; z < 4; ++z) {
        for (std::uint32_t y = 0; y < 4; ++y) {
            for (std::uint32_t x = 0; x < 4; ++x) {
                put((z * 4) + x, y,
                    {static_cast<std::uint8_t>(200 - (x * 50)), static_cast<std::uint8_t>(z * 60),
                     static_cast<std::uint8_t>(y * 70), static_cast<std::uint8_t>(60 + (x * 40))});
            }
        }
    }
    for (std::uint32_t z = 0; z < 2; ++z) {
        for (std::uint32_t y = 0; y < 2; ++y) {
            for (std::uint32_t x = 0; x < 2; ++x) {
                put((z * 2) + x, 4 + y, {90, 120, static_cast<std::uint8_t>(80 + (y * 100)), 255});
            }
        }
    }
    out.probe.to_cell = {1.0F, 0.0F, 0.0F, 2.0F, 0.0F, 2.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 3.0F};
    out.probe.size = {4, 4, 4};
    out.probe.levels = 2;
    out.probe.placement[0] = {0, 0, 4};
    out.probe.placement[1] = {0, 4, 2};
    out.probe.energy = 3.0F;
    return out;
}

// Thirty-two small glowing balls of fog in rows over the floor, as many as a frame takes.
std::vector<render::FogVolume3D> many_volumes() {
    std::vector<render::FogVolume3D> out;
    for (std::size_t i = 0; i < render::kMaxFogVolumes; ++i) {
        const auto column = static_cast<float>(i % 8);
        const auto row = static_cast<float>(i / 8);
        out.push_back({.transform = placed(-3.5F + column, 0.6F + (0.3F * row), -1.0F - row),
                       .size = {0.8F, 0.8F, 0.8F},
                       .shape = render::FogVolumeShape::Ellipsoid,
                       .material = true,
                       .density = 2.0F,
                       .emission = {0.2F + (0.1F * column), 0.5F, 1.0F - (0.2F * row)}});
    }
    return out;
}

// A gray floor seen at a slant under a wall that shades the sun, lit by the sun from behind it
// and a lamp, in fog, over a blue sky, 48 x 32 pixels.
Image draw_fog(RenderDevice& device, const Foggy& foggy) {
    auto target = device.create_texture({.width = 48, .height = 32, .render_target = true});
    REQUIRE(target.ok());
    auto floor = device.create_mesh(quad({0, 0, 0}, {1, 0, 0}, {0, 0, -1}, {0, 1, 0}, 16.0F), {});
    REQUIRE(floor.ok());
    auto wall = device.create_mesh(quad({0, 2.5F, -3}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, 3.0F), {});
    REQUIRE(wall.ok());
    const FogVoxelGI gi = fog_voxel_gi();
    auto atlas = device.create_texture({.width = gi.atlas.width, .height = gi.atlas.height},
                                       gi.atlas.pixels);
    REQUIRE(atlas.ok());
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
            .settings = {.volume_size = 32, .volume_depth = 32, .use_filter = foggy.filter},
            .volumes = foggy.volumes,
            .gi_inject = foggy.gi_inject};
        if (foggy.voxel_gi) {
            scene.volumetric_fog.voxel_gi_atlas = atlas.value();
            scene.volumetric_fog.voxel_gis = {gi.probe};
        }
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
    device.destroy_texture(atlas.value());
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
        // Fog volumes thicken it, glow and thin it.
        CHECK(differing(draw_fog(*device, {.volumes = some_volumes()}), fogged, 8) >
              fogged.pixels.size() / 20);
        // A VoxelGI's light lights the fog in its box, as much as gi_inject says, and none at 0.
        const Image glowing = draw_fog(*device, {.voxel_gi = true});
        CHECK(differing(glowing, fogged, 8) > fogged.pixels.size() / 20);
        CHECK(differing(draw_fog(*device, {.voxel_gi = true, .gi_inject = 4.0F}), glowing, 8) > 0);
        CHECK(difference(draw_fog(*device, {.voxel_gi = true, .gi_inject = 0.0F}), fogged) == 0);
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
        {.volumes = some_volumes()},
        {.frames = 4, .turn = 0.2F, .volumes = some_volumes()},
        {.volumes = many_volumes()},
        {.voxel_gi = true},
        {.frames = 4, .turn = 0.2F, .voxel_gi = true, .gi_inject = 2.5F},
        {.volumes = many_volumes(), .voxel_gi = true},
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

TEST_CASE("fog volumes fill the froxels between their corners, as Godot picks them") {
    render::Scene3D scene = emitting(0.0F);
    const std::array<std::uint32_t, 3> size{16, 16, 32};
    using Range = std::array<std::array<std::int32_t, 3>, 2>;
    // A world volume fills them all.
    CHECK(detail::fog_volume_froxels(scene, {.shape = render::FogVolumeShape::World}, size) ==
          Range{{{0, 0, 0}, {16, 16, 32}}});
    // A box ahead fills the froxels its corners are in, from the first to the last's.
    const render::FogVolume3D ahead{.transform = placed(0.0F, 0.0F, -20.0F),
                                    .size = {4.0F, 4.0F, 4.0F}};
    const Range box = detail::fog_volume_froxels(scene, ahead, size);
    CHECK(box[0][0] < 8);
    CHECK(box[1][0] > 8);
    CHECK(box[0][1] < 8);
    CHECK(box[1][1] > 8);
    // Froxel slices are spread as the fog's detail spread spreads them.
    CHECK(box[0][2] == 16);
    CHECK(box[1][2] == 18);
    // With the camera in it, the whole view across is filled, out to its far corners.
    const Range around = detail::fog_volume_froxels(
        scene, {.transform = placed(0.0F, 0.0F, -1.0F), .size = {4.0F, 4.0F, 4.0F}}, size);
    CHECK(around[0] == std::array<std::int32_t, 3>{0, 0, 0});
    CHECK(around[1][0] == 16);
    CHECK(around[1][1] == 16);
    CHECK(around[1][2] > 0);
    // Behind the camera, its corners are all in the first slice, which it reaches through.
    const Range behind = detail::fog_volume_froxels(
        scene, {.transform = placed(0.0F, 0.0F, 10.0F), .size = {2.0F, 2.0F, 2.0F}}, size);
    CHECK(behind[0][2] == 0);
    CHECK(behind[1][2] == 1);
    // One whose corners share a froxel fills none, and isn't sent.
    const render::FogVolume3D tiny{.transform = placed(0.0F, 0.0F, -40.0F),
                                   .size = {0.01F, 0.01F, 0.01F}};
    const Range none = detail::fog_volume_froxels(scene, tiny, size);
    CHECK(none[1][2] <= none[0][2]);
    scene.volumetric_fog.volumes = {tiny, ahead};
    detail::FogClock clock;
    const detail::FogUniforms uniforms =
        detail::volumetric_fog_uniforms(scene, detail::mesh_scene_uniforms(scene), 64, 64, clock);
    CHECK(uniforms.items[0] == 1);
    const std::vector<detail::Float4> items = detail::fog_items(scene, 64, 64);
    REQUIRE(items.size() == detail::kFogVolumeItems);
    detail::FogVolumeUniforms sent;
    std::memcpy(static_cast<void*>(&sent), items.data(), sizeof(sent));
    CHECK(sent.corner == std::array<std::int32_t, 4>{box[0][0], box[0][1], 16, 0});
}

TEST_CASE("fog volumes add their density, light and glow in Godot's fixed point") {
    render::Scene3D scene = emitting(0.0F);
    scene.volumetric_fog.ambient_inject = 0.0F;
    const auto density_at = [&scene](std::uint32_t x, std::uint32_t y, std::uint32_t z) {
        detail::FogClock clock;
        const detail::FogUniforms uniforms = detail::volumetric_fog_uniforms(
            scene, detail::mesh_scene_uniforms(scene), 64, 64, clock);
        const std::vector<detail::Float4> items = detail::fog_items(scene, 64, 64);
        detail::FogMaps maps;
        maps.items = items;
        return detail::fog_density(uniforms, maps, {}, x, y, z);
    };
    const render::FogVolume3D glowing{.shape = render::FogVolumeShape::World,
                                      .material = true,
                                      .density = 0.5F,
                                      .emission = {1.0F, 0.5F, 0.0F},
                                      .edge_fade = 0.0F};
    scene.volumetric_fog.volumes = {glowing};
    detail::Float4 at = density_at(3, 5, 7);
    // A world volume is as big as twice the froxels in each direction, as Godot sizes it, so
    // its edge fade shows everywhere; at 0 it doesn't fade. Densities in 1024ths, the glow in
    // 511ths of what it gives at the density, held to 1.
    CHECK(at[3] == 0.5F);
    const float green = std::floor(0.21404114F * 0.5F * 511.0F) / 511.0F;
    CHECK(at[0] == doctest::Approx(255.0F / 511.0F));
    CHECK(at[1] == doctest::Approx(green).epsilon(0.002));
    CHECK(at[2] == 0.0F);
    // Each channel's sum is held to its bits: 2047 units, 4 and a bit of glow.
    const render::FogVolume3D bright{.shape = render::FogVolumeShape::World,
                                     .material = true,
                                     .density = 2.0F,
                                     .emission = {1.0F, 1.0F, 1.0F},
                                     .edge_fade = 0.0F};
    scene.volumetric_fog.volumes = {bright, bright, bright, bright, bright};
    at = density_at(3, 5, 7);
    CHECK(at[3] == 10.0F);
    CHECK(at[0] == doctest::Approx(2047.0F / 511.0F));
    CHECK(at[2] == doctest::Approx(1023.0F / 255.0F));
    // A negative density thins the fog's own and adds no light.
    scene.volumetric_fog.density = 0.5F;
    scene.volumetric_fog.volumes = {{.shape = render::FogVolumeShape::World,
                                     .material = true,
                                     .density = -0.3F,
                                     .emission = {1.0F, 1.0F, 1.0F},
                                     .edge_fade = 0.0F}};
    at = density_at(3, 5, 7);
    CHECK(at[3] == doctest::Approx(0.5F - (307.0F / 1024.0F)));
    // Godot's default material fills its shape and nothing outside it.
    scene.volumetric_fog.density = 0.0F;
    scene.volumetric_fog.volumes = {
        {.transform = placed(0.0F, 0.0F, -20.0F), .size = {8.0F, 8.0F, 8.0F}}};
    CHECK(density_at(8, 8, 17)[3] == 1.0F);
    CHECK(density_at(0, 0, 17)[3] == 0.0F);
    CHECK(density_at(8, 8, 10)[3] == 0.0F);
}

TEST_CASE("the fog takes a VoxelGI's light inside its box, its levels front to back") {
    const FogVoxelGI gi = fog_voxel_gi();
    const detail::MeshVoxelGIUniforms uniforms = detail::voxel_gi_uniforms(gi.probe);
    // At the middle of cell (0, 1, 2): its texel's light, then the coarser level's through what
    // it lets through, times the energy.
    const detail::Float3 at = detail::fog_voxel_gi_light(uniforms, gi.atlas, {-1.5F, 0.75F, -0.5F});
    const float alpha = 60.0F / 255.0F;
    const float through = 1.0F - alpha;
    CHECK(at[0] == doctest::Approx(3.0F * ((200.0F / 255.0F) + (through * 90.0F / 255.0F))));
    CHECK(at[1] == doctest::Approx(3.0F * ((120.0F / 255.0F) + (through * 120.0F / 255.0F))));
    // Level 1 is read between its rows, a quarter of the way to the bluer one.
    CHECK(at[2] == doctest::Approx(3.0F * ((70.0F / 255.0F) + (through * 105.0F / 255.0F))));
    // Outside its box there is none.
    CHECK(detail::fog_voxel_gi_light(uniforms, gi.atlas, {-2.5F, 0.75F, -0.5F}) ==
          detail::Float3{});
    CHECK(detail::fog_voxel_gi_light(uniforms, gi.atlas, {0.0F, 2.0F, 0.0F}) == detail::Float3{});
    // The fog takes the first eight VoxelGIs, only above a gi_inject of 0.001.
    render::Scene3D scene;
    scene.method = render::RenderMethod::ForwardPlus;
    scene.volumetric_fog = {.enabled = true, .gi_inject = 2.0F};
    scene.volumetric_fog.voxel_gi_atlas = render::TextureId{1};
    scene.volumetric_fog.voxel_gis.assign(10, gi.probe);
    const detail::MeshSceneUniforms lights = detail::mesh_scene_uniforms(scene);
    detail::FogClock clock;
    detail::FogUniforms fog = detail::volumetric_fog_uniforms(scene, lights, 64, 64, clock);
    CHECK(fog.items[1] == 8);
    CHECK(fog.voxel_gi[0] == 2.0F);
    // They follow the fog volumes in the items the density pass reads.
    const std::vector<detail::Float4> items = detail::fog_items(scene, 64, 64);
    REQUIRE(items.size() == 8 * detail::kFogVoxelGIItems);
    detail::MeshVoxelGIUniforms last;
    std::memcpy(static_cast<void*>(&last), &items[7 * detail::kFogVoxelGIItems], sizeof(last));
    CHECK(last.size == uniforms.size);
    scene.volumetric_fog.gi_inject = 0.001F;
    fog = detail::volumetric_fog_uniforms(scene, lights, 64, 64, clock);
    CHECK(fog.items[1] == 0);
    CHECK(detail::fog_items(scene, 64, 64).size() == 1); // a storage buffer is never empty
}
