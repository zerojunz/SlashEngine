// Water against 2D bodies: rigid bodies float by the area of water they displace, measured at
// points through their shapes, and are dragged toward the water's own velocity. Water measures
// its shores by rays down past what moves.

#include "slash/world/water.hpp"

#include "space.hpp"

#include "slash/physics2d/physics2d.hpp"

#include <algorithm>
#include <box2d/box2d.h>
#include <cmath>
#include <cstddef>
#include <optional>
#include <vector>

namespace slash::physics2d::detail {

namespace {

// Points along each side of a body's bounds where it feels the water.
constexpr int kSide = 6;

} // namespace

void Space::apply_water(world::World& world, float seconds) {
    if (world.ecs().view<const world::Water2D>().empty()) {
        return;
    }
    const std::vector<world::WaterBody> waters = world::water_bodies(world, true);
    if (waters.empty()) {
        return;
    }
    const double time = world::water_time(world);
    const Vec2 pull = settings_.gravity_vector * settings_.gravity;
    const float gx = to_float(pull.x);
    const float gy = to_float(pull.y);
    const float g = std::sqrt((gx * gx) + (gy * gy));
    for (auto& [entity, body] : bodies_) {
        if (body->kind != Kind::Rigid || body->anchored || !b2Body_IsValid(body->id) ||
            b2Body_GetType(body->id) != b2_dynamicBody || body->shapes.empty()) {
            continue;
        }
        const b2AABB box = b2Body_ComputeAABB(body->id);
        const float width = box.upperBound.x - box.lowerBound.x;
        const float height = box.upperBound.y - box.lowerBound.y;
        if (!(width > 0.0F) || !(height > 0.0F)) {
            continue;
        }
        const float cell_area = width * height / static_cast<float>(kSide * kSide);
        std::vector<b2Vec2> inside;
        std::vector<b2Vec2> all;
        for (int i = 0; i < kSide; ++i) {
            for (int j = 0; j < kSide; ++j) {
                const b2Vec2 at{box.lowerBound.x + (width * (static_cast<float>(i) + 0.5F) /
                                                    static_cast<float>(kSide)),
                                box.lowerBound.y + (height * (static_cast<float>(j) + 0.5F) /
                                                    static_cast<float>(kSide))};
                all.push_back(at);
                for (const b2ShapeId shape : body->shapes) {
                    if (b2Shape_IsValid(shape) && !b2Shape_IsSensor(shape) &&
                        b2Shape_TestPoint(shape, at)) {
                        inside.push_back(at);
                        break;
                    }
                }
            }
        }
        const std::vector<b2Vec2>& points = inside.empty() ? all : inside;
        const float per_mass = b2Body_GetMass(body->id) / static_cast<float>(points.size());
        std::size_t wet = 0;
        float angular_drag = 0.0F;
        for (const b2Vec2 at : points) {
            for (const world::WaterBody& water : waters) {
                const world::WaterSample sample =
                    world::sample_water(water, {at.x, at.y, 0.0F}, time, false);
                // Below the surface (y down) and above the water's floor.
                // The share of the slab this point stands for under the surface (y down), above
                // the water's floor.
                const float slab = height / static_cast<float>(kSide);
                const float under =
                    std::clamp(((at.y + (slab * 0.5F)) - sample.height) / slab, 0.0F, 1.0F);
                if (!sample.inside || under <= 0.0F || at.y > sample.height + sample.depth) {
                    continue;
                }
                ++wet;
                const world::WaterRecipe& recipe = *water.recipe;
                angular_drag = std::max(angular_drag, recipe.angular_drag);
                const float lift = recipe.density * cell_area * under * g * recipe.buoyancy;
                const b2Vec2 up = g > 0.0F ? b2Vec2{-gx / g, -gy / g} : b2Vec2{0.0F, -1.0F};
                b2Body_ApplyForce(body->id, {up.x * lift, up.y * lift}, at, true);
                const b2Vec2 moving = b2Body_GetWorldPointVelocity(body->id, at);
                const float keep = std::min(recipe.drag * under * seconds, 1.0F);
                const b2Vec2 relative{moving.x - sample.velocity.x, moving.y - sample.velocity.y};
                // Bobbing settles as the waves a body makes carry it away: its speed along gravity
                // is damped where it is wet, however little of it is under.
                const float heave = (relative.x * up.x) + (relative.y * up.y);
                const float settle = std::min(recipe.heave_damping * seconds, 1.0F) * heave;
                b2Body_ApplyLinearImpulse(body->id,
                                          {(-relative.x * keep - up.x * settle) * per_mass,
                                           (-relative.y * keep - up.y * settle) * per_mass},
                                          at, true);
                break;
            }
        }
        if (wet > 0 && angular_drag > 0.0F) {
            const float share = static_cast<float>(wet) / static_cast<float>(points.size());
            const float keep = 1.0F - std::min(angular_drag * share * seconds, 1.0F);
            b2Body_SetAngularVelocity(body->id, b2Body_GetAngularVelocity(body->id) * keep);
        }
    }
}

world::WaterHost water_host_2d(world::World& world) {
    world::WaterHost host;
    world::World* held = &world;
    host.ground = [held](Vec3f from, float reach) -> std::optional<float> {
        QueryFilter filter;
        Vec2 start{from.x, from.y};
        const Vec2 end{from.x, from.y + reach};
        for (int tries = 0; tries < 8; ++tries) {
            const auto hit = raycast(*held, start, end, filter);
            if (!hit) {
                return std::nullopt;
            }
            const auto& ecs = held->ecs();
            if (!ecs.all_of<world::RigidBody2D>(hit->entity) &&
                !ecs.all_of<world::CharacterMover2D>(hit->entity)) {
                return to_float(hit->point.y);
            }
            filter.exclude.push_back(hit->entity);
            start = hit->point;
        }
        return std::nullopt;
    };
    return host;
}

} // namespace slash::physics2d::detail
