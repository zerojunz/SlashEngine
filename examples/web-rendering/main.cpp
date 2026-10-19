// Drawing with OpenGL ES 3.0, which browsers call WebGL 2: how a web export draws, since SDL3's
// GPU API has no web backend. The opengl render driver picks it anywhere else the system has
// OpenGL ES (Android, Linux), as Godot's compatibility renderer.
//
// This program writes the scene's textures, then draws main.scene.json into a texture on the
// OpenGL ES device, or on the software device where there is no OpenGL ES (macOS, Windows), and
// prints what each pixel shows. The two draw the same pixels, so the output is the same either
// way: CI checks it on Linux with Mesa's OpenGL ES and on macOS and Windows on the CPU.

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

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

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

// A PNG of one color. Games ship PNG files made in an image editor or by a generator; these are
// made here so the example has no binary file.
void write_png(const char* path, std::uint32_t width, std::uint32_t height, Rgba color) {
    render::Image image{.width = width, .height = height, .pixels = {}};
    for (std::uint32_t i = 0; i < width * height; ++i) {
        image.pixels.insert(image.pixels.end(), color.begin(), color.end());
    }
    check(slash::platform::write_file(path, render::encode_png(image).value()));
}

// What a pixel shows, by the color it is nearest: the sky (the clear color), the ground, the sun,
// the tree, or the hero, a red square half flashed to white by its material.
char what(const render::Image& image, std::uint32_t x, std::uint32_t y) {
    struct Named {
        char symbol;
        Rgba color;
    };
    constexpr std::array<Named, 5> kColors{{{'.', {64, 128, 255, 255}},
                                            {'#', {120, 72, 32, 255}},
                                            {'o', {255, 220, 0, 255}},
                                            {'t', {0, 153, 0, 255}},
                                            {'h', {255, 128, 128, 255}}}};
    const std::size_t at = ((std::size_t{y} * image.width) + x) * 4;
    char best = '?';
    int best_distance = 3 * 16 * 16; // within 16 a channel, or '?'
    for (const Named& named : kColors) {
        int distance = 0;
        for (std::size_t channel = 0; channel < 3; ++channel) {
            const int d = int{image.pixels[at + channel]} - int{named.color.at(channel)};
            distance += d * d;
        }
        if (distance < best_distance) {
            best_distance = distance;
            best = named.symbol;
        }
    }
    return best;
}

} // namespace

int main() {
    write_png("ground.png", 32, 3, {120, 72, 32, 255});
    write_png("hero.png", 4, 5, {255, 0, 0, 255});
    write_png("sun.png", 4, 4, {255, 220, 0, 255});
    write_png("white.png", 1, 1, {255, 255, 255, 255});

    world::ComponentRegistry components;
    world::register_builtin_components(components);
    command::Registry commands;
    command::register_builtin_commands(commands);
    command::SessionOptions options;
    options.capture_logs = false;
    command::Session session(components, commands, options);
    const command::Reply opened =
        session.call("scene.open", json::parse(R"({"path": "main.scene.json"})").value());
    std::printf("scene.open: %s\n", json::write(opened.value, json::Style::Compact).c_str());

    // The opengl driver with no window draws into textures through a hidden window of its own.
    // A game's window must be made for OpenGL: platform::Config::opengl, which project.json's
    // rendering.driver "opengl" sets, and which is always on in a web export.
    auto created = render::create_device({.window = nullptr, .driver = "opengl", .debug = false});
    const std::unique_ptr<render::RenderDevice> device =
        created ? std::move(created).value() : render::create_software_device();

    // Draw the 32x12 window's frame into a texture, as the game draws it in the browser's canvas.
    canvas::CanvasRenderer renderer(*device, session.assets());
    const render::TextureId target = device
                                         ->create_texture({.width = 32,
                                                           .height = 12,
                                                           .filter = render::Filter::Nearest,
                                                           .render_target = true})
                                         .value();
    check(renderer.draw(session.world(), {.target = target,
                                          .clear = {.r = 0.25F, .g = 0.5F, .b = 1.0F, .a = 1.0F},
                                          .width = 0.0F,
                                          .height = 0.0F,
                                          .interpolation = 1.0,
                                          .texture_filter = render::Filter::Nearest,
                                          .time = 0.0}));
    const render::Image image = device->read_texture(target).value();
    for (std::uint32_t y = 0; y < image.height; ++y) {
        std::string row;
        for (std::uint32_t x = 0; x < image.width; ++x) {
            row += what(image, x, y);
        }
        std::printf("%s\n", row.c_str());
    }
    // The material is its own draw; the other three sprites have different textures.
    const render::Stats stats = device->stats();
    std::printf("sprites drawn: %zu, draws: %llu, triangles: %llu\n", renderer.sprites_drawn(),
                static_cast<unsigned long long>(stats.draws),
                static_cast<unsigned long long>(stats.triangles));
    return 0;
}
