// Importing a Godot game: project.import converts a Godot 4 project into this one.
//
// godot/ holds "Coin Run", a small Godot 4 platformer as Godot saves it: project.godot, two
// scenes (the level instances the player scene), a SpriteFrames resource, an AnimationPlayer, a
// tile set, a theme, a bus layout, interactive music and three GDScript files. One call converts
// it. Its scenes become .scene.json files beside the originals, its sprite frames .frames.json
// files, its animation library a .anim.json file, its tile set a .tileset.json file, its theme a
// .theme.json file, its buses a .bus_layout.json file, its music and a sound's loop .audio.json
// files, its window, physics, rendering, GUI, audio and input settings project.json, and each
// script a C++ skeleton and a storyboard task to port it, since SlashEngine games are plain C++
// (examples/import-godot-scripts ports one). The report lists what didn't convert, and why. Games
// ship PNG and sound files made in other tools; this example makes its textures and sounds in code,
// so the repository holds no images or sounds.

#include "slash/audio/audio.hpp"
#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/gameplay/host.hpp"
#include "slash/importer/commands.hpp"
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
#include <vector>

namespace {

namespace audio = slash::audio;
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

// Writes a second of a square wave, made without libm so it is the same everywhere.
void write_sound(const std::string& path, int hz, float amplitude) {
    constexpr int kRate = 44100;
    std::vector<audio::Frame> frames(kRate);
    for (int i = 0; i < kRate; ++i) {
        const float value = (i * 2 * hz / kRate) % 2 == 0 ? amplitude : -amplitude;
        frames[static_cast<std::size_t>(i)] = {value, value};
    }
    if (!slash::platform::write_file(path, audio::encode_wav(frames, kRate)).ok()) {
        std::fprintf(stderr, "can't write %s\n", path.c_str());
        std::exit(1);
    }
}

void print_file(const std::string& path) {
    std::printf("--- %s\n%s", path.c_str(), slash::platform::read_file(path).value().c_str());
}

struct Agent {
    world::ComponentRegistry components;
    command::Registry commands;
    // The game's own C++ code. Until it is built, the components of the skeletons the import
    // writes are stand-ins that keep the values scenes give them.
    slash::gameplay::Host code{components, slash::gameplay::library_path(".")};
    std::unique_ptr<command::Session> session;

    Agent() {
        world::register_builtin_components(components);
        command::register_builtin_commands(commands);
        slash::importer::register_import_commands(commands);
        command::SessionOptions options;
        options.capture_logs = false;
        options.gameplay = &code;
        session = std::make_unique<command::Session>(components, commands, options);
    }

    // Prints the call as an agent would make it, then the reply.
    void call(std::string_view name, std::string_view params) const {
        const json::Value value = json::parse(params).value();
        std::printf("%.*s %s\n", static_cast<int>(name.size()), name.data(),
                    json::write(value, json::Style::Compact).c_str());
        command::Reply reply = session->call(name, value);
        const json::Value out = reply.ok ? std::move(reply.value) : json::Value(reply.message);
        std::puts(json::write(out, json::Style::Pretty).c_str());
    }
};

} // namespace

int main() {
    if (!slash::platform::create_directories("godot/art").ok()) {
        return 1;
    }
    write_texture("godot/art/ground.png", 640, 20, {90, 160, 70});
    write_texture("godot/art/coins.png", 24, 8, {250, 210, 50});
    write_texture("godot/art/tiles.png", 48, 16, {70, 130, 60});
    write_texture("godot/art/player_idle.png", 12, 16, {220, 80, 60});
    write_texture("godot/art/player_walk_0.png", 12, 16, {230, 90, 70});
    write_texture("godot/art/player_walk_1.png", 12, 16, {210, 70, 50});
    write_sound("godot/audio/calm.wav", 220, 0.25F);
    write_sound("godot/audio/chase.wav", 330, 0.5F);

    const Agent agent;
    // A dry run first: what the import would write, writing nothing.
    agent.call("project.import", R"({"source": "godot", "dry_run": true})");
    agent.call("project.import", R"({"source": "godot"})");

    // The level, the player's sprite frames, the coin's animations, the platforms' tile set, the
    // theme, the buses, the music, the loop calm.wav's import settings give it and the project's
    // settings.
    print_file("godot/main.scene.json");
    print_file("godot/player_frames.frames.json");
    print_file("godot/main_animation_library_coin.anim.json");
    print_file("godot/tiles.tileset.json");
    print_file("godot/ui_theme.theme.json");
    print_file("godot/default_bus_layout.bus_layout.json");
    print_file("godot/music.audio.json");
    print_file("godot/audio/calm.wav.audio.json");
    print_file("project.json");

    // The converted level opens and runs; the coin spins and bobs, and glints each loop.
    agent.call("scene.open", R"({"path": "godot/main.scene.json"})");
    agent.call("events.subscribe", R"({"kinds": ["glint"]})");
    agent.call("step", R"({"ticks": 25})");
    agent.call("world.query", R"({"with": ["AnimatedSprite2D"],
                                  "include": ["AnimatedSprite2D", "Transform2D"]})");
    agent.call("events.read", R"({"subscription": "events_1"})");
    agent.call("storyboard.query", R"({"kind": "task"})");
    agent.call("storyboard.check", "{}");
    return 0;
}
