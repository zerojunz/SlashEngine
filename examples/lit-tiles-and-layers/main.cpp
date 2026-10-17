// Lit tiles and canvas layers, as Godot's 2D lights.
//
// A dungeon floor of stone tiles lies in the dark under a torch. The tile set's atlas source has
// a normal map that bevels each block's edges and a specular map that makes its middle shine, as
// a Godot CanvasTexture gives an atlas source, so the torch lights the side of a block that faces
// it and leaves the far side dim. A HUD panel sits in a CanvasLayer of layer 1, with its own
// CanvasModulate and a lantern whose layer range is 1 to 1: the lantern lights only the HUD, and
// the torch, whose range is 0 to 0, only the world, until its range takes layer 1 too.
//
// This program draws the textures, asks light.sample how lit the panel is, asks frame.capture
// for the colors of a few pixels, widens the torch's layer range and asks again. Games ship PNG
// files made in an image editor; this example draws its textures in code, so the repository holds
// no images.

#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/platform/file.hpp"
#include "slash/render/image.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace {

namespace command = slash::command;
namespace json = slash::json;
namespace render = slash::render;
namespace world = slash::world;

using Rgba = std::array<std::uint8_t, 4>;

// Writes a PNG whose pixel at (x, y) is color(x, y).
void write_png(const std::string& path, std::uint32_t width, std::uint32_t height,
               const std::function<Rgba(std::uint32_t, std::uint32_t)>& color) {
    render::Image image{.width = width, .height = height, .pixels = {}};
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const Rgba pixel = color(x, y);
            image.pixels.insert(image.pixels.end(), pixel.begin(), pixel.end());
        }
    }
    if (!slash::platform::write_file(path, render::encode_png(image).value()).ok()) {
        std::fprintf(stderr, "can't write %s\n", path.c_str());
        std::exit(1);
    }
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

// Where a pixel of a 16-pixel block is: -1 within 3 pixels of its low edge, 1 within 3 of its
// high one, else 0. Pixel 0 is the grout between blocks.
float bevel(std::uint32_t at) {
    if (at >= 1 && at <= 3) {
        return -1.0F;
    }
    return at >= 13 ? 1.0F : 0.0F;
}

void write_textures() {
    // A grey block with dark grout along its top and left.
    write_png("stone.png", 16, 16, [](std::uint32_t x, std::uint32_t y) {
        return x == 0 || y == 0 ? Rgba{70, 70, 80, 255} : Rgba{150, 150, 160, 255};
    });
    // Its edges lean out: the left edge faces left, the top edge up the screen, and so on.
    write_png("stone_n.png", 16, 16, [](std::uint32_t x, std::uint32_t y) {
        return normal_pixel(bevel(x), -bevel(y), 1.0F);
    });
    // Its middle shines; black elsewhere has no highlight. Alpha is the shininess.
    write_png("stone_s.png", 16, 16, [](std::uint32_t x, std::uint32_t y) {
        const bool middle = bevel(x) == 0.0F && bevel(y) == 0.0F && x != 0 && y != 0;
        return middle ? Rgba{255, 255, 255, 160} : Rgba{0, 0, 0, 255};
    });
    write_png("panel.png", 48, 12,
              [](std::uint32_t /*x*/, std::uint32_t /*y*/) { return Rgba{230, 210, 160, 255}; });
}

struct Agent {
    world::ComponentRegistry components;
    command::Registry commands;
    std::unique_ptr<command::Session> session;

    Agent() {
        world::register_builtin_components(components);
        command::register_builtin_commands(commands);
        command::SessionOptions options;
        options.capture_logs = false;
        session = std::make_unique<command::Session>(components, commands, options);
    }

    // Prints the call as an agent would make it, then the reply.
    void call(std::string_view name, std::string_view params) const {
        const json::Value value = json::parse(params).value();
        std::printf("%.*s %s\n", static_cast<int>(name.size()), name.data(),
                    json::write(value, json::Style::Compact).c_str());
        command::Reply reply = session->call(name, value);
        const json::Value out = reply.ok ? std::move(reply.value) : json::Value(reply.message);
        std::puts(json::write(out, json::Style::Compact).c_str());
    }
};

} // namespace

int main() {
    write_textures();
    const Agent agent;
    agent.call("scene.open", R"({"path": "main.scene.json"})");

    std::puts("-- the panel is in the HUD: dimmed by the HUD's CanvasModulate, lit by the lantern "
              "and not the torch");
    agent.call("light.sample", R"({"id": "panel"})");

    std::puts("-- a block's left edge faces the torch and is bright, its right edge faces away, "
              "the far corner is dark, and the panel is lit by the lantern");
    agent.call("frame.capture", R"({"points": [[34, 40], [46, 40], [150, 80], [28, 10]]})");

    std::puts("-- the torch's layer range takes the HUD's layer too");
    agent.call("component.set",
               R"({"id": "torch", "components": {"PointLight2D": {"range_layer_max": 1}}})");
    agent.call("light.sample", R"({"id": "panel"})");
    agent.call("frame.capture", R"({"points": [[28, 10]]})");
    return 0;
}
