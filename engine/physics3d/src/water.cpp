// Water against 3D bodies: rigid bodies float by the water they displace, measured at points
// through their shapes, and are dragged toward the water's own velocity (its waves' motion and
// current), as Unreal's buoyancy component pontoons, with every point a pontoon. Water measures
// its shores by rays down past what moves.

#include "slash/world/water.hpp"

#include "space.hpp"

#include "slash/physics3d/physics3d.hpp"

#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Body/BodyLockInterface.h>
#include <Jolt/Physics/Collision/CollidePointResult.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/TransformedShape.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <optional>
#include <vector>

namespace slash::physics3d::detail {

namespace {

// Points along each side of a body's bounds where it feels the water.
constexpr int kSide = 4;

struct Column {
    world::WaterSample sample;
    const world::WaterBody* water = nullptr;
};

} // namespace

void Space::apply_water(world::World& world, float seconds) {
    if (world.ecs().view<const world::Water3D>().empty() || system_ == nullptr) {
        return;
    }
    const std::vector<world::WaterBody> waters = world::water_bodies(world, false);
    if (waters.empty()) {
        return;
    }
    const double time = world::water_time(world);
    JPH::BodyInterface& interface = system_->GetBodyInterfaceNoLock();
    const JPH::BodyLockInterfaceNoLock& locks = system_->GetBodyLockInterfaceNoLock();
    const Vec3f gravity =
        to_float(settings_.gravity_vector * static_cast<real_t>(settings_.gravity));
    const float g =
        std::sqrt((gravity.x * gravity.x) + (gravity.y * gravity.y) + (gravity.z * gravity.z));
    for (auto& [entity, body] : bodies_) {
        if (body->kind != Kind::Rigid || body->id.IsInvalid() || body->anchored) {
            continue;
        }
        JPH::Body* jolt = locks.TryGetBody(body->id);
        if (jolt == nullptr || !jolt->IsDynamic() || jolt->IsSensor() ||
            jolt->GetMotionProperties() == nullptr) {
            continue;
        }
        const JPH::AABox box = jolt->GetWorldSpaceBounds();
        const JPH::Vec3 low = box.mMin;
        const JPH::Vec3 size = box.GetSize();
        // The water over each column of points, from the first body of water it is in.
        std::array<Column, static_cast<std::size_t>(kSide * kSide)> columns{};
        bool any = false;
        for (int i = 0; i < kSide; ++i) {
            for (int k = 0; k < kSide; ++k) {
                const float x = low.GetX() + (size.GetX() * (static_cast<float>(i) + 0.5F) /
                                              static_cast<float>(kSide));
                const float z = low.GetZ() + (size.GetZ() * (static_cast<float>(k) + 0.5F) /
                                              static_cast<float>(kSide));
                Column& column =
                    columns.at((static_cast<std::size_t>(i) * static_cast<std::size_t>(kSide)) +
                               static_cast<std::size_t>(k));
                for (const world::WaterBody& water : waters) {
                    const world::WaterSample sample =
                        world::sample_water(water, {x, low.GetY(), z}, time, false);
                    if (sample.inside && sample.height > low.GetY()) {
                        column = {.sample = sample, .water = &water};
                        any = true;
                        break;
                    }
                }
            }
        }
        if (!any) {
            continue;
        }
        // Which points are inside the body: its shape's volume shared among them.
        const JPH::TransformedShape shape = jolt->GetTransformedShape();
        struct Point {
            JPH::RVec3 at;
            const Column* column = nullptr;
        };
        std::vector<Point> inside;
        std::vector<Point> all;
        for (int i = 0; i < kSide; ++i) {
            for (int j = 0; j < kSide; ++j) {
                for (int k = 0; k < kSide; ++k) {
                    const auto share = [&](int n, float extent) {
                        return extent * (static_cast<float>(n) + 0.5F) / static_cast<float>(kSide);
                    };
                    const JPH::RVec3 at(low.GetX() + share(i, size.GetX()),
                                        low.GetY() + share(j, size.GetY()),
                                        low.GetZ() + share(k, size.GetZ()));
                    const Point point{.at = at,
                                      .column = &columns.at((static_cast<std::size_t>(i) *
                                                             static_cast<std::size_t>(kSide)) +
                                                            static_cast<std::size_t>(k))};
                    all.push_back(point);
                    JPH::AnyHitCollisionCollector<JPH::CollidePointCollector> hit;
                    shape.CollidePoint(at, hit);
                    if (hit.HadHit()) {
                        inside.push_back(point);
                    }
                }
            }
        }
        // A shape no point is inside of (a thin plate) or that can't say (a mesh) floats as
        // its bounds.
        const std::vector<Point>& points = inside.empty() ? all : inside;
        float volume = jolt->GetShape()->GetVolume();
        if (!(volume > 0.0F)) {
            volume = size.GetX() * size.GetY() * size.GetZ();
        }
        const float per_volume = volume / static_cast<float>(points.size());
        const float mass = 1.0F / std::max(jolt->GetMotionProperties()->GetInverseMass(), 1e-9F);
        const float per_mass = mass / static_cast<float>(points.size());
        std::size_t wet = 0;
        float angular_drag = 0.0F;
        for (const Point& point : points) {
            const Column& column = *point.column;
            if (column.water == nullptr) {
                continue;
            }
            // The share of the slab of the body this point stands for that is under water, so
            // a body sinking into it is lifted smoothly.
            const float y = point.at.GetY();
            const float slab = size.GetY() / static_cast<float>(kSide);
            const float under =
                std::clamp((column.sample.height - (y - (slab * 0.5F))) / slab, 0.0F, 1.0F);
            if (under <= 0.0F) {
                continue;
            }
            ++wet;
            const world::WaterRecipe& recipe = *column.water->recipe;
            angular_drag = std::max(angular_drag, recipe.angular_drag);
            // Archimedes: the weight of the water this point stands for, against gravity.
            const float lift = recipe.density * per_volume * under * g * recipe.buoyancy;
            const Vec3f up = g > 0.0F ? gravity * (-1.0F / g) : Vec3f{0.0F, 1.0F, 0.0F};
            interface.AddForce(body->id, JPH::Vec3(up.x * lift, up.y * lift, up.z * lift), point.at,
                               JPH::EActivation::Activate);
            // Drag toward the water's own velocity, as an impulse that can't overshoot it.
            const JPH::Vec3 moving = jolt->GetPointVelocity(point.at);
            const Vec3f water = column.sample.velocity;
            const float keep = std::min(recipe.drag * under * seconds, 1.0F);
            const JPH::Vec3 relative(moving.GetX() - water.x, moving.GetY() - water.y,
                                     moving.GetZ() - water.z);
            // Bobbing settles as the waves a body makes carry it away: its speed along gravity is
            // damped where it is wet, however little of it is under.
            const JPH::Vec3 rise(up.x, up.y, up.z);
            const float settle =
                std::min(recipe.heave_damping * seconds, 1.0F) * relative.Dot(rise);
            interface.AddImpulse(
                body->id, (relative * (-per_mass * keep)) - (rise * (per_mass * settle)), point.at);
        }
        if (wet > 0 && angular_drag > 0.0F) {
            const float share = static_cast<float>(wet) / static_cast<float>(points.size());
            const float keep = 1.0F - std::min(angular_drag * share * seconds, 1.0F);
            interface.SetAngularVelocity(body->id, jolt->GetAngularVelocity() * keep);
        }
    }
}

world::WaterHost water_host(world::World& world) {
    world::WaterHost host;
    world::World* held = &world;
    host.ground = [held](Vec3f from, float reach) -> std::optional<float> {
        QueryFilter filter;
        Vec3 start{from.x, from.y, from.z};
        const Vec3 end{from.x, from.y - reach, from.z};
        // Past what moves (boats, crates, swimmers) to what stays.
        for (int tries = 0; tries < 8; ++tries) {
            const auto hit = raycast(*held, start, end, filter);
            if (!hit) {
                return std::nullopt;
            }
            const auto& ecs = held->ecs();
            if (!ecs.all_of<world::RigidBody3D>(hit->entity) &&
                !ecs.all_of<world::CharacterBody3D>(hit->entity)) {
                return to_float(hit->point.y);
            }
            filter.exclude.push_back(hit->entity);
            start = hit->point;
        }
        return std::nullopt;
    };
    return host;
}

} // namespace slash::physics3d::detail
