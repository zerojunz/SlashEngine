#pragma once

#include "slash/core/json.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/core/result.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Commands are the only way agents, tools and (later) the debug overlay change a world. Each one
// has a dotted name such as "entity.create", a JSON Schema for its parameters and the
// capabilities it needs. The same registry backs JSON-RPC, MCP and the REST mirror, so they
// never drift apart.
namespace slash::command {

// What a caller is allowed to do. A session grants a set of these; each command lists the ones
// it needs.
enum class Capability : std::uint8_t {
    Read,       // inspect the world and the engine
    Edit,       // change the world
    Run,        // advance the simulation
    Filesystem, // read and write files in the project folder
    Build,      // compile and package the game, which runs compilers and other tools
};

[[nodiscard]] std::string_view to_string(Capability capability) noexcept;
[[nodiscard]] std::optional<Capability> capability_from_string(std::string_view name) noexcept;

// Parses a comma-separated list such as "read,edit". "all" grants every capability.
[[nodiscard]] Result<std::vector<Capability>> parse_capabilities(std::string_view list);

// How a command affects the world and the undo history.
enum class Effect : std::uint8_t {
    Read,     // changes nothing in the world
    Edit,     // changes the world; one undo step reverts it
    Replace,  // replaces the whole world, e.g. opening a scene; clears the undo history
    History,  // moves through the undo history (undo and redo)
    Compound, // runs other commands (batch); one undo step reverts all of them
};

// Edit, Replace and Compound commands change the world, so the session snapshots the world
// before running one and they take "dry_run".
[[nodiscard]] constexpr bool changes_world(Effect effect) noexcept {
    return effect == Effect::Edit || effect == Effect::Replace || effect == Effect::Compound;
}

// Error codes, shared with JSON-RPC 2.0. The -326xx codes are the protocol's own; the -320xx
// codes are SlashEngine's.
namespace code {
inline constexpr int kParseError = -32700;
inline constexpr int kInvalidRequest = -32600;
inline constexpr int kMethodNotFound = -32601;
inline constexpr int kInvalidParams = -32602;
inline constexpr int kInternalError = -32603;
inline constexpr int kNotFound = -32001;
inline constexpr int kInvalidArgument = -32002;
inline constexpr int kPermissionDenied = -32003;
inline constexpr int kPlatformFailure = -32004;
inline constexpr int kConflict = -32005; // the data changed since the caller read it
} // namespace code

// The outcome of a command: a JSON value, or an error code, message and optional structured
// details (for example the list of schema errors).
struct Reply {
    bool ok = true;
    json::Value value;
    int code = 0;
    std::string message;
    json::Value data;

    [[nodiscard]] static Reply success(json::Value value);
    [[nodiscard]] static Reply failure(int code, std::string message, json::Value data = {});
    // Maps an engine Error to the matching code.
    [[nodiscard]] static Reply failure(const Error& error);
    // An invalid-params error for schema errors. The message names the first error and its
    // JSON Pointer inside the params; data holds {"path": ...} for a single error, or
    // {"errors": [{"path", "message"}, ...]} for several.
    [[nodiscard]] static Reply invalid_params(const std::vector<json::ValidationError>& errors);
};

class Session;

using Handler = std::function<Reply(Session& session, const json::Value& params)>;

struct Command {
    std::string name;
    std::string description;
    std::vector<Capability> capabilities;
    Effect effect = Effect::Read;
    // JSON Schema for the params object. Params are validated against it before the handler runs.
    // The registry adds "dry_run" to it for commands that change the world; the session handles
    // that param, so the handler never sees it.
    json::Value params;
    Handler handler;
};

class Registry {
public:
    // Adds a command. Names must be unique, and params must be an object schema.
    void add(Command command);

    // The command with this name, or nullptr.
    [[nodiscard]] const Command* find(std::string_view name) const;

    // Every command, sorted by name.
    [[nodiscard]] std::span<const Command> commands() const noexcept { return commands_; }

private:
    std::vector<Command> commands_;
};

} // namespace slash::command
