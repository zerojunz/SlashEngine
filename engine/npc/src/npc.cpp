// The npc system: each tick it runs every NpcAgent's current action, then has the agents due to
// think choose a goal and plan (on the world's job pool, each from its own facts, so the result
// doesn't depend on the thread count), then starts what they chose, in entity order.

#include "slash/npc/npc.hpp"

#include "slash/core/jobs.hpp"
#include "slash/core/log.hpp"
#include "slash/navigation/navigation.hpp"
#include "slash/world/components.hpp"
#include "slash/world/tree2d.hpp"
#include "slash/world/tree3d.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace slash::npc {

namespace {

struct State {
    asset::Assets* assets = nullptr;
    // Brains whose load failed, so each is reported once until it loads.
    std::set<std::string, std::less<>> warned;
};

State* find_state(world::World& world) {
    auto* state = world.ecs().ctx().find<std::shared_ptr<State>>();
    return state != nullptr ? state->get() : nullptr;
}

std::shared_ptr<const Brain> load_brain(State& state, std::string_view path, std::string* error) {
    if (path.empty()) {
        if (error != nullptr) {
            *error = "the agent names no brain";
        }
        return nullptr;
    }
    if (state.assets == nullptr) {
        if (error != nullptr) {
            *error = "no assets";
        }
        return nullptr;
    }
    auto loaded = state.assets->load<Brain>(path);
    if (!loaded) {
        if (error != nullptr) {
            *error = std::string(path) + ": " + loaded.error().message;
        }
        return nullptr;
    }
    return loaded.value().share();
}

std::vector<int> facts_from(const Brain& brain, const world::NpcAgent& agent) {
    std::vector<int> facts = brain.defaults;
    for (const auto& [name, value] : agent.facts) {
        if (const std::size_t index = brain.fact(name); index < facts.size()) {
            facts[index] = value;
        }
    }
    return facts;
}

// Keeps in the agent only the facts that differ from the brain's defaults.
void store_facts(const Brain& brain, std::span<const int> facts, world::NpcAgent& agent) {
    for (std::size_t i = 0; i < facts.size(); ++i) {
        if (facts[i] == brain.defaults[i]) {
            agent.facts.erase(brain.facts[i]);
        } else {
            agent.facts[brain.facts[i]] = facts[i];
        }
    }
}

const Action* action_named(const Brain& brain, std::string_view name) {
    const auto found = std::ranges::find(brain.actions, name, &Action::name);
    return found == brain.actions.end() ? nullptr : &*found;
}

Vec3 toward(Vec3 from, Vec3 to, real_t step) {
    const Vec3 way = to - from;
    const real_t length = std::sqrt((way.x * way.x) + (way.y * way.y) + (way.z * way.z));
    return length <= step ? to : from + (way * (step / length));
}

real_t distance(Vec3 a, Vec3 b) {
    const Vec3 d = a - b;
    return std::sqrt((d.x * d.x) + (d.y * d.y) + (d.z * d.z));
}

// Where things are this tick, made when a go_to first needs it.
struct Places {
    explicit Places(world::World& w) : world(w) {}

    // An entity's place in the world, as (x, y, 0) in 2D; none if it has no transform.
    std::optional<Vec3> of(entt::entity entity) {
        if (world.ecs().all_of<world::Transform3D>(entity)) {
            const world::Affine3D global = tree3d().global(entity);
            return global.origin;
        }
        if (world.ecs().all_of<world::Transform2D>(entity)) {
            const Vec2 p = tree2d().global(entity).position;
            return Vec3{p.x, p.y, 0.0F};
        }
        return std::nullopt;
    }

    // Moves an entity so its place in the world is to.
    void move(entt::entity entity, Vec3 to) {
        if (auto* transform = world.ecs().try_get<world::Transform3D>(entity);
            transform != nullptr) {
            const entt::entity parent = tree3d().transform_parent(entity);
            transform->position =
                parent == entt::null ? to : tree3d().global(parent).inverse().apply(to);
            return;
        }
        if (auto* transform = world.ecs().try_get<world::Transform2D>(entity);
            transform != nullptr) {
            const entt::entity parent = tree2d().transform_parent(entity);
            const Vec2 flat{to.x, to.y};
            transform->position =
                parent == entt::null ? flat : tree2d().global_affine(parent).inverse().apply(flat);
        }
    }

    world::Tree3D& tree3d() {
        if (!three_d) {
            three_d.emplace(world);
        }
        return *three_d;
    }
    world::Tree2D& tree2d() {
        if (!two_d) {
            two_d.emplace(world);
        }
        return *two_d;
    }

