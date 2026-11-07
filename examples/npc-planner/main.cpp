// NPCs that plan, as F.E.A.R.'s goal-oriented action planning: main.scene.json has a village of
// two farmers and a guard, each an NpcAgent with a .npc.json brain of facts, goals and actions.
// The npc system picks each one's goal and the cheapest chain of actions to it, walks, waits and
// emits through it; a plain C++ system answers the guard's bell by setting the farmers' alarm
// fact, as a game's code would. The program asks npc.plan what they would do, as an agent would.

#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/npc/npc.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/world.hpp"

#include <cstdio>
#include <string>
#include <string_view>

namespace {

namespace command = slash::command;
namespace json = slash::json;
namespace npc = slash::npc;
namespace world = slash::world;

// Rings the alarm in every farmer's head when the guard's bell rings.
void answer_bell(world::World& world, double /*seconds*/) {
    for (const world::Event& event : world.events()) {
        if (event.kind != "bell") {
            continue;
        }
        for (const entt::entity agent : world.ecs().view<world::NpcAgent>()) {
            (void)npc::set_fact(world, agent, "alarm", 1); // the guard has no such fact
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

void print_villagers(world::World& world) {
    std::printf("tick %llu:", static_cast<unsigned long long>(world.tick()));
    for (const char* id : {"ann", "bob", "guard"}) {
        const entt::entity entity = world.find(id);
        const auto& place = world.ecs().get<world::Transform3D>(entity).position;
        const auto& agent = world.ecs().get<world::NpcAgent>(entity);
        std::printf(" %s at (%.2f, %.2f) %s/%s", id, static_cast<double>(place.x),
                    static_cast<double>(place.z), agent.goal.empty() ? "-" : agent.goal.c_str(),
                    agent.plan.empty() ? "-" : agent.plan.front().c_str());
    }
    std::printf("\n");
}

void run(command::Session& session, int seconds) {
    for (int second = 0; second < seconds; ++second) {
        session.call("step", json::parse(R"({"ticks": 60})").value());
        print_villagers(session.world());
    }
}

} // namespace

int main() {
    world::ComponentRegistry components;
    world::register_builtin_components(components);
    command::Registry commands;
    command::register_builtin_commands(commands);
    command::SessionOptions options;
    options.capture_logs = false;
    command::Session session(components, commands, options);

    call(session, "scene.open", R"({"path": "main.scene.json"})");
    session.world().add_system("answer_bell", answer_bell, {.doc = "Spreads the guard's alarm."});
    call(session, "events.subscribe",
         R"({"kinds": ["npc_goal_changed", "npc_action_failed", "bell"]})");

    // What a farmer does from the brain's defaults: farm, three harvests in the field. With
    // check, the brain's warnings: none.
    call(session, "npc.plan", R"({"brain": "brains/villager.npc.json", "check": true})");
    // Bob starts hungry, so he fetches food first.
    call(session, "npc.plan", R"({"id": "bob"})");

    // Four seconds: Ann walks to the field and harvests, Bob eats, the guard climbs his tower.
    run(session, 4);

    // The guard sees a thief: his code sets the fact, and at the next tick he rings the bell,
    // which the farmers' code turns into their alarm.
    npc::set_fact(session.world(), session.world().find("guard"), "thief_seen", 1);
    run(session, 3);
    call(session, "events.read", R"({"subscription": "events_1"})");

    // Hidden in the hall, Ann has nothing left to want: npc.plan says why for each goal.
    call(session, "npc.plan", R"({"id": "ann"})");
    // With the pantry empty, eating is out of reach.
    call(
        session, "npc.plan",
        R"({"brain": "brains/villager.npc.json", "goal": "eat", "facts": {"hungry": true, "food": 0}})");
    // A misspelled fact.
    call(session, "npc.plan", R"({"id": "ann", "facts": {"hungri": true}})");
    return 0;
}
