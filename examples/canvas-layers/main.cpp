// Canvas layers and parallax, as Godot's CanvasLayer and Parallax2D.
//
// A level 1600 pixels wide scrolls in a 320x180 window behind a camera the player carries. The
// hills are a Parallax2D's child: they move at a quarter of the camera's speed and repeat every
// 320 pixels, so they always fill the window. The clouds scroll at half speed and drift on their
// own. The heart and the coin counter are in a CanvasLayer, so they stay put on the screen.
// canvas.convert turns a window pixel, such as a click, into the world, as Godot's
// get_global_mouse_position. Games ship PNG files made in an image editor; this example draws
// its textures in code, so the repository holds no images.

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
    write_texture("hills.png", 320, 40, {70, 130, 90});
    write_texture("cloud.png", 48, 16, {250, 250, 250});
    write_texture("ground.png", 1600, 20, {90, 160, 70});
    write_texture("player.png", 12, 16, {220, 80, 60});
    write_texture("heart.png", 12, 12, {230, 40, 60});

    const Agent agent;
    agent.call("scene.open", R"({"path": "main.scene.json"})");
    const std::string_view shown = R"({"ids": ["hills", "cloud", "player", "heart", "score"]})";

    std::puts("-- the hills fill the window; the heart and the counter are in the HUD layer");
    agent.call("step", R"({"ticks": 1})");
    agent.call("frame.summary", shown);

    std::puts("-- the player walks 200 pixels right: the hills go by a quarter as far, the "
              "clouds half as far as they drift");
    agent.call("component.set",
               R"({"id": "player", "components": {"Transform2D": {"position": [600, 152]}}})");
    agent.call("step", R"({"ticks": 60})");
    agent.call("frame.summary", shown);

    std::puts("-- a click at the middle of the window and one on the ground, in the world");
    agent.call("canvas.convert", R"({"points": [[160, 90], [100, 165]], "to": "world"})");
    std::puts("-- the same click in the HUD's canvas, which doesn't scroll");
    agent.call("canvas.convert", R"({"points": [[160, 90]], "to": "world", "id": "heart"})");
    std::puts("-- where the player shows in the window");
    agent.call("canvas.convert", R"({"points": [[600, 152]], "to": "screen"})");
    return 0;
}
