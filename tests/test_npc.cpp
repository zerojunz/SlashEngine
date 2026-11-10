#include "slash/asset/assets.hpp"
#include "slash/core/jobs.hpp"
#include "slash/core/json.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/navigation/navigation.hpp"
#include "slash/npc/npc.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/world.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace asset = slash::asset;
namespace json = slash::json;
namespace jobs = slash::jobs;
namespace npc = slash::npc;
namespace world = slash::world;

namespace {

constexpr double kTick = 1.0 / 60.0;

struct MemoryFiles {
    std::map<std::string, std::string, std::less<>> files;

    asset::FileSource source() {
        return {
            .read = [this](std::string_view path) -> slash::Result<std::string> {
                const auto it = files.find(path);
                if (it == files.end()) {
                    return slash::make_error(slash::ErrorCode::NotFound, "no such file");
                }
                return it->second;
            },
            .stamp = [this](std::string_view path) -> std::optional<asset::FileStamp> {
                const auto it = files.find(path);
                if (it == files.end()) {
                    return std::nullopt;
                }
                return asset::FileStamp{.modified = 1, .size = it->second.size()};
            },
        };
    }
};

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE(parsed.ok());
    return std::move(parsed).value();
}

npc::Brain brain_of(std::string_view text) {
    auto brain = npc::parse_brain(parse(text));
    REQUIRE(brain.ok());
    return std::move(brain).value();
}

// A villager who eats when hungry and sleeps when tired, at home.
constexpr std::string_view kVillager = R"({"format": 1,
    "facts": {"hungry": true, "has_food": false, "food_stock": 2, "tired": false,
              "at_home": false},
    "goals": [
        {"name": "rest", "priority": 1, "when": {"tired": true}, "want": {"tired": false}},
        {"name": "eat", "priority": 2, "when": {"hungry": true}, "want": {"hungry": false}},
        {"name": "go_home", "want": {"at_home": true}}],
    "actions": [
        {"name": "buy_food", "cost": 4, "needs": {"has_food": false},
         "effects": {"has_food": true}, "do": {"emit": "buy"}},
        {"name": "take_food", "needs": {"has_food": false, "food_stock": ">0"},
         "effects": {"has_food": true, "food_stock": "-1"}, "do": {"go_to": "pantry"}},
        {"name": "eat", "needs": {"has_food": true},
         "effects": {"has_food": false, "hungry": false}, "do": {"wait": 1}},
        {"name": "walk_home", "effects": {"at_home": true}, "do": {"go_to": "home"}},
        {"name": "sleep", "cost": 2, "needs": {"at_home": true}, "effects": {"tired": false},
         "do": {"wait": 2}}]})";

std::vector<std::string> names_of(const npc::Brain& brain, const npc::Plan& plan) {
    std::vector<std::string> out;
    for (const std::size_t action : plan.actions) {
        out.push_back(brain.actions[action].name);
    }
    return out;
}

using Names = std::vector<std::string>;

// A world with the built-in components, navigation and the npc system, and the events it emits.
struct Scene {
    MemoryFiles files;
    asset::Assets assets{files.source()};
    world::ComponentRegistry components;
    world::World world{components};
    std::vector<world::Event> events;

    Scene() {
        world::register_builtin_components(components);
        slash::navigation::install(world, assets);
        npc::install(world, assets);
        world.set_event_listener([this](const world::Event& event) { events.push_back(event); });
    }

    entt::entity create(std::string_view id, std::string_view components_json) {
        const entt::entity entity = world.create(id).value();
        const json::Value values = parse(components_json);
        for (const auto& [name, fields] : values.as_object()) {
            std::string path;
            REQUIRE(components.find(name)->assign(world, entity, fields, path).ok());
        }
        return entity;
    }

