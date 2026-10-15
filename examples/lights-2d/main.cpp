// 2D lights, as Godot's: a CanvasModulate darkens the scene in main.scene.json, a torch
// (PointLight2D) and the moon (DirectionalLight2D) light it, the crate's LightOccluder2D casts a
// shadow from the torch and its normal map gives it bevelled edges, and the neon sign's material
// is unshaded, so it glows in the dark.
//
// This program makes the textures, asks light.sample how lit some points are, as an agent would,
// changes the torch and asks again, then draws the scene into a texture.

#include "slash/canvas/canvas.hpp"
#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/platform/file.hpp"
#include "slash/render/device.hpp"
#include "slash/render/image.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <string_view>

namespace {

namespace canvas = slash::canvas;
namespace command = slash::command;
namespace json = slash::json;
namespace render = slash::render;
namespace world = slash::world;

using Rgba = std::array<std::uint8_t, 4>;

// Stops with the error if a call fails, so the output never hides one.
void check(const slash::Result<void>& result) {
    if (!result) {
        std::fprintf(stderr, "%s\n", result.error().message.c_str());
        std::exit(1);
    }
}

// Writes a PNG whose pixel at (x, y) is color(x, y). Games ship PNG files made in an image editor
// or by a generator; these are made here so the example has no binary file.
void write_png(const std::string& path, std::uint32_t width, std::uint32_t height,
               const std::function<Rgba(std::uint32_t, std::uint32_t)>& color) {
    render::Image image{.width = width, .height = height, .pixels = {}};
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const Rgba pixel = color(x, y);
            image.pixels.insert(image.pixels.end(), pixel.begin(), pixel.end());
        }
    }
    check(slash::platform::write_file(path, render::encode_png(image).value()));
    std::printf("wrote %s: %ux%u\n", path.c_str(), width, height);
}

// A normal map pixel, OpenGL style as Godot reads them: red is x, green is up, blue is toward
// the viewer, each from -1 (0) to 1 (255).
Rgba normal_pixel(float x, float up, float z) {
    const float length = std::sqrt((x * x) + (up * up) + (z * z));
    const auto channel = [&](float value) {
        return static_cast<std::uint8_t>(std::lround(((value / length) + 1.0F) * 127.5F));
    };
    return {channel(x), channel(up), channel(z), 255};
}

void write_textures() {
    // Stone tiles with dark grout, 16 pixels apart.
    write_png("stone.png", 32, 32, [](std::uint32_t x, std::uint32_t y) {
        return x % 16 == 0 || y % 16 == 0 ? Rgba{90, 90, 100, 255} : Rgba{150, 150, 160, 255};
    });
    // A wooden crate with a darker rim, and a normal map that bevels its outer 4 pixels.
    write_png("crate.png", 24, 24, [](std::uint32_t x, std::uint32_t y) {
        const bool rim = x < 2 || y < 2 || x > 21 || y > 21;
        return rim ? Rgba{100, 65, 30, 255} : Rgba{160, 110, 60, 255};
    });
    write_png("crate_n.png", 24, 24, [](std::uint32_t x, std::uint32_t y) {
        // -1 within 4 pixels of the low edge, 1 within 4 of the high one, else 0.
        const auto bevel = [](std::uint32_t at) {
            constexpr std::uint32_t kBevel = 4;
            if (at < kBevel) {
                return -1.0F;
            }
            return at >= 24 - kBevel ? 1.0F : 0.0F;
        };
        return normal_pixel(bevel(x), -bevel(y), 1.0F);
    });
    write_png("hero.png", 16, 16,
              [](std::uint32_t /*x*/, std::uint32_t /*y*/) { return Rgba{80, 200, 120, 255}; });
    write_png("sign.png", 32, 12, [](std::uint32_t x, std::uint32_t y) {
        const bool tube = x == 1 || y == 1 || x == 30 || y == 10;
        return tube ? Rgba{120, 240, 255, 255} : Rgba{20, 40, 60, 255};
    });
}

// Prints the call as an agent would make it, then the reply.
json::Value call(command::Session& session, std::string_view name, std::string_view params) {
    const json::Value value = json::parse(params).value();
    std::printf("%.*s %s\n", static_cast<int>(name.size()), name.data(),
                json::write(value, json::Style::Compact).c_str());
    const command::Reply reply = session.call(name, value);
    std::puts(json::write(reply.ok ? reply.value : json::Value(reply.message), json::Style::Compact)
                  .c_str());
    return reply.value;
}

} // namespace

int main() {
    world::ComponentRegistry components;
    world::register_builtin_components(components);
    command::Registry commands;
    command::register_builtin_commands(commands);
    command::SessionOptions options;
    options.capture_logs = false;
    command::Session session(components, commands, options);

    write_textures();
    call(session, "scene.open", R"({"path": "main.scene.json"})");

    // How lit are these points? Next to the torch, behind the crate from it, and in the far
    // corner, where only the moon reaches. Each light that reaches a point is listed with its
    // color there; a light with shadows also says how much of it the occluders hide.
    call(session, "light.sample", R"({"points": [[96, 60], [200, 90], [300, 170]]})");
    // The crate's own sprite is lit: its occluder leaves out the edges facing the light.
    call(session, "light.sample", R"({"id": "crate"})");
    // An entity: at its position, with its sprite's light mask and z index.
    call(session, "light.sample", R"({"id": "hero"})");

    // A cold torch, and the point beside it again.
    call(session, "component.set",
         R"({"id": "torch", "components": {"PointLight2D": {"color": [0.6, 0.8, 1, 1]}}})");
    call(session, "light.sample", R"({"points": [[96, 60]]})");

    // A blend mode lights don't have is refused, with the ones they do.
    call(session, "component.set",
         R"({"id": "torch", "components": {"PointLight2D": {"blend_mode": "ad"}}})");

    // Draw into a 320x180 texture. Lit sprites draw with the material shader, which shades
    // each pixel with the lights, the shadow map and the crate's normal map.
    const auto device =
        render::create_device_or_null({.window = nullptr, .driver = "auto", .debug = false});
    canvas::CanvasRenderer renderer(*device, session.assets());
    const render::TextureId target = device
                                         ->create_texture({.width = 320,
                                                           .height = 180,
                                                           .filter = render::Filter::Linear,
                                                           .render_target = true})
                                         .value();
    check(renderer.draw(session.world(), {.target = target,
                                          .clear = render::kDefaultClearColor,
                                          .width = 0.0F,
                                          .height = 0.0F,
                                          .interpolation = 1.0,
                                          .texture_filter = render::Filter::Linear,
                                          .time = 0.0}));
    const render::Stats stats = device->stats();
    std::printf("sprites drawn: %zu, lights: %zu, shadows: %zu, GPU draws: %llu\n",
                renderer.sprites_drawn(), renderer.lights_drawn(), renderer.shadows_drawn(),
                static_cast<unsigned long long>(stats.draws));
    return 0;
}
