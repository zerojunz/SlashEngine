#pragma once

// NPCs that plan, as F.E.A.R.'s goal-oriented action planning (Jeff Orkin, "Three States and a
// Plan", GDC 2006), which Godot doesn't have. A .npc.json file (a brain) names facts, goals and
// actions: each action needs some facts, changes some and costs something, and A* finds the
// cheapest chain of actions from the NPC's facts to its goal's. An NpcAgent component gives an
// entity a brain and keeps its facts, goal and plan, so saving, undo and replays keep them too;
// the npc system thinks every think_interval and runs each plan's actions: waiting, walking to an
// entity (along its NavigationAgent3D or 2D's path when it has one) or emitting an event that
// gameplay code answers by setting facts.

#include "slash/asset/assets.hpp"
#include "slash/core/json.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/core/result.hpp"
#include "slash/world/world.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace slash::npc {

inline constexpr std::string_view kBrainSuffix = ".npc.json";

// How a condition compares a fact with its value.
enum class Compare : std::uint8_t { Equal, NotEqual, Less, LessEqual, Greater, GreaterEqual };

// A test of one fact: true, 3, "<5" or ">=1" in a file.
struct Condition {
    std::size_t fact = 0;
    Compare compare = Compare::Equal;
    int value = 0;

    friend bool operator==(const Condition&, const Condition&) = default;
};

// A change to one fact: a value sets it, "+1" or "-2" in a file adds to it.
struct Effect {
    std::size_t fact = 0;
    bool add = false;
    int value = 0;

    friend bool operator==(const Effect&, const Effect&) = default;
};

// What an action does while it runs. Its effects apply when it ends.
struct Behavior {
    enum class Kind : std::uint8_t {
        None, // ends at once
        Wait, // ends after seconds
        GoTo, // walks to the entity named target, ending within the agent's arrive_distance
        Emit, // emits the world event named event, and ends at once
    };
    Kind kind = Kind::None;
    double seconds = 0.0;
    std::string target;
    std::string event;
    float speed = 0.0F; // a go_to's speed; 0 is the agent's

    friend bool operator==(const Behavior&, const Behavior&) = default;
};

struct Action {
    std::string name;
    float cost = 1.0F;
    std::vector<Condition> needs;
    std::vector<Effect> effects;
    Behavior behavior;

    friend bool operator==(const Action&, const Action&) = default;
};

// A goal is pursued while its when conditions hold and its want conditions don't yet. Goals are
// tried highest priority first, those of equal priority in the file's order.
struct Goal {
    std::string name;
    float priority = 0.0F;
    std::vector<Condition> when;
    std::vector<Condition> want;

    friend bool operator==(const Goal&, const Goal&) = default;
};

// A .npc.json file.
struct Brain {
    std::vector<std::string> facts;      // names, sorted
    std::vector<int> defaults;           // each fact's starting value (false 0, true 1)
    std::vector<Goal> goals;             // in the file's order
    std::vector<std::size_t> priorities; // goals' indices, highest priority first
    std::vector<Action> actions;         // in the file's order, which breaks ties between plans
    int search_limit = 2048;             // the most states a plan may look at

    // The index of a fact, or facts.size() when there is none of that name.
    [[nodiscard]] std::size_t fact(std::string_view name) const;
    // The goal of that name, or null.
    [[nodiscard]] const Goal* goal(std::string_view name) const;

    friend bool operator==(const Brain&, const Brain&) = default;
};

// JSON Schema for .npc.json files.
[[nodiscard]] json::Value brain_schema();
// Every error in a .npc.json document, each with the JSON Pointer of the bad value: the schema's,
// conditions and effects naming facts the file doesn't have, and values that don't read.
[[nodiscard]] std::vector<json::ValidationError> validate_brain(const json::Value& doc);
// The brain in a .npc.json document, or its first error.
[[nodiscard]] Result<Brain> parse_brain(const json::Value& doc);
// Problems with a valid brain, as warnings: goals no chain of actions reaches from the facts'
// defaults (with the goal's when conditions taken as met), and actions no goal's plan uses.
[[nodiscard]] std::vector<json::ValidationError> check_brain(const Brain& brain);

// Whether facts meet every condition.
[[nodiscard]] bool holds(std::span<const Condition> conditions, std::span<const int> facts);
// Applies effects to facts, adding up to the integers' limits.
void apply_effects(std::span<const Effect> effects, std::vector<int>& facts);

// The cheapest chain of actions that takes facts to where goal's want conditions hold, as A*
// over the facts' values with each action an edge; ties go to the action earlier in the file, so
// a plan is the same everywhere. found is false when none is within the brain's search_limit.
struct Plan {
    bool found = false;
    std::vector<std::size_t> actions;
    float cost = 0.0F;
    std::size_t searched = 0; // states looked at
};
[[nodiscard]] Plan plan(const Brain& brain, std::span<const int> facts, const Goal& goal);

// What an agent would do with these facts: the first goal by priority whose when conditions
// hold, whose want conditions don't, and that a plan reaches, with that plan. goal is null when
// none.
struct Choice {
    const Goal* goal = nullptr;
    Plan plan;
};
[[nodiscard]] Choice choose(const Brain& brain, std::span<const int> facts);

// Registers .npc.json files with assets.
void add_asset_types(asset::Assets& assets);

// Adds the npc system (priority 965, after navigation) and the asset type to a world.
void install(world::World& world, asset::Assets& assets);

// An agent's facts: its brain's defaults with its NpcAgent's facts over them. Empty when the
// entity has no agent or its brain doesn't load.
[[nodiscard]] std::vector<int> facts_of(world::World& world, entt::entity entity);
// An agent's brain, or null when the entity has no agent or its brain doesn't load (error says
// why).
[[nodiscard]] std::shared_ptr<const Brain> brain_of(world::World& world, entt::entity entity,
                                                    std::string* error = nullptr);
// Sets one of an agent's facts, as gameplay code does when the world changes. The agent thinks
// again at its next tick. False when the entity has no agent or its brain no such fact.
bool set_fact(world::World& world, entt::entity entity, std::string_view fact, int value);

// Every error in a scene's or prefab's NpcAgent components, each with the JSON Pointer of the bad
// value: a brain that doesn't load or validate, and facts the brain doesn't have. Files are read
// through reader.
[[nodiscard]] std::vector<json::ValidationError> check_scene(const json::Value& doc,
                                                             const world::FileReader& reader);

} // namespace slash::npc
