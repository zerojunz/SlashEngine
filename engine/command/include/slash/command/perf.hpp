#pragma once

#include "slash/command/command.hpp"
#include "slash/core/json.hpp"

namespace slash::command {

// Adds perf.stats: how long the world's ticks and its slowest systems took over the last 120
// ticks (World::tick_timing and SystemInfo::timing). A running game's register_live_commands
// replaces it with one that adds frame pacing.
void register_perf_commands(Registry& registry);

// What perf.stats reports of a session's world for its params:
// {"tick_ms": {"avg", "max"}, "systems": [{"name", "stage", "avg", "max"}, ...]}.
json::Value world_perf_stats(const Session& session, const json::Value& params);

} // namespace slash::command
