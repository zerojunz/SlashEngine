// Importing a shipped Godot game: project.import takes a game exported with Godot 4, its .pck
// file or the executable with the pack inside, unpacks it back into the project it was made from
// and converts that project as it would a project folder.
//
// jumper/ is "Jumper", a small Godot 4 game as its maker keeps it: project.godot, a scene, a
// script and the .import files of a texture and a sound. main.cpp first plays the part of
// Godot's export and writes shipped/jumper.pck as Godot 4.5's export does: the scene as a binary
// .scn file, the script as binary tokens, project.godot as project.binary, the PNG file as a
// CompressedTexture2D and the WAV file as an AudioStreamWAV. Then it makes the calls an agent
// would make with only the shipped game: one project.import call unpacks it into godot/ and
// converts it. Unpack only games you made or have the rights to; a pack encrypted with the
// game's own key is refused. The texture and sound are made in code, so the repository holds no
// binary files.

#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/gameplay/host.hpp"
#include "slash/importer/commands.hpp"
#include "slash/importer/godot_pack.hpp"
#include "slash/platform/file.hpp"
#include "slash/render/image.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <string_view>

namespace {

namespace command = slash::command;
namespace godot = slash::importer::godot;
namespace json = slash::json;
namespace platform = slash::platform;
namespace render = slash::render;
namespace world = slash::world;

[[noreturn]] void fail(const std::string& message) {
    std::fprintf(stderr, "%s\n", message.c_str());
    std::exit(1);
}

// The hero, 12 by 16 pixels: red, with a darker outline.
std::string hero_png() {
    render::Image image{.width = 12, .height = 16, .pixels = {}};
    for (std::uint32_t y = 0; y < 16; ++y) {
        for (std::uint32_t x = 0; x < 12; ++x) {
            const bool edge = x == 0 || y == 0 || x == 11 || y == 15;
            image.pixels.insert(image.pixels.end(),
                                {static_cast<std::uint8_t>(edge ? 120 : 220),
                                 static_cast<std::uint8_t>(edge ? 30 : 80),
                                 static_cast<std::uint8_t>(edge ? 20 : 60), 255});
        }
    }
    return render::encode_png(image).value();
}

void put16(std::string& out, std::uint32_t value) {
    out += static_cast<char>(value & 0xFFU);
    out += static_cast<char>((value >> 8U) & 0xFFU);
}

void put32(std::string& out, std::uint32_t value) {
    put16(out, value & 0xFFFFU);
    put16(out, value >> 16U);
}

// The jump sound: 50 ms of a square wave rising from 440 to 880 Hz, 16-bit mono at 22050 Hz.
std::string jump_wav() {
    constexpr std::uint32_t kRate = 22050;
    constexpr std::uint32_t kFrames = kRate / 20;
    std::string samples;
    std::uint32_t phase = 0;
    for (std::uint32_t i = 0; i < kFrames; ++i) {
        phase += 440 + (440 * i / kFrames); // Hz: one period is kRate of phase
        const bool high = (phase % kRate) < kRate / 2;
        put16(samples, static_cast<std::uint16_t>(high ? 6000 : -6000));
    }
    std::string wav = "RIFF";
    put32(wav, static_cast<std::uint32_t>(36 + samples.size()));
    wav += "WAVEfmt ";
    put32(wav, 16);
    put16(wav, 1); // PCM
    put16(wav, 1); // mono
    put32(wav, kRate);
    put32(wav, kRate * 2);
    put16(wav, 2);
    put16(wav, 16);
    wav += "data";
    put32(wav, static_cast<std::uint32_t>(samples.size()));
    return wav + samples;
}

void print_file(const std::string& path) {
    std::printf("--- %s\n%s", path.c_str(), platform::read_file(path).value().c_str());
}

struct Agent {
    world::ComponentRegistry components;
    command::Registry commands;
    // The game's own C++ code. Until it is built, the component of the skeleton the import writes
    // for the script is a stand-in that keeps the values the scene gives it.
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
    // The game's maker exports it, as Godot's export does.
    std::map<std::string, std::string> project;
    auto paths = platform::list_files("jumper");
    if (!paths) {
        fail(paths.error().message);
    }
    for (const std::string& path : paths.value()) {
        project[path] = platform::read_file("jumper/" + path).value();
    }
    project["art/hero.png"] = hero_png();
    project["sfx/jump.wav"] = jump_wav();
    auto exported = godot::export_project(project);
    if (!exported) {
        fail(exported.error().message);
    }
    std::puts("The files in shipped/jumper.pck:");
    for (const auto& [path, data] : exported.value()) {
        std::printf("  %s\n", path.c_str());
    }
    if (!platform::create_directories("shipped").ok() ||
        !platform::write_file("shipped/jumper.pck", godot::write_pack(exported.value())).ok()) {
        fail("can't write shipped/jumper.pck");
    }

    const Agent agent;
    // A dry run first: what the import would write, writing nothing.
    agent.call("project.import",
               R"({"source": "shipped/jumper.pck", "unpack_to": "godot", "dry_run": true})");
    agent.call("project.import", R"({"source": "shipped/jumper.pck", "unpack_to": "godot"})");

    // The project as it came out of the game: the script from its tokens, without its comments
    // but with every line where it was; the scene from its binary form; the settings.
    print_file("godot/player.gd");
    print_file("godot/main.tscn");
    print_file("godot/project.godot");
    // The texture and the sound are the ones the game was made with.
    const auto hero = render::decode_image(platform::read_file("godot/art/hero.png").value());
    const auto original = render::decode_image(project["art/hero.png"]);
    std::printf("godot/art/hero.png has the game's pixels: %s\n",
                hero && original && hero.value().pixels == original.value().pixels ? "yes" : "no");
    std::printf("godot/sfx/jump.wav is the game's sound: %s\n",
                platform::read_file("godot/sfx/jump.wav").value() == project["sfx/jump.wav"]
                    ? "yes"
                    : "no");

    // The converted scene and settings, as importing a project folder writes them.
    print_file("godot/main.scene.json");
    print_file("project.json");
    agent.call("scene.open", R"({"path": "godot/main.scene.json"})");
    agent.call("world.query",
               R"({"with": ["Sprite2D"], "include": ["Sprite2D"], "omit_defaults": true})");
    agent.call("storyboard.query", R"({"kind": "task"})");
}
