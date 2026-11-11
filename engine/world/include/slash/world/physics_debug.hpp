#pragma once

// A record of what physics did each step, as Chaos's Visual Debugger records it (Godot has
// none): the contacts each step solved, how hard each joint pulled and how far it stretched,
// what fields did to each body and how many bodies were awake. 3D and 2D physics add a step to
// it while it records; physics.debug reads it for agents.

#include "slash/core/math.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>

namespace slash::world {

class World;

// Two bodies touching in a step, by ID (a before b), at the deepest point of their contact.
struct DebugContact {
    std::string a;
    std::string b;
    Vec3 point;           // in the world; z is 0 in 2D
    Vec3 normal;          // from a toward b, a unit vector
    float depth = 0.0F;   // how far they overlap: meters, or pixels in 2D
    float speed = 0.0F;   // how fast they closed along the normal, per second; 0 when parting
    float impulse = 0.0F; // what the solver pushed them apart with (2D; 0 in 3D)
};

// A joint after a step.
struct DebugJoint {
    std::string id;   // the joint's entity, or a physical bone's
    std::string kind; // its component, such as "PinJoint3D", or "PhysicalBone3D"
    std::string a;    // the bodies it joins; b is empty for the world
    std::string b;
    float force = 0.0F;   // the force holding the bodies together: newtons, or 2D's units
    float stretch = 0.0F; // how far its two anchors are apart: meters, or pixels in 2D
};

// What the fields reaching a body did to it before a step.
struct DebugField {
    std::string body;
    Vec3 force;
    std::optional<Vec3> velocity;
    float strain = 0.0F;
    bool anchored = false;
    bool killed = false;
};

struct PhysicsDebugStep {
    std::uint64_t tick = 0;
    int dimensions = 3; // 3 for physics3d's step, 2 for physics2d's
    std::vector<DebugContact> contacts;
    std::vector<DebugJoint> joints;
    std::vector<DebugField> fields;
    int awake = 0;    // rigid bodies and bones that moved
    int sleeping = 0; // and those asleep
};

// The steps kept, oldest first, at most keep() of each dimension's.
class PhysicsDebug {
public:
    explicit PhysicsDebug(std::size_t keep) : keep_(keep) {}

    [[nodiscard]] std::size_t keep() const noexcept { return keep_; }
    [[nodiscard]] const std::deque<PhysicsDebugStep>& steps() const noexcept { return steps_; }
    // Starts a step and returns it, dropping the oldest of its dimension past keep(). The
    // reference stays good until the next add.
    PhysicsDebugStep& add(std::uint64_t tick, int dimensions);

private:
    std::size_t keep_;
    std::deque<PhysicsDebugStep> steps_;
};

// Starts recording a world's physics afresh, keeping the last keep steps of each dimension, or
// stops and forgets the record with 0.
void record_physics(World& world, std::size_t keep);
// The record, or nullptr when the world isn't recording.
[[nodiscard]] const PhysicsDebug* physics_debug(const World& world);
[[nodiscard]] PhysicsDebug* physics_debug(World& world);

} // namespace slash::world
