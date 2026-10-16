// Frame summaries: what a player would see, as data an agent reads instead of a screenshot.
//
// The scene is a level in a 320x180 window: a row of ground tiles, a player beside a bush, a coin
// past the right edge, a ghost faded out, sparks, a sign whose texture is missing, and a HUD
// with the score, a health bar and a pause button. The agent reads the frame, asks for a text
// map of it, then walks the player behind the bush and off the screen. Games ship PNG files
// made in an image editor; this example draws its textures in code, so the repository holds no
// images.

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
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace {

namespace command = slash::command;
namespace json = slash::json;
namespace render = slash::render;
namespace world = slash::world;

// Writes a texture whose pixels are opaque where inside says, in one color, and clear elsewhere.
void write_texture(const std::string& path, std::uint32_t width, std::uint32_t height,
                   std::array<std::uint8_t, 3> color,
                   const std::function<bool(int x, int y)>& inside) {
    render::Image image{.width = width, .height = height, .pixels = {}};
    image.pixels.resize(std::size_t{width} * height * 4);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::size_t at = ((std::size_t{y} * width) + x) * 4;
            const bool opaque = inside(static_cast<int>(x), static_cast<int>(y));
            image.pixels[at] = color[0];
            image.pixels[at + 1] = color[1];
            image.pixels[at + 2] = color[2];
            image.pixels[at + 3] = opaque ? 255 : 0;
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

    // Prints the call as an agent would make it, then the reply, and returns the reply.
    [[nodiscard]] json::Value call(std::string_view name, std::string_view params) const {
        const json::Value value = json::parse(params).value();
        std::printf("%.*s %s\n", static_cast<int>(name.size()), name.data(),
                    json::write(value, json::Style::Compact).c_str());
        command::Reply reply = session->call(name, value);
        const json::Value out = reply.ok ? std::move(reply.value) : json::Value(reply.message);
        std::puts(json::write(out, json::Style::Compact).c_str());
        return out;
    }

    // A call whose reply has a map, with the map's rows printed one to a line.
    void map(std::string_view params) const {
        const json::Value reply = call("frame.summary", params);
        for (const json::Value& row : reply.find("map")->find("rows")->as_array()) {
            std::printf("  | %s\n", row.as_string().c_str());
        }
    }
};

} // namespace

int main() {
    // A player 16x24 (a head over a body), a bush 48x32 (a dome), a coin and a grass tile.
    write_texture("player.png", 16, 24, {230, 90, 60},
                  [](int x, int y) { return y < 8 ? x >= 4 && x < 12 : x >= 2 && x < 14; });
    write_texture("bush.png", 48, 32, {40, 140, 60}, [](int x, int y) {
        const int dx = (2 * x) + 1 - 48;
        const int dy = (2 * y) + 1 - 64;
        return (dx * dx * 64 * 64) + (dy * dy * 48 * 48) <= 48 * 48 * 64 * 64;
    });
    write_texture("coin.png", 8, 8, {250, 210, 40}, [](int x, int y) {
        const int dx = (2 * x) + 1 - 8;
        const int dy = (2 * y) + 1 - 8;
        return (dx * dx) + (dy * dy) <= 64;
    });
    write_texture("ground.png", 16, 16, {110, 80, 50}, [](int /*x*/, int /*y*/) { return true; });

    const Agent agent;
    (void)agent.call("scene.open", R"({"path": "main.scene.json"})");
    (void)agent.call("step", R"({"ticks": 30})");

    std::puts("-- what the frame shows");
    (void)agent.call("frame.summary", "{}");

    std::puts("-- a text map of it: one key per thing, \".\" for the sky");
    agent.map(R"({"map": [40, 12]})");

    std::puts("-- a closer map of the player and the bush, by region");
    agent.map(R"({"region": [64, 120, 96, 60], "map": [24, 15]})");

    std::puts("-- the pause button takes focus");
    (void)agent.call("ui.focus", R"({"id": "pause"})");
    (void)agent.call("frame.summary", R"({"ids": ["player", "coin", "ghost", "pause"]})");

    std::puts("-- the player walks behind the bush, then off the screen");
    (void)agent.call(
        "component.set",
        R"({"id": "player", "components": {"Transform2D": {"position": [112, 148]}}})");
    (void)agent.call("frame.summary", R"({"ids": ["player", "bush"]})");
    (void)agent.call(
        "component.set",
        R"({"id": "player", "components": {"Transform2D": {"position": [340, 148]}}})");
    (void)agent.call("frame.summary", R"({"ids": ["player"]})");
    return 0;
}
