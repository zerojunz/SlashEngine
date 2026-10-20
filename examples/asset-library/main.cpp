// The asset library: images and sounds kept once on the machine and reused by every project.
//
// Two projects share one library. The first generates a sprite and a sound, refines the sprite
// and adds an icon of its own; the second asks for the same sprite and gets the stored one with
// no model call. The built-in "test" generator makes placeholders, so the example needs no model
// or key; a project names real generators in providers.json and calls them the same way.

#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/library/commands.hpp"
#include "slash/platform/file.hpp"
#include "slash/render/device.hpp"
#include "slash/render/image.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

namespace {

namespace command = slash::command;
namespace json = slash::json;

// Prints the call as an agent would make it, then the reply.
void call(command::Session& session, std::string_view name, std::string_view params) {
    const json::Value value = json::parse(params).value();
    std::printf("%.*s %s\n", static_cast<int>(name.size()), name.data(),
                json::write(value, json::Style::Compact).c_str());
    const command::Reply reply = session.call(name, value);
    std::puts(json::write(reply.ok ? reply.value : json::Value(reply.message), json::Style::Compact)
                  .c_str());
}

// An 8 by 8 heart, drawn by the game's artist, to add to the library.
std::string heart() {
    constexpr std::string_view kRows = "........"
                                       ".##..##."
                                       "########"
                                       "########"
                                       ".######."
                                       "..####.."
                                       "...##..."
                                       "........";
    slash::render::Image image;
    image.width = 8;
    image.height = 8;
    for (const char cell : kRows) {
        const bool filled = cell == '#';
        image.pixels.insert(image.pixels.end(),
                            {std::uint8_t{220}, std::uint8_t{40}, std::uint8_t{60},
                             filled ? std::uint8_t{255} : std::uint8_t{0}});
    }
    return slash::render::encode_png(image).value();
}

} // namespace

int main() {
    slash::world::ComponentRegistry components;
    slash::world::register_builtin_components(components);
    command::Registry commands;
    command::register_builtin_commands(commands);
    slash::library::register_commands(commands);

    // The library would be ~/.slash/library; the example keeps its own.
    command::SessionOptions options;
    options.capture_logs = false;
    options.library_dir = "library";
    options.project_root = "game";
    command::Session game(components, commands, options);

    // Nothing yet, so the agent generates. path copies the result into the project too.
    call(game, "asset.search", R"({"text": "slime"})");
    call(game, "asset.generate", R"({"prompt": "green slime sprite", "generator": "test",
        "width": 32, "tags": ["enemy"], "path": "art/slime.png"})");

    // A new version, linked to the first, which stays to go back to.
    call(game, "asset.refine", R"({"id": "0b2fd1b5", "prompt": "make it red",
        "description": "Red slime sprite", "path": "art/slime_red.png"})");
    call(game, "asset.generate", R"({"prompt": "coin pickup chime", "type": "sound",
        "generator": "test", "seconds": 0.3, "tags": ["pickup"], "path": "sfx/coin.wav"})");

    // The artist's heart goes in with its author and license, for other projects to find.
    (void)slash::platform::create_directories("game/ui");
    (void)slash::platform::write_file("game/ui/heart.png", heart());
    call(game, "asset.add", R"({"path": "ui/heart.png", "description": "Red heart icon, 8x8",
        "tags": ["ui", "health"], "license": "CC-BY-4.0", "author": "Mira"})");
    std::printf(
        "game/library.json %s\n",
        json::write(json::parse(slash::platform::read_file("game/library.json").value()).value(),
                    json::Style::Compact)
            .c_str());

    // Another project. The exact same request returns the stored sprite with no model call; a
    // close one gets the match back as a candidate instead of generating.
    options.project_root = "sequel";
    command::Session sequel(components, commands, options);
    call(sequel, "asset.generate", R"({"prompt": "green slime sprite", "generator": "test",
        "width": 32, "tags": ["enemy"], "path": "art/slime.png"})");
    call(sequel, "asset.generate", R"({"prompt": "a slime sprite, green", "generator": "test"})");
    call(sequel, "asset.search", R"({"text": "heart", "tags": ["ui"]})");
    call(sequel, "asset.use", R"({"id": "19b7f428", "path": "ui/heart.png"})");
    call(sequel, "asset.info", R"({"id": "833c6cd1"})");
    return 0;
}
