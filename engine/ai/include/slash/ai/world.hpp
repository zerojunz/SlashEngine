#pragma once

#include "slash/ai/config.hpp"
#include "slash/ai/provider.hpp"
#include "slash/core/result.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace slash::world {
class World;
} // namespace slash::world

// Language models for gameplay code, as the services are: a game asks a role's model and hears
// back through world events at a later tick, as it hears input, so it never waits on a model.
//
//   const auto asked = ai::ask(world, "dialogue", "The player asks Mira for bread.");
//   // ...each tick after:
//   for (const world::Event& event : world.events()) {
//       if (event.kind == "ai_reply" && event.id == asked.value()) {
//           show(event.data["text"].as_string());
//       }
//   }
//
// Games whose project has a providers.json get it; so do sessions, which never call a model
// without the network capability but still answer from the replay file.
namespace slash::ai {

struct WorldSettings {
    // Where providers.json is read, and how providers open. The file is read again when it
    // changes.
    OpenOptions open;
    // Each tick waits for the replies to the requests asked before it, so a reply arrives at the
    // same tick on every run, as headless runs and tests need. Windowed games never wait.
    bool wait = false;
};

// Adds the AI state and the "ai" system (priority 1150, after particles), which hands each
// finished reply to the game as an event. Installing again replaces the settings and cancels
// the requests being answered.
void install(world::World& world, WorldSettings settings);
[[nodiscard]] bool installed(const world::World& world);
void set_wait(world::World& world, bool wait);

// Asks a role's model, or a provider named directly ("" for the default role), streaming its
// reply. Returns the request's ID: "ai_1", "ai_2" and so on. At a tick after the reply ends the
// world emits "ai_reply" with that ID and {"role", "text", "message", "output" (with an
// output_schema), "stop", "usage"}, or "ai_failed" with {"role", "code", "message"}. A reply's
// "message" goes back in the next request of the conversation (message_from_json).
[[nodiscard]] Result<std::string> ask(world::World& world, std::string_view role, Request request);
// Asks with one user message.
[[nodiscard]] Result<std::string> ask(world::World& world, std::string_view role,
                                      std::string prompt);

// The reply's text so far while it streams, so a game can show it as it arrives; "" once its
// event is sent.
[[nodiscard]] std::string reply_text(const world::World& world, std::string_view request);
// Whether the request is still being answered: its event is not sent yet.
[[nodiscard]] bool pending(const world::World& world, std::string_view request);
// Stops a request, which then sends no event.
void cancel(world::World& world, std::string_view request);

// The requests asked so far. Sessions restore it when they roll back a failed command or a dry
// run, which cancels the requests asked since and gives their IDs out again.
struct Snapshot {
    std::uint64_t next_request = 1;
};
[[nodiscard]] Snapshot save(const world::World& world);
void restore(world::World& world, const Snapshot& snapshot);

} // namespace slash::ai
