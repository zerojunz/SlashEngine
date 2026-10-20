// Achievements, leaderboards and sign-in, as Unity's Social API over Google Play Games and Game
// Center: project.json names the achievements and the leaderboard, gameplay code unlocks and
// submits by name, and what happens arrives as events at a later tick. Each lap of a time trial
// submits its time to best_lap, where lower is better, and counts toward ten_laps.
//
// This program adds the race's system, then plays it as an agent does over MCP or JSON-RPC.
// Sessions run the services on the test backend, so the player signs in with no account:
// ui.press presses the race's buttons, services.test adds other players' times and progress as
// the game would, and services.state and events.read show what the services saw.

#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/core/reflect.hpp"
#include "slash/services/services.hpp"
#include "slash/ui/ui.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/world.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

namespace game {

struct Race {
    int laps = 0;
};
SLASH_COMPONENT(Race, laps)

} // namespace game

namespace {

namespace command = slash::command;
namespace json = slash::json;
namespace services = slash::services;
namespace ui = slash::ui;
namespace world = slash::world;

constexpr int kFirstLapMs = 64'000;
constexpr int kFasterEachLapMs = 1'500;
constexpr double kMsPerSecond = 1000.0;

// "62.5 s" for 62500 milliseconds.
std::string seconds(std::int64_t ms) {
    std::array<char, 32> text{};
    (void)std::snprintf(text.data(), text.size(), "%.1f s", static_cast<double>(ms) / kMsPerSecond);
    return text.data();
}

void set_text(world::World& game_world, std::string_view id, std::string text) {
    game_world.ecs().get<world::Label>(game_world.find(id)).text = std::move(text);
}

// The race: buttons call the services, and their events update the labels.
void race(world::World& game_world, double /*tick_seconds*/) {
    auto& state = game_world.ecs().get<game::Race>(game_world.find("laps"));
    if (ui::was_pressed(game_world, game_world.find("finish_lap"))) {
        const int lap_ms = kFirstLapMs - kFasterEachLapMs * state.laps;
        ++state.laps;
        // Kept and sent when the player signs in, if they aren't yet.
        (void)services::submit_score(game_world, "best_lap", lap_ms);
        (void)services::unlock_achievement(game_world, "first_finish");
        (void)services::increment_achievement(game_world, "ten_laps");
        set_text(game_world, "laps", std::to_string(state.laps) + " laps");
    }
    if (ui::was_pressed(game_world, game_world.find("best_laps"))) {
        // Fails if the player isn't signed in; a real game would grey the button out.
        (void)services::load_scores(game_world, "best_lap",
                                    {.time = services::TimeSpan::AllTime,
                                     .collection = services::Collection::Global,
                                     .count = 3});
    }
    if (ui::was_pressed(game_world, game_world.find("trophies"))) {
        (void)services::show_achievements(game_world);
    }
    for (const services::ServiceEvent& event : services::events(game_world)) {
        if (event.kind == "signed_in") {
            set_text(game_world, "status", "Signed in as " + event.player.name);
        } else if (event.kind == "sign_in_failed") {
            set_text(game_world, "status", "Not signed in");
        } else if (event.kind == "achievement_unlocked") {
            set_text(game_world, "status", "Unlocked " + event.id);
        } else if (event.kind == "scores_loaded") {
            std::string board;
            for (const services::Score& score : event.scores.entries) {
                board += (board.empty() ? "" : ", ") + std::to_string(score.rank) + ". " +
                         score.name + " " + seconds(score.value);
            }
            set_text(game_world, "status", board);
        }
    }
}

// Prints the call as an agent would make it, then the reply.
void call(command::Session& session, std::string_view name, std::string_view params) {
    const json::Value value = json::parse(params).value();
    std::printf("%.*s %s\n", static_cast<int>(name.size()), name.data(),
                json::write(value, json::Style::Compact).c_str());
    const command::Reply reply = session.call(name, value);
    std::puts(json::write(reply.ok ? reply.value : json::Value(reply.message), json::Style::Compact)
                  .c_str());
}

} // namespace

int main() {
    world::ComponentRegistry components;
    world::register_builtin_components(components);
    components.add<game::Race>()
        .set_doc("The time trial's laps so far.")
        .describe("laps", {.doc = "Laps finished."});
    command::Registry commands;
    command::register_builtin_commands(commands);
    command::SessionOptions options;
    options.project_root = ".";
    options.capture_logs = false;
    // The session reads project.json's achievements and leaderboard and runs them on the test
    // backend, which signs the player in at the first tick, as Play Games and Game Center do.
    command::Session session(components, commands, options);
    world::World& w = session.world();
    w.add_system("race", race);

    call(session, "scene.open", R"({"path": "main.scene.json"})");
    w.ecs().emplace<game::Race>(w.find("laps"));
    call(session, "events.subscribe", R"({"kinds": ["achievement_unlocked", "scores_loaded"]})");
    // Signed in at the first tick; the player's progress arrives at the second.
    call(session, "step", R"({"ticks": 2})");
    call(session, "services.state", R"({"events": 0})");

    // The first lap unlocks first_finish and is the player's best time so far.
    call(session, "ui.press", R"({"id": "finish_lap"})");
    call(session, "step", R"({"ticks": 2})");
    call(session, "entity.get", R"({"id": "status", "components": ["Label"]})");

    // Other players' times on the test leaderboard, and eight more laps' progress as the game
    // would make it. The tenth lap unlocks ten_laps.
    call(session, "services.test", R"({"scores": {"best_lap": [{"name": "Ana", "score": 61000},
        {"name": "Bo", "score": 66500}]}, "increment": {"achievement": "ten_laps", "steps": 8}})");
    call(session, "ui.press", R"({"id": "finish_lap"})");
    call(session, "step", R"({"ticks": 2})");
    call(session, "entity.get", R"({"id": "status", "components": ["Label"]})");

    // The best three laps, lower first.
    call(session, "ui.press", R"({"id": "best_laps"})");
    call(session, "step", R"({"ticks": 2})");
    call(session, "entity.get", R"({"id": "status", "components": ["Label"]})");
    call(session, "events.read", R"({"subscription": "events_1"})");

    // The achievements screen shows over the game until the player closes it.
    call(session, "ui.press", R"({"id": "trophies"})");
    call(session, "step", "{}");
    call(session, "services.state", R"({"events": 0})");
    call(session, "step", R"({"ticks": 2})");
    call(session, "services.state", R"({"events": 0})");

    // Mistakes are caught with the closest name, and a failed call changes nothing.
    call(session, "services.test", R"({"unlock": "first_finsh"})");
    call(session, "services.test", R"({"increment": {"achievement": "first_finish"}})");
    return 0;
}