    // The npc events about an entity, as "kind action-or-goal".
    Names seen(std::string_view entity) const {
        Names out;
        for (const world::Event& event : events) {
            if (event.id != entity || event.kind.starts_with("entity.") ||
                event.kind.starts_with("component.")) {
                continue;
            }
            std::string line = event.kind;
            if (const json::Value* action = event.data.find("action"); action != nullptr) {
                line += " " + action->as_string();
            } else if (const json::Value* goal = event.data.find("goal"); goal != nullptr) {
                line += " " + goal->as_string();
            }
            out.push_back(std::move(line));
        }
        return out;
    }

    void run(double seconds) {
        for (double t = 0.0; t < seconds - 1e-9; t += kTick) {
            world.step(kTick);
        }
    }
};

} // namespace

TEST_CASE("npc brains are validated with the JSON Pointer of each error") {
    CHECK(npc::validate_brain(parse(kVillager)).empty());
    CHECK(json::validate(parse(kVillager), npc::brain_schema()).empty());

    const auto errors = npc::validate_brain(parse(R"({"format": 1,
        "facts": {"hungry": true, "food": 0},
        "goals": [{"name": "eat", "want": {"hungyr": false}},
                  {"name": "eat", "when": {"food": "=>1"}, "want": {"hungry": false}}],
        "actions": [{"name": "a", "effects": {"food": "*2"}},
                    {"name": "a", "do": {"wait": 1, "emit": "x"}},
                    {"name": "b", "do": {"wait": 1, "speed": 2}}]})"));
    REQUIRE(errors.size() == 7);
    CHECK(errors[0].path == "/goals/0/want/hungyr");
    CHECK(errors[0].message == "no fact named \"hungyr\" in facts; did you mean \"hungry\"?");
    CHECK(errors[1].path == "/goals/1/name");
    CHECK(errors[1].message == "another goal has this name");
    CHECK(errors[2].path == "/goals/1/when/food");
    CHECK(errors[3].path == "/actions/0/effects/food");
    CHECK(errors[3].message ==
          "expected true, false, a whole number, or a change such as \"+1\" or \"-2\"");
    CHECK(errors[4].path == "/actions/1/name");
    CHECK(errors[5].path == "/actions/1/do");
    CHECK(errors[6].path == "/actions/2/do/speed");

    // The schema's errors come first, and alone.
    const auto schema_errors = npc::validate_brain(parse(R"({"format": 1, "facts": {},
        "goals": [], "actions": [{"name": "a", "cost": 0}]})"));
    REQUIRE(schema_errors.size() == 1);
    CHECK(schema_errors[0].path == "/actions/0/cost");

    const npc::Brain brain = brain_of(kVillager);
    CHECK(brain.facts == Names{"at_home", "food_stock", "has_food", "hungry", "tired"});
    CHECK(brain.defaults == std::vector<int>{0, 2, 0, 1, 0});
    REQUIRE(brain.goals.size() == 3);
    CHECK(brain.priorities == std::vector<std::size_t>{1, 0, 2});
    CHECK(brain.actions[1].needs[0].compare == npc::Compare::Greater); // in name order
    CHECK(brain.actions[1].effects[0] == npc::Effect{.fact = 1, .add = true, .value = -1});
    CHECK(brain.actions[1].behavior.kind == npc::Behavior::Kind::GoTo);
    CHECK(brain.actions[0].behavior.event == "buy");
    CHECK(brain.search_limit == 2048);
}

