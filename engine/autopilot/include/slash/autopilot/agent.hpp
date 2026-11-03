#pragma once

#include "slash/autopilot/bridge.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace slash::ai {
class Provider;
} // namespace slash::ai

namespace slash::world {
class World;
} // namespace slash::world

// SlashEngine's own playtest agent: an agent service in the game's process, after Autopilot's
// (github.com/ramenvr/autopilot, Apache 2.0), that plays the game with any model providers.json
// names instead of Autopilot's service and its hosted decision model. It speaks the bridge's
// protocol over an in-process Transport, so it asks for everything Autopilot asks for (what the
// player sees, keys, moves, clicks, pictures, the project's files) and the bridge's detectors,
// report files and exit codes work as they do with Autopilot. The model plays through tools, one
// turn at a time: the game is paused while it thinks and runs only inside tools that take time
// (keys held for seconds, moves, waits, clicks), so a slow model plays as well as a fast one.
// See docs/reference/autopilot.md.
namespace slash::autopilot {

struct AgentOptions {
    // The model that plays: a role or provider from providers.json, opened with ai::open.
    std::shared_ptr<ai::Provider> provider;
    // Its name, for reports.
    std::string model;
    // The project folder: notes from earlier runs are kept in .slash/autopilot/notes.md in it.
    std::string project_root = ".";
    // Turns (model replies) at most, the last of which may only report.
    int max_turns = 60;
    // Seconds of play at most: 0 for the run's own limit (an explore run's minutes, or any
    // run's time_limit_minutes), else 10 minutes.
    double play_seconds = 0.0;
    // Tokens in and out, summed over the run, at most; 0 for no limit.
    std::int64_t max_tokens = 0;
    // The most tokens one reply may have.
    std::int64_t reply_tokens = 4096;
    // Wait for each reply in poll, so headless runs and tests repeat exactly. Otherwise a reply
    // comes on a thread of its own while the game, paused, keeps drawing.
    bool wait = false;
    // Whether the model is sent pictures of the game when it asks (and its provider takes them).
    bool pictures = true;
};

// Connections to the agent: give it to a Bridge in place of websocket_connector. Each
// connection is a new agent that plays the run the bridge's hello asks for.
[[nodiscard]] Connector agent_connector(AgentOptions options);

// A clock of the world's game time, for Options::clock when the agent plays: it stands still
// while the game is paused for the model, so holds, waits and timeouts count play.
[[nodiscard]] std::function<double()> game_clock(const world::World& world, double tick_seconds);

} // namespace slash::autopilot
