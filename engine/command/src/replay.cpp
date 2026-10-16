#include "slash/command/replay.hpp"

#include "selection.hpp"

#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/core/math.hpp"
#include "slash/input/input.hpp"
#include "slash/platform/file.hpp"
#include "slash/ui/ui.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/scene.hpp"
#include "slash/world/world.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace slash::command {
namespace {

constexpr std::size_t kHashDigits = 16;

// Commands a recording leaves out: they drive the host or the recording rather than the world,
// change only project files, or run tests in sessions of their own.
constexpr std::array<std::string_view, 9> kUnrecorded = {
    "replay.record", "replay.stop", "replay.run",      "replay.seek", "play",
    "pause",         "display.set", "storyboard.edit", "test.run"};

bool unrecorded(std::string_view name) {
    return std::ranges::find(kUnrecorded, name) != kUnrecorded.end();
}

// FNV-1a 64 over the compact text of a world's entities, as 16 hex digits.
std::string world_hash(const json::Value& scene) {
    constexpr std::uint64_t kOffset = 14695981039346656037ULL;
    constexpr std::uint64_t kPrime = 1099511628211ULL;
    std::uint64_t hash = kOffset;
    for (const char c : json::write(scene, json::Style::Compact)) {
        hash ^= static_cast<unsigned char>(c);
        hash *= kPrime;
    }
    constexpr std::string_view kDigits = "0123456789abcdef";
    std::string out(kHashDigits, '0');
    for (std::size_t i = kHashDigits; i-- > 0;) {
        out[i] = kDigits[hash & 0xFU];
        hash >>= 4U;
    }
    return out;
}

bool is_hash(std::string_view text) {
    return text.size() == kHashDigits && std::ranges::all_of(text, [](char c) {
               return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
           });
}

json::Value vec_json(Vec2 v) {
    return json::Array{v.x, v.y};
}

Vec2 vec_of(const json::Value& value) {
    const json::Array& items = value.as_array();
    return {static_cast<float>(items[0].as_double()), static_cast<float>(items[1].as_double())};
}

std::uint64_t tick_of(const json::Value& value, std::string_view key = "tick") {
    const json::Value* found = value.find(key);
    return found != nullptr ? static_cast<std::uint64_t>(found->as_int()) : 0;
}

// ---- Recording ----

// The ticks the host stepped from outside commands, in runs of the same length.
struct Run {
    std::uint64_t tick = 0;
    std::uint64_t count = 0;
    double seconds = 0.0;
};

struct Check {
    std::uint64_t tick = 0;
    std::string hash;
};

struct Recording {
    std::string path;
    std::string scene;
    std::uint64_t start_tick = 0;
    std::uint64_t next_id = 1;
    Vec2 start_viewport;
    Vec2 viewport; // as last recorded
    json::Value start;
    json::Object prefabs;
    std::vector<Run> runs;
    json::Array timeline;
    std::vector<Check> checks;
    std::size_t commands = 0;
    std::size_t inputs = 0;
};

// In the world's context, so the recording lives as long as the world it listens to.
struct RecordingHolder {
    std::unique_ptr<Recording> active;
};

Recording* active_recording(const world::World& world) {
    const auto* holder = world.ecs().ctx().find<RecordingHolder>();
    return holder != nullptr ? holder->active.get() : nullptr;
}

// Keeps a copy of each prefab the entities extend. One whose file changed since the recording
// first kept it goes in the entry's own "prefabs".
void keep_prefabs(Recording& recording, const world::PrefabLinks& links, json::Value& entry) {
    for (const auto& [id, link] : links) {
        if (link.base == nullptr) {
            continue;
        }
        const auto kept = recording.prefabs.find(link.path);
        if (kept == recording.prefabs.end()) {
            recording.prefabs.emplace(link.path, *link.base);
        } else if (kept->second != *link.base) {
            entry["prefabs"][link.path] = *link.base;
        }
    }
}

// The host sets the viewport to the window's size, which decides where clicks land.
void note_viewport(Recording& recording, const world::World& world, std::uint64_t tick) {
    const Vec2 viewport = ui::settings(world).viewport;
    if (viewport == recording.viewport) {
        return;
    }
    recording.viewport = viewport;
    json::Value entry;
    entry["tick"] = tick;
    entry["viewport"] = vec_json(viewport);
    recording.timeline.push_back(std::move(entry));
}

enum class Kept : std::uint8_t {
    No,      // left out
    Command, // run again
    World,   // what it made is put back
};

Kept kept_as(const Session& session, std::string_view name, const json::Value& params) {
    if (name == "asset.reload") {
        return Kept::World; // the files may have changed again
    }
    const Command* command = session.commands().find(name);
    if (command == nullptr || unrecorded(name)) {
        return Kept::No;
    }
    // Undo, redo and restoring a snapshot depend on the session's history; opening a scene
    // depends on its file.
    if (command->effect == Effect::History || command->effect == Effect::Replace ||
        name == "world.restore") {
        return Kept::World;
    }
    if (command->effect == Effect::Compound) {
        const json::Value* commands = params.find("commands");
        if (commands != nullptr && commands->is_array()) {
            for (const json::Value& item : commands->as_array()) {
                const json::Value* nested = item.find("command");
                if (nested == nullptr || !nested->is_string()) {
                    continue;
                }
                const json::Value* nested_params = item.find("params");
                if (unrecorded(nested->as_string()) ||
                    kept_as(session, nested->as_string(),
                            nested_params != nullptr ? *nested_params : json::Value()) ==
                        Kept::World) {
                    return Kept::World;
                }
            }
        }
        return Kept::Command;
    }
    const bool runs =
        std::ranges::find(command->capabilities, Capability::Run) != command->capabilities.end();
    return changes_world(command->effect) || runs ? Kept::Command : Kept::No;
}

void listen(Session& session, Recording& recording) {
    world::World& world = session.world();
    world.set_step_listener([&session, &recording](world::World& stepped, double seconds) {
        if (session.depth() > 0) {
            return; // the command is recorded instead
        }
        const std::uint64_t tick = stepped.tick();
        note_viewport(recording, stepped, tick);
        if (tick > recording.start_tick && (tick - recording.start_tick) % kReplayCheckTicks == 0) {
            recording.checks.push_back({tick, world_hash(world::save_scene(stepped))});
        }
        if (!recording.runs.empty()) {
            Run& last = recording.runs.back();
            if (last.tick + last.count == tick && last.seconds == seconds) {
                ++last.count;
                return;
            }
        }
        recording.runs.push_back({tick, 1, seconds});
    });
    input::set_recorder(world, [&session, &recording](const input::Recorded& input) {
        if (session.depth() > 0) {
            return; // input.inject is recorded instead
        }
        const std::uint64_t tick = session.world().tick();
        note_viewport(recording, session.world(), tick);
        json::Value event;
        if (input.release_all) {
            event["type"] = "release_all";
        } else {
            event = input::event_json(input.event, input::Use::Event);
        }
        ++recording.inputs;
        if (!recording.timeline.empty()) {
            json::Value& last = recording.timeline.back();
            if (json::Value* events = last.find("input");
                events != nullptr && tick_of(last) == tick) {
                events->push_back(std::move(event));
                return;
            }
        }
        json::Value entry;
        entry["tick"] = tick;
        entry["input"] = json::Array{std::move(event)};
        recording.timeline.push_back(std::move(entry));
    });
    session.set_call_listener([&session, &recording](std::string_view name,
                                                     const json::Value& params,
                                                     std::uint64_t tick) {
        const Kept kept = kept_as(session, name, params);
        if (kept == Kept::No) {
            return;
        }
        note_viewport(recording, session.world(), tick);
        json::Value entry;
        entry["tick"] = tick;
        entry["command"] = name;
        if (kept == Kept::Command) {
            json::Value args = params;
            if (args.is_object()) {
                args.as_object().erase("dry_run");
            }
            entry["params"] = std::move(args);
            entry["hash"] = world_hash(world::save_scene(session.world()));
        } else {
            const Session::SavedWorld saved = session.save();
            const world::World::Counters counters = session.world().counters();
            entry["world"] = saved.scene;
            entry["next_id"] = counters.next_generated_id;
            if (counters.tick != tick) {
                entry["end_tick"] = counters.tick;
            }
            if (const Command* command = session.commands().find(name);
                command != nullptr && command->effect == Effect::Replace) {
                entry["path"] = session.scene_path();
            }
            keep_prefabs(recording, saved.links, entry);
        }
        ++recording.commands;
        recording.timeline.push_back(std::move(entry));
    });
}

void stop_listening(Session& session) {
    session.world().set_step_listener({});
    input::set_recorder(session.world(), {});
    session.set_call_listener({});
}

json::Value replay_json(Session& session, Recording& recording) {
    json::Value doc;
    doc["format"] = kReplayFormat;
    doc["scene"] = recording.scene;
    json::Value& start = doc["start"];
    start["tick"] = recording.start_tick;
    start["next_id"] = recording.next_id;
    start["viewport"] = vec_json(recording.start_viewport);
    start["world"] = recording.start;
    json::Array runs;
    for (const Run& run : recording.runs) {
        json::Value item;
        item["tick"] = run.tick;
        item["count"] = run.count;
        item["seconds"] = run.seconds;
        runs.push_back(std::move(item));
    }
    doc["steps"] = std::move(runs);
    doc["timeline"] = recording.timeline;
    json::Array checks;
    for (const Check& check : recording.checks) {
        json::Value item;
        item["tick"] = check.tick;
        item["hash"] = check.hash;
        checks.push_back(std::move(item));
    }
    doc["checks"] = std::move(checks);
    const Session::SavedWorld saved = session.save();
    json::Value& end = doc["end"];
    end["tick"] = session.world().tick();
    end["world"] = saved.scene;
    keep_prefabs(recording, saved.links, end);
    doc["prefabs"] = recording.prefabs;
    return doc;
}

Result<std::string> replay_file(const Session& session, std::string_view path) {
    if (!path.ends_with(kReplaySuffix) || path.size() == kReplaySuffix.size()) {
        return make_error(ErrorCode::InvalidArgument,
                          R"(replay files end in ".replay.json", e.g. "replays/run.replay.json")");
    }
    return session.resolve_path(path);
}

// ---- Schema ----

json::Value typed(std::string_view type, std::string_view description) {
    json::Value schema;
    schema["type"] = type;
    schema["description"] = description;
    return schema;
}

json::Value tick_schema(std::string_view description) {
    json::Value schema = typed("integer", description);
    schema["minimum"] = 0;
    return schema;
}

json::Value viewport_schema(std::string_view description) {
    json::Value schema = typed("array", description);
    schema["items"]["type"] = "number";
    schema["items"]["exclusiveMinimum"] = 0;
    schema["minItems"] = 2;
    schema["maxItems"] = 2;
    return schema;
}

json::Value hash_schema(std::string_view description) {
    json::Value schema = typed("string", description);
    schema["minLength"] = static_cast<std::int64_t>(kHashDigits);
    schema["maxLength"] = static_cast<std::int64_t>(kHashDigits);
    return schema;
}

json::Value object_schema(json::Object properties, json::Array required) {
    json::Value schema;
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["properties"] = std::move(properties);
    if (!required.empty()) {
        schema["required"] = std::move(required);
    }
    return schema;
}

json::Value prefabs_schema(std::string_view description) {
    json::Value schema = typed("object", description);
    schema["additionalProperties"]["type"] = "object";
    return schema;
}

// ---- Validation ----

void add_error(std::vector<json::ValidationError>& errors, std::string path, std::string message) {
    errors.push_back({std::move(path), std::move(message)});
}

std::string at(std::string_view base, std::size_t index, std::string_view key = {}) {
    std::string path = std::string(base) + "/" + std::to_string(index);
    if (!key.empty()) {
        path += "/";
        path += key;
    }
    return path;
}

void validate_timeline(const json::Array& timeline, std::uint64_t start, std::uint64_t end,
                       std::vector<json::ValidationError>& errors) {
    std::uint64_t previous = start;
    for (std::size_t i = 0; i < timeline.size(); ++i) {
        const json::Value& entry = timeline[i];
        const std::uint64_t tick = tick_of(entry);
        if (tick < previous) {
            add_error(errors, at("/timeline", i, "tick"),
                      "comes before tick " + std::to_string(previous) +
                          ", where the run already was");
        } else if (tick > end) {
            add_error(errors, at("/timeline", i, "tick"),
                      "is after the end tick " + std::to_string(end));
        }
        previous = std::max(previous, tick);
        const json::Value* input = entry.find("input");
        const json::Value* command = entry.find("command");
        const json::Value* viewport = entry.find("viewport");
        const int kinds = static_cast<int>(input != nullptr) +
                          static_cast<int>(command != nullptr) +
                          static_cast<int>(viewport != nullptr);
        if (kinds != 1) {
            add_error(errors, at("/timeline", i), "an entry has one of input, command or viewport");
            continue;
        }
        const bool params = entry.find("params") != nullptr;
        const bool world = entry.find("world") != nullptr;
        if (command == nullptr) {
            for (const std::string_view key :
                 {"params", "hash", "world", "next_id", "end_tick", "path", "prefabs"}) {
                if (entry.find(key) != nullptr) {
                    add_error(errors, at("/timeline", i, key), "only a command entry has this");
                }
            }
        } else if (params == world) {
            add_error(errors, at("/timeline", i),
                      "a command entry has params, to run it again, or world, to put back what it "
                      "made");
        } else if (params) {
            for (const std::string_view key : {"next_id", "end_tick", "path", "prefabs"}) {
                if (entry.find(key) != nullptr) {
                    add_error(errors, at("/timeline", i, key),
                              "only a command entry with world has this");
                }
            }
        } else if (entry.find("hash") != nullptr) {
            add_error(errors, at("/timeline", i, "hash"),
                      "only a command entry with params has this");
        }
        if (const json::Value* hash = entry.find("hash");
            hash != nullptr && !is_hash(hash->as_string())) {
            add_error(errors, at("/timeline", i, "hash"), "is 16 lowercase hex digits");
        }
        if (const json::Value* end_tick = entry.find("end_tick"); end_tick != nullptr) {
            const auto value = static_cast<std::uint64_t>(end_tick->as_int());
            if (value < tick || value > end) {
                add_error(errors, at("/timeline", i, "end_tick"),
                          "is outside ticks " + std::to_string(tick) + " to " +
                              std::to_string(end));
            } else {
                previous = std::max(previous, value);
            }
        }
        if (input == nullptr) {
            continue;
        }
        const json::Array& events = input->as_array();
        for (std::size_t e = 0; e < events.size(); ++e) {
            const json::Value& event = events[e];
            const std::string path = at(at("/timeline", i, "input"), e);
            const json::Value* type = event.find("type");
            if (type != nullptr && type->is_string() && type->as_string() == "release_all") {
                if (event.size() != 1) {
                    add_error(errors, path, "a release_all event has only its type");
                }
                continue;
            }
            for (json::ValidationError& error :
                 input::validate_event(event, input::Use::Event, path)) {
                errors.push_back(std::move(error));
            }
        }
    }
}

// ---- Playback ----

using SharedPrefabs = std::map<std::string, std::shared_ptr<const json::Value>, std::less<>>;

// The prefab each entity of a recorded world extends, from the replay's copies.
Result<world::PrefabLinks> links_for(const json::Value& scene, const json::Value* own,
                                     const SharedPrefabs& prefabs, std::string_view where) {
    world::PrefabLinks links;
    const json::Value* entities = scene.find("entities");
    if (entities == nullptr || !entities->is_object()) {
        return links;
    }
    for (const auto& [id, entity] : entities->as_object()) {
        const json::Value* extends = entity.find("extends");
        if (extends == nullptr || !extends->is_string()) {
            continue;
        }
        const std::string& path = extends->as_string();
        std::shared_ptr<const json::Value> base;
        if (const json::Value* copy = own != nullptr ? own->find(path) : nullptr; copy != nullptr) {
            base = std::make_shared<const json::Value>(*copy);
        } else if (const auto found = prefabs.find(path); found != prefabs.end()) {
            base = found->second;
        } else {
            return make_error(ErrorCode::InvalidArgument,
                              std::string(where) + "/entities/" + json::pointer_escape(id) +
                                  ": the replay has no copy of " + path);
        }
        links.emplace(id, world::PrefabLink{path, std::move(base)});
    }
    return links;
}

// Puts back a world the replay holds.
Result<void> put_back(Session& session, const json::Value& holder, const SharedPrefabs& prefabs,
                      std::string_view where) {
    const json::Value& scene = *holder.find("world");
    auto links = links_for(scene, holder.find("prefabs"), prefabs, std::string(where) + "/world");
    if (!links) {
        return links.error();
    }
    world::World& world = session.world();
    world.clear();
    if (auto restored = world::restore_scene(world, scene, links.value()); !restored) {
        return make_error(ErrorCode::InvalidArgument,
                          std::string(where) + "/world: " + restored.error().message);
    }
    return {};
}

std::string text_param(const json::Value& params, std::string_view key) {
    const json::Value* value = params.find(key);
    return value != nullptr && value->is_string() ? value->as_string() : std::string();
}

Reply param_failure(int error_code, std::string_view path, const std::string& message) {
    json::Value data;
    data["path"] = path;
    return Reply::failure(error_code, std::string(path) + ": " + message, std::move(data));
}

Reply nested(std::string_view name) {
    return Reply::failure(code::kInvalidArgument, std::string(name) + " can't run inside a batch");
}

// Reads a replay file named by params' path.
Result<std::string> read_replay(const Session& session, const std::string& path, Reply& failure) {
    auto file = replay_file(session, path);
    if (!file) {
        failure = param_failure(Reply::failure(file.error()).code, "/path", file.error().message);
        return file.error();
    }
    auto contents = platform::read_file(file.value());
    if (!contents) {
        failure =
            param_failure(Reply::failure(contents.error()).code, "/path", contents.error().message);
        return contents.error();
    }
    return std::move(contents).value();
}

// Parses and checks a replay file's text.
Result<json::Value> parse_replay(const std::string& path, const std::string& text, Reply& failure) {
    auto doc = json::parse(text);
    if (!doc) {
        failure = param_failure(code::kInvalidArgument, "/path",
                                path + " is not valid JSON: " + doc.error().message);
        return doc.error();
    }
    const auto errors = validate_replay(doc.value());
    if (!errors.empty()) {
        std::string message =
            path + " is not a valid replay: " +
            (errors.front().path.empty() ? std::string("(root)") : errors.front().path) + ": " +
            errors.front().message;
        if (errors.size() > 1) {
            message += " (and " + std::to_string(errors.size() - 1) + " more errors)";
        }
        json::Value data;
        data["file"] = path;
        json::Array items;
        for (const json::ValidationError& error : errors) {
            json::Value item;
            item["path"] = error.path;
            item["message"] = error.message;
            items.push_back(std::move(item));
        }
        data["file_errors"] = std::move(items);
        failure = Reply::failure(code::kInvalidArgument, message, std::move(data));
        return make_error(ErrorCode::InvalidArgument, message);
    }
    return std::move(doc).value();
}

// A replay being played back. The session keeps the last one between calls, so replay.seek can
// go on from where it stopped.
struct Playback {
    std::string path;
    std::string text; // the file as read, to notice when it changes
    json::Value replay;
    SharedPrefabs prefabs;
    std::vector<Run> runs;
    std::vector<Check> checks;
    std::uint64_t first = 0;
    std::uint64_t last = 0;