TEST_CASE("plans are the cheapest chain of actions, the same every time") {
    const npc::Brain brain = brain_of(kVillager);
    const npc::Goal& eat = *brain.goal("eat");
    std::vector<int> facts = brain.defaults;

    // Taking food from the pantry is cheaper than buying it.
    npc::Plan plan = npc::plan(brain, facts, eat);
    REQUIRE(plan.found);
    CHECK(names_of(brain, plan) == Names{"take_food", "eat"});
    CHECK(plan.cost == 2.0F);

    // With the pantry empty, it buys.
    facts[brain.fact("food_stock")] = 0;
    plan = npc::plan(brain, facts, eat);
    CHECK(names_of(brain, plan) == Names{"buy_food", "eat"});
    CHECK(plan.cost == 5.0F);

    // A goal already met takes no actions.
    facts[brain.fact("hungry")] = 0;
    plan = npc::plan(brain, facts, eat);
    CHECK(plan.found);
    CHECK(plan.actions.empty());

    // Effects add up to the integers' limits.
    std::vector<int> numbers{2147483600};
    npc::apply_effects(std::vector<npc::Effect>{{.fact = 0, .add = true, .value = 100}}, numbers);
    CHECK(numbers[0] == 2147483647);

    // Of two chains of equal cost, the one whose first action comes first in the file wins.
    const npc::Brain tied = brain_of(R"({"format": 1, "facts": {"a": 0, "b": 0, "done": false},
        "goals": [{"name": "g", "want": {"done": true}}],
        "actions": [{"name": "via_b", "needs": {"b": 1}, "effects": {"done": true}},
                    {"name": "set_a", "effects": {"a": 1}},
                    {"name": "set_b", "effects": {"b": 1}},
                    {"name": "via_a", "needs": {"a": 1}, "effects": {"done": true}}]})");
    const npc::Plan first = npc::plan(tied, tied.defaults, tied.goals[0]);
    CHECK(names_of(tied, first) == Names{"set_a", "via_a"});
    for (int i = 0; i < 4; ++i) {
        CHECK(npc::plan(tied, tied.defaults, tied.goals[0]).actions == first.actions);
    }

    // Counting needs several steps; a search limit too small for them finds nothing.
    const std::string_view counting = R"({"format": 1, "facts": {"wood": 0},
        "goals": [{"name": "stack", "want": {"wood": ">=5"}}],
        "actions": [{"name": "chop", "effects": {"wood": "+1"}},
                    {"name": "chop_two", "cost": 3, "effects": {"wood": "+2"}}]})";
    const npc::Brain wood = brain_of(counting);
    plan = npc::plan(wood, wood.defaults, wood.goals[0]);
    CHECK(names_of(wood, plan) == Names{"chop", "chop", "chop", "chop", "chop"});
    CHECK(plan.cost == 5.0F);
    json::Value limited = parse(counting);
    limited["search_limit"] = 3;
    const npc::Brain small = npc::parse_brain(limited).value();
    plan = npc::plan(small, small.defaults, small.goals[0]);
    CHECK(!plan.found);
    CHECK(plan.searched == 3);
}

TEST_CASE("an npc chooses the first goal by priority that applies, is unmet and can be reached") {
    const npc::Brain brain = brain_of(kVillager);
    std::vector<int> facts = brain.defaults;
    facts[brain.fact("tired")] = 1;
    npc::Choice choice = npc::choose(brain, facts);
    REQUIRE(choice.goal != nullptr);
    CHECK(choice.goal->name == "eat"); // priority 2 before rest's 1

    facts[brain.fact("hungry")] = 0;
    choice = npc::choose(brain, facts);
    REQUIRE(choice.goal != nullptr);
    CHECK(choice.goal->name == "rest");
    CHECK(names_of(brain, choice.plan) == Names{"walk_home", "sleep"});

    facts[brain.fact("tired")] = 0;
    facts[brain.fact("at_home")] = 1;
    CHECK(npc::choose(brain, facts).goal == nullptr);

    // A goal no chain reaches is passed over.
    const npc::Brain stuck = brain_of(R"({"format": 1, "facts": {"x": false, "y": false},
        "goals": [{"name": "x", "priority": 5, "want": {"x": true}},
                  {"name": "y", "want": {"y": true}}],
        "actions": [{"name": "make_y", "effects": {"y": true}}]})");
    choice = npc::choose(stuck, stuck.defaults);
    REQUIRE(choice.goal != nullptr);
    CHECK(choice.goal->name == "y");
    const auto warnings = npc::check_brain(stuck);
    REQUIRE(warnings.size() == 1);
    CHECK(warnings[0].path == "/goals/0");
    CHECK(warnings[0].message == "goal \"x\" is out of reach from the facts' defaults");

    // The villager's buy_food is a fallback no plan from the defaults takes.
    const auto villager = npc::check_brain(brain);
    REQUIRE(villager.size() == 1);
    CHECK(villager[0].path == "/actions/0");
}

