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
#include <Jolt/Physics/SoftBody/SoftBodyMotionProperties.h>
#include <algorithm>
#include <array>
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

// How thick a soft body's sheet is to the fluid on each side, past its points' own radius.
constexpr float kSheetThickness = 0.01F;

// The point of a triangle nearest p, as weights of its corners (Ericson's Real-Time Collision
// Detection, 5.1.5).
std::array<float, 3> nearest_weights(JPH::Vec3 p, JPH::Vec3 a, JPH::Vec3 b, JPH::Vec3 c) {
    const JPH::Vec3 ab = b - a;
    const JPH::Vec3 ac = c - a;
    const JPH::Vec3 ap = p - a;
    const float d1 = ab.Dot(ap);
    const float d2 = ac.Dot(ap);
    if (d1 <= 0.0F && d2 <= 0.0F) {
        return {1.0F, 0.0F, 0.0F};
    }
    const JPH::Vec3 bp = p - b;
    const float d3 = ab.Dot(bp);
    const float d4 = ac.Dot(bp);
    if (d3 >= 0.0F && d4 <= d3) {
        return {0.0F, 1.0F, 0.0F};
    }
    const float vc = (d1 * d4) - (d3 * d2);
    if (vc <= 0.0F && d1 >= 0.0F && d3 <= 0.0F) {
        const float v = d1 / (d1 - d3);
        return {1.0F - v, v, 0.0F};
    }
    const JPH::Vec3 cp = p - c;
    const float d5 = ab.Dot(cp);
    const float d6 = ac.Dot(cp);
    if (d6 >= 0.0F && d5 <= d6) {
        return {0.0F, 0.0F, 1.0F};
    }
    const float vb = (d5 * d2) - (d1 * d6);
    if (vb <= 0.0F && d2 >= 0.0F && d6 <= 0.0F) {
        const float w = d2 / (d2 - d6);
        return {1.0F - w, 0.0F, w};
    }
    const float va = (d3 * d6) - (d5 * d4);
    if (va <= 0.0F && (d4 - d3) >= 0.0F && (d5 - d6) >= 0.0F) {
        const float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return {0.0F, 1.0F - w, w};
    }
    const float total = va + vb + vc;
    if (total <= 0.0F) {
        return {1.0F / 3.0F, 1.0F / 3.0F, 1.0F / 3.0F};
    }
    const float v = vb / total;
    const float w = vc / total;
    return {1.0F - v - w, v, w};
}

const JPH::SoftBodyMotionProperties& soft_motion(const JPH::Body& body) {
    return static_cast<const JPH::SoftBodyMotionProperties&>( // NOLINT
        *body.GetMotionPropertiesUnchecked());
}

// The face of a soft body nearest a point (relative to its center of mass), with its weights and
// how far it is; nullopt for a body with no faces.
struct NearestFace {
    std::size_t face = 0;
    std::array<float, 3> weights{};
    JPH::Vec3 point;
    float distance = 0.0F;
};
std::optional<NearestFace> nearest_face(const JPH::SoftBodyMotionProperties& motion, JPH::Vec3 p,
                                        float reach) {
    const JPH::Array<JPH::SoftBodyVertex>& points = motion.GetVertices();
    const auto& faces = motion.GetSettings()->mFaces;
    std::optional<NearestFace> best;
    for (std::size_t f = 0; f < faces.size(); ++f) {
        const auto& corners = faces[f].mVertex;
        const JPH::Vec3 a = points[corners[0]].mPosition;
        const JPH::Vec3 b = points[corners[1]].mPosition;
        const JPH::Vec3 c = points[corners[2]].mPosition;
        const JPH::Vec3 low =
            JPH::Vec3::sMin(JPH::Vec3::sMin(a, b), c) - JPH::Vec3::sReplicate(reach);
        const JPH::Vec3 high =
            JPH::Vec3::sMax(JPH::Vec3::sMax(a, b), c) + JPH::Vec3::sReplicate(reach);
        if (JPH::Vec3::sLess(p, low).TestAnyXYZTrue() ||
            JPH::Vec3::sGreater(p, high).TestAnyXYZTrue()) {
            continue;
        }
        const std::array<float, 3> weights = nearest_weights(p, a, b, c);
        const JPH::Vec3 on = (a * weights[0]) + (b * weights[1]) + (c * weights[2]);
        const float distance = (p - on).Length();
        if (distance < reach && (!best || distance < best->distance)) {
            best = NearestFace{.face = f, .weights = weights, .point = on, .distance = distance};
        }
    }
    return best;
}

