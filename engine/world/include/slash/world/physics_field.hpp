#pragma once

// Physics fields, as Unreal's Chaos Fields (Godot has none): what a world's PhysicsField3Ds (or
// PhysicsField2Ds) do at a place. 3D and 2D physics apply them to their bodies each step, and
// CPU particles feel their forces, so one field acts on everything in its reach at once.

#include "slash/core/math.hpp"
#include "slash/world/components.hpp"
#include "slash/world/tree3d.hpp"

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace slash::world {

class World;
class Tree2D;
class Tree3D;

enum class FieldType : std::uint8_t {
    RadialForce,
    DirectionalForce,
    RadialVelocity,
    DirectionalVelocity,
    NoiseForce,
    Strain,
    Anchor,
    Kill,
    Sleep,
};

// A PhysicsField3D's or PhysicsField2D's type by name, or nullopt for an unknown one.
[[nodiscard]] std::optional<FieldType> field_type(std::string_view name);

// What the fields reaching a place do to a body there, each weakened by its falloff.
struct FieldEffect {
    Vec3 force;                       // newtons, summed (2D: kilogram-pixels per second squared)
    std::optional<Vec3> velocity;     // the velocity it is set to, summed over velocity fields
    float strain = 0.0F;              // the strongest strain, in kilogram-meters per second
    bool anchored = false;            // held where it is
    bool killed = false;              // removed
    std::optional<float> sleep_below; // sleeps when slower than this

    [[nodiscard]] bool empty() const {
        return force == Vec3{} && !velocity && strain == 0.0F && !anchored && !killed &&
               !sleep_below;
    }
};

// The fields of a world's enabled PhysicsField3Ds, each placed by its entity's global transform
// when this was made, in the registry's storage order so sums are the same in every session.
class PhysicsFields3D {
public:
    PhysicsFields3D() = default;
    PhysicsFields3D(const World& world, const Tree3D& tree);

    // What the fields do at a place to something in the given collision layers. seed makes noise
    // differ from body to body; time (seconds) moves it on.
    [[nodiscard]] FieldEffect at(Vec3 position, std::uint32_t layers, std::uint32_t seed,
                                 double time) const;
    // The force fields' push on a particle of a kilogram at a place, from fields with particles
    // on, in meters per second squared.
    [[nodiscard]] Vec3 particle_force(Vec3 position, std::uint32_t seed, double time) const;
    [[nodiscard]] bool empty() const noexcept { return fields_.empty(); }

    struct Field {
        FieldType type = FieldType::RadialForce;
        float magnitude = 0.0F;
        int shape = 0;   // sphere, box, plane, everywhere
        int falloff = 0; // none, linear, squared
        float radius = 0.0F;
        Vec3 half; // a box's half size
        std::uint32_t mask = 0;
        float noise_frequency = 0.0F;
        bool particles = true;
        std::uint32_t seed = 0;
        Affine3D place;    // the entity's global transform
        Affine3D to_local; // its inverse
        Vec3 origin;       // where radial fields push from
        Vec3 direction;    // which way directional fields push, as a unit vector
        Vec3 plane_normal; // a plane's up, as a unit vector; it acts below
    };

private:
    friend class PhysicsFields2D;
    void add(const Field& field) { fields_.push_back(field); }
    std::vector<Field> fields_;
};

// The same in a 2D world, from its PhysicsField2Ds: places and pushes in pixels.
class PhysicsFields2D {
public:
    PhysicsFields2D() = default;
    PhysicsFields2D(const World& world, const Tree2D& tree);

    // As PhysicsFields3D::at, with z left 0.
    [[nodiscard]] FieldEffect at(Vec2 position, std::uint32_t layers, std::uint32_t seed,
                                 double time) const;
    [[nodiscard]] Vec2 particle_force(Vec2 position, std::uint32_t seed, double time) const;
    [[nodiscard]] bool empty() const noexcept { return fields_.empty(); }

private:
    PhysicsFields3D fields_;
};

} // namespace slash::world