TEST_CASE("the npc system walks, waits and emits through its plan and keeps state in NpcAgent") {
    Scene scene;
    scene.files.files["villager.npc.json"] = std::string(kVillager);
    scene.create("pantry", R"({"Transform3D": {"position": [3, 0, 0]}})");
    scene.create("home", R"({"Transform3D": {"position": [0, 0, 4]}})");
    const entt::entity ann =
        scene.create("ann", R"({"Transform3D": {}, "NpcAgent": {"brain": "villager.npc.json",
                   "speed": 2, "arrive_distance": 0.25}})");

    scene.world.step(kTick);
    auto& agent = scene.world.ecs().get<world::NpcAgent>(ann);
    CHECK(agent.goal == "eat");
    CHECK(agent.plan == Names{"take_food", "eat"});
    CHECK(scene.seen("ann") == Names{"npc_goal_changed eat", "npc_action_started take_food"});

    // 2.75 m at 2 m a second, then a second's wait.
    scene.run(1.5);
    CHECK(agent.plan == Names{"eat"});
    const auto& place = scene.world.ecs().get<world::Transform3D>(ann).position;
    CHECK(place.x == doctest::Approx(2.75).epsilon(0.02));
    CHECK(agent.facts == std::map<std::string, int>{{"food_stock", 1}, {"has_food", 1}});
    // Fed, it chooses what next at once: nothing to do but go home.
    scene.run(1.1);
    CHECK(agent.goal == "go_home");
    CHECK(agent.plan == Names{"walk_home"});
    scene.run(3.0);
    CHECK(agent.facts ==
          std::map<std::string, int>{{"at_home", 1}, {"food_stock", 1}, {"hungry", 0}});
    CHECK(scene.seen("ann") == Names{"npc_goal_changed eat", "npc_action_started take_food",
                                     "npc_action_finished take_food", "npc_action_started eat",
                                     "npc_action_finished eat", "npc_goal_changed go_home",
                                     "npc_action_started walk_home",
                                     "npc_action_finished walk_home", "npc_goal_changed "});

    // Gameplay code sets a fact; the agent thinks again at the next tick.
    CHECK(npc::set_fact(scene.world, ann, "tired", 1));
    CHECK(!npc::set_fact(scene.world, ann, "sleepy", 1));
    scene.world.step(kTick);
    CHECK(agent.goal == "rest");
    CHECK(agent.plan == Names{"sleep"});
    CHECK(npc::facts_of(scene.world, ann) == std::vector<int>{1, 1, 0, 0, 1});
}

TEST_CASE("an npc whose action fails says so and plans again") {
    Scene scene;
    scene.files.files["villager.npc.json"] = std::string(kVillager);
    const entt::entity bob =
        scene.create("bob", R"({"Transform3D": {}, "NpcAgent": {"brain": "villager.npc.json"}})");
    scene.world.step(kTick);
    CHECK(scene.world.ecs().get<world::NpcAgent>(bob).plan == Names{"take_food", "eat"});
    // There is no pantry, so the walk fails; with food still in stock it tries again.
    scene.world.step(kTick);
    Names seen = scene.seen("bob");
    REQUIRE(seen.size() >= 4);
    CHECK(seen[2] == "npc_action_failed take_food");
    CHECK(seen[3] == "npc_action_started take_food");

    // An emit action emits its event about the agent, then ends.
    npc::set_fact(scene.world, bob, "food_stock", 0);
    scene.events.clear();
    scene.world.step(kTick); // the walk fails again, so it thinks with the new facts
    scene.world.step(kTick);
    seen = scene.seen("bob");
    CHECK(std::ranges::find(seen, "buy buy_food") != seen.end());
    CHECK(std::ranges::find(seen, "npc_action_finished buy_food") != seen.end());

    // A brain that doesn't load is warned about once and leaves the agent alone.
    scene.create("eve", R"({"NpcAgent": {"brain": "missing.npc.json"}})");
    scene.world.step(kTick);
    scene.world.step(kTick);
    CHECK(scene.seen("eve").empty());
}

