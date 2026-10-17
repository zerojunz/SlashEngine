#pragma once

#include "slash/asset/assets.hpp"
#include "slash/command/command.hpp"
#include "slash/core/json.hpp"
#include "slash/core/result.hpp"
#include "slash/platform/file.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/tree2d.hpp"
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
    // Whether commands may write files. Sessions that run tests don't, so a test leaves the
    // project as it was: a command that would write a file fails instead.
    bool write_files = true;
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

    // Called after each outermost command that succeeded and was not a dry run, with its name,
    // its params and the tick it started at, for recording replays; and with "asset.reload"
    // when poll_assets changed the world. An empty listener stops.
    using CallListener = std::function<void(std::string_view command, const json::Value& params,
                                            std::uint64_t tick)>;
    void set_call_listener(CallListener listener) { call_listener_ = std::move(listener); }
    // How many commands are running: 0 between commands, 1 inside one, more inside a batch.
    // What the world does while it is above 0 is a command's doing.
    [[nodiscard]] int depth() const noexcept { return depth_; }
    // Counts what may have changed the world from outside its systems: each outermost call of a
    // command that isn't a Read, as it starts, and each poll that reloaded a file. Code that
    // keeps state about the world can tell from it whether anything else ran since.
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }

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
    // Writes a file at a path resolve_path gave, creating its folder if needed, unless the
    // session doesn't write files (SessionOptions::write_files).
    [[nodiscard]] Result<void> write_file(const std::string& full, std::string_view contents) const;

    // Assets loaded from the project folder, and the prefab files the world reads through them.
    [[nodiscard]] asset::Assets& assets() noexcept { return assets_; }
    [[nodiscard]] const asset::Assets& assets() const noexcept { return assets_; }
    // Hot reload: reloads assets whose files changed and updates the instances of changed prefabs,
    // keeping their overrides. Queues an "asset.reloaded" event per changed file and a "command"
    // event named "asset.reload" with the changes to the world. It is not an undo step. Every
    // command polls first; hosts that tick the world also poll between ticks. A changed
    // project.json applies its window, rendering, physics, input and audio settings again, so
    // what project.import or an edit writes there takes effect at once.
    void poll_assets();

    // Project files that commands change, such as the storyboard. A command's changes are staged
    // and written together when the outermost command succeeds, so a failed command or a dry run
    // writes nothing, and later commands in a batch read the staged text. If someone else
    // changed a file since the command read it, nothing is written and the command fails.
    //
    // The file's text, or nullopt if it does not exist.
    [[nodiscard]] Result<std::optional<std::string>> read_project_file(std::string_view path);
    // Replaces a file's text. Only commands that change the world (Edit and Compound effects)
    // and undo and redo can stage files.
    Result<void> stage_file(std::string_view path, std::string text);
    // Appends text to an append-only log, such as the storyboard journal. Undo leaves logs alone.
    Result<void> stage_append(std::string_view path, std::string_view text);

    // Undo for changes outside the world, like Godot's UndoRedo: a command that changes files
    // records the call that reverses it and the call that makes it again. Undo and redo run those
    // calls as one staged change; if one fails, for example because the file changed since,
    // nothing changes and undo reports why.
    void add_undo_call(std::string command, json::Value undo_params, json::Value redo_params);

    // Who is calling, for records such as the storyboard journal: the MCP client's name, or "".
    [[nodiscard]] const std::string& client() const noexcept { return client_; }
    void set_client(std::string client) { client_ = std::move(client); }

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
    struct UndoCall {
        std::string command;
        json::Value undo;
        json::Value redo;
    };

    struct Snapshot {
        std::string label;
        SavedWorld saved;
        std::vector<UndoCall> calls;
        bool restores_world = true; // false when the step changed only files
    };

    struct StagedFile {
        std::optional<std::string> seen; // on disk when the command first read it
        std::optional<std::string> text; // what to write, if the command replaced it
        std::string append;              // what to append
    };

    struct Subscription {
        std::vector<std::string> kinds;
        std::vector<std::string> ids;
        std::deque<json::Value> events;
        std::uint64_t dropped = 0;
    };

    Reply call_command(std::string_view name, const json::Value& params);
    Reply run(const Command& command, const json::Value& params);
    // The reply, or a failure if the world's Transform2D children have a problem now that
    // wasn't in before.
    [[nodiscard]] Reply keeps_tree(const std::vector<world::TreeProblem>& before,
                                   Reply reply) const;
    void on_world_event(const world::Event& event);
    void publish(const world::Event& event);
    // Queues a command event if a subscription wants one.
    void publish_changes(std::string_view command, const SavedWorld& before,
                         const SavedWorld& after);
    [[nodiscard]] bool wants(std::string_view kind) const;
    void deliver(std::string_view kind, const std::vector<std::string>& ids, json::Value event);
    void remember(std::deque<Snapshot>& stack, Snapshot snapshot) const;
    // Stages files until commit_files or discard_files.
    void begin_files();
    Result<void> commit_files();
    void discard_files();
    Result<StagedFile*> staged(std::string_view path);
    [[nodiscard]] Result<std::optional<std::string>> read_disk(std::string_view path) const;
    // Runs a step's undo calls (or its redo calls) as one staged change.
    Reply replay(const Snapshot& step, bool undo);

    // Applies project.json's settings to the world if the file changed since they last were.
    void poll_project();

    const Registry* commands_;
    SessionOptions options_;
    std::optional<platform::FileStamp> project_stamp_; // project.json's when last applied
    asset::Assets assets_;
    world::World world_;
    std::string scene_path_;
    std::deque<Snapshot> undo_;
    std::deque<Snapshot> redo_;
    std::map<std::string, SavedWorld, std::less<>> snapshots_;
    int depth_ = 0; // how many commands are running (more than one inside a batch)
    std::uint64_t revision_ = 0;
    bool edited_ = false;   // an Edit command succeeded during the outermost call
    bool replaced_ = false; // a Replace command succeeded during the outermost call
    bool dry_running_ = false;
    bool playing_ = false;
    std::map<std::string, Subscription, std::less<>> subscriptions_;
    std::uint64_t next_subscription_ = 1;
    std::uint64_t next_event_ = 1;
    std::vector<world::Event> pending_events_; // from the command running now, kept if it succeeds
    bool buffering_events_ = false;
    bool muted_events_ = false; // while restoring a snapshot, which recreates every entity
    std::map<std::string, StagedFile, std::less<>> staged_;
    bool staging_ = false;
    std::vector<UndoCall> undo_calls_; // from the command running now
    bool replaying_ = false;           // undo or redo is running a step's calls
    std::string client_;
    CallListener call_listener_;
    mutable std::mutex logs_mutex_; // the log sink may run on any thread
    std::deque<std::string> logs_;
};

} // namespace slash::command