    // How far it got.
    std::size_t next_entry = 0;
    std::size_t next_run = 0;
    std::size_t next_check = 0;
    bool going = true;  // false once a command failed or the ticks went another way
    bool ended = false; // compared with the recording's end
    std::optional<std::uint64_t> diverged;
    json::Value failed;
    json::Value changes; // what differs at the end

    // The session as the playback left it, to tell whether anything else ran since.
    bool intact = true; // false after a dry run, which put back the components but not the rest
    std::uint64_t tick = 0;
    std::uint64_t revision = 0;
    std::size_t queued = 0;
    Vec2 viewport;

    void differ(std::uint64_t at) {
        if (!diverged) {
            diverged = at;
        }
    }
};

// In the world's context, beside the world it plays.
struct PlaybackHolder {
    std::unique_ptr<Playback> kept;
};

Playback* kept_playback(world::World& world) {
    auto* holder = world.ecs().ctx().find<PlaybackHolder>();
    return holder != nullptr ? holder->kept.get() : nullptr;
}

std::unique_ptr<Playback> take_playback(world::World& world) {
    auto* holder = world.ecs().ctx().find<PlaybackHolder>();
    return holder != nullptr ? std::move(holder->kept) : nullptr;
}

void keep_playback(Session& session, std::unique_ptr<Playback> playback) {
    world::World& world = session.world();
    playback->intact = true;
    playback->tick = world.tick();
    playback->revision = session.revision();
    playback->queued = input::queued(world);
    playback->viewport = ui::settings(world).viewport;
    auto& holder = world.ecs().ctx().contains<PlaybackHolder>()
                       ? world.ecs().ctx().get<PlaybackHolder>()
                       : world.ecs().ctx().emplace<PlaybackHolder>();
    holder.kept = std::move(playback);
}

// Whether the world is as the playback left it: nothing else has run since (the call running
// now counts once), and the host has neither stepped, queued input nor resized.
bool resumable(const Session& session, const Playback* playback) {
    const world::World& world = session.world();
    return playback != nullptr && playback->intact && world.tick() == playback->tick &&
           session.revision() == playback->revision + 1 &&
           input::queued(world) == playback->queued &&
           ui::settings(world).viewport == playback->viewport;
}

// The playback of a replay file's text.
Result<std::unique_ptr<Playback>> new_playback(const std::string& path, std::string text,
                                               Reply& failure) {
    auto replay = parse_replay(path, text, failure);
    if (!replay) {
        return replay.error();
    }
    auto playback = std::make_unique<Playback>();
    playback->path = path;
    playback->text = std::move(text);
    playback->replay = std::move(replay).value();
    const json::Value& doc = playback->replay;
    if (const json::Value* copies = doc.find("prefabs"); copies != nullptr) {
        for (const auto& [prefab, components] : copies->as_object()) {
            playback->prefabs.emplace(prefab, std::make_shared<const json::Value>(components));
        }
    }
    for (const json::Value& item : doc.find("steps")->as_array()) {
        playback->runs.push_back(
            {tick_of(item), tick_of(item, "count"), item.find("seconds")->as_double()});
    }
    for (const json::Value& item : doc.find("checks")->as_array()) {
        playback->checks.push_back({tick_of(item), item.find("hash")->as_string()});
    }
    playback->first = tick_of(*doc.find("start"));
    playback->last = tick_of(*doc.find("end"));
    return playback;
}

// The replay file at path, as the kept playback if the file is the same (fresh is left empty)
// or as a new one in fresh. Changes nothing.
Result<Playback*> open_playback(Session& session, const std::string& path,
                                std::unique_ptr<Playback>& fresh, Reply& failure) {
    auto text = read_replay(session, path, failure);
    if (!text) {
        return text.error();
    }
    if (Playback* kept = kept_playback(session.world());
        kept != nullptr && kept->path == path && kept->text == text.value()) {
        return kept;
    }
    auto made = new_playback(path, std::move(text).value(), failure);
    if (!made) {
        return made.error();
    }
    fresh = std::move(made).value();
    return fresh.get();
}

// Puts the start back, as a new world would play it.
Result<void> begin(Session& session, Playback& playback) {
    const json::Value& start = *playback.replay.find("start");
    world::World& world = session.world();
    if (auto put = put_back(session, start, playback.prefabs, "/start"); !put) {
        return put.error();
    }
    const json::Value* next_id = start.find("next_id");
    world.set_counters(
        {playback.first, next_id != nullptr ? static_cast<std::uint64_t>(next_id->as_int()) : 1});
    world.reset_state();
    if (const json::Value* viewport = start.find("viewport"); viewport != nullptr) {
        ui::set_viewport(world, vec_of(*viewport));
    }
    session.set_scene_path(text_param(playback.replay, "scene"));
    playback.next_entry = 0;
    playback.next_run = 0;
    playback.next_check = 0;
    playback.going = true;
    playback.ended = false;
    playback.diverged.reset();
    playback.failed = json::Value();
    playback.changes = json::Value();
    return {};
}

// Does what happened at the world's tick, which comes before the step from it.
Result<void> settle(Session& session, Playback& playback) {
    world::World& world = session.world();
    const json::Array& timeline = playback.replay.find("timeline")->as_array();
    while (playback.going && playback.next_entry < timeline.size()) {
        const std::size_t i = playback.next_entry;
        const json::Value& entry = timeline[i];
        const std::uint64_t tick = tick_of(entry);
        if (tick > world.tick()) {
            break;
        }
        if (tick < world.tick()) {
            // A command stepped past it, which it didn't when recorded.
            playback.differ(tick);
            playback.going = false;
            break;
        }
        ++playback.next_entry;
        if (const json::Value* events = entry.find("input"); events != nullptr) {
            for (const json::Value& event : events->as_array()) {
                if (event.find("type")->as_string() == "release_all") {
                    input::release_pressed(world);
                } else if (auto parsed = input::parse_event(event, input::Use::Event); parsed) {
                    input::push(world, std::move(parsed).value());
                }
            }
        } else if (const json::Value* viewport = entry.find("viewport"); viewport != nullptr) {
            ui::set_viewport(world, vec_of(*viewport));
        } else if (const json::Value* args = entry.find("params"); args != nullptr) {
            const std::string& name = entry.find("command")->as_string();
            const Reply reply = session.call(name, *args);
            if (!reply.ok) {
                playback.failed["entry"] = i;
                playback.failed["command"] = name;
                playback.failed["message"] = reply.message;
                playback.differ(tick);
                playback.going = false;
            } else if (const json::Value* hash = entry.find("hash");
                       hash != nullptr &&
                       world_hash(world::save_scene(world)) != hash->as_string()) {
                playback.differ(tick);
            }
        } else {
            if (auto put = put_back(session, entry, playback.prefabs, at("/timeline", i)); !put) {
                return put.error();
            }
            const json::Value* id = entry.find("next_id");
            const json::Value* end_tick = entry.find("end_tick");
            world.set_counters(
                {end_tick != nullptr ? static_cast<std::uint64_t>(end_tick->as_int()) : tick,
                 id != nullptr ? static_cast<std::uint64_t>(id->as_int())
                               : world.counters().next_generated_id});
            if (const json::Value* scene_path = entry.find("path"); scene_path != nullptr) {
                session.set_scene_path(scene_path->as_string());
            }
        }
    }
    return {};
}

// Compares the world with the recording's hash at its tick, if it took one there.
void compare(Playback& playback, const world::World& world) {
    const std::uint64_t tick = world.tick();
    std::vector<Check>& checks = playback.checks;
    while (playback.next_check < checks.size() && checks[playback.next_check].tick < tick) {
        ++playback.next_check;
    }
    if (playback.next_check < checks.size() && checks[playback.next_check].tick == tick) {
        if (world_hash(world::save_scene(world)) != checks[playback.next_check].hash) {
            playback.differ(tick);
        }
        ++playback.next_check;
    }
}

// Steps one tick as the host did.
bool step(Playback& playback, world::World& world) {
    const std::uint64_t tick = world.tick();
    std::vector<Run>& runs = playback.runs;
    while (playback.next_run < runs.size() &&
           runs[playback.next_run].tick + runs[playback.next_run].count <= tick) {
        ++playback.next_run;
    }
    if (playback.next_run == runs.size() || runs[playback.next_run].tick > tick) {
        // The recording reached this tick inside a command, which stepped a different number of
        // ticks this time.
        playback.differ(tick);
        playback.going = false;
        return false;
    }
    world.step(runs[playback.next_run].seconds);
    return true;
}

// Plays on to tick stop, or to the first tick found() holds at. Returns whether it found one.
Result<bool> play_to(Session& session, Playback& playback, std::uint64_t stop,
                     const std::function<bool()>& found = {}) {
    world::World& world = session.world();
    bool matched = false;
    while (true) {
        if (auto settled = settle(session, playback); !settled) {
            return settled.error();
        }
        if (!playback.going) {
            break;
        }
        compare(playback, world);
        if (found && found()) {
            matched = true;
            break;
        }
        if (world.tick() >= stop || !step(playback, world)) {
            break;
        }
    }
    const json::Value& end = *playback.replay.find("end");
    if (playback.going && !playback.ended && world.tick() == playback.last &&
        playback.next_entry == playback.replay.find("timeline")->size()) {
        playback.ended = true;
        const Session::SavedWorld now = session.save();
        if (now.scene != *end.find("world")) {
            playback.differ(playback.last);
            auto links =
                links_for(*end.find("world"), end.find("prefabs"), playback.prefabs, "/end");
            if (!links) {
                return links.error();
            }
            playback.changes = world::diff_scenes(session.components(), *end.find("world"),
                                                  now.scene, links.value(), now.links);
        }
    }
    return matched;
}

json::Value report(const Playback& playback, const world::World& world) {
    json::Value result;
    result["tick"] = world.tick();
    result["start"] = playback.first;
    result["end"] = playback.last;
    result["matched"] = !playback.diverged.has_value();
    if (playback.diverged) {
        result["diverged"] = *playback.diverged;
    }
    if (!playback.failed.is_null()) {
        result["failed"] = playback.failed;
    }
    if (world.tick() == playback.last && !playback.changes.is_null()) {
        result["changes"] = playback.changes;
    }
    return result;
}

Reply file_failure(const std::string& path, const Error& error) {
    json::Value data;
    data["file"] = path;
    return Reply::failure(code::kInvalidArgument, path + ": " + error.message, std::move(data));
}

Reply range_failure(std::string_view param, const Playback& playback) {
    return param_failure(code::kInvalidArgument, param,
                         "the replay runs from tick " + std::to_string(playback.first) + " to " +
                             std::to_string(playback.last));
}

std::optional<Reply> refuse(const Session& session, std::string_view name) {
    if (session.depth() > 1) {
        return nested(name);
    }
    if (const Recording* active = active_recording(session.world()); active != nullptr) {
        return Reply::failure(code::kInvalidArgument,
                              "recording to " + active->path + "; stop it with replay.stop first");
    }
    return std::nullopt;
}

// Keeps the playback for replay.seek. A dry run puts the world back to where it was before the
// call, but not contacts and other state outside the components, so after one the next seek
// plays from the start.
Reply finish(Session& session, std::unique_ptr<Playback> playback, json::Value result,
             std::uint64_t tick_before) {
    Playback& kept = *playback;
    keep_playback(session, std::move(playback));
    if (session.dry_running()) {
        kept.intact = false;
        kept.tick = tick_before;
    }
    return Reply::success(std::move(result));
}

// For a call that fails before changing anything: the kept playback stays resumable if it was.
Reply untouched(Session& session, bool could_resume, Reply failure) {
    if (Playback* kept = kept_playback(session.world()); kept != nullptr && could_resume) {
        kept->revision = session.revision();
    }
    return failure;
}

// The playback to change the world with: the kept one, or the fresh one in its place.
std::unique_ptr<Playback> commit(world::World& world, std::unique_ptr<Playback> fresh) {
    std::unique_ptr<Playback> kept = take_playback(world);
    return fresh != nullptr ? std::move(fresh) : std::move(kept);
}

Reply replay_run(Session& session, const json::Value& params) {
    if (auto refused = refuse(session, "replay.run")) {
        return std::move(*refused);
    }
    const std::uint64_t tick_before = session.world().tick();
    const bool could_resume = resumable(session, kept_playback(session.world()));
    const std::string path = text_param(params, "path");
    std::unique_ptr<Playback> fresh;
    Reply failure;
    auto opened = open_playback(session, path, fresh, failure);
    if (!opened) {
        return untouched(session, could_resume, std::move(failure));
    }
    const Playback& file = *opened.value();
    std::uint64_t stop = file.last;
    if (const json::Value* tick = params.find("tick"); tick != nullptr) {
        stop = static_cast<std::uint64_t>(tick->as_int());
        if (stop < file.first || stop > file.last) {
            return untouched(session, could_resume, range_failure("/tick", file));
        }
    }

    std::unique_ptr<Playback> playback = commit(session.world(), std::move(fresh));
    if (auto begun = begin(session, *playback); !begun) {
        return file_failure(path, begun.error());
    }
    if (auto played = play_to(session, *playback, stop); !played) {
        return file_failure(path, played.error());
    }
    json::Value result = report(*playback, session.world());
    return finish(session, std::move(playback), std::move(result), tick_before);
}

Reply replay_seek(Session& session, const json::Value& params) {
    if (auto refused = refuse(session, "replay.seek")) {
        return std::move(*refused);
    }
    world::World& world = session.world();
    const std::uint64_t tick_before = world.tick();
    const Playback* kept = kept_playback(world);
    const bool could_resume = resumable(session, kept);
    const auto fail = [&](Reply failure) {
        return untouched(session, could_resume, std::move(failure));
    };
    std::string path = text_param(params, "path");
    if (path.empty()) {
        if (kept == nullptr) {
            return param_failure(code::kInvalidParams, "/path",
                                 "nothing is playing; give a .replay.json file");
        }
        path = kept->path;
    }
    const json::Value* tick = params.find("tick");
    const json::Value* ticks = params.find("ticks");
    const json::Value* from = params.find("from");
    const bool searching = params.find("with") != nullptr || params.find("without") != nullptr ||
                           params.find("where") != nullptr;
    if (tick != nullptr && ticks != nullptr) {
        return fail(param_failure(code::kInvalidParams, "/ticks", "give tick or ticks, not both"));
    }
    if (searching && ticks != nullptr) {
        return fail(param_failure(code::kInvalidParams, "/ticks",
                                  "a search runs from from to tick; ticks moves without one"));
    }
    if (!searching && from != nullptr) {
        return fail(param_failure(code::kInvalidParams, "/from",
                                  "from starts a search; give with, without or where"));
    }
    if (!searching && tick == nullptr && ticks == nullptr) {
        return fail(Reply::failure(code::kInvalidParams,
                                   "give tick, ticks, or with, without or where to search for"));
    }
    detail::Selection selection;
    if (searching) {
        if (auto failed = detail::parse_selection(session.components(), params, selection)) {
            return fail(std::move(*failed));
        }
    }

    std::unique_ptr<Playback> fresh;
    Reply failure;
    auto opened = open_playback(session, path, fresh, failure);
    if (!opened) {
        return fail(std::move(failure));
    }
    const Playback& file = *opened.value();
    // Where the playback is: where it left off in the same file, else the start.
    const bool same = fresh == nullptr;
    const std::uint64_t position = same ? file.tick : file.first;
    const auto out_of_range = [&](std::string_view param) {
        return fail(range_failure(param, file));
    };
    std::uint64_t target = file.last;
    if (tick != nullptr) {
        target = static_cast<std::uint64_t>(tick->as_int());
    } else if (ticks != nullptr) {
        const std::int64_t moved = static_cast<std::int64_t>(position) + ticks->as_int();
        if (moved < static_cast<std::int64_t>(file.first) ||
            moved > static_cast<std::int64_t>(file.last)) {
            return out_of_range("/ticks");
        }
        target = static_cast<std::uint64_t>(moved);
    }
    if (target < file.first || target > file.last) {
        return out_of_range("/tick");
    }
    std::uint64_t first_stop = target;
    if (searching) {
        first_stop = from != nullptr ? static_cast<std::uint64_t>(from->as_int()) : file.first;
        if (first_stop < file.first || first_stop > target) {
            return fail(param_failure(code::kInvalidArgument, "/from",
                                      "the search runs from tick " + std::to_string(file.first) +
                                          " to " + std::to_string(target)));
        }
    }

    // Going on from where it stopped is exact. Going back plays the run again from its start,
    // since contacts and other state outside the components can't be put back.
    std::unique_ptr<Playback> playback = commit(world, std::move(fresh));
    if (!same || !could_resume || first_stop < world.tick()) {
        if (auto begun = begin(session, *playback); !begun) {
            return file_failure(path, begun.error());
        }
    }
    if (auto played = play_to(session, *playback, first_stop); !played) {
        return file_failure(path, played.error());
    }
    if (!searching) {
        json::Value result = report(*playback, world);
        return finish(session, std::move(playback), std::move(result), tick_before);
    }
    const bool until_none = text_param(params, "until") == "none";
    auto found = play_to(session, *playback, target,
                         [&] { return detail::any_selected(world, selection) != until_none; });
    if (!found) {
        return file_failure(path, found.error());
    }
    json::Value result = detail::select(world, selection);
    result["found"] = found.value();
    json::Value reported = report(*playback, world);
    for (auto& [key, value] : reported.as_object()) {
        result[key] = std::move(value);
    }
    return finish(session, std::move(playback), std::move(result), tick_before);
}

} // namespace

