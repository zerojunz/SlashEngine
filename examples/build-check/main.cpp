// Checking a game's C++ code as it is written, with errors returned as data.
//
// build.check compiles the gameplay files that changed since the last check or build, and those
// including a header that changed, without linking or loading anything. It returns the files it
// compiled and each error and warning with its file, line, column, message, notes and the
// compiler's suggested fix as edits, so an agent can fix a typo without reading a build log.
// gameplay.build then only has to link the objects the check compiled.

#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/exporter/commands.hpp"
#include "slash/gameplay/host.hpp"
#include "slash/platform/file.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

namespace {

namespace command = slash::command;
namespace json = slash::json;

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

// Calls build.check and prints what it found. An error's message and notes are the compiler's
// own, worded differently by GCC and Clang, so this prints where each error is and its fix.
json::Value check(command::Session& session) {
    std::puts("build.check {}");
    const command::Reply reply = session.call("build.check", json::Object{});
    if (!reply.ok) {
        std::printf("error: %s\n", reply.message.c_str());
        std::exit(1);
    }
    json::Value shown = reply.value;
    if (json::Value* errors = shown.find("errors"); errors != nullptr) {
        for (json::Value& error : errors->as_array()) {
            error.as_object().erase("message");
            error.as_object().erase("notes");
        }
    }
    std::puts(json::write(shown, json::Style::Compact).c_str());
    return reply.value;
}

// Edits a file as an agent would.
void rewrite(const std::string& path, std::string_view from, std::string_view to) {
    std::string text = slash::platform::read_file(path).value();
    text.replace(text.find(from), from.size(), to);
    (void)slash::platform::write_file(path, text);
}

// Applies an edit of a fix: the bytes from column up to end_column of a line become text.
void apply(const std::string& path, const json::Value& edit) {
    std::string text = slash::platform::read_file(path).value();
    std::size_t start = 0;
    for (std::int64_t line = 1; line < edit.find("line")->as_int(); ++line) {
        start = text.find('\n', start) + 1;
    }
    const auto from = static_cast<std::size_t>(edit.find("column")->as_int() - 1);
    const auto to = static_cast<std::size_t>(edit.find("end_column")->as_int() - 1);
    text.replace(start + from, to - from, edit.find("text")->as_string());
    (void)slash::platform::write_file(path, text);
}

} // namespace

int main() {
    slash::world::ComponentRegistry components;
    slash::world::register_builtin_components(components);
    command::Registry commands;
    command::register_builtin_commands(commands);
    slash::exporter::register_export_commands(commands); // build.check and gameplay.build
    slash::gameplay::Host code(components, slash::gameplay::library_path("."));
    command::SessionOptions options;
    options.gameplay = &code;
    command::Session session(components, commands, options);

    // The first check compiles every file; the next has nothing to compile.
    check(session);
    check(session);

    // A header changed: each file including it is compiled again.
    rewrite("src/bank.hpp", "int interest(int coins);",
            "int interest(int coins); // a hundredth of them, rounded down");
    check(session);

    // Two mistakes in src/game.cpp: a misspelled field and a variable left unused. Only that
    // file is compiled. The error comes with the compiler's fix; the warning is reported too.
    rewrite("src/game.cpp", "wallet.income + bank", "wallet.incme + bank");
    rewrite("src/game.cpp", "SLASH_GAME_MODULE(game) {\n",
            "SLASH_GAME_MODULE(game) {\n    const int limit = 1000;\n");
    const json::Value found = check(session);

    // Apply each error's fix as the data says, remove the unused variable, and check again.
    for (const json::Value& error : found.find("errors")->as_array()) {
        for (const json::Value& edit : error.find("fix")->as_array()) {
            apply(error.find("file")->as_string(), edit);
        }
    }
    rewrite("src/game.cpp", "    const int limit = 1000;\n", "");
    check(session);

    // The build links what the checks compiled, and the code goes in.
    call(session, "gameplay.build", "{}");
    call(session, "scene.open", R"({"path": "scenes/main.scene.json"})");
    call(session, "step", R"({"ticks": 3})");
    call(session, "world.query", R"({"include": ["Wallet"]})");
    return 0;
}
