// Plays a video in a control and on a sprite. The clip is made here with the engine's own AV1
// encoder: four colors, a quarter of a second each, with a white bar that moves across. The
// VideoStreamPlayer "tv" plays it at its own size in the top left corner, and the sprite
// "screen" shows the same frames at half size through the texture "video:tv". The output is
// the colors frame.capture sees as it plays, pauses, seeks, finishes and loops.
#include "slash/command/builtin.hpp"
#include "slash/command/session.hpp"
#include "slash/platform/file.hpp"
#include "slash/video/video.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace slash;

namespace {

constexpr std::array<std::array<std::uint8_t, 3>, 4> kColors{
    {{200, 40, 40}, {40, 160, 60}, {40, 70, 200}, {230, 190, 40}}};

// The clip: 64x32 frames, four a second, each a color with a white bar at its index.
std::vector<render::Image> clip_frames() {
    std::vector<render::Image> frames;
    for (std::size_t i = 0; i < kColors.size(); ++i) {
        render::Image image;
        image.width = 64;
        image.height = 32;
        image.pixels.resize(std::size_t{64} * 32 * 4);
        for (std::uint32_t y = 0; y < 32; ++y) {
            for (std::uint32_t x = 0; x < 64; ++x) {
                const bool bar = x / 16 == i && y >= 24;
                std::uint8_t* pixel = &image.pixels[((std::size_t{y} * 64) + x) * 4];
                for (std::size_t c = 0; c < 3; ++c) {
                    pixel[c] = bar ? 255 : kColors.at(i).at(c);
                }
                pixel[3] = 255;
            }
        }
        frames.push_back(std::move(image));
    }
    return frames;
}

struct Game {
    world::ComponentRegistry components;
    command::Registry commands;
    std::unique_ptr<command::Session> session;

    Game() {
        world::register_builtin_components(components);
        command::register_builtin_commands(commands);
        command::register_run_commands(commands);
        command::SessionOptions options;
        options.capture_logs = false;
        session = std::make_unique<command::Session>(components, commands, std::move(options));
    }

    [[nodiscard]] json::Value ok(std::string_view command, std::string_view params = "{}") const {
        command::Reply reply = session->call(command, json::parse(params).value());
        if (!reply.ok) {
            std::printf("  %.*s failed: %s\n", static_cast<int>(command.size()), command.data(),
                        reply.message.c_str());
            return {};
        }
        return std::move(reply.value);
    }

    // Steps ticks of 1/20 s.
    void step(int ticks) const {
        (void)ok("step", R"({"ticks": )" + std::to_string(ticks) + R"(, "tick_seconds": 0.05})");
    }

    // A pixel's color as 0..255, from the frame drawn on the CPU.
    [[nodiscard]] std::string color_at(int x, int y) const {
        const json::Value captured = ok("frame.capture", R"({"points": [[)" + std::to_string(x) +
                                                             ", " + std::to_string(y) + "]]}");
        const json::Array& color = captured.find("colors")->as_array().at(0).as_array();
        std::string out;
        for (std::size_t c = 0; c < 3; ++c) {
            out += (c > 0 ? " " : "") +
                   std::to_string(static_cast<int>(std::lround(color.at(c).as_double() * 255.0)));
        }
        return out;
    }

    void show(const char* what) const {
        const json::Value got = ok("entity.get", R"({"id": "tv"})");
        const json::Value& player = *got.find("components")->find("VideoStreamPlayer");
        const json::Value* position = player.find("stream_position");
        const json::Value* playing = player.find("playing");
        // The bar is in one of four places along the bottom of the video.
        int bar = -1;
        for (int i = 0; i < 4; ++i) {
            if (color_at(16 + (16 * i), 36) == "255 255 255") {
                bar = i;
            }
        }
        std::printf("%s\n  position %.2f s, %s\n  tv (20, 16): %s, bar in place %d\n  screen "
                    "(120, 60): %s\n",
                    what, position != nullptr ? position->as_double() : 0.0,
                    playing != nullptr && playing->as_bool() ? "playing" : "stopped",
                    color_at(20, 16).c_str(), bar, color_at(120, 60).c_str());
    }
};

} // namespace

int main() {
    const auto clip = video::encode_ivf(clip_frames(), 4);
    if (!clip ||
        !platform::write_file("clip.ivf", std::string(clip.value().begin(), clip.value().end()))) {
        std::printf("couldn't write clip.ivf\n");
        return 1;
    }
    std::printf("clip.ivf: 4 frames of 64x32, %zu bytes\n", clip.value().size());
    const Game game;
    (void)game.ok("scene.open", R"({"path": "main.scene.json"})");

    std::printf("before the first tick, nothing is decoded: tv (20, 16) %s\n",
                game.color_at(20, 16).c_str());
    game.step(1);
    game.show("tick 1: autoplay starts it on its first frame");
    const json::Value layout = game.ok("ui.layout", R"({"ids": ["tv"]})");
    std::printf(
        "  tv's rect: %s\n",
        json::write(*layout.find("controls")->as_array().at(0).find("rect"), json::Style::Compact)
            .c_str());

    game.step(5);
    game.show("tick 6: a quarter second on, the second frame");

    (void)game.ok("component.set", R"({"id": "tv", "components": {"VideoStreamPlayer":
                                    {"paused": true, "stream_position": 0.75}}})");
    game.step(4);
    game.show("paused and seeked to 0.75 s: the last frame, held");

    (void)game.ok("component.set",
                  R"({"id": "tv", "components": {"VideoStreamPlayer": {"paused": false}}})");
    const std::string subscription =
        game.ok("events.subscribe", R"({"kinds": ["finished"]})").find("subscription")->as_string();
    game.step(6);
    game.show("6 ticks later: past the end, it stops on the last frame");
    std::printf("  events: %s\n", json::write(*game.ok("events.read", R"({"subscription": ")" +
                                                                          subscription + R"("})")
                                                   .find("events"),
                                              json::Style::Compact)
                                      .c_str());

    (void)game.ok("component.set", R"({"id": "tv", "components": {"VideoStreamPlayer":
                                    {"loop": true, "playing": true, "stream_position": 0.5}}})");
    game.step(11);
    game.show("looping from 0.5 s, 11 ticks later: round again to the first frame");
    return 0;
}