json::Value replay_schema() {
    json::Value schema;
    schema["$schema"] = "https://json-schema.org/draft/2020-12/schema";
    schema["title"] = "SlashEngine replay";
    schema["description"] =
        "A run recorded with replay.record or slash-cli run --record: the entities it started "
        "from, then the ticks the host stepped, the input it queued and the commands that "
        "changed the world, in order. replay.run plays it back.";
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["required"] = json::Array{"format", "start", "steps", "timeline", "checks", "end"};
    json::Value& properties = schema["properties"];
    properties["format"]["const"] = kReplayFormat;
    properties["format"]["description"] = "Format version. Must be 1.";
    properties["scene"] =
        typed("string", "The scene file open when the recording started, or \"\".");
    properties["start"] = object_schema(
        {
            {"tick", tick_schema("The world's tick when the recording started.")},
            {"next_id",
             [] {
                 json::Value id = typed("integer", "The number in the next generated entity ID.");
                 id["minimum"] = 1;
                 return id;
             }()},
            {"viewport", viewport_schema("The UI viewport, width and height in pixels.")},
            {"world", typed("object", "The entities, as a scene file holds them.")},
        },
        {"tick", "world"});
    properties["prefabs"] = prefabs_schema(
        "The components of each prefab the entities extend, by file path, as they were when "
        "the recording kept them.");
    json::Value run = object_schema(
        {
            {"tick", tick_schema("The first tick of the run.")},
            {"count",
             [] {
                 json::Value count = typed("integer", "How many ticks.");
                 count["minimum"] = 1;
                 return count;
             }()},
            {"seconds",
             [] {
                 json::Value seconds = typed("number", "The length of each tick, in seconds.");
                 seconds["exclusiveMinimum"] = 0;
                 return seconds;
             }()},
        },
        {"tick", "count", "seconds"});
    properties["steps"] = typed("array", "The ticks the host stepped outside commands, in runs.");
    properties["steps"]["items"] = std::move(run);
    json::Value input = typed("array", "Input events queued before the tick, as input.inject "
                                       "takes them, or {\"type\": \"release_all\"}.");
    input["items"]["type"] = "object";
    input["minItems"] = 1;
    json::Value entry = object_schema(
        {
            {"tick", tick_schema("The tick it happened before.")},
            {"input", std::move(input)},
            {"command", typed("string", "A command that ran.")},
            {"params", typed("object", "Its params, to run it again.")},
            {"hash", hash_schema("A hash of the entities after it ran.")},
            {"world", typed("object", "Or the entities it left, to put back: for undo, redo, "
                                      "world.restore, scene.open, scene.new and asset reloads.")},
            {"next_id",
             [] {
                 json::Value id = typed("integer", "The next generated ID after it.");
                 id["minimum"] = 1;
                 return id;
             }()},
            {"end_tick", tick_schema("The tick it ended at, if it stepped.")},
            {"path", typed("string", "The scene file it opened.")},
            {"prefabs", prefabs_schema("Prefabs whose files changed since the start.")},
            {"viewport", viewport_schema("The UI viewport the host set.")},
        },
        {"tick"});
    properties["timeline"] = typed("array", "What happened from outside the systems, in order.");
    properties["timeline"]["items"] = std::move(entry);
    properties["checks"] =
        typed("array", "A hash of the entities before every 60th tick the host stepped.");
    properties["checks"]["items"] =
        object_schema({{"tick", tick_schema("The tick it was taken before.")},
                       {"hash", hash_schema("FNV-1a 64 of the entities' compact JSON, in hex.")}},
                      {"tick", "hash"});
    properties["end"] = object_schema(
        {
            {"tick", tick_schema("The world's tick when the recording stopped.")},
            {"world", typed("object", "The entities then.")},
            {"prefabs", prefabs_schema("Prefabs whose files changed since the start.")},
        },
        {"tick", "world"});
    return schema;
}