    world::World& world;
    std::optional<world::Tree3D> three_d;
    std::optional<world::Tree2D> two_d;
};

enum class Progress : std::uint8_t { Running, Done, Failed };

// One tick of a go_to: along the agent's navigation path when it has one, else straight.
Progress walk(world::World& world, Places& places, entt::entity entity,
              const world::NpcAgent& agent, const Behavior& behavior, double seconds) {
    const entt::entity target = world.find(behavior.target);
    if (target == entt::null) {
        return Progress::Failed;
    }
    const std::optional<Vec3> here = places.of(entity);
    const std::optional<Vec3> there = places.of(target);
    if (!here || !there) {
        return Progress::Failed;
    }
    if (distance(*here, *there) <= static_cast<real_t>(agent.arrive_distance)) {
        return Progress::Done;
    }
    const auto step =
        static_cast<real_t>((behavior.speed > 0.0F ? behavior.speed : agent.speed) * seconds);
    Vec3 next = *there;
    if (const auto* nav = world.ecs().try_get<world::NavigationAgent3D>(entity); nav != nullptr) {
        if (distance(nav->target_position, *there) > static_cast<real_t>(0.001)) {
            navigation::set_target_position(world, entity, *there);
        }
        next = navigation::next_path_position_3d(world, entity);
    } else if (const auto* nav2 = world.ecs().try_get<world::NavigationAgent2D>(entity);
               nav2 != nullptr) {
        const Vec2 target2{there->x, there->y};
        const Vec2 d = nav2->target_position - target2;
        if ((d.x * d.x) + (d.y * d.y) > static_cast<real_t>(1e-6)) {
            navigation::set_target_position(world, entity, target2);
        }
        const Vec2 p = navigation::next_path_position_2d(world, entity);
        next = {p.x, p.y, 0.0F};
    }
    if (distance(*here, next) <= static_cast<real_t>(1e-6)) {
        // The path ends short of the target: it can't be reached from here.
        return Progress::Failed;
    }
    const Vec3 moved = toward(*here, next, step);
    places.move(entity, moved);
    return distance(moved, *there) <= static_cast<real_t>(agent.arrive_distance)
               ? Progress::Done
               : Progress::Running;
}

json::Value action_data(std::string_view action) {
    json::Value data;
    data["action"] = std::string(action);
    return data;
}

// Starts the plan's first action: its events, and an emit's own.
void start_action(world::World& world, entt::entity entity, const Brain& brain,
                  world::NpcAgent& agent) {
    agent.action_time = 0.0F;
    if (agent.plan.empty()) {
        return;
    }
    const std::string_view id = world.id_of(entity);
    world.emit("npc_action_started", id, action_data(agent.plan.front()));
    if (const Action* action = action_named(brain, agent.plan.front());
        action != nullptr && action->behavior.kind == Behavior::Kind::Emit) {
        world.emit(action->behavior.event, id, action_data(action->name));
    }
}

// What an agent due to think decides, worked out apart from the world.
struct Thought {
    entt::entity entity = entt::null;
    std::shared_ptr<const Brain> brain;
    std::vector<int> facts;
    bool replan = false; // the current plan stopped working
    Choice choice;
};

void step(world::World& world, State& state, double seconds) {
    std::vector<Thought> thoughts;
    std::optional<Places> places;
    auto view = world.ecs().view<world::NpcAgent>();
    for (const entt::entity entity : view) {
        world::NpcAgent& agent = view.get<world::NpcAgent>(entity);
        if (!agent.enabled) {
            continue;
        }
        std::string error;
        std::shared_ptr<const Brain> brain = load_brain(state, agent.brain, &error);
        if (brain == nullptr) {
            if (state.warned.insert(agent.brain).second) {
                log::warn("an NPC's brain doesn't load",
                          {{"entity", std::string(world.id_of(entity))}, {"error", error}});
            }
            continue;
        }
        state.warned.erase(agent.brain);
        std::vector<int> facts = facts_from(*brain, agent);
        bool replan = false;
        if (!agent.plan.empty()) {
            const Action* action = action_named(*brain, agent.plan.front());
            Progress progress = Progress::Failed;
            if (action != nullptr) {
                switch (action->behavior.kind) {
                case Behavior::Kind::None:
                case Behavior::Kind::Emit:
                    progress = Progress::Done;
                    break;
                case Behavior::Kind::Wait:
                    agent.action_time += static_cast<float>(seconds);
                    progress = agent.action_time >= action->behavior.seconds ? Progress::Done
                                                                             : Progress::Running;
                    break;
                case Behavior::Kind::GoTo:
                    if (!places) {
                        places.emplace(world);
                    }
                    agent.action_time += static_cast<float>(seconds);
                    progress = walk(world, *places, entity, agent, action->behavior, seconds);
                    break;
                }
            }
            const std::string_view id = world.id_of(entity);
            if (progress == Progress::Failed) {
                world.emit("npc_action_failed", id, action_data(agent.plan.front()));
                agent.plan.clear();
                agent.action_time = 0.0F;
                replan = true;
            } else if (progress == Progress::Done) {
                apply_effects(action->effects, facts);
                store_facts(*brain, facts, agent);
                world.emit("npc_action_finished", id, action_data(action->name));
                agent.plan.erase(agent.plan.begin());
                if (agent.plan.empty()) {
                    replan = true; // done: choose what next at once
                } else {
                    const Action* next = action_named(*brain, agent.plan.front());
                    if (next == nullptr || !holds(next->needs, facts)) {
                        agent.plan.clear();
                        replan = true;
                    } else {
                        start_action(world, entity, *brain, agent);
                    }
                }
                agent.action_time = 0.0F;
            }
        }
        agent.think_in = std::max(0.0F, agent.think_in - static_cast<float>(seconds));
        if (replan || agent.think_in <= 0.0F) {
            thoughts.push_back({.entity = entity,
                                .brain = std::move(brain),
                                .facts = std::move(facts),
                                .replan = replan,
                                .choice = {}});
        }
    }
    world.pool().parallel_for(thoughts.size(), [&](std::size_t i) {
        thoughts[i].choice = choose(*thoughts[i].brain, thoughts[i].facts);
    });
    for (const Thought& thought : thoughts) {
        world::NpcAgent& agent = world.ecs().get<world::NpcAgent>(thought.entity);
        agent.think_in = agent.think_interval;
        const std::string_view id = world.id_of(thought.entity);
        const std::string goal = thought.choice.goal != nullptr ? thought.choice.goal->name : "";
        if (goal == agent.goal && !agent.plan.empty() && !thought.replan) {
            continue; // still on its way to the same goal
        }
        if (goal != agent.goal) {
            json::Value data;
            data["goal"] = goal;
            data["previous"] = agent.goal;
            world.emit("npc_goal_changed", id, std::move(data));
            agent.goal = goal;
        }
        agent.plan.clear();
        for (const std::size_t action : thought.choice.plan.actions) {
            agent.plan.push_back(thought.brain->actions[action].name);
        }
        start_action(world, thought.entity, *thought.brain, agent);
    }
}

} // namespace

