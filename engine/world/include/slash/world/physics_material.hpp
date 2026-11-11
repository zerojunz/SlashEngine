#pragma once

// Physics materials shared by bodies, as Godot's PhysicsMaterial resource (friction, rough,
// bounce, absorbent) with what Unreal's Chaos physical materials add: density, strength and the
// surface's name, which queries and contacts report so footsteps, impact sounds and effects can
// follow what was hit. A body's physics_material_override names a .physics_material.json file.

#include "slash/core/json.hpp"
#include "slash/core/result.hpp"

#include <string>

namespace slash::world {

struct PhysicsMaterial {
    float friction = 1.0F;
    // With rough, the material's friction wins over a smoother one's, as Godot's.
    bool rough = false;
    float bounce = 0.0F;
    // With absorbent, the material's bounce is taken from the other's, as Godot's.
    bool absorbent = false;
    // Kilograms a cubic meter (in 2D, a square meter of 64 pixels a side); 0 leaves a rigid
    // body's mass as it is, else it weighs its shapes' volume times this.
    float density = 0.0F;
    // Kilogram-meters per second of one impact that breaks a Destructible3D made of it whose
    // damage_threshold is 0; 0 never.
    float strength = 0.0F;
    // A name for the surface, such as "wood" or "grass", that raycasts and contacts report.
    std::string surface;

    friend bool operator==(const PhysicsMaterial&, const PhysicsMaterial&) = default;
};

// The friction and bounce a body takes from a material, as Godot's PhysicsMaterial's
// computed_friction and computed_bounce: negative when rough or absorbent, which physics combines
// as Godot does (the smaller friction, without its sign; the sum of bounces, from 0 to 1).
[[nodiscard]] float computed_friction(const PhysicsMaterial& material);
[[nodiscard]] float computed_bounce(const PhysicsMaterial& material);

// JSON Schema for .physics_material.json files.
[[nodiscard]] json::Value physics_material_schema();
// Reads a .physics_material.json file's JSON; an error names the bad member's JSON Pointer.
[[nodiscard]] Result<PhysicsMaterial> parse_physics_material(const json::Value& value);
// Whether a path names a physics material file.
[[nodiscard]] bool is_physics_material_path(std::string_view path);

} // namespace slash::world