std::vector<json::ValidationError> validate_replay(const json::Value& replay) {
    std::vector<json::ValidationError> errors = json::validate(replay, replay_schema());
    if (!errors.empty()) {
        return errors;
    }
    const std::uint64_t start = tick_of(*replay.find("start"));
    const std::uint64_t end = tick_of(*replay.find("end"));
    if (end < start) {
        add_error(errors, "/end/tick", "is before the start tick " + std::to_string(start));
        return errors;
    }
    std::uint64_t next = start;
    const json::Array& runs = replay.find("steps")->as_array();
    for (std::size_t i = 0; i < runs.size(); ++i) {
        const std::uint64_t tick = tick_of(runs[i]);
        const std::uint64_t count = tick_of(runs[i], "count");
        if (tick < next) {
            add_error(errors, at("/steps", i, "tick"),
                      "comes before tick " + std::to_string(next) + ", where the run before ends");
        } else if (tick + count > end) {
            add_error(errors, at("/steps", i, "count"),
                      "runs past the end tick " + std::to_string(end));
        }
        next = std::max(next, tick + count);
    }
    validate_timeline(replay.find("timeline")->as_array(), start, end, errors);
    std::uint64_t previous = start;
    const json::Array& checks = replay.find("checks")->as_array();
    for (std::size_t i = 0; i < checks.size(); ++i) {
        const std::uint64_t tick = tick_of(checks[i]);
        if (tick <= previous) {
            add_error(errors, at("/checks", i, "tick"),
                      "comes at or before tick " + std::to_string(previous));
        } else if (tick > end) {
            add_error(errors, at("/checks", i, "tick"),
                      "is after the end tick " + std::to_string(end));
        }
        previous = std::max(previous, tick);
        if (!is_hash(checks[i].find("hash")->as_string())) {
            add_error(errors, at("/checks", i, "hash"), "is 16 lowercase hex digits");
        }
    }
    return errors;
}

