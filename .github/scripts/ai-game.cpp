// Copied over the game CI exports for the web, so the export proves a web game asks a model
// through the browser's fetch() and hears its streamed reply. It asks on the first tick and logs
// the reply, which the workflow looks for in the export's log.

#include "slash/ai/world.hpp"
#include "slash/core/log.hpp"
#include "slash/world/game_module.hpp"

#include <string>

namespace {

std::string asked;

void talk(slash::world::World& world, double /*tick_seconds*/) {
    if (asked.empty()) {
        auto id = slash::ai::ask(world, "npc", "What's good today?");
        asked = id ? id.value() : "-";
        if (!id) {
            slash::log::error("could not ask", {{"message", id.error().message}});
        }
    }
    for (const slash::world::Event& event : world.events()) {
        if (event.id == asked && event.kind == "ai_reply") {
            slash::log::info("heard", {{"text", event.data.find("text")->as_string()}});
        } else if (event.id == asked && event.kind == "ai_failed") {
            slash::log::error("no reply", {{"message", event.data.find("message")->as_string()}});
        }
    }
}

} // namespace

SLASH_GAME_MODULE(game) {
    game.add_system("talk", talk, {.doc = "Asks a model once and logs its reply."});
}
