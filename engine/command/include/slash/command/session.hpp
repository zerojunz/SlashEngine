#pragma once

#include "slash/command/command.hpp"
#include "slash/core/json.hpp"
#include "slash/core/result.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/world.hpp"

#include <cstddef>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace slash::command {

struct SessionOptions {
    // Folder that scene paths are relative to. Commands cannot reach files outside it.
    std::string project_root = ".";
    // What callers of this session may do.
    std::vector<Capability> capabilities = {Capability::Read, Capability::Edit, Capability::Run,
                                            Capability::Filesystem, Capability::Build};
    // How many edits undo remembers.
    std::size_t history_limit = 100;
    // Keep recent log lines for log.tail. The session installs a log sink that also forwards
    // every line to stderr, and restores the default sink when it is destroyed.
    bool capture_logs = true;
};

// One editing session: a world, the commands that act on it, undo history and recent logs.
// JSON-RPC, MCP and the REST mirror are thin adapters over Session::call.
class Session {
public:
    Session(const world::ComponentRegistry& component_registry, const Registry& command_registry,
            SessionOptions session_options = {});
    ~Session();

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    Session(Session&&) = delete;
    Session& operator=(Session&&) = delete;

    // Runs a command: checks capabilities, validates params against the command's schema, runs
    // it, and records it for undo if it changed the world. A failed command leaves the world as
    // it was. Null params count as an empty object. Commands may call other commands (batch
    // does); nested calls are recorded and rolled back as part of the outermost one.
    //
    // With "dry_run": true, an Edit, Replace or Compound command runs and is then rolled back. Its
    // reply is {"dry_run": true, "result": <its result>, "changes": <diff_scenes>}, or its error.
    Reply call(std::string_view name, const json::Value& params);

    [[nodiscard]] world::World& world() noexcept { return world_; }
    [[nodiscard]] const world::World& world() const noexcept { return world_; }
    [[nodiscard]] const world::ComponentRegistry& components() const noexcept {
        return world_.components();
    }
    [[nodiscard]] const Registry& commands() const noexcept { return *commands_; }
    [[nodiscard]] const SessionOptions& options() const noexcept { return options_; }
    [[nodiscard]] bool allowed(Capability capability) const;

    // Resolves a path inside the project folder. Rejects absolute paths and any ".." segment.
    [[nodiscard]] Result<std::string> resolve_path(std::string_view relative) const;

    // The project-relative path of the open scene, or "" for an unsaved one.
    [[nodiscard]] const std::string& scene_path() const noexcept { return scene_path_; }
    void set_scene_path(std::string path) { scene_path_ = std::move(path); }

    // Undo history. Each entry is labelled with the command that made it. Undo restores the
    // entities and their components; the tick counter keeps counting.
    Reply undo();
    Reply redo();
    // Oldest first: the last label is the next one undo reverts.
    [[nodiscard]] std::vector<std::string> undo_labels() const;
    // Next first: the first label is the next one redo applies.
    [[nodiscard]] std::vector<std::string> redo_labels() const;
    void clear_history();

    // Named copies of the world's entities, for world.snapshot, world.diff and world.restore.
    // Taking a snapshot under a name in use replaces it; a session holds at most kMaxSnapshots.
    static constexpr std::size_t kMaxSnapshots = 100;
    Result<void> take_snapshot(std::string name);
    // The snapshot with this name, in scene-file form, or nullptr.
    [[nodiscard]] const json::Value* find_snapshot(std::string_view name) const;
    // Every snapshot name, sorted.
    [[nodiscard]] std::vector<std::string_view> snapshot_names() const;

    // The most recent captured log lines, oldest first. Only one session at a time should
    // capture logs, since the log sink is global.
    [[nodiscard]] std::vector<std::string> recent_logs(std::size_t count) const;

private:
    struct Snapshot {
        std::string label;
        json::Value scene;
    };

    Reply run(const Command& command, const json::Value& params);
    void restore(const json::Value& scene);
    void remember(std::deque<Snapshot>& stack, Snapshot snapshot) const;

    const Registry* commands_;
    SessionOptions options_;
    world::World world_;
    std::string scene_path_;
    std::deque<Snapshot> undo_;
    std::deque<Snapshot> redo_;
    std::map<std::string, json::Value, std::less<>> snapshots_;
    int depth_ = 0;                 // how many commands are running (more than one inside a batch)
    bool edited_ = false;           // an Edit command succeeded during the outermost call
    bool replaced_ = false;         // a Replace command succeeded during the outermost call
    mutable std::mutex logs_mutex_; // the log sink may run on any thread
    std::deque<std::string> logs_;
};

} // namespace slash::command
