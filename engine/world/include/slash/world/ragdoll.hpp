#pragma once

// Ragdoll profiles, SlashEngine's own after Unreal's physics assets and its Physical Animation
// component (Godot has no profiles: its PhysicalBone3D nodes are made one by one). A
// .ragdoll.json file names, for each bone of a character or creature, the body over it (shape,
// size, mass), the joint that holds it to the body of the bone above (limits), and the muscle
// that drives it toward the animated pose (stiffness, damping, strength), with how the whole
// keeps its balance, takes hits and gets back up. Ragdoll3D bodies are named by a model's bone
// names in meters, and Ragdoll2D bodies by the entities of a Transform2D tree in pixels.

#include "slash/core/json.hpp"
#include "slash/core/math.hpp"
#include "slash/core/result.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <string_view>

namespace slash::world {

// One bone's body, joint and muscle.
struct RagdollBone {
    // 3D: "capsule", "sphere" or "box"; 2D: "capsule", "circle" or "rectangle".
    std::string shape = "capsule";
    float radius = 0.05F;
    float height = 0.3F;           // a capsule's, end to end, along the body's y
    Vec3 size{0.1F, 0.1F, 0.1F};   // a box's or rectangle's
    Vec3 offset;                   // where the body is in the bone's space
    Vec3 rotation;                 // radians, as Transform3D's (2D: z alone)
    float mass = 1.0F;             // kilograms
    std::string joint = "cone";    // "none", "pin", "cone" or "hinge" (2D: no "cone")
    Vec3 joint_rotation;           // the joint's axes in the body's space, radians
    float swing_span = 0.7853982F; // cone, radians
    float twist_span = 0.5235988F; // cone, radians
    float lower = -1.5707964F;     // hinge, radians
    float upper = 1.5707964F;      // hinge, radians
    // The muscle, as Unreal's physical animation drives: "local" turns the body toward the
    // animated pose in its parent bone's body, "world" toward it in the world, "off" not at all.
    std::string drive = "local";
    float stiffness = 0.0F;        // how hard it turns toward the pose, per second squared
    float damping = 0.0F;          // how much it slows its turning, per second
    float strength = 1.0F;         // a multiplier on both, 0 for no muscle
    float max_acceleration = 0.0F; // radians a second squared it can turn by at most; 0 no limit
    // How hard the muscle also pins the body to the animated pose's place in the world, so it
    // holds its own weight, as PuppetMaster's pins, and how much that slows it.
    float pin = 0.0F;
    float pin_damping = 0.0F;
    float linear_damp = 0.0F;
    float angular_damp = 0.0F;
    std::string physics_material; // a .physics_material.json file for the body

    friend bool operator==(const RagdollBone&, const RagdollBone&) = default;
};

// How an active ragdoll keeps its balance: its bone's body is driven toward the animated pose in
// the world (orientation and place), and it falls (goes limp) once the body leans more than
// max_tilt away from the pose.
struct RagdollBalance {
    std::string bone; // "" for no balance
    float stiffness = 0.0F;
    float damping = 0.0F;
    float position_stiffness = 0.0F;
    float position_damping = 0.0F;
    float max_tilt = 0.7853982F; // radians; 0 never falls

    friend bool operator==(const RagdollBalance&, const RagdollBalance&) = default;
};

// How hits weaken muscles: a hit takes weaken of its bone's strength for each kilogram-meter per
// second, spread of that from each bone to the ones under it, and muscles get it back over
// recovery seconds. A hit of knockdown or more knocks the ragdoll down (0 never).
struct RagdollHit {
    float weaken = 0.5F;
    float spread = 0.5F;
    float recovery = 1.0F;
    float knockdown = 0.0F;

    friend bool operator==(const RagdollHit&, const RagdollHit&) = default;
};

// How a limp ragdoll gets back up: once every body has moved slower than settle_speed for
// settle_time seconds, the entity moves to where it lies, plays face_up or face_down on its
// AnimationPlayer (as forward, in the balance bone's space, points up or down) and blends from
// the bodies to the animation over blend seconds.
struct RagdollGetUp {
    bool enabled = true;
    float settle_speed = 0.3F;
    float settle_time = 0.5F;
    std::string face_up;
    std::string face_down;
    float blend = 0.4F;
    Vec3 forward{0.0F, 0.0F, 1.0F};

    friend bool operator==(const RagdollGetUp&, const RagdollGetUp&) = default;
};

struct RagdollProfile {
    std::string model; // the model it was made for, if any; checks use it
    std::uint32_t collision_layer = 1;
    std::uint32_t collision_mask = 1;
    // Whether its bodies collide with each other: off, as bodies fitted to a skin overlap where
    // they meet, though joined bodies never collide either way.
    bool self_collision = false;
    std::map<std::string, RagdollBone> bones; // by bone name (2D: by entity, from the root)
    RagdollBalance balance;
    RagdollHit hit;
    RagdollGetUp get_up;

    friend bool operator==(const RagdollProfile&, const RagdollProfile&) = default;
};

// JSON Schema for .ragdoll.json files.
[[nodiscard]] json::Value ragdoll_schema();
// A JSON merge patch (RFC 7396): patch's members over base's, objects merged member by member,
// null removing one.
[[nodiscard]] json::Value merge_patch(const json::Value& base, const json::Value& patch);
// Reads a .ragdoll.json file's JSON with an instance's overrides (a merge patch, as
// Ragdoll3D.overrides) over it: each bone is the file's "defaults" with its own members over
// them. An error names the bad member's JSON Pointer.
[[nodiscard]] Result<RagdollProfile> parse_ragdoll(const json::Value& file,
                                                   const json::Value& overrides = {});
// Whether a path names a ragdoll profile.
[[nodiscard]] bool is_ragdoll_path(std::string_view path);

} // namespace slash::world
