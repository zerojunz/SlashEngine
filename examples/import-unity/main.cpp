// Importing a Unity game: project.import converts a Unity project into this one.
//
// unity/ holds "Pond Hop", a small Unity 2D game as Unity saves it: project settings, a scene
// that instances three prefabs (one of them a prefab variant) and has a tile map, 2D lights and a
// HUD canvas, sprites and their import settings, tile assets, an Animator controller and its
// clips, an audio mixer, a physics material, an Input System actions file and four C# scripts.
// One call converts it.
// Its scene and prefabs become .scene.json files beside the originals, the prefabs staying
// prefabs that the scene instances; its sprites, tile map, cameras, 2D physics and joints, 2D
// lights and shadow casters, animation, sounds and UI become SlashEngine's components, measured
// in pixels, the tile map's sprites a tile set, the controller an animation library and its state
// machine an animation tree, and the mixer a bus layout; its player, physics, time, sorting and
// input settings become project.json; and each script a C++ skeleton and a storyboard task to
// port it to C++, since SlashEngine games are plain C++, as the controller's parameters are a
// task for gameplay code to set. The report lists what didn't convert,
// and why. Games ship PNG and WAV files made in other tools; this example makes its textures and
// sounds in code (without libm, so they are the same everywhere), so the repository holds no
// images or sounds.

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

constexpr int kRate = 44100;

void write_file(const std::string& path, const std::string& contents) {
    if (!slash::platform::write_file(path, contents).ok()) {
        std::fprintf(stderr, "can't write %s\n", path.c_str());
        std::exit(1);
    }
}

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
    write_file(path, render::encode_png(image).value());
}

// Writes a square wave that falls from one pitch to another, fading out.
void write_sound(const std::string& path, int from_hz, int to_hz, float amplitude, int frames) {
    std::vector<audio::Frame> out(static_cast<std::size_t>(frames));
    int phase = 0; // in cycles times kRate
    for (int i = 0; i < frames; ++i) {
        const int hz = from_hz + ((to_hz - from_hz) * i / frames);
        phase = (phase + (2 * hz)) % (2 * kRate);
        const float value = (phase < kRate ? amplitude : -amplitude) *
                            static_cast<float>(frames - i) / static_cast<float>(frames);
        out[static_cast<std::size_t>(i)] = {.left = value, .right = value};
    }
    write_file(path, audio::encode_wav(out, kRate));
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
    if (!slash::platform::create_directories("unity/Assets/Sprites").ok() ||
        !slash::platform::create_directories("unity/Assets/Audio").ok()) {
        return 1;
    }
    write_texture("unity/Assets/Sprites/frog.png", 32, 32, {80, 170, 60});
    write_texture("unity/Assets/Sprites/frog_blink.png", 32, 32, {60, 140, 45});
    write_texture("unity/Assets/Sprites/coins.png", 32, 32, {250, 210, 50});
    write_texture("unity/Assets/Sprites/lily_pad.png", 64, 16, {40, 120, 50});
    write_texture("unity/Assets/Sprites/sky.png", 640, 360, {110, 175, 220});
    write_texture("unity/Assets/Sprites/hat.png", 16, 8, {200, 50, 50});
    write_texture("unity/Assets/Sprites/bank.png", 96, 32, {120, 90, 50});
    write_sound("unity/Assets/Audio/croak.wav", 220, 110, 0.5F, kRate / 4);
    write_sound("unity/Assets/Audio/pond.wav", 330, 330, 0.2F, kRate);

    const Agent agent;
    // A dry run first: what the import would write, writing nothing.
    agent.call("project.import", R"({"source": "unity", "dry_run": true})");
    agent.call("project.import", R"({"source": "unity"})");

    // The scene, the frog and gold coin prefabs, the bank's tile set, the frog's animation
    // library and tree, the mixer's buses, the looping music and the project's settings.
    print_file("unity/Assets/Scenes/Pond.scene.json");
    print_file("unity/Assets/Prefabs/Frog.scene.json");
    print_file("unity/Assets/Prefabs/Gold Coin.scene.json");
    print_file("unity/Assets/tiles.tileset.json");
    print_file("unity/Assets/Animations/Frog.anim.json");
    print_file("unity/Assets/Animations/Frog.animtree.json");
    print_file("unity/Assets/Audio/Pond.bus_layout.json");
    print_file("unity/Assets/Audio/pond.loop.audio.json");
    print_file("project.json");

    // The converted scene opens and runs: the frog falls onto the ground and rests there, and
    // the lantern swings on its hinge. The frog's tree starts in Idle: a second in, it has
    // ribbited, and it blinks at 1.75 s.
    agent.call("scene.open", R"({"path": "unity/Assets/Scenes/Pond.scene.json"})");
    agent.call("events.subscribe", R"({"kinds": ["Ribbit"]})");
    agent.call("step", R"({"ticks": 60})");
    agent.call("world.query", R"({"with": ["RigidBody2D"], "include": ["Transform2D"]})");
    agent.call("step", R"({"ticks": 50})");
    agent.call("events.read", R"({"subscription": "events_1"})");
    agent.call("entity.get", R"({"id": "frog/frog_sprite", "components": ["Sprite2D"],
                                 "omit_defaults": true})");
    // Gameplay code sets the Hop trigger, as SetTrigger("Hop") did: the frog hops, stretching
    // its feet.
    agent.call("animation.set_parameters", R"({"id": "frog", "parameters": {"Hop": true}})");
    agent.call("step", R"({"ticks": 16})");
    agent.call("entity.get", R"({"id": "frog/feet", "components": ["Transform2D"]})");
    // The HUD canvas is controls drawn over the game: the coin count laid out in a row at the
    // top left, the pause button at the top right and the hint along the bottom.
    agent.call("ui.layout", R"({"ids": ["coins", "icon", "count", "pause", "hint"]})");
    // URP's 2D lights are Godot's: the dusky Global Light 2D is the scene's CanvasModulate and
    // the lantern a PointLight2D, through a texture of its falloff, that swings with it. The lily
    // pad's ShadowCaster2D is an occluder: beside the lantern it lights the pond on top of the
    // dusk, and under the lily pad only the dusk does.
    agent.call("entity.get", R"({"id": "global_light_2d", "components": ["CanvasModulate"]})");
    agent.call("entity.get", R"({"id": "lantern", "components": ["PointLight2D"],
                                 "omit_defaults": true})");
    agent.call("light.sample", R"({"points": [[-160, -32], [144, 112]]})");
    // The Hop animation stretches the frog's feet over its half second.
    agent.call("animation.eval", R"({"path": "unity/Assets/Animations/Frog.anim.json",
                                     "animation": "Hop", "times": [0, 0.25],
                                     "tracks": ["Feet:Transform2D/position/y"]})");
    // The bank's tiles are solid where their sprites' physics shapes are: a ray down onto its
    // first slope stops halfway up the slope.
    agent.call("physics.raycast", R"({"from": [176, 0], "to": [176, 200]})");
    // The coins are areas the frog can touch; the hidden one's shape is disabled.
    agent.call("physics.overlap", R"({"shape": {"shape": "rectangle", "size": [96, 16]},
                                      "position": [-16, 128], "collide_with_areas": true})");
    agent.call("storyboard.query", R"({"kind": "task"})");
    agent.call("storyboard.check", "{}");
    return 0;
}
