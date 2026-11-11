#include "slash/world/physics_field.hpp"

#include "slash/world/tree2d.hpp"
#include "slash/world/tree3d.hpp"
#include "slash/world/world.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>

namespace slash::world {

namespace {

constexpr real_t kTiny = 1e-6F;
constexpr int kSphere = 0;
constexpr int kBox = 1;
constexpr int kPlane = 2;
constexpr int kEverywhere = 3;
constexpr int kNone = 0;
constexpr int kLinear = 1;
constexpr int kSquared = 2;

real_t dot(Vec3 a, Vec3 b) {
    return (a.x * b.x) + (a.y * b.y) + (a.z * b.z);
}

real_t length(Vec3 v) {
    return std::sqrt(dot(v, v));
}

Vec3 unit(Vec3 v) {
    const real_t size = length(v);
    return size <= kTiny ? Vec3{} : v * (1.0F / size);
}

// FNV-1a of an entity's ID, so a field's noise follows it from session to session.
std::uint32_t hash_id(std::string_view id) {
    std::uint32_t hash = 2166136261U;
    for (const char c : id) {
        hash = (hash ^ static_cast<unsigned char>(c)) * 16777619U;
    }
    return hash;
}

// A whole number's value from -1 to 1, as PCG's output permutation of it with the seed.
float lattice(std::int64_t at, std::uint32_t seed) {
    std::uint32_t state = (static_cast<std::uint32_t>(at) * 747796405U) + 2891336453U + seed;
    state ^= static_cast<std::uint32_t>(static_cast<std::uint64_t>(at) >> 32U) * 2654435761U;
    const std::uint32_t word = ((state >> ((state >> 28U) + 4U)) ^ state) * 277803737U;
    const auto unit_value = static_cast<float>(((word >> 22U) ^ word) >> 8U) * (1.0F / 16777216.0F);
    return (unit_value * 2.0F) - 1.0F;
}

// Smooth noise from -1 to 1 along a line: the lattice values on each side of x, eased together.
float noise(double x, std::uint32_t seed) {
    const double whole = std::floor(x);
    const auto t = static_cast<float>(x - whole);
    const auto at = static_cast<std::int64_t>(whole);
    const float a = lattice(at, seed);
    const float b = lattice(at + 1, seed);
    const float eased = t * t * (3.0F - (2.0F * t));
    return a + ((b - a) * eased);
}

int shape_of(std::string_view shape) {
    if (shape == "box") {
        return kBox;
    }
    if (shape == "plane") {
        return kPlane;
    }
    if (shape == "everywhere") {
        return kEverywhere;
    }
    return kSphere;
}

int falloff_of(std::string_view falloff) {
    if (falloff == "linear") {
        return kLinear;
    }
    if (falloff == "squared") {
        return kSquared;
    }
    return kNone;
}

// How strongly a field reaches a place, from 1 to 0, or a negative number out of its reach: the
// share of the way from the edge of its shape to its middle (a plane's, to radius below it).
real_t reach(const PhysicsFields3D::Field& field, Vec3 position) {
    real_t inside = 1.0F; // 1 in the middle, 0 at the edge
    switch (field.shape) {
    case kSphere: {
        if (field.radius <= 0.0F) {
            return -1.0F;
        }
        const real_t distance = length(position - field.origin);
        if (distance > field.radius) {
            return -1.0F;
        }
        inside = 1.0F - (distance / field.radius);
        break;
    }
    case kBox: {
        const Vec3 local = field.to_local.apply(position);
        const std::array<real_t, 3> shares{
            field.half.x > 0.0F ? std::abs(local.x) / field.half.x : 2.0F,
            field.half.y > 0.0F ? std::abs(local.y) / field.half.y : 2.0F,
            field.half.z > 0.0F ? std::abs(local.z) / field.half.z : 2.0F};
        const real_t out = std::max({shares[0], shares[1], shares[2]});
        if (out > 1.0F) {
            return -1.0F;
        }
        inside = 1.0F - out;
        break;
    }
    case kPlane: {
        const real_t depth = -dot(position - field.origin, field.plane_normal);
        if (depth < 0.0F) {
            return -1.0F;
        }
        inside = field.radius > 0.0F ? std::min(depth / field.radius, real_t{1}) : 1.0F;
        break;
    }
    default:
        return 1.0F;
    }
    switch (field.falloff) {
    case kLinear:
        return inside;
    case kSquared:
        return inside * inside;
    default:
        return 1.0F;
    }
}

// Which way a field pushes at a place, as a unit vector (zero at a radial field's middle).
Vec3 push_way(const PhysicsFields3D::Field& field, Vec3 position) {
    switch (field.type) {
    case FieldType::RadialForce:
    case FieldType::RadialVelocity:
        return unit(position - field.origin);
    default:
        return field.direction;
    }
}

Vec3 noise_push(const PhysicsFields3D::Field& field, std::uint32_t seed, double time) {
    const double phase = static_cast<double>(field.noise_frequency) * time;
    const std::uint32_t mixed = field.seed ^ (seed * 2654435761U);
    return {noise(phase, mixed), noise(phase + 17.0, mixed ^ 0x9E3779B9U),
            noise(phase + 31.0, mixed ^ 0x7F4A7C15U)};
}

} // namespace

std::optional<FieldType> field_type(std::string_view name) {
    static constexpr std::array<std::pair<std::string_view, FieldType>, 9> kTypes{{
        {"radial_force", FieldType::RadialForce},
        {"directional_force", FieldType::DirectionalForce},
        {"radial_velocity", FieldType::RadialVelocity},
        {"directional_velocity", FieldType::DirectionalVelocity},
        {"noise_force", FieldType::NoiseForce},
        {"strain", FieldType::Strain},
        {"anchor", FieldType::Anchor},
        {"kill", FieldType::Kill},
        {"sleep", FieldType::Sleep},
    }};
    for (const auto& [known, type] : kTypes) {
        if (known == name) {
            return type;
        }
    }
    return std::nullopt;
}

PhysicsFields3D::PhysicsFields3D(const World& world, const Tree3D& tree) {
    for (const auto [entity, component] : world.ecs().view<const PhysicsField3D>().each()) {
        const std::optional<FieldType> type = field_type(component.type);
        if (!component.enabled || !type) {
            continue;
        }
        const Affine3D global = tree.global(entity);
        fields_.push_back({.type = *type,
                           .magnitude = component.magnitude,
                           .shape = shape_of(component.shape),
                           .falloff = falloff_of(component.falloff),
                           .radius = component.radius,
                           .half = component.size * 0.5F,
                           .mask = component.collision_mask,
                           .noise_frequency = component.noise_frequency,
                           .particles = component.particles,
                           .seed = hash_id(world.id_of(entity)),
                           .place = global,
                           .to_local = global.inverse(),
                           .origin = global.origin,
                           .direction = unit(global.basis({0.0F, 0.0F, -1.0F})),
                           .plane_normal = unit(global.basis({0.0F, 1.0F, 0.0F}))});
    }
}

FieldEffect PhysicsFields3D::at(Vec3 position, std::uint32_t layers, std::uint32_t seed,
                                double time) const {
    FieldEffect effect;
    for (const Field& field : fields_) {
        if ((field.mask & layers) == 0U) {
            continue;
        }
        const real_t weight = reach(field, position);
        if (weight < 0.0F) {
            continue;
        }
        const float amount = field.magnitude * weight;
        switch (field.type) {
        case FieldType::RadialForce:
        case FieldType::DirectionalForce:
            effect.force = effect.force + (push_way(field, position) * amount);
            break;
        case FieldType::NoiseForce:
            effect.force = effect.force + (noise_push(field, seed, time) * amount);
            break;
        case FieldType::RadialVelocity:
        case FieldType::DirectionalVelocity:
            effect.velocity =
                effect.velocity.value_or(Vec3{}) + (push_way(field, position) * amount);
            break;
        case FieldType::Strain:
            effect.strain = std::max(effect.strain, amount);
            break;
        case FieldType::Anchor:
            effect.anchored = true;
            break;
        case FieldType::Kill:
            effect.killed = true;
            break;
        case FieldType::Sleep:
            effect.sleep_below = std::max(effect.sleep_below.value_or(0.0F), amount);
            break;
        }
    }
    return effect;
}

Vec3 PhysicsFields3D::particle_force(Vec3 position, std::uint32_t seed, double time) const {
    Vec3 force;
    for (const Field& field : fields_) {
        if (!field.particles) {
            continue;
        }
        const bool pushes = field.type == FieldType::RadialForce ||
                            field.type == FieldType::DirectionalForce ||
                            field.type == FieldType::NoiseForce;
        if (!pushes) {
            continue;
        }
        const real_t weight = reach(field, position);
        if (weight < 0.0F) {
            continue;
        }
        const Vec3 way = field.type == FieldType::NoiseForce ? noise_push(field, seed, time)
                                                             : push_way(field, position);
        force = force + (way * (field.magnitude * weight));
    }
    return force;
}

PhysicsFields2D::PhysicsFields2D(const World& world, const Tree2D& tree) {
    for (const auto [entity, component] : world.ecs().view<const PhysicsField2D>().each()) {
        const std::optional<FieldType> type = field_type(component.type);
        if (!component.enabled || !type) {
            continue;
        }
        // The 2D transform as a 3D one in the xy plane, so one sampler serves both.
        const Affine2D flat = tree.global_affine(entity);
        Affine3D global;
        global.origin = {flat.origin.x, flat.origin.y, 0.0F};
        global.x = {flat.x.x, flat.x.y, 0.0F};
        global.y = {flat.y.x, flat.y.y, 0.0F};
        global.z = {0.0F, 0.0F, 1.0F};
        const Vec3 along = global.basis({1.0F, 0.0F, 0.0F});
        // y points down the screen, so a plane's up is its -y.
        const Vec3 up = global.basis({0.0F, -1.0F, 0.0F});
        fields_.add({.type = *type,
                     .magnitude = component.magnitude,
                     .shape = shape_of(component.shape),
                     .falloff = falloff_of(component.falloff),
                     .radius = component.radius,
                     .half = Vec3{component.size.x * 0.5F, component.size.y * 0.5F, 1.0F},
                     .mask = component.collision_mask,
                     .noise_frequency = component.noise_frequency,
                     .particles = component.particles,
                     .seed = hash_id(world.id_of(entity)),
                     .place = global,
                     .to_local = global.inverse(),
                     .origin = global.origin,
                     .direction = unit(along),
                     .plane_normal = unit(up)});
    }
}

FieldEffect PhysicsFields2D::at(Vec2 position, std::uint32_t layers, std::uint32_t seed,
                                double time) const {
    FieldEffect effect = fields_.at({position.x, position.y, 0.0F}, layers, seed, time);
    // Noise pushes in the plane only.
    effect.force.z = 0.0F;
    if (effect.velocity) {
        effect.velocity->z = 0.0F;
    }
    return effect;
}

Vec2 PhysicsFields2D::particle_force(Vec2 position, std::uint32_t seed, double time) const {
    const Vec3 force = fields_.particle_force({position.x, position.y, 0.0F}, seed, time);
    return {force.x, force.y};
}

} // namespace slash::world
