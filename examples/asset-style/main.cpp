// One style per game, refined from the assets agents accept: asset.style drafts the storyboard's
// palette, budgets and references from them, and from then on storyboard.check holds new models
// to the triangle budget and generation requests carry the palette and the references.
//
// The cafe starts with a tone and nothing else. Its stool, cup and chalkboard sign are added to
// the library and accepted with reasons; asset.style turns them into edits, storyboard.edit
// keeps them, and a brass lamp of a few thousand triangles then breaks the budget they set.

#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/library/commands.hpp"
#include "slash/platform/file.hpp"
#include "slash/render/image.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

namespace {

namespace command = slash::command;
namespace json = slash::json;

// Prints the call as an agent would make it, then the reply.
json::Value call(command::Session& session, std::string_view name, std::string_view params) {
    const json::Value value = json::parse(params).value();
    std::printf("%.*s %s\n", static_cast<int>(name.size()), name.data(),
                json::write(value, json::Style::Compact).c_str());
    const command::Reply reply = session.call(name, value);
    const json::Value shown = reply.ok ? reply.value : json::Value(reply.message);
    std::puts(json::write(shown, json::Style::Compact).c_str());
    return shown;
}

// A chalkboard sign drawn in two flat colors: a dark board in a light frame.
void write_sign(const std::string& path) {
    slash::render::Image image;
    image.width = 24;
    image.height = 12;
    for (std::uint32_t y = 0; y < image.height; ++y) {
        for (std::uint32_t x = 0; x < image.width; ++x) {
            const bool frame = x < 2 || y < 2 || x >= image.width - 2 || y >= image.height - 2;
            if (frame) {
                image.pixels.insert(image.pixels.end(), {0xe8, 0xd8, 0xb0, 0xff});
            } else {
                image.pixels.insert(image.pixels.end(), {0x2a, 0x3a, 0x30, 0xff});
            }
        }
    }
    (void)slash::platform::create_directories(path.substr(0, path.rfind('/')));
    (void)slash::platform::write_file(path, slash::render::encode_png(image).value());
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

    write_sign("game/art/sign.png");
    const std::string stool =
        call(game, "asset.add",
             R"({"path": "props/stool.model.json", "description": "A cafe stool"})")["id"]
            .as_string();
    const std::string cup =
        call(game, "asset.add", R"({"path": "props/cup.model.json", "description": "A cup"})")["id"]
            .as_string();
    const std::string sign =
        call(game, "asset.add",
             R"({"path": "art/sign.png", "description": "A chalkboard sign"})")["id"]
            .as_string();
    call(game, "asset.feedback",
         R"({"id": ")" + stool +
             R"(", "verdict": "accepted", "reason": "eight-sided parts, flat oak and steel"})");
    call(game, "asset.feedback",
         R"({"id": ")" + cup +
             R"(", "verdict": "accepted", "reason": "reads at a glance on the counter"})");
    call(game, "asset.feedback",
         R"({"id": ")" + sign +
             R"(", "verdict": "accepted", "reason": "two flat colors, a thick frame"})");

    // The style drafted from them, kept as it is.
    json::Value edits;
    edits["edits"] = call(game, "asset.style", "{}")["edits"];
    edits["why"] = "the style of the accepted stool, cup and sign";
    call(game, "storyboard.edit", json::write(edits, json::Style::Compact));

    // A lamp far over the budget is caught, and a new image request carries the palette and the
    // references.
    call(game, "asset.add", R"({"path": "props/lamp.model.json", "description": "A brass lamp"})");
    call(game, "storyboard.check", "{}");
    json::Value made = call(game, "asset.generate", R"({"prompt": "a coffee menu board",
        "generator": "test", "width": 16, "height": 8, "path": "art/menu.png"})");
    call(game, "asset.info", R"({"id": ")" + made["id"].as_string() + R"("})");
    return 0;
}
