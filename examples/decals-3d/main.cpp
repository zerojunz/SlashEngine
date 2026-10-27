// Decals, as Godot 4.6's Decal in its Forward+ renderer.
//
// A street in a 320x180 window: a dashed yellow line painted down it, a puddle that is darker and
// smoother than the asphalt around it, and a sign glowing on the wall behind. Each is a Decal, a
// box that projects its textures down its -Y onto the meshes inside it: the line an albedo
// texture, the puddle an albedo and an ORM texture (its roughness low), the sign only an emission
// texture, turned to face the wall. Games ship PNG files made in an image editor; this example
// draws its textures in code, so the repository holds no images.

#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/platform/file.hpp"
#include "slash/render/image.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <array>
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

// Writes a texture whose texels color gives.
void write_texture(const std::string& path, std::uint32_t width, std::uint32_t height,
                   const std::function<Rgba(std::uint32_t, std::uint32_t)>& color) {
    render::Image image{.width = width, .height = height, .pixels = {}};
    image.pixels.resize(std::size_t{width} * height * 4);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const Rgba texel = color(x, y);
            const std::size_t at = ((std::size_t{y} * width) + x) * 4;
            for (std::size_t i = 0; i < 4; ++i) {
                image.pixels[at + i] = texel.at(i);
            }
        }
    }
    if (!slash::platform::write_file(path, render::encode_png(image).value()).ok()) {
        std::fprintf(stderr, "can't write %s\n", path.c_str());
        std::exit(1);
    }
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
    // A dash every 32 texels down the line, 20 long, clear between.
    write_texture("stripe.png", 8, 64, [](std::uint32_t /*x*/, std::uint32_t y) {
        return y % 32 < 20 ? Rgba{240, 200, 40, 255} : Rgba{0, 0, 0, 0};
    });
    // A round puddle, dark and clear around it, and its roughness (ORM green) low.
    const auto inside = [](std::uint32_t x, std::uint32_t y) {
        const int dx = static_cast<int>(x) - 16;
        const int dy = static_cast<int>(y) - 16;
        return (dx * dx) + (dy * dy) < 14 * 14;
    };
    write_texture("puddle.png", 32, 32, [&](std::uint32_t x, std::uint32_t y) {
        return inside(x, y) ? Rgba{25, 30, 40, 255} : Rgba{0, 0, 0, 0};
    });
    write_texture("puddle_orm.png", 32, 32,
                  [](std::uint32_t /*x*/, std::uint32_t /*y*/) { return Rgba{255, 15, 0, 255}; });
    // A sign's glowing frame and bars of text, black (no light) between.
    write_texture("sign.png", 48, 16, [](std::uint32_t x, std::uint32_t y) {
        const bool frame = x == 1 || x == 46 || y == 1 || y == 14;
        const bool text = y >= 5 && y <= 10 && x >= 6 && x <= 41 && x % 8 < 5;
        return frame || text ? Rgba{40, 220, 255, 255} : Rgba{0, 0, 0, 255};
    });

    const Agent agent;
    agent.call("scene.open", R"({"path": "main.scene.json"})");
    std::puts(
        "-- the line, the asphalt beside it, the puddle, a bar of the sign and the bare wall");
    agent.call("frame.capture", R"({"points": [[96, 120], [130, 120], [214, 128], [137, 18],
        [60, 40]], "path": "shots/street.png"})");

    std::puts("-- the line fades to 40%, the puddle projects only onto layer 2, where the street "
              "isn't, and the sign glows three times as bright");
    agent.call("batch", R"({"commands": [
        {"command": "component.set", "params": {"id": "paint",
            "components": {"Decal": {"modulate": [1, 1, 1, 0.4]}}}},
        {"command": "component.set", "params": {"id": "puddle",
            "components": {"Decal": {"cull_mask": 2}}}},
        {"command": "component.set", "params": {"id": "sign",
            "components": {"Decal": {"emission_energy": 6}}}}]})");
    agent.call("frame.capture",
               R"({"points": [[96, 120], [130, 120], [214, 128], [137, 18], [60, 40]]})");
    return 0;
}