Result<json::Value> start_recording(Session& session, std::string path) {
    if (const Recording* active = active_recording(session.world()); active != nullptr) {
        return make_error(ErrorCode::InvalidArgument,
                          "already recording to " + active->path + "; stop it with replay.stop");
    }
    if (auto file = replay_file(session, path); !file) {
        return file.error();
    }
    if (!session.options().write_files) {
        return make_error(ErrorCode::PermissionDenied,
                          "tests don't write files, so a test can't record a replay");
    }
    world::World& world = session.world();
    // Start afresh, with the entities stored as a replay puts them back, so playing it back
    // begins alike.
    session.restore(session.save());
    world.reset_state();

    auto recording = std::make_unique<Recording>();
    recording->path = std::move(path);
    recording->scene = session.scene_path();
    recording->start_tick = world.tick();
    recording->next_id = world.counters().next_generated_id;
    recording->start_viewport = ui::settings(world).viewport;
    recording->viewport = recording->start_viewport;
    const Session::SavedWorld saved = session.save();
    recording->start = saved.scene;
    json::Value unused;
    keep_prefabs(*recording, saved.links, unused);

    auto& holder = world.ecs().ctx().contains<RecordingHolder>()
                       ? world.ecs().ctx().get<RecordingHolder>()
                       : world.ecs().ctx().emplace<RecordingHolder>();
    holder.active = std::move(recording);
    listen(session, *holder.active);
    json::Value result;
    result["recording"] = holder.active->path;
    result["tick"] = world.tick();
    return result;
}

