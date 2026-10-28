// Checks for complex assets: what a model must be, so a change that breaks what other files
// count on is caught before the library keeps it and again in CI.
//
// The stool recipe carries its own checks (size, pivot, triangle budget, material and node
// names); the lamp gets its checks from asset.add, which keeps them with every version and in the
// project's library.json. Each is then broken the way an edit breaks it, and the library, the
// project's asset.check and test.run say where.

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
void call(command::Session& session, std::string_view name, std::string_view params) {
    const json::Value value = json::parse(params).value();
    std::printf("%.*s %s\n", static_cast<int>(name.size()), name.data(),
                json::write(value, json::Style::Compact).c_str());
    const command::Reply reply = session.call(name, value);
    std::puts(json::write(reply.ok ? reply.value : json::Value(reply.message), json::Style::Compact)
                  .c_str());
}

// Replaces the first from in a project file with to, as an agent's edit does.
void edit(const std::string& path, std::string_view from, std::string_view to) {
    std::string text = slash::platform::read_file(path).value();
    text.replace(text.find(from), from.size(), to);
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

    // The stool passes its own checks, so the library keeps it; the lamp's come with the call.
    call(
        game, "asset.add",
        R"({"path": "props/stool.model.json", "description": "A cafe stool.", "license": "CC0-1.0"})");
    call(game, "asset.add", R"({"path": "props/lamp.model.json", "description": "A floor lamp.",
        "license": "CC0-1.0", "checks": {"size": {"max": [0.5, 1.8, 0.5]}, "nodes": ["bulb"]}})");
    std::printf("game/library.json %s\n",
                json::write(json::parse(slash::platform::read_file("game/library.json").value())
                                .value()["assets"]["props/lamp.model.json"],
                            json::Style::Compact)
                    .c_str());
    call(game, "test.run", "{}");

    // A taller stool no longer fits under the counter: the recipe's own checks refuse it.
    edit("game/props/stool.model.json", "\"height\": 0.45", "\"height\": 0.75");
    call(game, "asset.add", R"({"path": "props/stool.model.json"})");
    edit("game/props/stool.model.json", "\"height\": 0.75", "\"height\": 0.45");

    // The lamp loses its bulb, which the scene's light hangs from: the project's copy fails the
    // checks it was kept with, so asset.check and test.run say where, and the library won't keep
    // the edit as a version.
    edit("game/props/lamp.model.json", R"("name": "bulb")", R"("name": "bub")");
    call(game, "asset.check", "{}");
    call(game, "test.run", "{}");
    call(game, "asset.add", R"({"path": "props/lamp.model.json"})");
    return 0;
}
