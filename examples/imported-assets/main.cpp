// Imported assets: a model made in another tool goes into the asset library as generated ones
// do, converted to a .glb file, cleaned up, described, reviewed and kept with its license and the
// file it came from; an edit to that file makes a new version.
//
// The warehouse game has a crate an artist exported from a CAD tool as an OBJ file, in
// centimeters with z up. asset.add imports it turned, scaled and standing on its origin, with
// convex collision, and reviews it. The artist then makes the crate taller: asset.check sees the
// file changed, and adding it again keeps the new version, cleaned up as the first was. A market
// town game then finds the crate in the library and takes its .glb file.

#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/library/commands.hpp"
#include "slash/platform/file.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

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

// Replaces every from in a project file with to, as the artist's tool saving it again does.
void edit(const std::string& path, std::string_view from, std::string_view to) {
    std::string text = slash::platform::read_file(path).value();
    for (std::size_t at = text.find(from); at != std::string::npos;
         at = text.find(from, at + to.size())) {
        text.replace(at, from.size(), to);
    }
    (void)slash::platform::write_file(path, text);
    std::printf("edit %s\n", path.c_str());
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

    // The crate goes in cleaned up and reviewed. With no providers.json, no vision model looks at
    // the turntable views, so the review is of its geometry.
    const json::Value crate = call(game, "asset.add", R"({"path": "props/crate.obj",
        "description": "A wooden shipping crate.", "tags": ["crate", "prop"], "author": "Ana",
        "license": "CC-BY-4.0", "review": true,
        "cleanup": {"up": "z", "scale": 0.01, "pivot": [0.5, 0, 0.5], "collision": "convex"}})");
    const std::string id = crate.find("id")->as_string();
    call(game, "asset.info", R"({"id": ")" + id + R"("})");
    std::printf("game/library.json %s\n",
                json::write(json::parse(slash::platform::read_file("game/library.json").value())
                                .value()["assets"]["props/crate.obj"],
                            json::Style::Compact)
                    .c_str());

    // The artist makes the crate taller and saves over the file.
    edit("game/props/crate.obj", " 80\n", " 120\n");
    call(game, "asset.check", "{}");
    const json::Value taller = call(game, "asset.add", R"({"path": "props/crate.obj"})");
    call(game, "asset.info", R"({"id": ")" + taller.find("id")->as_string() + R"("})");
    call(game, "asset.check", "{}");

    // Another game finds the crate and takes it as a .glb file.
    options.project_root = "town";
    command::Session town(components, commands, options);
    call(town, "asset.search", R"({"text": "crate"})");
    call(town, "asset.use",
         R"({"id": ")" + taller.find("id")->as_string() + R"(", "path": "props/crate.glb"})");
    call(town, "model.inspect", R"({"path": "props/crate.glb", "brief": true})");
    return 0;
}
