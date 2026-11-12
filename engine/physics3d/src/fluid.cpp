// Fluids against 3D bodies: each particle a sphere tested against the bodies whose bounds it
// reaches, and the push the fluid gives back to rigid bodies as impulses.
#include "slash/world/fluid.hpp"

#include "space.hpp"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyLockInterface.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseQuery.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/TransformedShape.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace slash::physics3d::detail {

namespace {

// The most bodies one fluid tests in a step, nearest the fluid's middle first in the broad
// phase's order.
constexpr std::size_t kMaxFluidBodies = 256;

class BodiesFound final : public JPH::CollideShapeBodyCollector {
public:
    void AddHit(const JPH::BodyID& id) override {
        if (ids.size() < kMaxFluidBodies) {
            ids.push_back(id);
        }
    }
    std::vector<JPH::BodyID> ids;
};

// The deepest contact of a sphere with one body.
class Deepest final : public JPH::CollideShapeCollector {
public:
    void AddHit(const JPH::CollideShapeResult& hit) override {
        if (!found || hit.mPenetrationDepth > best.mPenetrationDepth) {
            best = hit;
            found = true;
        }
    }
    JPH::CollideShapeResult best;
    bool found = false;
};

Vec3f to_vec3f(JPH::Vec3 v) {
    return {v.GetX(), v.GetY(), v.GetZ()};
}

void collide(const Space& space, std::span<const Vec3f> points, float radius,
             std::vector<std::optional<world::FluidContact>>& out) {
    out.assign(points.size(), std::nullopt);
    const JPH::PhysicsSystem* system = space.system();
    if (system == nullptr || points.empty()) {
        return;
    }
    // The bodies the particles' bounds reach, in body ID order so steps repeat.
    JPH::Vec3 low(points[0].x, points[0].y, points[0].z);
    JPH::Vec3 high = low;
    for (const Vec3f& p : points) {
        const JPH::Vec3 at(p.x, p.y, p.z);
        low = JPH::Vec3::sMin(low, at);
        high = JPH::Vec3::sMax(high, at);
    }
    const JPH::Vec3 pad = JPH::Vec3::sReplicate(radius);
    BodiesFound found;
    system->GetBroadPhaseQuery().CollideAABox(JPH::AABox(low - pad, high + pad), found);
    std::sort(found.ids.begin(), found.ids.end());
    const JPH::BodyLockInterfaceNoLock& locks = system->GetBodyLockInterfaceNoLock();
    const JPH::SphereShape sphere(radius);
    JPH::CollideShapeSettings settings;
    settings.mBackFaceMode = JPH::EBackFaceMode::CollideWithBackFaces;
    for (const JPH::BodyID& id : found.ids) {
        const JPH::Body* body = locks.TryGetBody(id);
        if (body == nullptr || body->IsSensor() || body->IsSoftBody()) {
            continue;
        }
        const Body* owner = Space::body_of(*body);
        if (owner == nullptr || owner->kind == Kind::Area) {
            continue;
        }
        JPH::AABox bounds = body->GetWorldSpaceBounds();
        bounds.ExpandBy(pad);
        const JPH::TransformedShape shape = body->GetTransformedShape();
        const Vec3f com = to_float(from_jolt_place(body->GetCenterOfMassPosition()));
        const JPH::Vec3 linear = body->GetLinearVelocity();
        const JPH::Vec3 angular = body->GetAngularVelocity();
        for (std::size_t i = 0; i < points.size(); ++i) {
            const JPH::Vec3 at(points[i].x, points[i].y, points[i].z);
            if (!bounds.Contains(at)) {
                continue;
            }
            Deepest deepest;
            shape.CollideShape(&sphere, JPH::Vec3::sOne(),
                               JPH::RMat44::sTranslation(JPH::RVec3(at)), settings,
                               JPH::RVec3::sZero(), deepest);
            if (!deepest.found || deepest.best.mPenetrationDepth <= 0.0F) {
                continue;
            }
            if (out[i] && out[i]->depth >= deepest.best.mPenetrationDepth) {
                continue;
            }
            // The axis moves the body out of the sphere; the particle goes the other way.
            JPH::Vec3 axis = deepest.best.mPenetrationAxis;
            const float axis_length = axis.Length();
            if (axis_length <= 1e-9F) {
                continue;
            }
            axis = axis / axis_length;
            const JPH::Vec3 arm(points[i].x - com.x, points[i].y - com.y, points[i].z - com.z);
            out[i] = world::FluidContact{.normal = to_vec3f(-axis),
                                         .depth = deepest.best.mPenetrationDepth,
                                         .velocity = to_vec3f(linear + angular.Cross(arm)),
                                         .body = owner->entity};
        }
    }
}

} // namespace

world::FluidHost fluid_host(const std::shared_ptr<Space>& space) {
    world::FluidHost host;
    const std::weak_ptr<Space> weak = space;
    host.collide = [weak](std::span<const Vec3f> points, float radius,
                          std::vector<std::optional<world::FluidContact>>& out) {
        if (const auto held = weak.lock(); held != nullptr) {
            collide(*held, points, radius, out);
        } else {
            out.assign(points.size(), std::nullopt);
        }
    };
    host.push = [weak](entt::entity body, Vec3f impulse, Vec3f at) {
        const auto held = weak.lock();
        const Body* owner = held != nullptr ? held->body(body) : nullptr;
        if (owner == nullptr || owner->kind != Kind::Rigid || held->system() == nullptr) {
            return;
        }
        const JPH::Body* jolt = held->system()->GetBodyLockInterfaceNoLock().TryGetBody(owner->id);
        if (jolt == nullptr) {
            return;
        }
        const Vec3 origin = from_jolt_place(jolt->GetPosition());
        held->queue({.entity = body,
                     .impulse = Vec3{impulse.x, impulse.y, impulse.z},
                     .offset = Vec3{at.x, at.y, at.z} - origin,
                     .torque = {}});
    };
    host.gravity = [weak]() {
        const auto held = weak.lock();
        if (held == nullptr) {
            return Vec3f{0.0F, -9.8F, 0.0F};
        }
        const Settings& settings = held->settings();
        return to_float(settings.gravity_vector * static_cast<real_t>(settings.gravity));
    };
    return host;
}

} // namespace slash::physics3d::detail
