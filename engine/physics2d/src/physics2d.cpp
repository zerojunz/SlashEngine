#include "slash/physics2d/physics2d.hpp"

#include "polygon.hpp"
#include "space.hpp"

#include "slash/core/assert.hpp"

#include <algorithm>
#include <array>
#include <memory>
#include <string_view>
#include <utility>

namespace slash::physics2d {

namespace {

using detail::find_space;
using detail::from_b2;
using detail::info_of;
using detail::Kind;
using detail::ShapeInfo;
using detail::Space;
using detail::to_b2;

json::Value number_property(std::string_view description, double fallback) {
    json::Value property;
    property["type"] = "number";
    property["default"] = fallback;
    property["description"] = description;
    return property;
}

// Box2D's own checks fail as SLASH_ASSERT does: logged, then the program stops.
int on_box2d_assert(const char* condition, const char* file, int line) {
    slash::detail::assert_failed(condition, "Box2D check failed", file, line);
}

// Box2D's lengths are pixels: its tolerances suit objects about kPixelsPerMeter across.
void set_up_box2d() {
    static const bool done = [] {
        b2SetLengthUnitsPerMeter(detail::kPixelsPerMeter);
        b2SetAssertFcn(on_box2d_assert);
        return true;
    }();
    (void)done;
}

bool accepts(const ShapeInfo* info, const QueryFilter& filter) {
    if (info == nullptr) {
        return false;
    }
    if (info->kind == Kind::Area ? !filter.collide_with_areas : !filter.collide_with_bodies) {
        return false;
    }
    return std::find(filter.exclude.begin(), filter.exclude.end(), info->entity) ==
           filter.exclude.end();
}

b2QueryFilter query_filter(const QueryFilter& filter) {
    return {.categoryBits = detail::kQueryBit, .maskBits = filter.collision_mask};
}

// The space with its Box2D world up to date, or nullptr if it has no bodies.
Space* synced(world::World& world) {
    Space* space = find_space(world);
    if (space == nullptr) {
        return nullptr;
    }
    space->sync(world);
    return b2World_IsValid(space->id()) ? space : nullptr;
}

// Entities by ID, each once, at most max of them.
std::vector<entt::entity> sorted(const world::World& world, std::vector<entt::entity> entities,
                                 std::size_t max) {
    std::sort(entities.begin(), entities.end(),
              [&](entt::entity a, entt::entity b) { return world.id_of(a) < world.id_of(b); });
    entities.erase(std::unique(entities.begin(), entities.end()), entities.end());
    if (entities.size() > max) {
        entities.resize(max);
    }
    return entities;
}

struct Overlaps {
    const QueryFilter* filter = nullptr;
    std::vector<entt::entity> found;
    b2Vec2 point = b2Vec2_zero; // overlap_point only
};

bool collect_overlap(b2ShapeId shape, void* context) {
    auto& overlaps = *static_cast<Overlaps*>(context);
    const ShapeInfo* info = info_of(shape);
    if (accepts(info, *overlaps.filter)) {
        overlaps.found.push_back(info->entity);
    }
    return true;
}

bool collect_point(b2ShapeId shape, void* context) {
    auto& overlaps = *static_cast<Overlaps*>(context);
    const ShapeInfo* info = info_of(shape);
    if (accepts(info, *overlaps.filter) && b2Shape_TestPoint(shape, overlaps.point)) {
        overlaps.found.push_back(info->entity);
    }
    return true;
}

struct RayQuery {
    const QueryFilter* filter = nullptr;
    std::optional<RayHit> hit;
};

float closest_hit(b2ShapeId shape, b2Vec2 point, b2Vec2 normal, float fraction, void* context) {
    auto& query = *static_cast<RayQuery*>(context);
    const ShapeInfo* info = info_of(shape);
    // A ray that starts inside a shape comes with no normal; it doesn't hit, as in Godot.
    if (!accepts(info, *query.filter) || (normal.x == 0.0F && normal.y == 0.0F)) {
        return -1.0F;
    }
    query.hit = RayHit{.entity = info->entity,
                       .point = from_b2(point),
                       .normal = from_b2(normal),
                       .fraction = fraction,
                       .cell = info->cell};
    return fraction;
}

// A shape's outlines for Box2D's overlap test, each at most B2_MAX_POLYGON_VERTICES points.
std::vector<b2ShapeProxy> proxies(const world::CollisionShape2D& shape, Vec2 position,
                                  float rotation) {
    const detail::ShapePlacement place{.rotation = b2MakeRot(shape.rotation),
                                       .position = shape.position,
                                       .scale = {1.0F, 1.0F},
                                       .owner = std::nullopt};
    const b2Transform transform{.p = to_b2(position), .q = b2MakeRot(rotation)};
    const auto point = [&](Vec2 local) { return b2TransformPoint(transform, place.point(local)); };
    std::vector<b2ShapeProxy> out;
    if (shape.shape == "circle") {
        const b2Vec2 center = point({});
        out.push_back(b2MakeProxy(&center, 1, std::max(shape.radius, 0.0F)));
    } else if (shape.shape == "capsule") {
        const float half = std::max(0.0F, (shape.height * 0.5F) - shape.radius);
        const std::array<b2Vec2, 2> ends{point({0.0F, -half}), point({0.0F, half})};
        out.push_back(b2MakeProxy(ends.data(), 2, std::max(shape.radius, 0.0F)));
    } else if (shape.shape == "segment") {
        const std::array<b2Vec2, 2> ends{point(shape.a), point(shape.b)};
        out.push_back(b2MakeProxy(ends.data(), 2, 0.0F));
    } else if (shape.shape == "polygon") {
        std::vector<Vec2> points;
        points.reserve(shape.points.size());
        for (const Vec2 local : shape.points) {
            points.push_back(from_b2(point(local)));
        }
        for (const detail::Polygon& piece :
             detail::split_convex(detail::convex_hull(points), B2_MAX_POLYGON_VERTICES)) {
            std::vector<b2Vec2> corners;
            corners.reserve(piece.size());
            for (const Vec2 corner : piece) {
                corners.push_back(to_b2(corner));
            }
            out.push_back(b2MakeProxy(corners.data(), static_cast<int>(corners.size()), 0.0F));
        }
    } else {
        const float hx = shape.size.x * 0.5F;
        const float hy = shape.size.y * 0.5F;
        const std::array<b2Vec2, 4> corners{point({-hx, -hy}), point({hx, -hy}), point({hx, hy}),
                                            point({-hx, hy})};
        out.push_back(b2MakeProxy(corners.data(), 4, 0.0F));
    }
    return out;
}

} // namespace

json::Value settings_schema() {
    const Settings defaults;
    json::Value gravity_vector;
    gravity_vector["type"] = "array";
    gravity_vector["items"]["type"] = "number";
    gravity_vector["minItems"] = 2;
    gravity_vector["maxItems"] = 2;
    gravity_vector["default"] = json::Array{defaults.gravity_vector.x, defaults.gravity_vector.y};
    gravity_vector["description"] = "Gravity's direction, [x, y]; y points down.";

    json::Value linear_damp = number_property(
        "How fast rigid bodies slow down, per second; a body's own linear_damp adds to it unless "
        "its linear_damp_mode is replace.",
        defaults.linear_damp);
    linear_damp["minimum"] = 0;
    json::Value angular_damp = number_property(
        "How fast rigid bodies stop turning, per second; a body's own angular_damp adds to it "
        "unless its angular_damp_mode is replace.",
        defaults.angular_damp);
    angular_damp["minimum"] = 0;

    json::Value schema;
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["description"] = "2D physics, as Godot's physics/2d project settings.";
    json::Value& properties = schema["properties"];
    properties["gravity"] =
        number_property("Gravity's strength in pixels per second squared.", defaults.gravity);
    properties["gravity_vector"] = std::move(gravity_vector);
    properties["linear_damp"] = std::move(linear_damp);
    properties["angular_damp"] = std::move(angular_damp);
    return schema;
}

Settings parse_settings(const json::Value& settings) {
    Settings out;
    const auto number = [&](std::string_view key, float& field) {
        if (const json::Value* value = settings.find(key); value != nullptr) {
            field = static_cast<float>(value->as_double());
        }
    };
    number("gravity", out.gravity);
    number("linear_damp", out.linear_damp);
    number("angular_damp", out.angular_damp);
    if (const json::Value* vector = settings.find("gravity_vector"); vector != nullptr) {
        const json::Array& xy = vector->as_array();
        out.gravity_vector = {static_cast<float>(xy.at(0).as_double()),
                              static_cast<float>(xy.at(1).as_double())};
    }
    return out;
}

void install(world::World& world, const Settings& settings) {
    set_up_box2d();
    if (Space* space = find_space(world); space != nullptr) {
        space->set_settings(settings);
        return;
    }
    auto space = std::make_shared<Space>(settings);
    world.ecs().ctx().emplace<std::shared_ptr<Space>>(space);
    world.add_reset([space](world::World& /*world*/) { space->reset(); });
    world.add_system(
        "physics2d",
        [space](world::World& w, double tick_seconds) {
            space->step(w, static_cast<float>(tick_seconds));
        },
        {.doc = "Steps 2D physics: moves rigid and character bodies and reports contacts.",
         .priority = 950});
}

void set_settings(world::World& world, const Settings& settings) {
    if (Space* space = find_space(world); space != nullptr) {
        space->set_settings(settings);
    }
}

std::optional<RayHit> raycast(world::World& world, Vec2 from, Vec2 to, const QueryFilter& filter) {
    Space* space = synced(world);
    if (space == nullptr || from == to) {
        return std::nullopt;
    }
    RayQuery query{.filter = &filter, .hit = std::nullopt};
    b2World_CastRay(space->id(), to_b2(from), to_b2(to - from), query_filter(filter), closest_hit,
                    &query);
    return query.hit;
}

std::vector<entt::entity> overlap_point(world::World& world, Vec2 point, const QueryFilter& filter,
                                        std::size_t max_results) {
    Space* space = synced(world);
    if (space == nullptr) {
        return {};
    }
    Overlaps overlaps{.filter = &filter, .found = {}, .point = to_b2(point)};
    const b2AABB box{.lowerBound = to_b2(point), .upperBound = to_b2(point)};
    b2World_OverlapAABB(space->id(), box, query_filter(filter), collect_point, &overlaps);
    return sorted(world, std::move(overlaps.found), max_results);
}

std::vector<entt::entity> overlap_shape(world::World& world, const world::CollisionShape2D& shape,
                                        Vec2 position, float rotation, const QueryFilter& filter,
                                        std::size_t max_results) {
    Space* space = synced(world);
    if (space == nullptr) {
        return {};
    }
    Overlaps overlaps{.filter = &filter, .found = {}, .point = b2Vec2_zero};
    for (const b2ShapeProxy& proxy : proxies(shape, position, rotation)) {
        b2World_OverlapShape(space->id(), &proxy, query_filter(filter), collect_overlap, &overlaps);
    }
    return sorted(world, std::move(overlaps.found), max_results);
}

Contacts contacts(world::World& world, entt::entity entity) {
    Space* space = synced(world);
    return space != nullptr ? space->contacts(world, entity) : Contacts{};
}

void set_tile_shape_source(world::World& world, TileShapeSource source) {
    world.ecs().ctx().insert_or_assign<TileShapeSource>(std::move(source));
}

TileShapes tile_shapes(const world::World& world, entt::entity layer) {
    const auto* source = world.ecs().ctx().find<TileShapeSource>();
    if (source == nullptr || !*source || !world.valid(layer)) {
        return nullptr;
    }
    return (*source)(world, layer);
}

void apply_impulse(world::World& world, entt::entity entity, Vec2 impulse, Vec2 offset) {
    if (Space* space = find_space(world); space != nullptr) {
        space->queue({.entity = entity, .impulse = impulse, .offset = offset, .torque = 0.0F});
    }
}

void apply_torque_impulse(world::World& world, entt::entity entity, float impulse) {
    if (Space* space = find_space(world); space != nullptr) {
        space->queue({.entity = entity, .impulse = {}, .offset = {}, .torque = impulse});
    }
}

} // namespace slash::physics2d
