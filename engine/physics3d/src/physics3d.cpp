#include "slash/physics3d/physics3d.hpp"

#include "space.hpp"

#include <Jolt/Geometry/ConvexSupport.h>
#include <Jolt/Geometry/GJKClosestPoint.h>
#include <Jolt/Physics/Body/BodyLockInterface.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollidePointResult.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/CompoundShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexShape.h>
#include <Jolt/Physics/Collision/Shape/DecoratedShape.h>
#include <Jolt/Physics/Collision/TransformedShape.h>
#include <algorithm>
#include <cfloat>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace slash::physics3d {

namespace {

using detail::find_space;
using detail::from_jolt;
using detail::Kind;
using detail::Space;
using detail::to_jolt;

json::Value number_property(std::string_view description, double fallback) {
    json::Value property;
    property["type"] = "number";
    property["default"] = fallback;
    property["description"] = description;
    return property;
}

// What a query sees, as Godot's JoltQueryFilter3D: bodies, areas or both, on the mask's layers,
// less the excluded entities.
class QueryFilters final : public JPH::BroadPhaseLayerFilter,
                           public JPH::ObjectLayerFilter,
                           public JPH::BodyFilter {
public:
    QueryFilters(const Space& space, const QueryFilter& filter)
        : space_(&space), filter_(&filter) {}

    using JPH::BodyFilter::ShouldCollide;
    [[nodiscard]] bool ShouldCollide(JPH::BroadPhaseLayer layer) const override {
        return layer == detail::broad::kArea ? filter_->collide_with_areas
                                             : filter_->collide_with_bodies;
    }
    [[nodiscard]] bool ShouldCollide(JPH::ObjectLayer layer) const override {
        const auto [broad, collision_layer, mask] = space_->layers().from_object_layer(layer);
        (void)broad;
        (void)mask;
        return (filter_->collision_mask & collision_layer) != 0;
    }
    [[nodiscard]] bool ShouldCollideLocked(const JPH::Body& body) const override {
        const detail::Body* found = Space::body_of(body);
        return found != nullptr && std::find(filter_->exclude.begin(), filter_->exclude.end(),
                                             found->entity) == filter_->exclude.end();
    }

private:
    const Space* space_;
    const QueryFilter* filter_;
};

// The space with its Jolt system up to date, or nullptr if it has no bodies.
Space* synced(world::World& world) {
    Space* space = find_space(world);
    if (space == nullptr) {
        return nullptr;
    }
    space->sync(world);
    return space->system() != nullptr ? space : nullptr;
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

// The entities of the bodies hit, in the order Jolt found them.
template <typename Hits>
std::vector<entt::entity> entities_of(const Space& space, const Hits& hits) {
    std::vector<entt::entity> out;
    const JPH::BodyLockInterfaceNoLock& locks = space.system()->GetBodyLockInterfaceNoLock();
    for (const auto& hit : hits) {
        const JPH::Body* body = locks.TryGetBody(hit.mBodyID2);
        if (const detail::Body* found = body != nullptr ? Space::body_of(*body) : nullptr;
            found != nullptr) {
            out.push_back(found->entity);
        }
    }
    return out;
}

} // namespace

json::Value settings_schema() {
    const Settings defaults;
    json::Value gravity_vector;
    gravity_vector["type"] = "array";
    gravity_vector["items"]["type"] = "number";
    gravity_vector["minItems"] = 3;
    gravity_vector["maxItems"] = 3;
    gravity_vector["default"] = json::Array{defaults.gravity_vector.x, defaults.gravity_vector.y,
                                            defaults.gravity_vector.z};
    gravity_vector["description"] = "Gravity's direction, [x, y, z]; y points up.";

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

    json::Value detect_static;
    detect_static["type"] = "boolean";
    detect_static["default"] = defaults.areas_detect_static_bodies;
    detect_static["description"] =
        "Whether Area3D reports static bodies entering it, which Godot's Jolt module leaves off.";

    json::Value schema;
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["description"] =
        "3D physics, as Godot's physics/3d and physics/jolt_physics_3d project settings.";
    json::Value& properties = schema["properties"];
    properties["gravity"] =
        number_property("Gravity's strength in meters per second squared.", defaults.gravity);
    properties["gravity_vector"] = std::move(gravity_vector);
    properties["linear_damp"] = std::move(linear_damp);
    properties["angular_damp"] = std::move(angular_damp);
    properties["areas_detect_static_bodies"] = std::move(detect_static);
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
        const json::Array& xyz = vector->as_array();
        out.gravity_vector = {static_cast<float>(xyz.at(0).as_double()),
                              static_cast<float>(xyz.at(1).as_double()),
                              static_cast<float>(xyz.at(2).as_double())};
    }
    if (const json::Value* detect = settings.find("areas_detect_static_bodies");
        detect != nullptr) {
        out.areas_detect_static_bodies = detect->as_bool();
    }
    return out;
}

