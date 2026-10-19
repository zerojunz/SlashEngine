// A game's own C++ code, changed while the game runs.
//
// gameplay.build compiles the project's gameplay sources (src/game.cpp) and swaps the new code
// into the world, which keeps its entities and each field the code still has. A mistake comes
// back as compiler errors with file and line, and the code that was in keeps running. slash-cli
// and slash-mcp serve gameplay.build the same way: an agent edits src/game.cpp, calls it, and
// carries on with the same world.

#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/exporter/commands.hpp"
#include "slash/gameplay/host.hpp"
#include "slash/platform/file.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <cstdio>
#include <string>
#include <string_view>

namespace {

namespace command = slash::command;
namespace json = slash::json;

constexpr const char* kSource = "src/game.cpp";

// Prints the call as an agent would make it, then the result or the error.
command::Reply call(command::Session& session, std::string_view name, std::string_view params) {
    const json::Value value = json::parse(params).value();
    std::printf("%.*s %s\n", static_cast<int>(name.size()), name.data(),
                json::write(value, json::Style::Compact).c_str());
    command::Reply reply = session.call(name, value);
    if (reply.ok) {
        std::puts(json::write(reply.value, json::Style::Compact).c_str());
    } else {
        std::printf("error: %s\n", reply.message.c_str());
    }
    return reply;
}

// Edits the game's code as an agent would.
void rewrite(std::string_view from, std::string_view to) {
    std::string text = slash::platform::read_file(kSource).value();
    text.replace(text.find(from), from.size(), to);
    (void)slash::platform::write_file(kSource, text);
}

} // namespace

int main() {
    slash::world::ComponentRegistry components;
    slash::world::register_builtin_components(components);
    command::Registry commands;
    command::register_builtin_commands(commands);
    slash::exporter::register_export_commands(commands); // gameplay.build and export.run
    // The project's code, from the library gameplay.build makes. Nothing is built yet.
    slash::gameplay::Host code(components, slash::gameplay::library_path("."));
    command::SessionOptions options;
    options.gameplay = &code;
    command::Session session(components, commands, options);

    // Build the code, then play the scene that uses its Wallet component.
    call(session, "gameplay.build", "{}");
    call(session, "scene.open", R"({"path": "scenes/main.scene.json"})");
    call(session, "step", R"({"ticks": 60})");
    call(session, "world.query", R"({"include": ["Wallet"]})");

    // New code: wallets also earn a bonus each tick. The coins earned so far stay.
    rewrite("    int income = 1; // coins a tick\n",
            "    int income = 1; // coins a tick\n    int bonus = 10;\n");
    rewrite("SLASH_COMPONENT(Wallet, coins, income)",
            "SLASH_COMPONENT(Wallet, coins, income, bonus)");
    rewrite("wallet.coins += wallet.income;", "wallet.coins += wallet.income + wallet.bonus;");
    call(session, "gameplay.build", "{}");
    call(session, "step", R"({"ticks": 60})");
    call(session, "world.query", R"({"include": ["Wallet"]})");

    // A mistake: a misspelled field. The error names the file and line; its message is the
    // compiler's own.
    rewrite("wallet.income + wallet.bonus;", "wallet.income + wallet.bonsu;");
    const command::Reply failed = call(session, "gameplay.build", "{}");
    for (const json::Value& error : failed.data.find("errors")->as_array()) {
        std::printf("  %s:%s\n", error.find("file")->as_string().c_str(),
                    std::to_string(error.find("line")->as_int()).c_str());
    }
    // The code that was in keeps running.
    call(session, "step", R"({"ticks": 1})");
    call(session, "world.query", R"({"include": ["Wallet"]})");
    return 0;
}
