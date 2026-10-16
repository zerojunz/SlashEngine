// Camera2D: the part of a level a window shows, as Godot's Camera2D.
//
// The level is 1600 pixels wide in a 320x180 window. The player carries a camera 32 pixels above
// it, as a child entity, so the view follows the player: the camera's drag margins let the
// player walk a little before it moves, smoothing makes it glide, and its limits keep the view
// inside the level. The coin counter is a control, so it stays put on the screen. Games ship PNG
// files made in an image editor; this example draws its textures in code, so the repository
// holds no images.

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
#include <memory>
#include <string>
#include <string_view>

namespace {

namespace command = slash::command;
namespace json = slash::json;
namespace render = slash::render;
namespace world = slash::world;

// Writes an opaque texture in one color.
void write_texture(const std::string& path, std::uint32_t width, std::uint32_t height,
                   std::array<std::uint8_t, 3> color) {
    render::Image image{.width = width, .height = height, .pixels = {}};
    image.pixels.resize(std::size_t{width} * height * 4);
    for (std::size_t at = 0; at < image.pixels.size(); at += 4) {
        image.pixels[at] = color[0];
        image.pixels[at + 1] = color[1];
        image.pixels[at + 2] = color[2];
        image.pixels[at + 3] = 255;
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
    write_texture("ground.png", 1600, 20, {90, 160, 70});
    write_texture("player.png", 12, 16, {220, 80, 60});
    write_texture("coin.png", 8, 8, {250, 210, 50});

    const Agent agent;
    agent.call("scene.open", R"({"path": "main.scene.json"})");
    const std::string_view shown = R"({"ids": ["player", "coin", "score"]})";

    std::puts("-- the window shows the level around the camera, with the counter where it was");
    agent.call("step", R"({"ticks": 1})");
    agent.call("frame.summary", shown);

    std::puts(
        "-- the player walks 100 pixels right: past the drag margin, the camera glides after");
    agent.call("component.set",
               R"({"id": "player", "components": {"Transform2D": {"position": [500, 152]}}})");
    agent.call("step", R"({"ticks": 1})");
    agent.call("frame.summary", shown);
    agent.call("step", R"({"ticks": 120})");
    agent.call("frame.summary", shown);

    std::puts("-- at the end of the level the limits stop the view, and the coin comes into it");
    agent.call("component.set",
               R"({"id": "player", "components": {"Transform2D": {"position": [1580, 152]}}})");
    agent.call("step", R"({"ticks": 120})");
    agent.call("frame.summary", shown);

    std::puts("-- zoomed in twice, the view is a quarter of the size and still inside the level");
    agent.call("component.set", R"({"id": "cam", "components": {"Camera2D": {"zoom": [2, 2]}}})");
    agent.call("frame.summary", shown);
    return 0;
}
