// 2D cloth, as Unreal's Chaos Cloth on a cut-out (Box2D and Godot have none): a Cloth2D's sheet
// of points carried by its entity's transform, the skinned ones exactly and the rest up to their
// max distance, held together by edges, bends and long range attachments and pushed out of
// Box2D's shapes, solved with small steps of position based dynamics.
#include "space.hpp"

#include "slash/core/math.hpp"
#include "slash/world/cloth2d.hpp"
#include "slash/world/physics_field.hpp"
#include "slash/world/wind.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

namespace slash::physics2d::detail {

namespace {

// How fast a point takes up the wind's speed at a drag coefficient of 1, per second.
constexpr float kWindRate = 4.0F;

std::uint32_t seed_of(std::string_view id) {
    std::uint32_t hash = 2166136261U;
    for (const char c : id) {
        hash = (hash ^ static_cast<unsigned char>(c)) * 16777619U;
    }
    return hash;
}

real_t length(Vec2 v) {
    return std::sqrt((v.x * v.x) + (v.y * v.y));
}

// The share of a constraint's error one of n small steps puts right, so that n steps together
// put right stiffness of it.
float per_step(float stiffness, int steps) {
    const float s = std::clamp(stiffness, 0.0F, 1.0F);
    if (s >= 1.0F) {
        return 1.0F;
    }
    if (s <= 0.0F) {
        return 0.0F;
    }
    return 1.0F - slash::exp(slash::ln(1.0F - s) / static_cast<float>(steps));
}

// Moves two points toward a length, each by its share.
void hold(Vec2& a, Vec2& b, bool a_free, bool b_free, real_t length_at_rest, float stiffness) {
    const real_t w = (a_free ? 1.0F : 0.0F) + (b_free ? 1.0F : 0.0F);
    const Vec2 d = b - a;
    const real_t at = length(d);
    if (w == 0.0F || at <= 0.0F) {
        return;
    }
    const Vec2 fix = d * (((at - length_at_rest) / (at * w)) * stiffness);
    if (a_free) {
        a = a + fix;
    }
    if (b_free) {
        b = b - fix;
    }
}

// Keeps a point within reach of a place.
void within(Vec2& point, Vec2 place, real_t reach) {
    const Vec2 d = point - place;
    const real_t at = length(d);
    if (at > reach && at > 0.0F) {
        point = place + (d * (reach / at));
    }
}

struct RayHit {
    std::uint32_t mask = 0;
    bool hit = false;
    b2Vec2 point{};
    b2Vec2 normal{};
};

float closest_solid(b2ShapeId shape, b2Vec2 point, b2Vec2 normal, float fraction, void* context) {
    auto& out = *static_cast<RayHit*>(context);
    const ShapeInfo* info = info_of(shape);
    if (info == nullptr || info->kind == Kind::Area || (info->layer & out.mask) == 0) {
        return -1.0F;
    }
    out.hit = true;
    out.point = point;
    out.normal = normal;
    return fraction;
}

struct Near {
    std::uint32_t mask = 0;
    std::vector<b2ShapeId> shapes;
};

bool collect_solid(b2ShapeId shape, void* context) {
    auto& out = *static_cast<Near*>(context);
    const ShapeInfo* info = info_of(shape);
    if (info != nullptr && info->kind != Kind::Area && (info->layer & out.mask) != 0) {
        out.shapes.push_back(shape);
    }
    return true;
}

} // namespace

void Space::step_cloths(world::World& world, const world::Tree2D& tree, float seconds) {
    const entt::registry& registry = world.ecs();
    cloths_.erase_if([&](entt::entity entity, const std::unique_ptr<Cloth2DBody>& /*cloth*/) {
        return !world.valid(entity) || !registry.all_of<world::Cloth2D>(entity);
    });
    const auto view = registry.view<const world::Cloth2D>();
    if (view.empty() || seconds <= 0.0F) {
        return;
    }
    const world::PhysicsFields2D fields(world, tree);
    const world::WindField2D wind(world, tree);
    const double wind_time = world::wind_time(world);
    const double field_time = static_cast<double>(world.tick()) * static_cast<double>(seconds);
    const Vec2 gravity = settings_.gravity_vector * settings_.gravity;
    const bool solids = b2World_IsValid(world_);
    for (const entt::entity entity : view) {
        const auto& cloth = registry.get<const world::Cloth2D>(entity);
        const int columns = world::cloth_columns(cloth);
        const int rows = world::cloth_rows(cloth);
        const std::vector<Vec2> targets = world::cloth_rest_2d(cloth, tree.global_affine(entity));
        const std::size_t count = targets.size();
        Cloth2DBody* body = nullptr;
        if (std::unique_ptr<Cloth2DBody>* found = cloths_.find(entity); found != nullptr) {
            body = found->get();
        } else {
            auto made = std::make_unique<Cloth2DBody>();
            made->entity = entity;
            made->name = std::string(world.id_of(entity));
            body = cloths_.emplace(entity, std::move(made)).get();
        }
        // A new sheet, or one cut into other points, starts where its transform puts it.
        if (body->columns != columns || body->rows != rows ||
            body->points.current.size() != count) {
            body->columns = columns;
            body->rows = rows;
            body->points.current = targets;
            body->points.previous = targets;
            body->targets = targets;
            body->velocity.assign(count, Vec2{});
            // Each point's neighbors across and down, and diagonally and two along for bending.
            body->edges.clear();
            body->bends.clear();
            const auto at = [columns](int r, int c) { return (r * columns) + c; };
            for (int r = 0; r < rows; ++r) {
                for (int c = 0; c < columns; ++c) {
                    if (c + 1 < columns) {
                        body->edges.push_back({at(r, c), at(r, c + 1)});
                    }
                    if (r + 1 < rows) {
                        body->edges.push_back({at(r, c), at(r + 1, c)});
                    }
                    // Diagonals keep it from shearing as hard as bends keep it flat: a
                    // sheet held along its top swings sideways only by shearing.
                    if (r + 1 < rows && c + 1 < columns) {
                        body->bends.push_back({at(r, c), at(r + 1, c + 1)});
                        body->bends.push_back({at(r, c + 1), at(r + 1, c)});
                    }
                    if (c + 2 < columns) {
                        body->bends.push_back({at(r, c), at(r, c + 2)});
                    }
                    if (r + 2 < rows) {
                        body->bends.push_back({at(r, c), at(r + 2, c)});
                    }
                }
            }
        }
        // Each point's reach from where the transform puts it, and the skinned point nearest
        // across the sheet at rest that tethers it.
        const std::vector<float> shares = world::cloth_shares_2d(cloth);
        std::vector<real_t> reach(count);
        std::vector<bool> free(count);
        for (std::size_t i = 0; i < count; ++i) {
            reach[i] = std::max(cloth.max_distance, 0.0F) * std::clamp(shares[i], 0.0F, 1.0F);
            free[i] = reach[i] > 0.0F;
        }
        std::vector<int> anchor(count, -1);
        if (cloth.long_range_attachment) {
            for (std::size_t i = 0; i < count; ++i) {
                real_t best = 0.0F;
                for (std::size_t j = 0; j < count && free[i]; ++j) {
                    if (free[j]) {
                        continue;
                    }
                    const real_t d = length(targets[j] - targets[i]);
                    if (anchor[i] < 0 || d < best) {
                        anchor[i] = static_cast<int>(j);
                        best = d;
                    }
                }
            }
        }
        const auto free_count = static_cast<float>(std::ranges::count(free, true));
        const float inverse_mass =
            cloth.total_mass > 0.0F && free_count > 0.0F ? free_count / cloth.total_mass : 1.0F;
        const float share = free_count > 0.0F ? 1.0F / free_count : 0.0F;
        const std::uint32_t seed = seed_of(body->name);
        const int steps = std::clamp(cloth.simulation_precision, 1, 100);
        const real_t h = seconds / static_cast<float>(steps);
        const float stretch = per_step(cloth.stiffness, steps);
        const float bend = per_step(cloth.bend_stiffness, steps);
        const real_t radius = std::max(cloth.point_radius, 0.0F);
        std::vector<Vec2>& x = body->points.current;
        std::vector<Vec2>& v = body->velocity;
        body->points.previous = x;
        std::vector<Vec2> place(count);
        std::vector<Vec2> start(count);
        for (int step = 0; step < steps; ++step) {
            const real_t f = static_cast<float>(step + 1) / static_cast<float>(steps);
            for (std::size_t i = 0; i < count; ++i) {
                place[i] = body->targets[i] + ((targets[i] - body->targets[i]) * f);
            }
            // Gravity, fields and the wind move the free points; the skinned ones go where the
            // transform takes them.
            for (std::size_t i = 0; i < count; ++i) {
                start[i] = x[i];
                if (!free[i]) {
                    x[i] = place[i];
                    v[i] = Vec2{};
                    continue;
                }
                Vec2 accelerate = gravity * cloth.gravity_scale;
                if (cloth.drag_coefficient > 0.0F && !wind.empty()) {
                    accelerate = accelerate + ((wind.at(x[i], wind_time) - v[i]) *
                                               (cloth.drag_coefficient * kWindRate));
                }
                if (!fields.empty()) {
                    const world::FieldEffect effect =
                        fields.at(x[i], cloth.collision_mask, seed, field_time);
                    if (effect.velocity) {
                        v[i] = Vec2{effect.velocity->x, effect.velocity->y};
                    } else {
                        accelerate = accelerate + (Vec2{effect.force.x, effect.force.y} *
                                                   (inverse_mass * share));
                    }
                }
                v[i] = v[i] + (accelerate * h);
                v[i] = v[i] * std::max<real_t>(0.0F, 1.0F - (cloth.damping_coefficient * h));
                x[i] = x[i] + (v[i] * h);
            }
            for (const auto& [a, b] : body->edges) {
                hold(
                    x[static_cast<std::size_t>(a)], x[static_cast<std::size_t>(b)],
                    free[static_cast<std::size_t>(a)], free[static_cast<std::size_t>(b)],
                    length(place[static_cast<std::size_t>(b)] - place[static_cast<std::size_t>(a)]),
                    stretch);
            }
            for (const auto& [a, b] : body->bends) {
                hold(
                    x[static_cast<std::size_t>(a)], x[static_cast<std::size_t>(b)],
                    free[static_cast<std::size_t>(a)], free[static_cast<std::size_t>(b)],
                    length(place[static_cast<std::size_t>(b)] - place[static_cast<std::size_t>(a)]),
                    bend);
            }
            for (std::size_t i = 0; i < count; ++i) {
                if (!free[i]) {
                    continue;
                }
                if (const int a = anchor[i]; a >= 0) {
                    const Vec2 tether = place[static_cast<std::size_t>(a)];
                    within(x[i], tether, length(place[i] - tether));
                }
                within(x[i], place[i], reach[i]);
                if (solids && radius > 0.0F) {
                    // Through a shape on the way: back out at its surface.
                    if (const Vec2 moved = x[i] - start[i]; moved != Vec2{}) {
                        RayHit ray{.mask = cloth.collision_mask};
                        b2World_CastRay(world_, to_b2(start[i]), to_b2(moved),
                                        {.categoryBits = kQueryBit, .maskBits = ~std::uint64_t{0}},
                                        closest_solid, &ray);
                        if (ray.hit) {
                            x[i] = from_b2(b2MulAdd(ray.point, radius, ray.normal));
                        }
                    }
                    // Near a shape: out to the point's radius from it.
                    Near near{.mask = cloth.collision_mask, .shapes = {}};
                    const b2AABB box{.lowerBound = to_b2(x[i] - Vec2{radius, radius}),
                                     .upperBound = to_b2(x[i] + Vec2{radius, radius})};
                    b2World_OverlapAABB(world_, box,
                                        {.categoryBits = kQueryBit, .maskBits = ~std::uint64_t{0}},
                                        collect_solid, &near);
                    for (const b2ShapeId shape : near.shapes) {
                        const Vec2 closest = from_b2(b2Shape_GetClosestPoint(shape, to_b2(x[i])));
                        const Vec2 away = x[i] - closest;
                        const real_t d = length(away);
                        if (d > 0.0F && d < radius) {
                            x[i] = closest + (away * (radius / d));
                        }
                    }
                }
            }
            for (std::size_t i = 0; i < count; ++i) {
                if (free[i]) {
                    v[i] = (x[i] - start[i]) * (1.0F / h);
                }
            }
        }
        body->targets = targets;
    }
}

const world::ClothPoints2D* Space::cloth_points(entt::entity entity) const {
    const std::unique_ptr<Cloth2DBody>* found = cloths_.find(entity);
    return found != nullptr ? &(*found)->points : nullptr;
}

} // namespace slash::physics2d::detail
