// Physics materials and fields on 3D bodies: the .physics_material.json files bodies name, and
// what PhysicsField3Ds do to rigid bodies, physical bones and soft bodies each step, as Unreal's
// Chaos physical materials and fields.

#include "space.hpp"

#include "slash/asset/assets.hpp"
#include "slash/core/log.hpp"
#include "slash/world/physics_field.hpp"
#include "slash/world/world.hpp"

#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Body/BodyLockInterface.h>
#include <Jolt/Physics/SoftBody/SoftBodyMotionProperties.h>
#include <cstdint>
#include <string_view>

namespace slash::physics3d {

namespace {

// FNV-1a of a body's ID, so its noise follows it from session to session.
std::uint32_t seed_of(std::string_view id) {
    std::uint32_t hash = 2166136261U;
    for (const char c : id) {
        hash = (hash ^ static_cast<unsigned char>(c)) * 16777619U;
    }
    return hash;
}

} // namespace

namespace detail {

std::optional<world::PhysicsMaterial> Space::material(const world::World& world,
                                                      const std::string& path) {
    asset::Assets* assets = asset::assets_of(world.ecs());
    if (assets == nullptr) {
        return std::nullopt;
    }
    auto loaded = assets->load<json::Value>(path);
    CachedMaterial& cached = materials_[path];
    if (!loaded) {
        if (cached.version != 1) {
            log::warn("physics material can't be read",
                      {{"path", path}, {"problem", loaded.error().message}});
            cached = {.version = 1, .material = std::nullopt};
        }
        return std::nullopt;
    }
    // Versions start at 1, so 1 above is never a loaded one's; keep them apart by two.
    const std::uint64_t version = loaded.value().version() + 1;
    if (cached.version != version) {
        auto parsed = world::parse_physics_material(*loaded.value());
        if (!parsed) {
            log::warn("physics material is invalid",
                      {{"path", path}, {"problem", parsed.error().message}});
            cached = {.version = version, .material = std::nullopt};
        } else {
            cached = {.version = version, .material = std::move(parsed.value())};
        }
    }
    return cached.material;
}

float Space::mass_of(const Body& body, float own) const {
    if (body.density <= 0.0F || body.shape == nullptr) {
        return own;
    }
    const float volume = body.shape->GetVolume();
    // A shape with no volume (a plane, a mesh) keeps the body's own mass.
    return volume > 0.0F ? body.density * volume : own;
}

void Space::apply_fields(const world::World& world, const world::Tree3D& tree, float seconds) {
    const world::PhysicsFields3D fields(world, tree);
    JPH::BodyInterface& interface = system_->GetBodyInterfaceNoLock();
    const JPH::BodyLockInterfaceNoLock& locks = system_->GetBodyLockInterfaceNoLock();
    // Whole ticks of game time, so noise is the same at the same tick in every session.
    const double time = static_cast<double>(world.tick()) * static_cast<double>(seconds);
    for (auto& [entity, body] : bodies_) {
        if (body->id.IsInvalid()) {
            continue;
        }
        // Fields move rigid bodies and bones; strain breaks any breakable body, as a wall.
        const bool movable = body->kind == Kind::Rigid || body->kind == Kind::Bone;
        const auto threshold = breakable_.find(entity);
        if (!movable && threshold == breakable_.end()) {
            continue;
        }
        JPH::Body* jolt = locks.TryGetBody(body->id);
        if (jolt == nullptr) {
            continue;
        }
        const world::FieldEffect effect =
            fields.empty() ? world::FieldEffect{}
                           : fields.at(from_jolt_place(jolt->GetCenterOfMassPosition()),
                                       body->layer, seed_of(body->name), time);
        if (debug_ != nullptr && !effect.empty()) {
            debug_->fields.push_back({.body = body->name,
                                      .force = effect.force,
                                      .velocity = effect.velocity,
                                      .strain = effect.strain,
                                      .anchored = effect.anchored,
                                      .killed = effect.killed});
        }
        // Strain at or past a breakable body's threshold breaks it after the step, as an impact
        // that strong would.
        if (effect.strain > 0.0F && threshold != breakable_.end() &&
            effect.strain >= threshold->second) {
            Impact& impact = impacts_[entity];
            if (effect.strain > impact.impulse) {
                impact = {.impulse = effect.strain,
                          .point = from_jolt_place(jolt->GetCenterOfMassPosition())};
            }
        }
        if (!movable) {
            continue;
        }
        // Anchors hold dynamic bodies as kinematic ones, and let go when they leave.
        if (effect.anchored && !body->anchored && jolt->IsDynamic()) {
            body->anchored = true;
            interface.SetMotionType(body->id, JPH::EMotionType::Kinematic,
                                    JPH::EActivation::DontActivate);
            interface.SetLinearAndAngularVelocity(body->id, JPH::Vec3::sZero(), JPH::Vec3::sZero());
            continue;
        }
        if (!effect.anchored && body->anchored) {
            body->anchored = false;
            if (body->rigid_key.motion == JPH::EMotionType::Dynamic) {
                interface.SetMotionType(body->id, JPH::EMotionType::Dynamic,
                                        JPH::EActivation::Activate);
                // It goes on as its components last said.
                interface.SetLinearAndAngularVelocity(body->id, to_jolt(body->linear_velocity),
                                                      to_jolt(body->angular_velocity));
            }
        }
        if (effect.empty() || body->anchored || !jolt->IsDynamic()) {
            continue;
        }
        if (effect.killed) {
            doomed_.push_back(entity);
            continue;
        }
        if (effect.force != Vec3{}) {
            interface.AddForce(body->id, to_jolt(effect.force), JPH::EActivation::Activate);
        }
        if (effect.velocity) {
            interface.SetLinearVelocity(body->id, to_jolt(*effect.velocity));
        }
        if (effect.sleep_below && jolt->IsActive() &&
            jolt->GetLinearVelocity().Length() < *effect.sleep_below && effect.force == Vec3{} &&
            !effect.velocity) {
            interface.DeactivateBody(body->id);
        }
    }

    // Soft bodies: each point feels its share of the force at it, or the velocity set there.
    if (fields.empty()) {
        return;
    }
    for (auto* bodies : {&soft_bodies_, &cloths_}) {
        for (auto& [entity, soft] : *bodies) {
            if (soft->id.IsInvalid()) {
                continue;
            }
            JPH::Body* jolt = locks.TryGetBody(soft->id);
            if (jolt == nullptr || !jolt->IsSoftBody()) {
                continue;
            }
            auto& motion = static_cast<JPH::SoftBodyMotionProperties&>( // NOLINT
                *jolt->GetMotionProperties());
            JPH::Array<JPH::SoftBodyVertex>& points = motion.GetVertices();
            if (points.empty()) {
                continue;
            }
            const JPH::RVec3 center = jolt->GetCenterOfMassPosition();
            const std::uint32_t seed = seed_of(soft->name);
            const float share = 1.0F / static_cast<float>(points.size());
            bool moved = false;
            for (JPH::SoftBodyVertex& point : points) {
                if (point.mInvMass <= 0.0F) {
                    continue;
                }
                const world::FieldEffect effect =
                    fields.at(from_jolt_place(center + point.mPosition), soft->layer, seed, time);
                if (effect.velocity) {
                    point.mVelocity = to_jolt(*effect.velocity);
                    moved = true;
                } else if (effect.force != Vec3{}) {
                    // The body's force spread over its points: each takes its share.
                    point.mVelocity += to_jolt(effect.force) * (point.mInvMass * seconds * share);
                    moved = true;
                }
            }
            if (moved) {
                interface.ActivateBody(soft->id);
            }
        }
    }
}

} // namespace detail

} // namespace slash::physics3d
