#pragma once

#include "devices.hpp"

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
#include <optional>
#include <utility>
#include <vector>

// A square drawn over frames with FSR 2 or FSR 3 and what the upscaler tests measure in it,
// shared by test_fsr2.cpp and test_fsr3.cpp.
namespace slash::test::upscale {

using render::Image;
using render::Material3D;
using render::RenderDevice;
using render::Vertex3D;

// The devices that draw Forward+: the software device and the GPU device. The OpenGL devices
// always draw as Compatibility, which has neither FSR 2 nor FSR 3.
inline std::vector<std::unique_ptr<RenderDevice>> forward_devices() {
    std::vector<std::unique_ptr<RenderDevice>> devices;
    devices.push_back(render::create_software_device());
    if (auto gpu = slash::test::gpu_device(); gpu != nullptr) {
        devices.push_back(std::move(gpu));
    }
    return devices;
}

inline int difference(const Image& a, const Image& b) {
    int worst = 0;
    for (std::size_t i = 0; i < a.pixels.size(); ++i) {
        worst = std::max(worst, std::abs(int{a.pixels[i]} - int{b.pixels[i]}));
    }
    return worst;
}

// How many of the channels of a and b differ by more than by.
inline std::size_t differing(const Image& a, const Image& b, int by) {
    std::size_t count = 0;
    for (std::size_t i = 0; i < a.pixels.size(); ++i) {
        count += std::abs(int{a.pixels[i]} - int{b.pixels[i]}) > by ? 1U : 0U;
    }
    return count;
}

inline int red(const Image& image, std::uint32_t x, std::uint32_t y) {
    return image.pixels.at((std::size_t{y} * image.width + x) * 4);
}

// A square facing +Z, one unit across, its corners clockwise from the front, moved right by x.
inline std::vector<Vertex3D> square(float x = 0.0F) {
    const auto corner = [x](float cx, float cy) {
        return Vertex3D{.position = {x + (cx * 0.5F), cy * 0.5F, 0.0F},
                        .normal = {0.0F, 0.0F, 1.0F},
                        .tangent = {1.0F, 0.0F, 0.0F, 1.0F},
                        .uv = {(cx + 1.0F) * 0.5F, (1.0F - cy) * 0.5F}};
    };
    return {corner(-1, 1), corner(1, 1),  corner(1, -1),
            corner(-1, 1), corner(1, -1), corner(-1, -1)};
}

// What draw_square draws.
struct Square {
    std::optional<render::Fsr2Scaling> fsr2 = render::Fsr2Scaling{.scale = 0.5F, .sharpness = 0.2F};
    render::RenderMethod method = render::RenderMethod::ForwardPlus;
    bool taa = false;
    std::uint32_t frames = 1; // drawn in a row on one target
    float step = 0.0F;        // how far the square moves right each frame
    float turn = 0.0F;        // how far the camera moves right each frame
    bool perspective = false; // seen through a perspective camera
    float yaw = 0.0F;         // how far the camera turns left each frame, in radians
    bool sky = false;         // over a panorama sky of upright stripes
};

// A white unshaded square turned 30 degrees, on a dark red background, seen in an orthogonal
// projection three units across into a 32x32 target, its edges stepping across the pixels.
// Moving, it ends at the same place as a still one: it starts as far to the left as it moves.
inline Image draw_square(RenderDevice& device, const Square& settings) {
    auto target = device.create_texture({.width = 32, .height = 32, .render_target = true});
    REQUIRE(target.ok());
    auto mesh = device.create_mesh(square(), {});
    REQUIRE(mesh.ok());
    // Black and white stripes, 16 of each all round.
    std::vector<std::uint8_t> stripes;
    for (int i = 0; i < 32; ++i) {
        const std::uint8_t value = i % 2 == 0 ? 255 : 0;
        stripes.insert(stripes.end(), {value, value, value, 255});
    }
    auto panorama = device.create_texture(
        {.width = 32, .height = 1, .filter = render::Filter::Nearest}, stripes);
    REQUIRE(panorama.ok());
    const float start = -settings.step * static_cast<float>(settings.frames - 1);
    const float turn = 0.5235988F;
    const float c = std::cos(turn);
    const float s = std::sin(turn);
    std::array<float, 16> last{};
    for (std::uint32_t frame = 0; frame < settings.frames; ++frame) {
        const float x = start + (settings.step * static_cast<float>(frame));
        const std::array<float, 16> transform{c, s, 0, 0, -s, c, 0, 0, 0, 0, 1, 0, x, 0, 0, 1};
        render::Scene3D scene;
        scene.method = settings.method;
        scene.use_taa = settings.taa;
        scene.fsr2 = settings.fsr2;
        const float eye = settings.turn * static_cast<float>(frame);
        // It turns to look straight ahead in the last frame.
        const float yaw = settings.yaw * static_cast<float>(settings.frames - 1 - frame);
        scene.camera = render::look_at(
            {eye, 0.0F, 2.5F}, {eye - (2.5F * std::sin(yaw)), 0.0F, 2.5F - (2.5F * std::cos(yaw))},
            {0.0F, 1.0F, 0.0F});
        if (settings.sky) {
            scene.sky.kind = render::SkyKind::Panorama;
            scene.sky.panorama.panorama = panorama.value();
        }
        scene.projection = settings.perspective ? render::perspective(60.0F, 1.0F, 0.1F, 20.0F)
                                                : render::orthogonal(3.0F, 1.0F, 0.1F, 20.0F);
        REQUIRE(device
                    .begin_frame({.target = target.value(),
                                  .clear = {0.15F, 0.02F, 0.02F, 1.0F},
                                  .scene = scene})
                    .ok());
        const Material3D white{.unshaded = true, .cull = render::CullMode::Disabled};
        std::optional<std::array<float, 16>> previous;
        if (frame > 0) {
            previous = last;
        }
        REQUIRE(
            device.draw_mesh(mesh.value(), transform, white, 1, render::ShadowCasting::On, previous)
                .ok());
        REQUIRE(device.end_frame().ok());
        last = transform;
    }
    auto image = device.read_texture(target.value());
    REQUIRE(image.ok());
    device.destroy_mesh(mesh.value());
    device.destroy_texture(panorama.value());
    device.destroy_texture(target.value());
    return image.value();
}

// How many pixels are neither the square's white nor the background's red: the square's edges
// where a pixel is partly covered.
inline std::size_t blended(const Image& image) {
    std::size_t count = 0;
    for (std::size_t i = 0; i < image.pixels.size(); i += 4) {
        count += image.pixels[i] > 40 && image.pixels[i] < 215 ? 1U : 0U;
    }
    return count;
}

} // namespace slash::test::upscale
