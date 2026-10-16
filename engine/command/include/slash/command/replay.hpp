#pragma once

#include "slash/command/command.hpp"
#include "slash/core/json.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/core/result.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Replays: a run recorded from a session into a .replay.json file, played back exactly. A
// recording keeps the entities it started from (and the prefabs they extend), then everything
// that changed the world from outside its systems, in order: the commands run, the input the
// platform queued and the ticks the host stepped, with how long each was. It also keeps a hash
// of the world every kReplayCheckTicks host ticks and the entities it ended with.
//
// Playing one back puts the start back, starts the world afresh (World::reset_state), then
// runs the same commands, queues the same input and steps the same ticks, so it ends where the
// recording did; the hashes say where it went another way, if it did. Recording starts the
// world afresh too, so both begin alike.
namespace slash::command {

class Session;

inline constexpr std::string_view kReplaySuffix = ".replay.json";
inline constexpr std::int64_t kReplayFormat = 1;
inline constexpr std::uint64_t kReplayCheckTicks = 60;

// The JSON Schema for .replay.json files.
[[nodiscard]] json::Value replay_schema();

// Every error in a replay: the schema's, then what it can't say (ticks in order and inside the
// run, input events that read).
[[nodiscard]] std::vector<json::ValidationError> validate_replay(const json::Value& replay);

// Starts recording the session's world to path, a project-relative .replay.json file, written by
// stop_recording. Returns {"recording": path, "tick": the tick it starts at}.
Result<json::Value> start_recording(Session& session, std::string path);
// Writes the recording and stops. Returns {"path", "ticks", "commands", "inputs"}.
Result<json::Value> stop_recording(Session& session);
[[nodiscard]] bool recording(const Session& session);

// Adds replay.record, replay.stop and replay.run.
void register_replay_commands(Registry& registry);

} // namespace slash::command
