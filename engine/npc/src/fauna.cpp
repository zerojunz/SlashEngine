// The fauna system: creatures that roam, herd, flee, hunt, eat from hands, follow their owners
// and carry riders, in 3D and 2D.
#include "slash/npc/fauna.hpp"

#include "slash/core/log.hpp"
#include "slash/npc/npc.hpp"
#include "slash/world/character.hpp"
#include "slash/world/creature.hpp"
#include "slash/world/tree2d.hpp"
#include "slash/world/tree3d.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace slash::npc {
namespace {

using world::CreatureMind;
using world::CreatureRecipe;

constexpr real_t kMoving = 0.05F; // meters a second below which it stands
constexpr real_t kNear = 1.5F;    // how near a friendly or tame creature comes, in meters
constexpr real_t kFollow = 3.0F;  // how far its owner goes before it follows
constexpr float kScared = 3.0F;   // seconds scare makes it flee
constexpr float kEating = 2.0F;   // seconds it eats what it is fed or caught
constexpr real_t kHungry = 0.5F;  // the hunger past which hunters hunt

real_t dot(Vec3 a, Vec3 b) {
    return (a.x * b.x) + (a.y * b.y) + (a.z * b.z);
}

real_t length(Vec3 v) {
    return std::sqrt(dot(v, v));
}

Vec3 normalized(Vec3 v) {
    const real_t l = length(v);
    return l > 0.0F ? v * (1.0F / l) : Vec3{};
}

// A number from 0 to 1, the same in every run: a hash of the creature's ID, the tick and salt.
float random(std::string_view id, std::uint64_t tick, std::uint64_t salt) {
    std::uint64_t h = 1469598103934665603ULL;
    for (const char c : id) {
        h = (h ^ static_cast<unsigned char>(c)) * 1099511628211ULL;
    }
    h ^= (tick * 0x9E3779B97F4A7C15ULL) + (salt * 0xBF58476D1CE4E5B9ULL);
    h = (h ^ (h >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    h = (h ^ (h >> 27U)) * 0x94D049BB133111EBULL;
    h ^= h >> 31U;
    return static_cast<float>(h >> 40U) / static_cast<float>(1U << 24U);
}

// A creature or a person, where it is this tick in meters with y up (2D: in pixels, turned into
// meters by each creature's own scale).
struct Thing {
    entt::entity entity{entt::null};
    bool two_d = false;
    Vec3 place;     // 3D: meters; 2D: pixels with y down
    real_t ppm = 1; // a 2D creature's pixels a meter
    std::shared_ptr<const CreatureRecipe> recipe;
};

// What the system keeps between ticks outside components: the facts it last gave each agent and
// the problems it has reported.
struct FaunaState {
    std::map<entt::entity, std::array<int, 3>> facts;
    std::map<entt::entity, std::string> warned;
};
using FaunaStateHolder = std::shared_ptr<FaunaState>;

bool hunts(const CreatureRecipe& hunter, const CreatureRecipe& prey) {
    return std::ranges::any_of(hunter.behavior.hunts, [&](const std::string& name) {
        return name == prey.kind || name == prey.species;
    });
}

std::string_view id_or_empty(const world::World& world, entt::entity entity) {
    return entity != entt::null && world.valid(entity) ? world.id_of(entity) : std::string_view{};
}

struct Tick {
    world::World& world;
    double dt = 0.0;
    std::vector<Thing> creatures;
    std::vector<Thing> people;
};

// Where other is, in this creature's meters.
Vec3 seen(const Thing& self, const Thing& other) {
    if (!self.two_d) {
        return other.place;
    }
    return {other.place.x / self.ppm, -other.place.y / self.ppm, 0.0F};
}

const Thing* find_thing(const std::vector<Thing>& things, entt::entity entity) {
    const auto found =
        std::ranges::find_if(things, [&](const Thing& t) { return t.entity == entity; });
    return found != things.end() ? &*found : nullptr;
}

// The nearest of things in self's dimension within range that pass keep, and how far.
template <typename Keep>
std::pair<const Thing*, real_t> nearest(const Thing& self, Vec3 at,
                                        const std::vector<Thing>& things, real_t range, Keep keep) {
    const Thing* best = nullptr;
    real_t best_distance = range;
    for (const Thing& other : things) {
        if (other.entity == self.entity || other.two_d != self.two_d || !keep(other)) {
            continue;
        }
        const real_t d = length(seen(self, other) - at);
        if (d <= best_distance) {
            best = &other;
            best_distance = d;
        }
    }
    return {best, best_distance};
}

CreatureMind& mind_of(world::World& world, const Thing& thing) {
    return thing.two_d ? world.ecs().get<world::Creature2D>(thing.entity).mind
                       : world.ecs().get<world::Creature3D>(thing.entity).mind;
}

void emit(world::World& world, std::string kind, entt::entity creature, std::string_view key,
          entt::entity other) {
    json::Value data;
    data[std::string(key)] = std::string(id_or_empty(world, other));
    world.emit(std::move(kind), world.id_of(creature), std::move(data));
}

// Which animation fits what it is doing, and how fast to play it.
std::pair<std::string, float> animation_for(const CreatureRecipe& recipe, const CreatureMind& mind,
                                            real_t speed) {
    const world::CreatureBehavior& b = recipe.behavior;
    if (world::creature_flies(recipe)) {
        return {"fly", 1.0F};
    }
    if (world::creature_swims(recipe)) {
        return {"swim", std::clamp(static_cast<float>(speed / b.walk_speed), 0.3F, 3.0F)};
    }
    if (mind.mode == "attacking" && mind.timer > b.attack_cooldown - 0.6F) {
        return {"attack", 1.0F};
    }
    if (speed > kMoving * 2.0F) {
        if (speed > (b.walk_speed + b.run_speed) * 0.5F) {
            return {"run", static_cast<float>(speed / b.run_speed)};
        }
        return {"walk", static_cast<float>(speed / b.walk_speed)};
    }
    if (mind.mode == "grazing" || mind.mode == "eating") {
        return {"eat", 1.0F};
    }
    return {"idle", 1.0F};
}

// Turns a 2D creature's cut-out bones as the animation poses them at its gait.
void pose_cutout(world::World& world, entt::entity entity, const CreatureRecipe& recipe,
                 std::string_view animation, double time) {
    auto& ecs = world.ecs();
    const std::string owner(world.id_of(entity));
    const std::vector<world::CreatureTurn> turns = world::creature_pose(recipe, animation, time);
    for (const world::CreatureBone& bone : world::creature_skeleton(recipe)) {
        const entt::entity part = world.find(world::creature_bone_id(owner, bone.name));
        if (part == entt::null || !ecs.all_of<world::Transform2D>(part)) {
            continue;
        }
        // Seen from the side: pitches turn in the picture, a tail's wag shows half as much and a
        // wing's flap as its tip rising and falling.
        float angle = 0.0F;
        const bool wing = bone.name.starts_with("wing");
        for (const world::CreatureTurn& turn : turns) {
            if (turn.bone != bone.name) {
                continue;
            }
            if (turn.axis.x != 0.0F) {
                angle += turn.angle;
            } else if (turn.axis.y != 0.0F && !wing) {
                angle += turn.angle * 0.5F;
            } else if (turn.axis.z != 0.0F && animation == "fly") {
                angle += turn.angle * 0.8F;
            }
        }
        auto& transform = ecs.get<world::Transform2D>(part);
        if (transform.rotation != angle) {
            transform.rotation = angle;
        }
    }
}

// Keeps its rider on its back.
void carry_rider(world::World& world, const Thing& self, const CreatureMind& mind,
                 const CreatureRecipe& recipe) {
    auto& ecs = world.ecs();
    if (mind.rider == entt::null || !world.valid(mind.rider)) {
        return;
    }
    if (!self.two_d && ecs.all_of<world::Transform3D>(mind.rider)) {
        const auto& body = ecs.get<const world::Transform3D>(self.entity);
        auto& rider = ecs.get<world::Transform3D>(mind.rider);
        rider.position = body.position + Vec3{0.0F, recipe.body.height, 0.0F};
        rider.rotation.y = body.rotation.y;
        if (auto* character = ecs.try_get<world::CharacterBody3D>(mind.rider);
            character != nullptr) {
            character->velocity = {};
        }
    } else if (self.two_d && ecs.all_of<world::Transform2D>(mind.rider)) {
        const auto& body = ecs.get<const world::Transform2D>(self.entity);
        auto& rider = ecs.get<world::Transform2D>(mind.rider);
        rider.position = body.position + Vec2{0.0F, -recipe.body.height * self.ppm};
        if (auto* character = ecs.try_get<world::CharacterBody2D>(mind.rider);
            character != nullptr) {
            character->velocity = {};
        }
    }
}

// Decides what one creature does this tick and moves it.
void step_creature(Tick& tick, FaunaState& state, const Thing& self) {
    world::World& world = tick.world;
    auto& ecs = world.ecs();
    const CreatureRecipe& recipe = *self.recipe;
    const world::CreatureBehavior& b = recipe.behavior;
    const auto dt = static_cast<float>(tick.dt);
    const std::string_view id = world.id_of(self.entity);
    const Vec3 at = seen(self, self);
    const bool flies = world::creature_flies(recipe);
    const bool swims = world::creature_swims(recipe);
    const bool free_moving = flies || swims;
    CreatureMind& mind = mind_of(world, self);
    if (!mind.homed) {
        mind.homed = true;
        mind.home = at;
        mind.goal = at;
        mind.timer = 0.0F;
    }
    mind.hunger = std::min(1.0F, mind.hunger + (b.hunger_rate * dt));
    mind.timer -= dt;
    const std::string before = mind.mode;
    const entt::entity target_before = mind.target;
    Vec3 want;
    const auto toward = [&](Vec3 place, real_t speed) {
        Vec3 way = place - at;
        if (!free_moving) {
            way.y = 0.0F;
        }
        return normalized(way) * speed;
    };
    const auto is_person = [&](entt::entity e) { return find_thing(tick.people, e) != nullptr; };
    const auto place_of = [&](entt::entity e) -> std::optional<Vec3> {
        const Thing* thing = find_thing(tick.people, e);
        if (thing == nullptr) {
            thing = find_thing(tick.creatures, e);
        }
        if (thing == nullptr || thing->two_d != self.two_d) {
            return std::nullopt;
        }
        return seen(self, *thing);
    };
    const bool tame = mind.owner != entt::null && world.valid(mind.owner);
    if (mind.owner != entt::null && !tame) {
        mind.owner = entt::null;
    }

    bool decided = false;
    if (mind.mode == "caught") {
        decided = true;
    }
    // Ridden: its rider steers.
    if (!decided && mind.rider != entt::null) {
        if (!world.valid(mind.rider)) {
            const entt::entity rider = mind.rider;
            mind.rider = entt::null;
            mind.mode = "idle";
            emit(world, "creature_dismounted", self.entity, "rider", rider);
        } else {
            mind.mode = "ridden";
            Vec3 steer = mind.steer;
            if (!free_moving) {
                steer.y = 0.0F;
            }
            if (length(steer) > 1.0F) {
                steer = normalized(steer);
            }
            want = steer * b.run_speed;
            decided = true;
        }
    }
    // Fleeing what it was scared by, what hunts it and people, by its temperament.
    if (!decided) {
        const real_t notice = b.notice;
        const auto [hunter, hunter_distance] =
            nearest(self, at, tick.creatures, notice, [&](const Thing& other) {
                return hunts(*other.recipe, recipe) && mind_of(world, other).mode != "caught";
            });
        real_t person_range = 0.0F;
        if (!tame && b.temperament == "timid") {
            person_range = notice;
        } else if (!tame && b.temperament == "wary") {
            person_range = std::min<real_t>(b.flee, notice);
        }
        const auto [person, person_distance] = nearest(self, at, tick.people, person_range,
                                                       [](const Thing& /*other*/) { return true; });
        entt::entity threat = entt::null;
        if (hunter != nullptr && (person == nullptr || hunter_distance <= person_distance)) {
            threat = hunter->entity;
        } else if (person != nullptr) {
            threat = person->entity;
        }
        // Keeps fleeing until well clear, or until a scare wears off.
        if (threat == entt::null && mind.mode == "fleeing" && mind.target != entt::null) {
            const auto from = place_of(mind.target);
            const real_t clear = std::max<real_t>(std::max<real_t>(b.flee, notice), 1.0F) * 1.5F;
            if (from && (mind.timer > 0.0F || length(*from - at) < clear)) {
                threat = mind.target;
            }
        }
        if (threat != entt::null) {
            if (const auto from = place_of(threat)) {
                mind.mode = "fleeing";
                mind.target = threat;
                want = toward(at + (at - *from), b.run_speed);
                if (free_moving) {
                    want.y = flies ? b.run_speed * 0.3F : 0.0F;
                }
                decided = true;
            }
        }
    }
    // Attacking people it notices, if aggressive.
    if (!decided && b.temperament == "aggressive") {
        real_t range = b.notice;
        if (mind.mode == "attacking") {
            range *= 1.5F;
        }
        const auto [person, distance] =
            nearest(self, at, tick.people, range,
                    [&](const Thing& other) { return other.entity != mind.owner; });
        if (person != nullptr) {
            mind.mode = "attacking";
            mind.target = person->entity;
            if (distance > b.reach) {
                want = toward(seen(self, *person), b.run_speed);
            } else if (mind.timer <= 0.0F) {
                mind.timer = b.attack_cooldown;
                json::Value data;
                data["target"] = std::string(world.id_of(person->entity));
                data["damage"] = static_cast<double>(b.damage);
                world.emit("creature_attacked", id, std::move(data));
            }
            decided = true;
        }
    }
    // Hunting, once hungry.
    if (!decided && !b.hunts.empty() && (mind.hunger > kHungry || mind.mode == "hunting")) {
        const auto [prey, distance] =
            nearest(self, at, tick.creatures, b.notice, [&](const Thing& other) {
                return hunts(recipe, *other.recipe) && mind_of(world, other).mode != "caught";
            });
        if (prey != nullptr) {
            mind.mode = "hunting";
            mind.target = prey->entity;
            if (distance > b.reach) {
                want = toward(seen(self, *prey), b.run_speed);
            } else {
                CreatureMind& caught = mind_of(world, *prey);
                caught.mode = "caught";
                caught.velocity = {};
                caught.rider = entt::null;
                mind.hunger = 0.0F;
                mind.mode = "eating";
                mind.timer = kEating;
                emit(world, "creature_caught", self.entity, "prey", prey->entity);
            }
            decided = true;
        } else if (mind.mode == "hunting") {
            mind.mode = "idle";
            mind.target = entt::null;
        }
    }
    // Following its owner, or coming to people when friendly.
    if (!decided && mind.mode != "eating") {
        entt::entity lead = entt::null;
        real_t distance = 0.0F;
        if (tame) {
            if (const auto place = place_of(mind.owner)) {
                lead = mind.owner;
                distance = length(*place - at);
            }
        } else if (b.temperament == "friendly") {
            const auto [person, d] = nearest(self, at, tick.people, b.notice,
                                             [](const Thing& /*other*/) { return true; });
            if (person != nullptr) {
                lead = person->entity;
                distance = d;
            }
        }
        if (lead != entt::null) {
            const real_t start = tame ? kFollow : kNear;
            if (distance > start || (mind.mode == "following" && distance > kNear)) {
                mind.mode = "following";
                mind.target = lead;
                want =
                    toward(*place_of(lead), distance > kFollow * 3.0F ? b.run_speed : b.walk_speed);
            } else {
                mind.mode = "idle";
                mind.target = lead;
            }
            decided = true;
        }
    }
    // Roaming: resting, grazing and wandering around home, with its herd.
    if (!decided) {
        if (mind.mode == "fleeing" || mind.mode == "following" || mind.mode == "attacking" ||
            mind.mode == "hunting" || mind.mode == "ridden") {
            mind.mode = "idle";
            mind.timer = 1.0F;
        }
        mind.target = entt::null;
        // Its herd's middle, from those of its species near enough to see.
        Vec3 middle = at;
        int herd = 1;
        if (b.herd) {
            for (const Thing& other : tick.creatures) {
                if (other.entity == self.entity || other.two_d != self.two_d ||
                    other.recipe->species != recipe.species) {
                    continue;
                }
                const Vec3 there = seen(self, other);
                if (length(there - at) <= b.notice) {
                    middle = middle + there;
                    ++herd;
                }
            }
            middle = middle * (1.0F / static_cast<real_t>(herd));
        }
        const bool strayed = herd > 1 && length(middle - at) > b.herd_radius;
        if (mind.timer <= 0.0F || ((mind.mode == "idle" || mind.mode == "grazing") && strayed)) {
            const float roll = random(id, world.tick(), 1);
            const float span = random(id, world.tick(), 2);
            if (strayed || free_moving || roll < 0.35F) {
                mind.mode = "wandering";
                // Somewhere near the herd, or around home.
                const Vec3 center = herd > 1 ? middle : mind.home;
                const real_t reach = herd > 1 ? b.herd_radius * 0.6F : b.roam;
                const auto [c, s] = cos_sin(random(id, world.tick(), 3) * 6.2831853F);
                const real_t r = reach * std::sqrt(random(id, world.tick(), 4));
                if (self.two_d && !free_moving) {
                    mind.goal = {center.x + (r * c), mind.home.y, 0.0F};
                } else if (self.two_d) {
                    mind.goal = {center.x + (r * c), mind.home.y + (r * s * 0.3F), 0.0F};
                } else {
                    mind.goal = {center.x + (r * c), free_moving ? mind.home.y : at.y,
                                 center.z + (r * s)};
                }
                mind.timer = 20.0F;
            } else if (roll < 0.7F && recipe.kind != "predator") {
                mind.mode = "grazing";
                mind.timer = 3.0F + (5.0F * span);
            } else {
                mind.mode = "idle";
                mind.timer = 2.0F + (3.0F * span);
            }
        }
        if (mind.mode == "wandering") {
            const real_t arrive = 0.3F + (recipe.body.length * 0.5F);
            Vec3 way = mind.goal - at;
            if (!free_moving) {
                way.y = 0.0F;
            }
            if (length(way) <= arrive) {
                mind.mode = "idle";
                mind.timer = 1.0F + (2.0F * random(id, world.tick(), 5));
            } else {
                want = toward(mind.goal, b.walk_speed);
            }
        }
    }
    // A herd keeps apart, heads the same way and holds together as it moves.
    if (b.herd && length(want) > 0.0F) {
        Vec3 apart;
        Vec3 heading;
        int near = 0;
        const real_t room = std::max<real_t>(recipe.body.length * 1.2F, 0.3F);
        for (const Thing& other : tick.creatures) {
            if (other.entity == self.entity || other.two_d != self.two_d ||
                other.recipe->species != recipe.species) {
                continue;
            }
            const Vec3 offset = at - seen(self, other);
            const real_t d = length(offset);
            if (d < room && d > 0.0F) {
                apart = apart + (offset * ((room - d) / (room * d)));
            }
            if (d < b.herd_radius) {
                heading = heading + mind_of(world, other).velocity;
                ++near;
            }
        }
        const real_t speed = length(want);
        want = want + (apart * speed);
        if (near > 0) {
            want = want + (heading * (0.3F / static_cast<real_t>(near)));
        }
        if (!free_moving) {
            want.y = 0.0F;
        }
        want = normalized(want) * speed;
    }
    // Fliers and swimmers drift back toward their height.
    if (free_moving && mind.mode != "fleeing") {
        want.y =
            want.y + std::clamp<real_t>((mind.home.y - at.y) * 0.5F, -b.walk_speed, b.walk_speed);
    }
    if (self.two_d) {
        want.z = 0.0F;
    }
    // Speeds up and slows down at three times its run speed a second.
    const real_t step = b.run_speed * 3.0F * dt;
    const Vec3 change = want - mind.velocity;
    mind.velocity = length(change) <= step ? want : mind.velocity + (normalized(change) * step);
    const Vec3 velocity = mind.velocity;
    const real_t speed = length(velocity);

    if (mind.target != target_before && mind.target != entt::null &&
        (is_person(mind.target) || mind.mode == "fleeing")) {
        emit(world, "creature_noticed", self.entity, "who", mind.target);
    }
    if (mind.mode == "fleeing" && before != "fleeing") {
        emit(world, "creature_fled", self.entity, "from", mind.target);
    }

    // Moves and faces it.
    const std::pair<std::string, float> chosen = animation_for(recipe, mind, speed);
    const std::string& animation = chosen.first;
    const float rate = chosen.second;
    if (!self.two_d) {
        auto& transform = ecs.get<world::Transform3D>(self.entity);
        auto* body = ecs.try_get<world::CharacterBody3D>(self.entity);
        if (body != nullptr) {
            body->velocity.x = velocity.x;
            body->velocity.z = velocity.z;
            if (free_moving) {
                body->velocity.y = velocity.y;
            }
        } else {
            transform.position = transform.position + (velocity * dt);
        }
        if (length(Vec3{velocity.x, 0.0F, velocity.z}) > kMoving) {
            transform.rotation.y =
                slash::atan2(static_cast<float>(velocity.x), static_cast<float>(velocity.z));
        }
        const std::string& path = ecs.get<const world::Creature3D>(self.entity).recipe;
        if (auto* player = ecs.try_get<world::AnimationPlayer>(self.entity);
            player != nullptr && player->library == path) {
            if (player->animation != animation) {
                player->animation = animation;
                player->time = 0.0;
            }
            player->playing = true;
            player->speed_scale = rate;
        }
    } else {
        auto& transform = ecs.get<world::Transform2D>(self.entity);
        const Vec2 pixels{velocity.x * self.ppm, -velocity.y * self.ppm};
        if (auto* body = ecs.try_get<world::CharacterBody2D>(self.entity); body != nullptr) {
            body->velocity = free_moving ? pixels : Vec2{pixels.x, body->velocity.y};
        } else {
            transform.position = transform.position + (pixels * dt);
        }
        auto& creature = ecs.get<world::Creature2D>(self.entity);
        if (std::abs(velocity.x) > kMoving) {
            creature.flip_h = velocity.x < 0.0F;
        }
        // The gait is how far through the animation it is, in cycles; an attack plays from
        // when it struck.
        const std::vector<world::CreatureAnimation> animations = world::creature_animations(recipe);
        const auto playing = std::ranges::find_if(
            animations, [&](const world::CreatureAnimation& a) { return a.name == animation; });
        const double length_s = playing != animations.end() ? playing->length : 1.0;
        CreatureMind& kept = creature.mind;
        double time = 0.0;
        if (animation == "attack") {
            time = std::clamp(static_cast<double>(b.attack_cooldown - kept.timer), 0.0, length_s);
        } else {
            kept.gait += static_cast<float>(tick.dt * rate / length_s);
            kept.gait -= std::floor(kept.gait);
            time = kept.gait * length_s;
        }
        pose_cutout(world, self.entity, recipe, animation, time);
    }
    carry_rider(world, self, mind_of(world, self), recipe);

    // An agent's brain hears what it feels.
    if (ecs.all_of<world::NpcAgent>(self.entity)) {
        const CreatureMind& now = mind_of(world, self);
        const std::array<int, 3> facts{now.mode == "fleeing" ? 1 : 0, now.hunger > kHungry ? 1 : 0,
                                       now.owner != entt::null ? 1 : 0};
        const auto known = state.facts.find(self.entity);
        constexpr std::array<std::string_view, 3> kFacts{"threatened", "hungry", "tamed"};
        for (std::size_t i = 0; i < facts.size(); ++i) {
            if (known == state.facts.end() || known->second[i] != facts[i]) {
                (void)set_fact(world, self.entity, kFacts[i], facts[i]);
            }
        }
        state.facts[self.entity] = facts;
    }
}

void step(world::World& world, FaunaState& state, double dt) {
    auto& ecs = world.ecs();
    Tick tick{.world = world, .dt = dt, .creatures = {}, .people = {}};
    // Everyone in storage order, the order they were made, so every run goes the same way.
    const auto in_order = [](auto view) {
        std::vector<entt::entity> out(view.begin(), view.end());
        std::ranges::reverse(out);
        return out;
    };
    const world::Tree3D tree3(world);
    const world::Tree2D tree2(world);
    const auto warn = [&](entt::entity entity, const std::string& problem) {
        std::string& warned = state.warned[entity];
        if (warned != problem) {
            warned = problem;
            log::warn("creature can't behave",
                      {{"id", std::string(world.id_of(entity))}, {"problem", problem}});
        }
    };
    for (const entt::entity entity :
         in_order(ecs.view<const world::Creature3D, const world::Transform3D>())) {
        const auto& creature = ecs.get<const world::Creature3D>(entity);
        auto recipe = world::load_creature(world, creature.recipe);
        if (!recipe) {
            warn(entity, recipe.error().message);
            continue;
        }
        state.warned.erase(entity);
        tick.creatures.push_back({.entity = entity,
                                  .two_d = false,
                                  .place = tree3.global(entity).origin,
                                  .ppm = 1.0F,
                                  .recipe = recipe.value()});
    }
    for (const entt::entity entity :
         in_order(ecs.view<const world::Creature2D, const world::Transform2D>())) {
        const auto& creature = ecs.get<const world::Creature2D>(entity);
        auto recipe = world::load_creature(world, creature.recipe);
        if (!recipe) {
            warn(entity, recipe.error().message);
            continue;
        }
        state.warned.erase(entity);
        const Vec2 p = tree2.global(entity).position;
        tick.creatures.push_back({.entity = entity,
                                  .two_d = true,
                                  .place = {p.x, p.y, 0.0F},
                                  .ppm = std::max(creature.pixels_per_meter, 1.0F),
                                  .recipe = recipe.value()});
    }
    // People: character bodies and characters, which creatures notice, flee, attack and follow.
    for (const entt::entity entity : in_order(ecs.view<const world::Transform3D>())) {
        if (ecs.all_of<world::Creature3D>(entity)) {
            continue;
        }
        const auto* mesh = ecs.try_get<const world::MeshInstance3D>(entity);
        if (ecs.all_of<world::CharacterBody3D>(entity) ||
            (mesh != nullptr && mesh->mesh.ends_with(world::kCharacterSuffix))) {
            tick.people.push_back({.entity = entity,
                                   .two_d = false,
                                   .place = tree3.global(entity).origin,
                                   .ppm = 1.0F,
                                   .recipe = nullptr});
        }
    }
    for (const entt::entity entity : in_order(ecs.view<const world::Transform2D>())) {
        if (ecs.all_of<world::Creature2D>(entity) || !(ecs.all_of<world::CharacterBody2D>(entity) ||
                                                       ecs.all_of<world::Character2D>(entity))) {
            continue;
        }
        const Vec2 p = tree2.global(entity).position;
        tick.people.push_back({.entity = entity,
                               .two_d = true,
                               .place = {p.x, p.y, 0.0F},
                               .ppm = 1.0F,
                               .recipe = nullptr});
    }
    for (const Thing& creature : tick.creatures) {
        const bool enabled = creature.two_d
                                 ? ecs.get<const world::Creature2D>(creature.entity).enabled
                                 : ecs.get<const world::Creature3D>(creature.entity).enabled;
        if (enabled) {
            step_creature(tick, state, creature);
        }
    }
    std::erase_if(state.facts, [&](const auto& e) { return !world.valid(e.first); });
    std::erase_if(state.warned, [&](const auto& e) { return !world.valid(e.first); });
}

Result<std::shared_ptr<const CreatureRecipe>> recipe_of(world::World& world,
                                                        entt::entity creature) {
    if (!world.valid(creature)) {
        return make_error(ErrorCode::NotFound, "no such creature");
    }
    if (const auto* c3 = world.ecs().try_get<const world::Creature3D>(creature); c3 != nullptr) {
        return world::load_creature(world, c3->recipe);
    }
    if (const auto* c2 = world.ecs().try_get<const world::Creature2D>(creature); c2 != nullptr) {
        return world::load_creature(world, c2->recipe);
    }
    return make_error(ErrorCode::InvalidArgument, "\"" + std::string(world.id_of(creature)) +
                                                      "\" has no Creature3D or Creature2D");
}

} // namespace

void add_fauna(world::World& world) {
    if (world.ecs().ctx().find<FaunaStateHolder>() != nullptr) {
        return;
    }
    auto state = std::make_shared<FaunaState>();
    world.ecs().ctx().emplace<FaunaStateHolder>(state);
    world.add_reset([state](world::World& /*world*/) {
        state->facts.clear();
        state->warned.clear();
    });
    world.add_system(
        "fauna", [state](world::World& w, double tick_seconds) { step(w, *state, tick_seconds); },
        {.doc = "Runs each Creature3D and Creature2D as its recipe's behavior says: roaming, "
                "herding, fleeing, hunting, following and carrying riders, moving, facing and "
                "animating it.",
         .priority = 964});
}

CreatureMind* creature_mind(world::World& world, entt::entity creature) {
    if (!world.valid(creature)) {
        return nullptr;
    }
    if (auto* c3 = world.ecs().try_get<world::Creature3D>(creature); c3 != nullptr) {
        return &c3->mind;
    }
    if (auto* c2 = world.ecs().try_get<world::Creature2D>(creature); c2 != nullptr) {
        return &c2->mind;
    }
    return nullptr;
}

Result<Feeding> feed(world::World& world, entt::entity creature, entt::entity person,
                     std::string_view food) {
    auto recipe = recipe_of(world, creature);
    if (!recipe) {
        return recipe.error();
    }
    if (!world.valid(person)) {
        return make_error(ErrorCode::NotFound, "no one to feed it");
    }
    const world::CreatureBehavior& b = recipe.value()->behavior;
    if (!b.food.empty() && std::ranges::find(b.food, food) == b.food.end()) {
        std::string eats;
        for (const std::string& name : b.food) {
            eats += (eats.empty() ? "" : ", ") + name;
        }
        return make_error(ErrorCode::InvalidArgument,
                          "it doesn't eat \"" + std::string(food) + "\"; it eats " + eats);
    }
    CreatureMind& mind = *creature_mind(world, creature);
    if (mind.mode == "caught") {
        return make_error(ErrorCode::InvalidArgument, "it has been caught");
    }
    mind.hunger = 0.0F;
    mind.mode = "eating";
    mind.timer = kEating;
    mind.velocity = {};
    ++mind.fed;
    json::Value data;
    data["by"] = std::string(world.id_of(person));
    data["food"] = std::string(food);
    world.emit("creature_fed", world.id_of(creature), std::move(data));
    Feeding out;
    if (b.tame > 0 && mind.owner == entt::null && mind.fed >= b.tame) {
        mind.owner = person;
        out.tamed = true;
        emit(world, "creature_tamed", creature, "owner", person);
    }
    return out;
}

Result<void> ride(world::World& world, entt::entity creature, entt::entity rider) {
    auto recipe = recipe_of(world, creature);
    if (!recipe) {
        return recipe.error();
    }
    if (!recipe.value()->behavior.ride) {
        return make_error(ErrorCode::InvalidArgument, "its recipe says it can't be ridden");
    }
    CreatureMind& mind = *creature_mind(world, creature);
    if (mind.owner == entt::null || mind.owner != rider) {
        return make_error(ErrorCode::InvalidArgument, "only its owner can ride it; tame it first");
    }
    if (mind.mode == "caught") {
        return make_error(ErrorCode::InvalidArgument, "it has been caught");
    }
    mind.rider = rider;
    mind.mode = "ridden";
    mind.steer = {};
    emit(world, "creature_mounted", creature, "rider", rider);
    return {};
}

Result<void> dismount(world::World& world, entt::entity creature) {
    if (auto recipe = recipe_of(world, creature); !recipe) {
        return recipe.error();
    }
    CreatureMind& mind = *creature_mind(world, creature);
    if (mind.rider == entt::null) {
        return make_error(ErrorCode::InvalidArgument, "no one rides it");
    }
    const entt::entity rider = mind.rider;
    mind.rider = entt::null;
    mind.steer = {};
    mind.mode = "idle";
    mind.timer = 1.0F;
    emit(world, "creature_dismounted", creature, "rider", rider);
    return {};
}

Result<void> steer(world::World& world, entt::entity creature, Vec3 direction) {
    if (auto recipe = recipe_of(world, creature); !recipe) {
        return recipe.error();
    }
    CreatureMind& mind = *creature_mind(world, creature);
    if (mind.rider == entt::null) {
        return make_error(ErrorCode::InvalidArgument, "only a ridden creature is steered");
    }
    mind.steer = direction;
    return {};
}

Result<void> release(world::World& world, entt::entity creature) {
    if (auto recipe = recipe_of(world, creature); !recipe) {
        return recipe.error();
    }
    CreatureMind& mind = *creature_mind(world, creature);
    if (mind.rider != entt::null) {
        if (auto off = dismount(world, creature); !off) {
            return off;
        }
    }
    mind.owner = entt::null;
    mind.fed = 0;
    mind.mode = "idle";
    mind.target = entt::null;
    return {};
}

Result<void> scare(world::World& world, entt::entity creature, entt::entity from) {
    if (auto recipe = recipe_of(world, creature); !recipe) {
        return recipe.error();
    }
    if (!world.valid(from)) {
        return make_error(ErrorCode::NotFound, "nothing to scare it");
    }
    CreatureMind& mind = *creature_mind(world, creature);
    if (mind.mode == "caught") {
        return make_error(ErrorCode::InvalidArgument, "it has been caught");
    }
    if (mind.rider != entt::null) {
        if (auto off = dismount(world, creature); !off) {
            return off;
        }
    }
    mind.mode = "fleeing";
    mind.target = from;
    mind.timer = kScared;
    emit(world, "creature_fled", creature, "from", from);
    return {};
}

} // namespace slash::npc