Result<json::Value> stop_recording(Session& session) {
    auto* holder = session.world().ecs().ctx().find<RecordingHolder>();
    if (holder == nullptr || holder->active == nullptr) {
        return make_error(ErrorCode::InvalidArgument,
                          "nothing is recording; start with replay.record");
    }
    Recording& recording = *holder->active;
    auto file = session.resolve_path(recording.path);
    if (!file) {
        return file.error();
    }
    const json::Value doc = replay_json(session, recording);
    if (auto written = session.write_file(file.value(), json::write(doc) + "\n"); !written) {
        return written.error();
    }
    stop_listening(session);
    json::Value result;
    result["path"] = recording.path;
    result["ticks"] = session.world().tick() - recording.start_tick;
    result["commands"] = recording.commands;
    result["inputs"] = recording.inputs;
    holder->active.reset();
    return result;
}

bool recording(const Session& session) {
    return active_recording(session.world()) != nullptr;
}

void register_replay_commands(Registry& registry) {
    json::Value path = typed("string", "A .replay.json file in the project.");
    registry.add({
        .name = "replay.record",
        .description = "Starts recording the run to a .replay.json file: the entities now, then "
                       "each tick, input event and command that changes the world. Held input, "
                       "focus, particles, contacts and sounds start afresh, as in playback. "
                       "replay.stop writes it.",
        .capabilities = {Capability::Read, Capability::Filesystem},
        .effect = Effect::Read,
        .params = object_schema({{"path", path}}, {"path"}),
        .handler =
            [](Session& session, const json::Value& params) {
                if (session.depth() > 1) {
                    return nested("replay.record");
                }
                auto started = start_recording(session, text_param(params, "path"));
                if (!started) {
                    return param_failure(Reply::failure(started.error()).code, "/path",
                                         started.error().message);
                }
                return Reply::success(std::move(started).value());
            },
    });
    registry.add({
        .name = "replay.stop",
        .description = "Stops recording and writes the file. Returns its path and how many ticks, "
                       "commands and input events it holds.",
        .capabilities = {Capability::Filesystem},
        .effect = Effect::Read,
        .params = object_schema({}, {}),
        .handler =
            [](Session& session, const json::Value& /*params*/) {
                if (session.depth() > 1) {
                    return nested("replay.stop");
                }
                auto stopped = stop_recording(session);
                if (!stopped) {
                    return Reply::failure(stopped.error());
                }
                return Reply::success(std::move(stopped).value());
            },
    });
    json::Value stop = tick_schema("Stop at this tick instead of the end.");
    registry.add({
        .name = "replay.run",
        .description = "Plays a .replay.json file back from its start, with the same ticks, input "
                       "and commands, to its end or tick. Returns the tick, whether the world "
                       "matched the recording, the first tick it differed and what differs at "
                       "the end.",
        .capabilities = {Capability::Edit, Capability::Run, Capability::Filesystem},
        .effect = Effect::Replace,
        .params = object_schema({{"path", std::move(path)}, {"tick", std::move(stop)}}, {"path"}),
        .handler = replay_run,
    });
    json::Object seek = detail::selection_properties();
    seek.emplace("path", typed("string", "A .replay.json file. Left out, the one playing."));
    seek.emplace("tick", tick_schema("Go to this tick. With a search, the last tick to search."));
    seek.emplace("ticks", typed("integer", "Move this many ticks from where the playback is; "
                                           "negative goes back."));
    seek.emplace("from", tick_schema("Where a search starts. Left out, the replay's start."));
    json::Value until = typed("string", "Find a tick where any entity matches, or none does.");
    until["enum"] = json::Array{"any", "none"};
    until["default"] = "any";
    seek.emplace("until", std::move(until));
    registry.add({
        .name = "replay.seek",
        .description = "Moves a replay's playback to a tick, by a number of ticks, or to the first "
                       "tick where an entity matches with/without/where (e.g. the player's "
                       "position.y > 600). Forward goes on from where it is; back plays again "
                       "from the start. Returns the tick and whether it still matches the "
                       "recording.",
        .capabilities = {Capability::Edit, Capability::Run, Capability::Filesystem},
        .effect = Effect::Replace,
        .params = object_schema(std::move(seek), {}),
        .handler = replay_seek,
    });
}

} // namespace slash::command