void add_asset_types(asset::Assets& assets) {
    assets.add_type<Brain>(
        "npc_brain", {std::string(kBrainSuffix)},
        [](std::string_view text, std::string_view /*path*/) -> Result<Brain> {
            auto doc = json::parse(text);
            if (!doc) {
                return doc.error();
            }
            return parse_brain(doc.value());
        },
        [](const Brain& brain) {
            std::size_t bytes = sizeof(brain);
            for (const std::string& fact : brain.facts) {
                bytes += sizeof(std::string) + fact.capacity() + sizeof(int);
            }
            for (const Goal& goal : brain.goals) {
                bytes += sizeof(Goal) + goal.name.capacity() +
                         ((goal.when.capacity() + goal.want.capacity()) * sizeof(Condition));
            }
            for (const Action& action : brain.actions) {
                bytes += sizeof(Action) + action.name.capacity() +
                         (action.needs.capacity() * sizeof(Condition)) +
                         (action.effects.capacity() * sizeof(Effect)) +
                         action.behavior.target.capacity() + action.behavior.event.capacity();
            }
            return bytes;
        });
}

void install(world::World& world, asset::Assets& assets) {
    add_asset_types(assets);
    if (State* state = find_state(world); state != nullptr) {
        state->assets = &assets;
        return;
    }
    auto state = std::make_shared<State>();
    state->assets = &assets;
    world.ecs().ctx().emplace<std::shared_ptr<State>>(state);
    world.add_reset([state](world::World& /*world*/) { state->warned.clear(); });
    world.add_system(
        "npc", [state](world::World& w, double tick_seconds) { step(w, *state, tick_seconds); },
        {.doc = "Runs each NpcAgent's action, then has those due to think choose a goal and plan "
                "the cheapest chain of actions to it.",
         .priority = 965});
}

