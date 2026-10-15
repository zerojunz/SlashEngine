// Materials, as Godot's canvas shaders: a .material.json file is a node graph that colors each
// pixel of a sprite, and Sprite2D's material and material_params pick one and set its params.
//
// main.scene.json has three sprites: the hero flashes white with hit_flash.material.json, the
// pond scrolls ripples over itself with water.material.json, and the spark adds its light with
// glow.material.json. This program writes their textures, checks colors with material.eval as an
// agent would, then draws the scene into a texture.

#include "slash/canvas/canvas.hpp"
#include "slash/canvas/material.hpp"
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
#include <string_view>
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

// A PNG of colored columns, each as wide as the image divided by their count. Games ship PNG
// files made in an image editor or by a generator; these are made here so the example has no
// binary file.
void write_png(const char* path, std::uint32_t width, std::uint32_t height,
               const std::vector<Rgba>& columns) {
    render::Image image{.width = width, .height = height, .pixels = {}};
    image.pixels.resize(std::size_t{width} * height * 4);
    const auto column_width = static_cast<std::uint32_t>(width / columns.size());
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::size_t at = ((std::size_t{y} * width) + x) * 4;
            for (std::size_t channel = 0; channel < 4; ++channel) {
                image.pixels[at + channel] = columns.at(x / column_width).at(channel);
            }
        }
    }
    check(slash::platform::write_file(path, render::encode_png(image).value()));
    std::printf("wrote %s: %ux%u\n", path, width, height);
}

// Prints the call as an agent would make it, then the reply.
void call(command::Session& session, std::string_view name, std::string_view params) {
    const json::Value value = json::parse(params).value();
    std::printf("%.*s %s\n", static_cast<int>(name.size()), name.data(),
                json::write(value, json::Style::Compact).c_str());
    const command::Reply reply = session.call(name, value);
    std::puts(json::write(reply.ok ? reply.value : json::Value(reply.message), json::Style::Compact)
                  .c_str());
}

} // namespace

int main() {
    write_png("hero.png", 16, 16, {{255, 64, 0, 255}});
    write_png("pond.png", 64, 16, {{64, 128, 255, 255}});
    write_png("ripples.png", 2, 1, {{255, 255, 255, 255}, {128, 128, 128, 255}});

    world::ComponentRegistry components;
    world::register_builtin_components(components);
    command::Registry commands;
    command::register_builtin_commands(commands);
    command::SessionOptions options;
    options.capture_logs = false;
    command::Session session(components, commands, options);

    call(session, "scene.open", R"({"path": "main.scene.json"})");

    // material.eval returns the colors a material draws at texture coordinates, from 0 to 1,
    // computed on the CPU as the GPU computes them: a check that needs no screenshot. Given a
    // sprite, it uses the sprite's texture, material_params and modulate.
    call(session, "material.eval", R"({"id": "hero", "uv": [[0.5, 0.5]]})");
    // A hit: the hero flashes white, then halfway back.
    call(session, "component.set",
         R"({"id": "hero", "components": {"Sprite2D": {"material_params": {"flash": 1}}}})");
    call(session, "material.eval", R"({"id": "hero", "uv": [[0.5, 0.5]]})");
    call(session, "material.eval",
         R"({"id": "hero", "uv": [[0.5, 0.5]], "params": {"flash": 0.5}})");
    // A param the material doesn't have, or a value outside its range, is refused by name.
    call(session, "material.eval", R"({"id": "hero", "uv": [[0.5, 0.5]], "params": {"flsh": 1}})");
    call(session, "material.eval", R"({"id": "hero", "uv": [[0.5, 0.5]], "params": {"flash": 2}})");

    // The ripples tile four times across the pond and scroll a quarter of the way along each
    // second: the same spot is light, then dark two seconds later.
    call(session, "material.eval", R"({"id": "pond", "uv": [[0.05, 0.5], [0.15, 0.5]]})");
    call(session, "material.eval",
         R"({"id": "pond", "uv": [[0.05, 0.5], [0.15, 0.5]], "time": 2})");

    // Each material compiles once, when it loads, to a program the GPU's material shader runs.
    for (const char* path :
         {"hit_flash.material.json", "water.material.json", "glow.material.json"}) {
        const auto material = session.assets().load<canvas::CanvasMaterial>(path).value();
        std::printf("%s: blend %s, %zu instructions, %zu constants\n", path,
                    std::string(render::blend_mode_name(material->blend)).c_str(),
                    material->program.code.size(), material->program.constants.size());
    }

    // Draw into a 128x96 texture at 1.5 seconds of game time. Each material is its own GPU draw.
    const auto device =
        render::create_device_or_null({.window = nullptr, .driver = "auto", .debug = false});
    canvas::CanvasRenderer renderer(*device, session.assets());
    const render::TextureId target = device
                                         ->create_texture({.width = 128,
                                                           .height = 96,
                                                           .filter = render::Filter::Linear,
                                                           .render_target = true})
                                         .value();
    check(renderer.draw(session.world(), {.target = target,
                                          .clear = render::kDefaultClearColor,
                                          .width = 0.0F,
                                          .height = 0.0F,
                                          .interpolation = 1.0,
                                          .texture_filter = render::Filter::Linear,
                                          .time = 1.5}));
    const render::Stats stats = device->stats();
    std::printf("sprites drawn: %zu, GPU draws: %llu, triangles: %llu, textures: %u\n",
                renderer.sprites_drawn(), static_cast<unsigned long long>(stats.draws),
                static_cast<unsigned long long>(stats.triangles), stats.textures);
    return 0;
}