void install(world::World& world, const Settings& settings) {
    detail::set_up_jolt();
    if (Space* space = find_space(world); space != nullptr) {
        space->set_settings(settings);
        return;
    }
    auto space = std::make_shared<Space>(settings);
    world.ecs().ctx().emplace<std::shared_ptr<Space>>(space);
    world.add_reset([space](world::World& /*world*/) { space->reset(); });
    world.add_memory_source("physics3d",
                            [space](const world::World& /*world*/) { return space->memory(); });
    world.add_system(
        "physics3d",
        [space](world::World& w, double tick_seconds) {
            space->step(w, static_cast<float>(tick_seconds));
        },
        {.doc = "Steps 3D physics: moves rigid and character bodies and reports contacts.",
         .priority = 950});
}

void set_settings(world::World& world, const Settings& settings) {
    if (Space* space = find_space(world); space != nullptr) {
        space->set_settings(settings);
    }
}

std::optional<RayHit> raycast(world::World& world, Vec3 from, Vec3 to, const QueryFilter& filter) {
    const Space* space = synced(world);
    if (space == nullptr) {
        return std::nullopt;
    }
    return space->cast_ray(from, to, filter, false);
}

std::vector<entt::entity> overlap_point(world::World& world, Vec3 point, const QueryFilter& filter,
                                        std::size_t max_results) {
    Space* space = synced(world);
    if (space == nullptr) {
        return {};
    }
    // Godot's intersect_point.
    const QueryFilters filters(*space, filter);
    JPH::AllHitCollisionCollector<JPH::CollidePointCollector> collector;
    space->system()->GetNarrowPhaseQueryNoLock().CollidePoint(to_jolt(point), collector, filters,
                                                              filters, filters);
    std::vector<entt::entity> found;
    const JPH::BodyLockInterfaceNoLock& locks = space->system()->GetBodyLockInterfaceNoLock();
    for (const JPH::CollidePointResult& hit : collector.mHits) {
        const JPH::Body* body = locks.TryGetBody(hit.mBodyID);
        if (const detail::Body* owner = body != nullptr ? Space::body_of(*body) : nullptr;
            owner != nullptr) {
            found.push_back(owner->entity);
        }
    }
    return sorted(world, std::move(found), max_results);
}

std::vector<entt::entity> overlap_shape(world::World& world, const world::CollisionShape3D& shape,
                                        Vec3 position, Vec3 rotation, const QueryFilter& filter,
                                        std::size_t max_results) {
    Space* space = synced(world);
    if (space == nullptr) {
        return {};
    }
    const detail::BuiltShape built = detail::build_shape(shape);
    if (built.shape == nullptr) {
        return {};
    }
    // Godot's intersect_shape: the shape at its transform, from its center of mass.
    const auto affine = [](Vec3 at, Vec3 turn) {
        world::Transform3D transform;
        transform.position = at;
        transform.rotation = turn;
        return world::affine(transform);
    };
    const world::Affine3D placed =
        affine(position, rotation) * affine(shape.position, shape.rotation);
    const detail::Unscaled unscaled = detail::unscale(placed);
    const JPH::RefConst<JPH::Shape> scaled = detail::scaled(built.shape.GetPtr(), unscaled.scale);
    world::Affine3D transform_com = unscaled.transform;
    transform_com.origin = transform_com.apply(from_jolt(scaled->GetCenterOfMass()));
    const QueryFilters filters(*space, filter);
    JPH::CollideShapeSettings settings;
    settings.mBackFaceMode = JPH::EBackFaceMode::CollideWithBackFaces;
    JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> collector;
    space->system()->GetNarrowPhaseQueryNoLock().CollideShape(
        scaled.GetPtr(), JPH::Vec3::sOne(), detail::matrix_of(transform_com), settings,
        JPH::RVec3::sZero(), collector, filters, filters, filters);
    return sorted(world, entities_of(*space, collector.mHits), max_results);
}