std::shared_ptr<const Brain> brain_of(world::World& world, entt::entity entity,
                                      std::string* error) {
    const auto* agent =
        world.valid(entity) ? world.ecs().try_get<world::NpcAgent>(entity) : nullptr;
    if (agent == nullptr) {
        if (error != nullptr) {
            *error = "the entity has no NpcAgent";
        }
        return nullptr;
    }
    State* state = find_state(world);
    if (state == nullptr) {
        if (error != nullptr) {
            *error = "the world has no npc system";
        }
        return nullptr;
    }
    return load_brain(*state, agent->brain, error);
}

std::vector<int> facts_of(world::World& world, entt::entity entity) {
    const std::shared_ptr<const Brain> brain = brain_of(world, entity);
    if (brain == nullptr) {
        return {};
    }
    return facts_from(*brain, world.ecs().get<world::NpcAgent>(entity));
}

bool set_fact(world::World& world, entt::entity entity, std::string_view fact, int value) {
    const std::shared_ptr<const Brain> brain = brain_of(world, entity);
    if (brain == nullptr) {
        return false;
    }
    const std::size_t index = brain->fact(fact);
    if (index >= brain->facts.size()) {
        return false;
    }
    auto& agent = world.ecs().get<world::NpcAgent>(entity);
    std::vector<int> facts = facts_from(*brain, agent);
    facts[index] = value;
    store_facts(*brain, facts, agent);
    agent.think_in = 0.0F; // think again at the next tick
    return true;
}

std::vector<json::ValidationError> check_scene(const json::Value& doc,
                                               const world::FileReader& reader) {
    std::vector<json::ValidationError> errors;
    if (!reader) {
        return errors;
    }
    // Each file's brain, or its error.
    std::map<std::string, std::pair<std::optional<Brain>, std::string>, std::less<>> checked;
    const auto check_file = [&](const std::string& path) -> const auto& {
        if (const auto found = checked.find(path); found != checked.end()) {
            return found->second;
        }
        std::pair<std::optional<Brain>, std::string> out;
        if (!path.ends_with(kBrainSuffix)) {
            out.second = "NPC brains are .npc.json files";
        } else if (auto text = reader(path); !text) {
            out.second = path + ": " + text.error().message;
        } else if (auto parsed = json::parse(text.value()); !parsed) {
            out.second = path + ": " + parsed.error().message;
        } else if (auto brain = parse_brain(parsed.value()); !brain) {
            out.second = path + ": " + brain.error().message;
        } else {
            out.first = std::move(brain).value();
        }
        return checked.emplace(path, std::move(out)).first->second;
    };
    const auto check = [&](const json::Value& components, const std::string& at) {
        const json::Value* agent = components.find("NpcAgent");
        if (agent == nullptr || !agent->is_object()) {
            return;
        }
        const json::Value* path = agent->find("brain");
        if (path == nullptr || !path->is_string() || path->as_string().empty()) {
            errors.push_back({at + "/NpcAgent/brain", "an NpcAgent needs a .npc.json brain"});
            return;
        }
        const auto& [brain, error] = check_file(path->as_string());
        if (!brain) {
            errors.push_back({at + "/NpcAgent/brain", error});
            return;
        }
        if (const json::Value* facts = agent->find("facts");
            facts != nullptr && facts->is_object()) {
            for (const auto& [name, value] : facts->as_object()) {
                if (brain->fact(name) >= brain->facts.size()) {
                    errors.push_back({at + "/NpcAgent/facts/" + json::pointer_escape(name),
                                      "the brain has no fact named \"" + name + "\""});
                }
            }
        }
        if (const json::Value* goal = agent->find("goal");
            goal != nullptr && goal->is_string() && !goal->as_string().empty() &&
            brain->goal(goal->as_string()) == nullptr) {
            errors.push_back({at + "/NpcAgent/goal",
                              "the brain has no goal named \"" + goal->as_string() + "\""});
        }
        if (const json::Value* plan = agent->find("plan"); plan != nullptr && plan->is_array()) {
            const json::Array& steps = plan->as_array();
            for (std::size_t i = 0; i < steps.size(); ++i) {
                if (steps[i].is_string() && action_named(*brain, steps[i].as_string()) == nullptr) {
                    errors.push_back(
                        {at + "/NpcAgent/plan/" + std::to_string(i),
                         "the brain has no action named \"" + steps[i].as_string() + "\""});
                }
            }
        }
    };
    if (const json::Value* entities = doc.find("entities"); entities != nullptr) {
        for (const auto& [id, entity] : entities->as_object()) {
            if (const json::Value* components = entity.find("components"); components != nullptr) {
                check(*components, "/entities/" + json::pointer_escape(id) + "/components");
            }
        }
    } else if (const json::Value* components = doc.find("components"); components != nullptr) {
        check(*components, "/components");
    }
    return errors;
}

} // namespace slash::npc
