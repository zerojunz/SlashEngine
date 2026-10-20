#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// C++ skeletons for an imported game's scripts, for the agent porting them to fill in. A script's
// variables become a component, its signals world events, and its functions the calls of a
// system that runs them when Godot would: on the entities made since the last tick, once a tick,
// for each input event and for each signal a scene connects to them. Each function keeps the
// script's code to port in its comments.
namespace slash::importer {

// How a scene's value for a script's variable converts to its field.
enum class FieldKind : std::uint8_t {
    Value,    // as written: numbers, strings, vectors, colors, lists and resource paths
    Entity,   // a node path, written as the ID of the entity it names
    Entities, // a list of node paths
};

// A variable of a script, as a field of its component.
struct ScriptField {
    std::string name;        // in C++
    std::string source_name; // as the script names it
    std::string type;        // in C++
    std::string init;        // its initializer, or "" for the type's default
    std::string comment;     // what the script declared, where the C++ doesn't say it
    std::string doc;         // the script's ## comment
    std::optional<double> min = std::nullopt; // @export_range's
    std::optional<double> max = std::nullopt;
    FieldKind kind = FieldKind::Value;
    bool exported = false; // scenes set it
};

struct ScriptParam {
    std::string name;   // in C++
    std::string type;   // in C++, as a parameter: strings, lists and JSON by const reference
    std::string source; // as written, such as "body: Node2D"
};

struct ScriptSignal {
    std::string name; // the kind of its events
    std::vector<ScriptParam> params;
    std::string declaration;
};

struct ScriptFunction {
    std::string name;        // in C++
    std::string source_name; // as the script names it
    std::vector<ScriptParam> params;
    std::string returns = "void";
    std::string declaration;       // as written
    std::vector<std::string> body; // its lines, one indent less, tabs as four spaces
    int line = 0;                  // where it starts, from 1
    bool is_static = false;
};

struct ScriptConstant {
    std::string name;        // in C++: kPascalCase
    std::string type;        // in C++, or "" when the value doesn't translate
    std::string value;       // in C++
    std::string declaration; // as written
};

struct ScriptEnum {
    std::string name;                // "" for an unnamed enum
    std::vector<std::string> values; // in C++, such as "kIdle" and "kJump = 5"
    std::string declaration;         // as written
};

// A scene's signal connected to a function of the script.
struct ScriptConnection {
    std::string scene;
    std::string signal;
    std::string from; // entity IDs
    std::string to;
    std::string method; // as the script names it
};

struct Script {
    std::string path; // the script's file, in the project the import writes
    std::string extends;
    std::string class_name;
    std::string doc; // its ## comments before the first declaration
    // It extends a node, so its component goes on entities and a system calls its functions;
    // otherwise (a Resource, a RefCounted) it is a reflected struct with functions.
    bool node = true;
    std::string component; // in C++
    std::string system;
    std::string ns;       // its namespace, imported::<ns>, and its file's name
    std::string task;     // the storyboard task to port it
    std::string autoload; // the name Godot loads it under, or ""
    std::vector<ScriptConstant> constants;
    std::vector<ScriptEnum> enums;
    std::vector<ScriptField> fields;
    std::vector<ScriptSignal> signals;
    std::vector<ScriptFunction> functions;
    std::vector<ScriptConnection> connections;
    std::vector<std::string> notes; // what the skeleton leaves out
};

struct GdscriptOptions {
    // The path a res:// path in preload() or load() has in the project the import writes.
    std::function<std::string(std::string_view)> resource_path;
};

// Reads a GDScript file's declarations: what it extends, its class name, constants, enums,
// variables, signals and functions. Names its component after the class name, or the file in
// PascalCase, and its namespace and system after the file in snake_case.
[[nodiscard]] Script read_gdscript(std::string_view path, std::string_view text,
                                   const GdscriptOptions& options = {});

// The C++ file of a script: its component (or struct), a helper that emits each signal, a stub
// per function with the script's code in comments, the system and imported::register_<ns>.
[[nodiscard]] std::string skeleton(const Script& script);

// The file defining imported::register_scripts, which registers the components and systems of
// the node scripts in these namespaces.
[[nodiscard]] std::string registration(const std::vector<std::string>& namespaces);

// A game module that calls imported::register_scripts, for a project whose code has none.
[[nodiscard]] std::string module_source();

// Whether the engine adds a system of this name, which a script's system can't then take.
[[nodiscard]] bool is_engine_system(std::string_view name);

// A name as a C++ identifier: other characters become '_', and keywords get a trailing '_'.
[[nodiscard]] std::string cpp_identifier(std::string_view name);

// "enemy_ai" -> "EnemyAi", "hud" -> "Hud", "HUDLabel" -> "HUDLabel".
[[nodiscard]] std::string pascal_case(std::string_view name);

} // namespace slash::importer
