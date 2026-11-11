#pragma once

// NPCs that plan, as F.E.A.R.'s goal-oriented action planning (Jeff Orkin, "Three States and a
// Plan", GDC 2006), which Godot doesn't have. A .npc.json file (a brain) names facts, goals and
// actions: each action needs some facts, changes some and costs something, and A* finds the
// cheapest chain of actions from the NPC's facts to its goal's. An NpcAgent component gives an
// entity a brain and keeps its facts, goal and plan, so saving, undo and replays keep them too;
// the npc system thinks every think_interval and runs each plan's actions: waiting, walking to an
// entity (along its NavigationAgent3D or 2D's path when it has one) or emitting an event that
// gameplay code answers by setting facts.
//
// A brain can extend a shared one (extends), so each NPC keeps its own traits, goals, priorities
// and routes over a village's common behavior. Facts named hour and day follow the npc clock
// (project.json's npc settings), so goals can keep daily schedules, and facts named story.<name>
// are the story's facts, kept in a QuestLog (slash/npc/story.hpp), which quests and dialogue
// share with the NPCs.

#include "slash/asset/assets.hpp"
#include "slash/core/json.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/core/math.hpp"
#include "slash/core/result.hpp"
#include "slash/world/world.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace slash::npc {

inline constexpr std::string_view kBrainSuffix = ".npc.json";
// Facts whose names start with this are the story's facts, from a QuestLog.
inline constexpr std::string_view kStoryPrefix = "story.";
// No fact.
inline constexpr std::size_t kNoFact = std::numeric_limits<std::size_t>::max();

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
        None,        // ends at once
        Wait,        // ends after seconds
        GoTo,        // walks to the entity named target, ending within the agent's arrive_distance
        Emit,        // emits the world event named event, and ends at once
        FollowRoute, // walks the route named target point by point, ending at its last
    };
    Kind kind = Kind::None;
    double seconds = 0.0;
    std::string target;
    std::string event;
    float speed = 0.0F; // a go_to's or follow_route's speed; 0 is the agent's

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

// A point of a route: an entity, or a place ((x, y, 0) in 2D).
struct RoutePoint {
    std::string target;
    std::optional<Vec3> place;

    friend bool operator==(const RoutePoint&, const RoutePoint&) = default;
};

// A path an NPC walks: a patrol (closed, back to its first point) or a round.
struct Route {
    std::string name;
    std::vector<RoutePoint> points;
    bool closed = false;

    friend bool operator==(const Route&, const Route&) = default;
};

// A .npc.json file, with what it extends merged in.
struct Brain {
    std::vector<std::string> facts;      // names of facts and traits, sorted
    std::vector<int> defaults;           // each fact's starting value (false 0, true 1)
    std::vector<std::size_t> traits;     // the facts that are traits, which no action changes
    std::vector<std::size_t> story;      // the facts named story.<name>
    std::size_t hour = kNoFact;          // the fact named hour, which the clock sets
    std::size_t day = kNoFact;           // the fact named day
    std::vector<Route> routes;           // by name
    std::vector<Goal> goals;             // in the file's order
    std::vector<std::size_t> priorities; // goals' indices, highest priority first
    std::vector<Action> actions;         // in the file's order, which breaks ties between plans
    int search_limit = 2048;             // the most states a plan may look at

    // The index of a fact, or facts.size() when there is none of that name.
    [[nodiscard]] std::size_t fact(std::string_view name) const;
    // The goal of that name, or null.
    [[nodiscard]] const Goal* goal(std::string_view name) const;
    // The route of that name, or null.
    [[nodiscard]] const Route* route(std::string_view name) const;
    // Whether a fact comes from the world (the clock's and the story's) rather than the agent.
    [[nodiscard]] bool world_fact(std::size_t index) const;

    friend bool operator==(const Brain&, const Brain&) = default;
};

// JSON Schema for .npc.json files.
[[nodiscard]] json::Value brain_schema();
// A .npc.json document with the brains it extends merged in, read through reader: their facts,
// traits and routes by name, then their goals and actions by name, field by field. Errors when a
// brain doesn't read or validate, or extends itself.
[[nodiscard]] Result<json::Value> resolve_brain(const json::Value& doc,
                                                const world::FileReader& reader);
// Every error in a .npc.json document, each with the JSON Pointer of the bad value: the schema's,
// conditions and effects naming facts the file doesn't have, effects on traits, routes it lacks
// and values that don't read. A brain that extends another is checked with it merged in, read
// through reader.
[[nodiscard]] std::vector<json::ValidationError>
validate_brain(const json::Value& doc, const world::FileReader& reader = {});
// The brain in a .npc.json document, with what it extends, or its first error.
[[nodiscard]] Result<Brain> parse_brain(const json::Value& source,
                                        const world::FileReader& reader = {});
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

// Registers .npc.json, .quest.json and .dialogue.json files with assets. A brain reads what it
// extends through them, so it reloads when that changes.
void add_asset_types(asset::Assets& assets);

// The npc clock, project.json's npc settings: a day lasts day_seconds of game time (0: the clock
// stands still at start_hour), and the world starts at start_hour of day 0.
struct Settings {
    double day_seconds = 0.0;
    double start_hour = 6.0;

    friend bool operator==(const Settings&, const Settings&) = default;
};
[[nodiscard]] json::Value settings_schema();
[[nodiscard]] Settings parse_settings(const json::Value& value);
void set_settings(world::World& world, const Settings& settings);

// The time on the npc clock: whole days since the start, and the hour and minute of the day.
struct Clock {
    int day = 0;
    int hour = 0;
    int minute = 0;
};
[[nodiscard]] Clock clock(const world::World& world);

// Adds the npc system (priority 965, after navigation), the quests system (priority 966) and the
// asset types to a world.
void install(world::World& world, asset::Assets& assets, const Settings& settings = {});

// An agent's facts: its brain's defaults with its NpcAgent's facts over them. Empty when the
// entity has no agent or its brain doesn't load.
[[nodiscard]] std::vector<int> facts_of(world::World& world, entt::entity entity);
// An agent's brain, or null when the entity has no agent or its brain doesn't load (error says
// why).
[[nodiscard]] std::shared_ptr<const Brain> brain_of(world::World& world, entt::entity entity,
                                                    std::string* error = nullptr);
// Sets one of an agent's facts, as gameplay code does when the world changes. The agent thinks
// again at its next tick. A story.<name> fact sets the story's fact. False when the entity has no
// agent, its brain no such fact or the fact is a trait or the clock's.
bool set_fact(world::World& world, entt::entity entity, std::string_view fact, int value);

// Every error in a scene's or prefab's NpcAgent components, each with the JSON Pointer of the bad
// value: a brain that doesn't load or validate, and facts the brain doesn't have. Files are read
// through reader.
[[nodiscard]] std::vector<json::ValidationError> check_scene(const json::Value& doc,
                                                             const world::FileReader& reader);

} // namespace slash::npc