// Particles against a soft body's or a cloth's faces, a sheet as thick as its points' radius.
void collide_soft(const JPH::Body& body, const SoftBody& soft, std::span<const Vec3f> points,
                  float radius, std::vector<std::optional<world::FluidContact>>& out) {
    const JPH::SoftBodyMotionProperties& motion = soft_motion(body);
    const JPH::Array<JPH::SoftBodyVertex>& vertices = motion.GetVertices();
    const auto& faces = motion.GetSettings()->mFaces;
    if (faces.empty()) {
        return;
    }
    const float reach = radius + motion.GetVertexRadius() + kSheetThickness;
    JPH::AABox bounds = body.GetWorldSpaceBounds();
    bounds.ExpandBy(JPH::Vec3::sReplicate(reach));
    const JPH::RVec3 center = body.GetCenterOfMassPosition();
    for (std::size_t i = 0; i < points.size(); ++i) {
        const JPH::Vec3 at(points[i].x, points[i].y, points[i].z);
        if (!bounds.Contains(at)) {
            continue;
        }
        const JPH::Vec3 local = JPH::Vec3(JPH::RVec3(at) - center);
        const std::optional<NearestFace> found = nearest_face(motion, local, reach);
        if (!found) {
            continue;
        }
        const NearestFace& near = *found;
        const float depth = reach - near.distance;
        if (const auto& had = out[i]; had && had->depth >= depth) {
            continue;
        }
        const auto& corners = faces[near.face].mVertex;
        JPH::Vec3 normal = JPH::Vec3::sAxisY();
        if (near.distance > 1e-6F) {
            normal = (local - near.point) / near.distance;
        } else {
            const JPH::Vec3 a = vertices[corners[0]].mPosition;
            normal = (vertices[corners[1]].mPosition - a)
                         .Cross(vertices[corners[2]].mPosition - a)
                         .NormalizedOr(JPH::Vec3::sAxisY());
        }
        const JPH::Vec3 velocity = (vertices[corners[0]].mVelocity * near.weights[0]) +
                                   (vertices[corners[1]].mVelocity * near.weights[1]) +
                                   (vertices[corners[2]].mVelocity * near.weights[2]);
        out[i] = world::FluidContact{.normal = to_vec3f(normal),
                                     .depth = depth,
                                     .velocity = to_vec3f(velocity),
                                     .body = soft.entity};
    }
}

// Hands a soft body's points the push a particle of a mass gave it at a place: the nearest face's
// corners by their weights, each as if it and the particle moved together, so a light point takes
// no more speed than the particle had.
void push_soft(Space& space, const SoftBody& soft, JPH::Vec3 impulse, JPH::RVec3 at, float mass) {
    JPH::PhysicsSystem* system = space.system();
    JPH::Body* body = system->GetBodyLockInterfaceNoLock().TryGetBody(soft.id);
    if (body == nullptr) {
        return;
    }
    auto& motion = static_cast<JPH::SoftBodyMotionProperties&>( // NOLINT
        *body->GetMotionPropertiesUnchecked());
    const JPH::Vec3 local = JPH::Vec3(at - body->GetCenterOfMassPosition());
    const float reach = (2.0F * motion.GetVertexRadius()) + 1.0F;
    const std::optional<NearestFace> near = nearest_face(motion, local, reach);
    if (!near) {
        return;
    }
    JPH::Array<JPH::SoftBodyVertex>& vertices = motion.GetVertices();
    const auto& corners = motion.GetSettings()->mFaces[near->face].mVertex;
    bool moved = false;
    for (std::size_t c = 0; c < 3; ++c) {
        JPH::SoftBodyVertex& vertex = vertices[corners[c]];
        if (vertex.mInvMass > 0.0F && near->weights[c] > 0.0F) {
            const float together = (1.0F / vertex.mInvMass) + std::max(mass, 0.0F);
            vertex.mVelocity += impulse * (near->weights[c] / together);
            moved = true;
        }
    }
    if (moved) {
        system->GetBodyInterfaceNoLock().ActivateBody(soft.id);
    }
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
        if (body == nullptr || body->IsSensor()) {
            continue;
        }
        if (body->IsSoftBody()) {
            if (const SoftBody* soft = space.soft_body(id); soft != nullptr) {
                collide_soft(*body, *soft, points, radius, out);
            }
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
    host.push = [weak](entt::entity body, Vec3f impulse, Vec3f at, float mass) {
        const auto held = weak.lock();
        if (held == nullptr || held->system() == nullptr) {
            return;
        }
        if (const SoftBody* soft = held->soft_body(body); soft != nullptr) {
            push_soft(*held, *soft, JPH::Vec3(impulse.x, impulse.y, impulse.z),
                      JPH::RVec3(at.x, at.y, at.z), mass);
            return;
        }
        const Body* owner = held->body(body);
        if (owner == nullptr || owner->kind != Kind::Rigid) {
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
