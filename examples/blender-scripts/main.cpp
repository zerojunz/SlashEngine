// A Blender bridge: agents build models by running Blender headless with a Python script, as a
// generator of the asset library. The script and its parameters are kept with the model, so it
// can be built again with other parameters, or another script, later.
//
// game/tools/shelf.py builds a bookshelf in Blender from its params. asset.generate runs it with
// the built-in "blender" generator, a version twice as wide comes from asset.refine with one
// parameter changed, and asking for the first again reuses it with no Blender run. A session
// without the build capability, such as one serving a game, may not run Blender.
//
// The tests run Blender's stand-in (tests/fake_blender.cpp), which can't run Python: it stands in
// for any script with a box the params' width, height and depth in size. With Blender installed,
// the same calls build the shelf itself.

#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/library/commands.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <algorithm>
#include <cstdio>
#include <string>
#include <string_view>

namespace {

namespace command = slash::command;
namespace json = slash::json;

// Prints the call as an agent would make it, then the reply unless it is left out.
json::Value call(command::Session& session, std::string_view name, std::string_view params,
                 bool show = true) {
    const json::Value value = json::parse(params).value();
    std::printf("%.*s %s\n", static_cast<int>(name.size()), name.data(),
                json::write(value, json::Style::Compact).c_str());
    const command::Reply reply = session.call(name, value);
    const json::Value shown = reply.ok ? reply.value : json::Value(reply.message);
    if (show) {
        std::puts(json::write(shown, json::Style::Compact).c_str());
    }
    return shown;
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

    const json::Value shelf = call(game, "asset.generate", R"({"type": "model",
        "prompt": "a wooden bookshelf", "script": "tools/shelf.py", "tags": ["furniture"],
        "parameters": {"name": "Shelf", "width": 0.8, "depth": 0.3, "height": 1.8, "boards": 5},
        "path": "props/shelf.glb"})");
    const std::string id = shelf.find("id")->as_string();
    // The record keeps the script's text, not its path, so the asset can be made again anywhere.
    const json::Value info = call(game, "asset.info", R"({"id": ")" + id + R"("})", false);
    std::printf("generator %s, model %s, script: %s...\n",
                info.find("generator")->as_string().c_str(),
                info.find("model")->as_string().c_str(),
                info.find("request")->find("script")->as_string().substr(0, 47).c_str());

    // Twice as wide: the script it was made with runs again with the new width.
    const json::Value wide = call(game, "asset.refine",
                                  R"({"id": ")" + id +
                                      R"(", "parameters": {"width": 1.6},
        "path": "props/wide_shelf.glb"})");
    call(game, "model.inspect", R"({"path": "props/wide_shelf.glb", "brief": true})");

    // The same script and parameters are the same asset; Blender doesn't run.
    call(game, "asset.generate", R"({"type": "model", "prompt": "a wooden bookshelf",
        "script": "tools/shelf.py",
        "parameters": {"name": "Shelf", "width": 0.8, "depth": 0.3, "height": 1.8, "boards": 5}})");

    // A session that may not build, as one serving a running game, can't run Blender.
    std::erase(options.capabilities, command::Capability::Build);
    command::Session served(components, commands, options);
    call(served, "asset.generate", R"({"type": "model", "prompt": "a low bookshelf",
        "script": "tools/shelf.py",
        "parameters": {"name": "Shelf", "width": 0.8, "depth": 0.3, "height": 0.9, "boards": 3}})");
    return 0;
}
