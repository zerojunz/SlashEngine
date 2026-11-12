#pragma once

// Animals that behave, after Unreal's animal AI (Godot has none): the fauna system runs each
// Creature3D and Creature2D as its .creature.json recipe's behavior says
// (slash/world/creature.hpp). It roams around home, grazes and rests, keeps with its herd, flock or
// school, flees people as its temperament says and what hunts it, hunts when hungry, attacks or
// comes to people, follows its owner once tamed, and carries its rider. In 3D it moves its
// Transform3D (or sets its CharacterBody3D's velocity), faces where it goes and plays its model's
// animations on its AnimationPlayer; in 2D it moves its Transform2D in pixels (ground animals left
// and right, fliers and swimmers anywhere), mirrors it with flip_h and turns its cut-out's bones.
//
// Events, each with the creature's ID: creature_noticed (who), creature_fled (from),
// creature_attacked (target, damage), creature_caught (prey), creature_fed (by, food),
// creature_tamed (owner), creature_mounted (rider) and creature_dismounted (rider). A creature
// with an NpcAgent has its brain's "threatened", "hungry" and "tamed" facts set as they change,
// so a planner can act on them.

#include "slash/core/math.hpp"
#include "slash/core/result.hpp"
#include "slash/world/components.hpp"
#include "slash/world/world.hpp"

#include <string_view>

namespace slash::npc {

// Adds the fauna system (priority 964, after navigation and before the npc system); install calls
// it.
void add_fauna(world::World& world);

// The creature's mind, from its Creature3D or Creature2D; null for neither.
[[nodiscard]] world::CreatureMind* creature_mind(world::World& world, entt::entity creature);

// What feeding did.
struct Feeding {
    bool tamed = false; // this feeding tamed it
};

// person feeds the creature food (anything when its recipe's food is empty): it eats, its
// hunger goes to 0, and once fed as often as its recipe's tame says it is person's. Refused when
// it doesn't eat that food or is caught.
[[nodiscard]] Result<Feeding> feed(world::World& world, entt::entity creature, entt::entity person,
                                   std::string_view food);
// Its owner gets on it, when its recipe says it can be ridden.
[[nodiscard]] Result<void> ride(world::World& world, entt::entity creature, entt::entity rider);
// Its rider gets off.
[[nodiscard]] Result<void> dismount(world::World& world, entt::entity creature);
// Which way its rider steers it, in meters (x and z in 3D, x and y in 2D, y up), as a share of
// its run speed up to 1; zero stops it.
[[nodiscard]] Result<void> steer(world::World& world, entt::entity creature, Vec3 direction);
// Its owner lets it go wild again: no owner, no feedings, no rider.
[[nodiscard]] Result<void> release(world::World& world, entt::entity creature);
// It flees from from for a few seconds, whatever its temperament (a tame one too).
[[nodiscard]] Result<void> scare(world::World& world, entt::entity creature, entt::entity from);

} // namespace slash::npc