std::optional<Vec3> closest_point(world::World& world, entt::entity entity, Vec3 point) {
    Space* space = synced(world);
    const detail::Body* owner = space != nullptr ? space->body(entity) : nullptr;
    const JPH::Body* body =
        owner != nullptr ? space->system()->GetBodyLockInterfaceNoLock().TryGetBody(owner->id)
                         : nullptr;
    if (body == nullptr) {
        return std::nullopt;
    }
    // Godot's JoltPhysicsDirectSpaceState3D::get_closest_point_to_object_volume: GJK from the
    // point to each convex shape, at most 32 of them.
    class Shapes final : public JPH::TransformedShapeCollector {
    public:
        void AddHit(const JPH::TransformedShape& shape) override {
            hits.push_back(shape);
            if (hits.size() >= 32) {
                ForceEarlyOut();
            }
        }
        std::vector<JPH::TransformedShape> hits;
    };
    // Read before the shapes are collected: clang-analyzer can't tell the body keeps its shape.
    const JPH::RVec3 position = body->GetPosition();
    const JPH::TransformedShape body_shape = body->GetTransformedShape();
    Shapes shapes;
    body_shape.CollectTransformedShapes(body->GetWorldSpaceBounds(), shapes);
    const JPH::RVec3 at = to_jolt(point);
    float closest_distance_sq = FLT_MAX;
    JPH::RVec3 closest = JPH::RVec3::sZero();
    bool found = false;
    for (const JPH::TransformedShape& transformed : shapes.hits) {
        const JPH::Shape& shape = *transformed.mShape;
        if (shape.GetType() != JPH::EShapeType::Convex) {
            continue;
        }
        const auto& convex = static_cast<const JPH::ConvexShape&>(shape);
        JPH::GJKClosestPoint gjk;
        JPH::ConvexShape::SupportBuffer buffer{};
        const JPH::ConvexShape::Support* support =
            convex.GetSupportFunction(JPH::ConvexShape::ESupportMode::IncludeConvexRadius, buffer,
                                      transformed.GetShapeScale());
        const JPH::RMat44 rotation = JPH::RMat44::sRotation(transformed.mShapeRotation);
        const JPH::Vec3 com = rotation.Multiply3x3(shape.GetCenterOfMass());
        const JPH::RVec3 shape_position = transformed.mShapePositionCOM - JPH::RVec3(com);
        const JPH::RMat44 placed = rotation.PostTranslated(shape_position);
        const JPH::PointConvexSupport point_support{
            .mPoint = JPH::Vec3(placed.InversedRotationTranslation() * at)};
        JPH::Vec3 separating_axis = JPH::Vec3::sAxisX();
        JPH::Vec3 point_on_a = JPH::Vec3::sZero();
        JPH::Vec3 point_on_b = JPH::Vec3::sZero();
        const float distance_sq =
            gjk.GetClosestPoints(*support, point_support, JPH::cDefaultCollisionTolerance, FLT_MAX,
                                 separating_axis, point_on_a, point_on_b);
        if (distance_sq == 0.0F) {
            closest = at;
            found = true;
            break;
        }
        if (distance_sq < closest_distance_sq) {
            closest_distance_sq = distance_sq;
            closest = placed * point_on_a;
            found = true;
        }
    }
    return from_jolt(found ? closest : position);
}

Contacts contacts(world::World& world, entt::entity entity) {
    Space* space = synced(world);
    return space != nullptr ? space->contacts(world, entity) : Contacts{};
}

std::optional<Gravity> gravity(world::World& world, entt::entity entity) {
    Space* space = synced(world);
    const detail::Body* body = space != nullptr ? space->body(entity) : nullptr;
    if (body == nullptr || body->kind == Kind::Area) {
        return std::nullopt;
    }
    Vec3 position = body->placed.origin;
    if (!body->id.IsInvalid()) {
        position =
            from_jolt(JPH::Vec3(space->system()->GetBodyInterfaceNoLock().GetPosition(body->id)));
    }
    detail::AreaForces forces = space->forces(world, *body, position);
    Gravity out{.gravity = forces.gravity,
                .linear_damp = forces.linear_damp,
                .angular_damp = forces.angular_damp,
                .areas = std::move(forces.areas)};
    if (const std::optional<detail::OwnForces> own =
            detail::own_forces(world.ecs(), entity, body->kind)) {
        out.gravity = out.gravity * own->gravity_scale;
        out.linear_damp =
            detail::with_own(out.linear_damp, own->linear_damp_mode, own->linear_damp);
        out.angular_damp =
            detail::with_own(out.angular_damp, own->angular_damp_mode, own->angular_damp);
    }
    return out;
}