TEST_CASE("npc agents plan the same on any number of threads") {
    const auto run = [](int threads) {
        jobs::Pool pool(threads);
        Scene scene;
        scene.world.set_pool(pool);
        scene.files.files["villager.npc.json"] = std::string(kVillager);
        scene.create("pantry", R"({"Transform3D": {"position": [3, 0, 0]}})");
        scene.create("home", R"({"Transform3D": {"position": [0, 0, 4]}})");
        for (int i = 0; i < 64; ++i) {
            const std::string facts = i % 3 == 0   ? R"({"tired": 1})"
                                      : i % 3 == 1 ? R"({"hungry": 0, "tired": 1})"
                                                   : "{}";
            scene.create("npc" + std::to_string(i),
                         R"({"Transform3D": {"position": [)" + std::to_string(i % 8) + ", 0, " +
                             std::to_string(i / 8) +
                             R"(]}, "NpcAgent": {"brain": "villager.npc.json", "facts": )" + facts +
                             "}}");
        }
        scene.run(4.0);
        std::vector<std::pair<world::NpcAgent, world::Transform3D>> out;
        for (int i = 0; i < 64; ++i) {
            const entt::entity e = scene.world.find("npc" + std::to_string(i));
            out.emplace_back(scene.world.ecs().get<world::NpcAgent>(e),
                             scene.world.ecs().get<world::Transform3D>(e));
        }
        return std::make_pair(out, scene.events.size());
    };
    const auto none = run(0);
    CHECK(none == run(3));
    CHECK(none.second > 64);
}

TEST_CASE("scenes' npc agents are checked against their brains") {
    MemoryFiles files;
    files.files["villager.npc.json"] = std::string(kVillager);
    files.files["broken.npc.json"] = R"({"format": 1, "facts": {}, "goals": [],
        "actions": [{"name": "a", "needs": {"x": 1}}]})";
    const world::FileReader reader = [&](std::string_view path) -> slash::Result<std::string> {
        const auto it = files.files.find(path);
        if (it == files.files.end()) {
            return slash::make_error(slash::ErrorCode::NotFound, "no such file");
        }
        return it->second;
    };
    const auto errors = npc::check_scene(parse(R"({"entities": {
        "a": {"components": {"NpcAgent": {"brain": "villager.npc.json",
              "facts": {"hungri": 0}, "goal": "sleep", "plan": ["eat", "nap"]}}},
        "b": {"components": {"NpcAgent": {"brain": "broken.npc.json"}}},
        "c": {"components": {"NpcAgent": {}}},
        "d": {"components": {"NpcAgent": {"brain": "brain.json"}}}}})"),
                                         reader);
    REQUIRE(errors.size() == 6);
    CHECK(errors[0].path == "/entities/a/components/NpcAgent/facts/hungri");
    CHECK(errors[1].path == "/entities/a/components/NpcAgent/goal");
    CHECK(errors[2].path == "/entities/a/components/NpcAgent/plan/1");
    CHECK(errors[3].path == "/entities/b/components/NpcAgent/brain");
    CHECK(errors[3].message.starts_with("broken.npc.json: /actions/0/needs/x"));
    CHECK(errors[4].path == "/entities/c/components/NpcAgent/brain");
    CHECK(errors[5].message == "NPC brains are .npc.json files");
}
