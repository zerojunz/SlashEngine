#pragma once

#include "slash/core/json.hpp"
#include "slash/core/result.hpp"
#include "slash/scene3d/model.hpp"
#include "slash/scene3d/skeleton.hpp"
#include "slash/world/ragdoll.hpp"
#include "slash/world/world.hpp"

#include <map>
#include <string>
#include <string_view>
#include <vector>

// Characters' and creatures' own ragdolls (Ragdoll3D), SlashEngine's own after Unreal's physics
// assets and Physical Animation component, on Godot's PhysicalBoneSimulator3D and PhysicalBone3D:
// the ragdoll system keeps a simulator and a physical bone per profile bone, sets their joints
// and muscles from the profile, and runs the ragdoll's states (animated, active, limp, getting
// up). engine/physics3d drives the bodies' muscles toward the animated pose
// (PhysicalBone3D.drive).
namespace slash::scene3d {

// Adds the ragdoll system (priority 915, after animations and before skeleton_modifiers) to a
// world. Profiles load through the world's assets (asset::attach), so they reload.
void add_ragdoll_system(world::World& world, ModelLoader load);

// A Ragdoll3D's profile: its file with its overrides over it.
[[nodiscard]] Result<world::RagdollProfile> ragdoll_profile(const world::World& world,
                                                            entt::entity entity);

// The physical bone entity of each of a ragdoll's profile bones that has one, by bone name.
[[nodiscard]] std::map<std::string, entt::entity> ragdoll_bones(const world::World& world,
                                                                entt::entity entity);

// What a hit did.
struct RagdollHitReport {
    std::string state;                     // the ragdoll's state after it
    std::map<std::string, float> weakness; // each bone's, after it
    bool knocked_down = false;
};

// Hits a ragdoll's bone with an impulse in kilogram-meters per second, as gameplay code does
// when a blow or a bullet lands: the body takes it, the bone's muscle and those under it weaken
// as the profile's hit says, an animated ragdoll reacts (goes active until its muscles recover),
// and a hit of the profile's knockdown or more makes it limp. Emits ragdoll_hit, and ragdoll_fell
// when it knocks it down.
[[nodiscard]] Result<RagdollHitReport> hit_ragdoll(world::World& world, entt::entity entity,
                                                   std::string_view bone, Vec3 impulse);

// How generate_ragdoll makes a profile.
struct RagdollOptions {
    float mass = 70.0F;       // kilograms, shared by the bodies by their volumes
    float stiffness = 400.0F; // the muscles'
    float damping = 40.0F;
    float pin = 200.0F; // how hard each body is pinned to the pose's place
    float pin_damping = 28.0F;
    // Bones shorter than this share of the skeleton's height get no body of their own; 0 for 4%.
    float min_length = 0.0F;
};

// A ragdoll profile for a model's skeleton, as Unreal's physics asset tool makes one: a capsule
// from each bone to its one child, or a sphere for a bone with several (hips, chest), fitted to
// the vertices the bone moves most, with masses by volume. A humanoid (auto_map_bones finds hips)
// gets bodies on its main bones only, hinges at elbows and knees and cones elsewhere with limits
// for each kind of joint, balance on its hips and its get_up animations left to name; another
// skeleton gets cones on every bone long enough.
[[nodiscard]] Result<json::Value> generate_ragdoll(const Model& model,
                                                   const RagdollOptions& options = {});

// Problems with an entity's Ragdoll3D for scene validate: a profile that can't be read or is
// invalid, and bones the model (when given) doesn't have, each with the JSON Pointer of the
// field.
struct RagdollProblem {
    std::string path;
    std::string message;
};
[[nodiscard]] std::vector<RagdollProblem> check_ragdoll(const world::World& world,
                                                        entt::entity entity, const Model* model);

} // namespace slash::scene3d
