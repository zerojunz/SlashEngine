#pragma once

#include "slash/command/command.hpp"
#include "slash/core/json.hpp"
#include "slash/core/result.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/world.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
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

    // True while a dry run is running. Commands that write files, such as scene.save inside a
    // batch, check everything but write nothing.
    [[nodiscard]] bool dry_running() const noexcept { return dry_running_; }

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

    // A copy of the world's entities in scene-file form, with the prefabs they extend so it
    // restores without reading prefab files.
    struct SavedWorld {
        json::Value scene;
        world::PrefabLinks links;
    };
    [[nodiscard]] SavedWorld save() const;
    // Replaces the world's entities with a saved copy. Entity events are not queued; the
    // command's own event says what changed.
    void restore(const SavedWorld& saved);

    // Undo history. Each entry is labelled with the command that made it. Undo restores the
    // entities and their components; the tick counter keeps counting.
    Reply undo();
    Reply redo();
    // Oldest first: the last label is the next one undo reverts.
    [[nodiscard]] std::vector<std::string> undo_labels() const;
    // Next first: the first label is the next one redo applies.
    [[nodiscard]] std::vector<std::string> redo_labels() const;
    void clear_history();

    // Whether the world should advance in real time. Hosts that tick the world on their own clock,
    // such as slash-cli run --serve, register play and pause (register_run_commands) and step the
    // world only while this is true. Sessions start paused; such hosts call set_playing(true).
    [[nodiscard]] bool playing() const noexcept { return playing_; }
    void set_playing(bool playing) noexcept { playing_ = playing; }

    // Named copies of the world's entities, for world.snapshot, world.diff and world.restore.
    // Taking a snapshot under a name in use replaces it; a session holds at most kMaxSnapshots.
    static constexpr std::size_t kMaxSnapshots = 100;
    Result<void> take_snapshot(std::string name);
    // The snapshot with this name, or nullptr.
    [[nodiscard]] const SavedWorld* find_snapshot(std::string_view name) const;
    // Every snapshot name, sorted.
    [[nodiscard]] std::vector<std::string_view> snapshot_names() const;

    // Event subscriptions, for events.subscribe, events.read and events.unsubscribe. While one
    // wants them, the session queues entity.created and entity.destroyed, events gameplay code
    // emits, and a "command" event with the changes for each command that changed the world
    // (undo and redo included). A failed command or a dry run queues nothing.
    static constexpr std::size_t kMaxSubscriptions = 16;
    static constexpr std::size_t kMaxQueuedEvents = 1000;
    // Kinds and ids filter the events; empty lists let every one through. Returns the
    // subscription's name.
    Result<std::string> subscribe(std::vector<std::string> kinds, std::vector<std::string> ids);
    struct EventBatch {
        json::Array events;        // oldest first
        std::uint64_t dropped = 0; // events lost since the last read because the queue was full
        bool more = false;         // events are still queued
    };
    // Takes up to limit queued events. Null if no subscription has this name.
    [[nodiscard]] std::optional<EventBatch> read_events(std::string_view subscription,
                                                        std::size_t limit);
    // False if no subscription has this name.
    bool unsubscribe(std::string_view subscription);
    [[nodiscard]] std::vector<std::string_view> subscription_names() const;

    // The most recent captured log lines, oldest first. Only one session at a time should
    // capture logs, since the log sink is global.
    [[nodiscard]] std::vector<std::string> recent_logs(std::size_t count) const;

private:
    struct Snapshot {
        std::string label;
        SavedWorld saved;
    };

    struct Subscription {
        std::vector<std::string> kinds;
        std::vector<std::string> ids;
        std::deque<json::Value> events;
        std::uint64_t dropped = 0;
    };

    Reply run(const Command& command, const json::Value& params);
    void on_world_event(const world::Event& event);
    void publish(const world::Event& event);
    // Queues a command event if a subscription wants one.
    void publish_changes(std::string_view command, const SavedWorld& before,
                         const SavedWorld& after);
    [[nodiscard]] bool wants(std::string_view kind) const;
    void deliver(std::string_view kind, const std::vector<std::string>& ids, json::Value event);
    void remember(std::deque<Snapshot>& stack, Snapshot snapshot) const;

    const Registry* commands_;
    SessionOptions options_;
    world::World world_;
    std::string scene_path_;
    std::deque<Snapshot> undo_;
    std::deque<Snapshot> redo_;
    std::map<std::string, SavedWorld, std::less<>> snapshots_;
    int depth_ = 0;         // how many commands are running (more than one inside a batch)
    bool edited_ = false;   // an Edit command succeeded during the outermost call
    bool replaced_ = false; // a Replace command succeeded during the outermost call
    bool dry_running_ = false;
    bool playing_ = false;
    std::map<std::string, Subscription, std::less<>> subscriptions_;
    std::uint64_t next_subscription_ = 1;
    std::uint64_t next_event_ = 1;
    std::vector<world::Event> pending_events_; // from the command running now, kept if it succeeds
    bool buffering_events_ = false;
    bool muted_events_ = false;     // while restoring a snapshot, which recreates every entity
    mutable std::mutex logs_mutex_; // the log sink may run on any thread
    std::deque<std::string> logs_;
};

} // namespace slash::command