bool has_body(const world::World& world, entt::entity entity) {
    return world.valid(entity) &&
           world.ecs()
               .any_of<world::RigidBody3D, world::CharacterBody3D, world::AnimatableBody3D,
                       world::StaticBody3D, world::Area3D, world::PhysicalBone3D>(entity);
}

void apply_impulse(world::World& world, entt::entity entity, Vec3 impulse, Vec3 offset) {
    if (Space* space = find_space(world); space != nullptr) {
        space->queue({.entity = entity, .impulse = impulse, .offset = offset, .torque = {}});
    }
}

void apply_torque_impulse(world::World& world, entt::entity entity, Vec3 impulse) {
    if (Space* space = find_space(world); space != nullptr) {
        space->queue({.entity = entity, .impulse = {}, .offset = {}, .torque = impulse});
    }
}

namespace detail {

std::optional<RayHit> Space::cast_ray(Vec3 from, Vec3 to, const QueryFilter& filter,
                                      bool hit_back_faces) const {
    if (system_ == nullptr || from == to) {
        return std::nullopt;
    }
    // Godot's JoltPhysicsDirectSpaceState3D::intersect_ray, not hitting from inside.
    const QueryFilters filters(*this, filter);
    const JPH::RRayCast ray(to_jolt(from), to_jolt(to - from));
    JPH::RayCastSettings settings;
    settings.mTreatConvexAsSolid = false;
    settings.mBackFaceModeTriangles = hit_back_faces ? JPH::EBackFaceMode::CollideWithBackFaces
                                                     : JPH::EBackFaceMode::IgnoreBackFaces;
    JPH::ClosestHitCollisionCollector<JPH::CastRayCollector> collector;
    system_->GetNarrowPhaseQueryNoLock().CastRay(ray, settings, collector, filters, filters,
                                                 filters);
    if (!collector.HadHit()) {
        return std::nullopt;
    }
    const JPH::RayCastResult& hit = collector.mHit;
    const JPH::Body* body = system_->GetBodyLockInterfaceNoLock().TryGetBody(hit.mBodyID);
    const Body* found = body != nullptr ? body_of(*body) : nullptr;
    if (found == nullptr) {
        return std::nullopt;
    }
    const JPH::RVec3 position = ray.GetPointOnRay(hit.mFraction);
    JPH::Vec3 normal = body->GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, position);
    // If we got a back-face normal we need to flip it.
    if (normal.Dot(ray.mDirection) > 0.0F) {
        normal = -normal;
    }
    return RayHit{.entity = found->entity,
                  .point = from_jolt(JPH::Vec3(position)),
                  .normal = from_jolt(normal),
                  .fraction = hit.mFraction,
                  .cell = grid_cell(*found, hit.mSubShapeID2)};
}

std::optional<Vec3i> Space::grid_cell(const Body& body, const JPH::SubShapeID& id) {
    if (!body.generated || body.shape_key.grid == nullptr) {
        return std::nullopt;
    }
    // Scaling and placing a shape keep its sub-shape IDs, so the compound under them names the
    // part, as its user data.
    const JPH::Shape* shape = body.shape.GetPtr();
    while (shape->GetType() == JPH::EShapeType::Decorated) {
        shape = static_cast<const JPH::DecoratedShape*>(shape)->GetInnerShape();
    }
    std::optional<std::size_t> part = body.grid_part;
    if (shape->GetType() == JPH::EShapeType::Compound) {
        const auto* compound = static_cast<const JPH::CompoundShape*>(shape);
        JPH::SubShapeID remainder;
        part = compound->GetCompoundUserData(compound->GetSubShapeIndexFromID(id, remainder));
    }
    if (!part || *part >= body.shape_key.grid->size()) {
        return std::nullopt;
    }
    return (*body.shape_key.grid)[*part].cell;
}

} // namespace detail

} // namespace slash::physics3d
