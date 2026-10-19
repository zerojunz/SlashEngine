// Importing a shipped Unity game: project.import also takes a game built with Unity, its folder,
// executable, .apk or .ipa file or web build, has AssetRipper rebuild the Unity project it was
// made from, fixes what a project rebuilt from a build gets wrong, and converts it as it would a
// project made in Unity.
//
// main.cpp first plays the part of Unity's build and writes shipped/Hopper/, "Hopper" as a Linux
// IL2CPP build lays it out. AssetRipper is a GPL program, run on its own and installed on first
// use; here it is the stand-in from tests/, which exports ripped/ as AssetRipper exports this
// build's project: the scripts of Unity's packages decompiled under their assemblies with GUIDs of
// its own, the game's script a stand-in whose code stayed compiled, and sprites as assets of their
// own on the atlas texture. Then it makes the calls an agent would make with only the shipped
// game. Unpack only games you made or have the rights to; a build with encrypted bundles is
// refused. The atlas is made in code, so the repository holds no binary files.

#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/importer/commands.hpp"
#include "slash/platform/file.hpp"
#include "slash/render/image.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace command = slash::command;
namespace json = slash::json;
namespace platform = slash::platform;
namespace render = slash::render;
namespace world = slash::world;

[[noreturn]] void fail(const std::string& message) {
    std::fprintf(stderr, "%s\n", message.c_str());
    std::exit(1);
}

void write(const std::string& path, const std::string& data) {
    const std::size_t slash = path.rfind('/');
    if (!platform::create_directories(path.substr(0, slash)).ok() ||
        !platform::write_file(path, data).ok()) {
        fail("can't write " + path);
    }
}

// The atlas Unity packed, 32 by 16 pixels: the hopper, green, fills the left half, and the star,
// yellow, 12 by 12 pixels with its transparent edges trimmed, sits 2 in from the right half's.
std::string atlas_png() {
    render::Image image{.width = 32, .height = 16, .pixels = {}};
    for (std::uint32_t y = 0; y < 16; ++y) {
        for (std::uint32_t x = 0; x < 32; ++x) {
            const bool hopper = x < 16 && (x == 0 || x == 15 || y == 0 || y == 15 || y > 4);
            const bool star = x >= 18 && x < 30 && y >= 2 && y < 14;
            const std::uint8_t alpha = hopper || star ? 255 : 0;
            image.pixels.insert(image.pixels.end(),
                                {static_cast<std::uint8_t>(star ? 250 : 60),
                                 static_cast<std::uint8_t>(star ? 210 : 180),
                                 static_cast<std::uint8_t>(star ? 40 : 70), alpha});
        }
    }
    return render::encode_png(image).value();
}

// A serialized file's header as Unity 2022.3 writes it: format 22, then the Unity version.
std::string serialized_file() {
    std::string header(48, '\0');
    header[11] = 22;
    return header + "2022.3.20f1" + std::string(1, '\0');
}

// The lines of a file that contain a piece of text.
void print_lines(const std::string& path, std::string_view text) {
    std::printf("--- %s, the lines with \"%.*s\"\n", path.c_str(), static_cast<int>(text.size()),
                text.data());
    std::istringstream lines(platform::read_file(path).value());
    for (std::string line; std::getline(lines, line);) {
        if (line.find(text) != std::string::npos) {
            std::printf("%s\n", line.c_str());
        }
    }
}

void print_file(const std::string& path) {
    std::printf("--- %s\n%s", path.c_str(), platform::read_file(path).value().c_str());
}

struct Agent {
    world::ComponentRegistry components;
    command::Registry commands;
    std::unique_ptr<command::Session> session;

    explicit Agent(std::vector<command::Capability> capabilities) {
        world::register_builtin_components(components);
        command::register_builtin_commands(commands);
        slash::importer::register_import_commands(commands);
        command::SessionOptions options;
        options.capture_logs = false;
        options.capabilities = std::move(capabilities);
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
    // The game as Unity's Linux build lays it out, with IL2CPP's compiled code and metadata. Only
    // the headers matter here; AssetRipper reads the rest.
    write("shipped/Hopper/Hopper.x86_64", "\x7f"
                                          "ELF");
    write("shipped/Hopper/UnityPlayer.so", "\x7f"
                                           "ELF");
    write("shipped/Hopper/GameAssembly.so", "\x7f"
                                            "ELF");
    write("shipped/Hopper/Hopper_Data/globalgamemanagers", serialized_file());
    write("shipped/Hopper/Hopper_Data/level0", serialized_file());
    write("shipped/Hopper/Hopper_Data/il2cpp_data/Metadata/global-metadata.dat",
          "\xaf\x1b\xb1\xfa");
    // The atlas texture AssetRipper writes from the build.
    write("ripped/Assets/Texture2D/Atlas.png", atlas_png());

    // AssetRipper runs as a program of its own, so a session must grant "build" to start it.
    const Agent reader(
        {command::Capability::Read, command::Capability::Edit, command::Capability::Filesystem});
    reader.call("project.import", R"({"source": "shipped/Hopper"})");

    const Agent agent(command::SessionOptions{}.capabilities);
    agent.call("project.import", R"({"source": "shipped/Hopper", "unpack_to": "unity"})");

    // The project AssetRipper rebuilt, tidied: the scene's package scripts name the GUIDs they
    // have in Unity's packages again, and the stand-ins of Unity's own scripts are gone.
    print_lines("unity/Assets/Scenes/Main.unity", "m_Script:");
    const auto scripts = platform::list_files("unity/Assets/Scripts");
    std::puts("--- unity/Assets/Scripts");
    for (const std::string& path : scripts.value()) {
        std::printf("  %s\n", path.c_str());
    }
    print_file("unity/Assets/Scripts/Assembly-CSharp/Hopper.cs");

    // The converted scene and settings, as importing a Unity project writes them: the sprites in
    // pixels at their own 16 pixels per unit, the star's trimmed pixels offset in its rect.
    print_file("unity/Assets/Scenes/Main.scene.json");
    print_file("project.json");
    agent.call("scene.open", R"({"path": "unity/Assets/Scenes/Main.scene.json"})");
    agent.call("world.query",
               R"({"with": ["Sprite2D"], "include": ["Sprite2D", "Transform2D"],
                   "omit_defaults": true})");
    agent.call("storyboard.query", R"({"kind": "task"})");
}
