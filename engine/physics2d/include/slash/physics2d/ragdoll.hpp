#pragma once

#include "slash/core/json.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/core/math.hpp"
#include "slash/core/result.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/ragdoll.hpp"
#include "slash/world/world.hpp"

#include <map>
#include <string>
#include <string_view>
#include <vector>

// 2D characters' and creatures' own ragdolls (Ragdoll2D), the 2D side of Ragdoll3D: the same
// .ragdoll.json profiles, in pixels, over a cut-out character's Transform2D tree. Each profile bone
// is an entity under the Ragdoll2D's (named by its ID, without the ragdoll's own "<id>/" when it
// is an instance's), which the ragdoll system makes a RigidBody2D with a CollisionShape2D and
// joins to the bone above it with a PinJoint2D ("<bone>_joint"). Animated, the bodies are frozen
// and follow the animation; active, their muscles drive them toward it; limp, they fall; and once
// they lie still the ragdoll gets up, as Ragdoll3D's.
namespace slash::physics2d {

// Adds the ragdoll systems to a world (install does): "ragdoll_2d" (priority 940, after
// animations and before physics) and "ragdoll_2d_pose" (955, after physics), which keeps where
// physics left each bone so the next tick tells the animation's pose from it.
void add_ragdoll_systems(world::World& world);

// A Ragdoll2D's profile: its file with its overrides over it.
[[nodiscard]] Result<world::RagdollProfile> ragdoll_profile(const world::World& world,
                                                            entt::entity entity);

// The entity of each of a Ragdoll2D's profile bones that its tree has, by bone name.
[[nodiscard]] std::map<std::string, entt::entity> ragdoll_bones(const world::World& world,
                                                                entt::entity entity);

// What a hit did.
struct RagdollHitReport {
    std::string state;                     // the ragdoll's state after it
    std::map<std::string, float> weakness; // each bone's, after it
    bool knocked_down = false;
};

// Hits a Ragdoll2D's bone with an impulse in kilogram-pixels per second, as gameplay code does
// when a blow lands: the body takes it, the bone's muscle and those under it weaken as the
// profile's hit says, an animated ragdoll reacts (goes active until its muscles recover), and a
// hit of the profile's knockdown or more makes it limp. Emits ragdoll_hit, and ragdoll_fell when
// it knocks it down.
[[nodiscard]] Result<RagdollHitReport> hit_ragdoll(world::World& world, entt::entity entity,
                                                   std::string_view bone, Vec2 impulse);

// How generate_ragdoll makes a 2D profile.
struct RagdollOptions {
    float mass = 70.0F;       // kilograms, shared by the bodies by their areas
    float stiffness = 400.0F; // the muscles'
    float damping = 40.0F;
    float pin = 200.0F; // how hard each body is pinned to the pose's place
    float pin_damping = 28.0F;
    // Bones shorter than this, in pixels, get no body of their own; 0 for 4% of the tree's height.
    float min_length = 0.0F;
};

// A ragdoll profile for the Transform2D tree under an entity, as Unreal's physics asset tool
// makes one for a skeleton: each entity under it with children or a Sprite2D is a bone, a
// capsule from it to its one child bone, or a rectangle round its children for one with several,
// with masses by area, hinges between them, balance on the topmost and its get-up facing +x.
[[nodiscard]] Result<json::Value> generate_ragdoll(const world::World& world, entt::entity entity,
                                                   const RagdollOptions& options = {});

// Problems with an entity's Ragdoll2D for scene validate: a profile that can't be read or is
// invalid, bones its tree doesn't have, shapes and joints 2D lacks, and a profile that makes the
// Ragdoll2D's own entity a bone, each with the JSON Pointer of the field.
struct RagdollProblem {
    std::string path;
    std::string message;
};
[[nodiscard]] std::vector<RagdollProblem> check_ragdoll(const world::World& world,
                                                        entt::entity entity);
// Every Ragdoll2D's problems in a scene file, with the JSON Pointer of each, for scene validate
// and exports.
[[nodiscard]] std::vector<json::ValidationError>
check_ragdolls(const world::ComponentRegistry& components, const json::Value& scene,
               const world::FileReader& reader);

} // namespace slash::physics2d
