// Destruction (Godot has none; Unreal's geometry collections are the model): a Destructible3D
// is replaced by a rigid body per piece of its mesh's fracture (scene3d::fracture_of), each
// moving as that part of the body moved, when an impact reaches its damage_threshold or
// break_body says so. Pieces with a lifetime go when it runs out.

#include "space.hpp"

#include "slash/core/json.hpp"
#include "slash/core/log.hpp"
#include "slash/physics3d/physics3d.hpp"
#include "slash/scene3d/fracture.hpp"
#include "slash/scene3d/primitive.hpp"
#include "slash/world/components.hpp"
#include "slash/world/tree3d.hpp"
#include "slash/world/world.hpp"

#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace slash::physics3d {

namespace {

using world::Destructible3D;
using world::FracturePiece3D;

Vec3 cross(Vec3 a, Vec3 b) {
    return {(a.y * b.z) - (a.z * b.y), (a.z * b.x) - (a.x * b.z), (a.x * b.y) - (a.y * b.x)};
}

float length(Vec3 v) {
    return std::sqrt((v.x * v.x) + (v.y * v.y) + (v.z * v.z));
}

// The body's place in the world as a Transform3D: its own at the root, else its global
// transform taken apart.
world::Transform3D placed(const world::World& world, const world::Tree3D& tree,
                          entt::entity entity) {
    world::Transform3D out;
    const auto* own = world.ecs().try_get<const world::Transform3D>(entity);
    if (own != nullptr && tree.transform_parent(entity) == entt::null) {
        out = *own;
        out.children.clear();
        return out;
    }
    const world::Affine3D global = tree.global(entity);
    out.position = global.origin;
    out.scale = {length(global.x), length(global.y), length(global.z)};
    world::Affine3D turn = global;
    turn.origin = {};
    const auto unit = [](Vec3 v, float size) {
        return size > 0.0F ? Vec3{v.x / size, v.y / size, v.z / size} : v;
    };
    turn.x = unit(turn.x, out.scale.x);
    turn.y = unit(turn.y, out.scale.y);
    turn.z = unit(turn.z, out.scale.z);
    out.rotation = world::euler(turn);
    if (own != nullptr) {
        out.visible = own->visible;
    }
    return out;
}

// The ID a piece takes: the body's, then "_" and its number, or the next free one after it.
Result<entt::entity> create_piece(world::World& world, std::string_view body, std::size_t number) {
    if (body.empty()) {
        return world.create();
    }
    for (std::size_t extra = 0;; ++extra) {
        std::string id = std::string(body) + "_" + std::to_string(number);
        if (extra > 0) {
            id += "_" + std::to_string(extra);
        }
        if (world.find(id) == entt::null) {
            return world.create(id);
        }
    }
}

// Gives a piece the body's MeshInstance3D, primitive mesh and material.
void copy_looks(entt::registry& ecs, entt::entity out, const world::MeshInstance3D& instance,
                const std::optional<scene3d::Primitive>& shape,
                const std::optional<world::StandardMaterial3D>& material) {
    ecs.emplace<world::MeshInstance3D>(out, instance);
    if (shape.has_value()) {
        std::visit(
            [&ecs, out](const auto& mesh) { ecs.emplace<std::decay_t<decltype(mesh)>>(out, mesh); },
            *shape);
    }
    if (material.has_value()) {
        ecs.emplace<world::StandardMaterial3D>(out, *material);
    }
}

} // namespace

Result<std::vector<entt::entity>> break_body(world::World& world, entt::entity entity, float push,
                                             std::optional<Vec3> from) {
    auto& ecs = world.ecs();
    const auto* destructible =
        world.valid(entity) ? ecs.try_get<const Destructible3D>(entity) : nullptr;
    if (destructible == nullptr) {
        return Error{.code = ErrorCode::InvalidArgument, .message = "has no Destructible3D"};
    }
    const auto* rigid = ecs.try_get<const world::RigidBody3D>(entity);
    const auto* fixed = ecs.try_get<const world::StaticBody3D>(entity);
    if (rigid == nullptr && fixed == nullptr) {
        return Error{.code = ErrorCode::InvalidArgument,
                     .message = "is no StaticBody3D or RigidBody3D, so it has no body to break"};
    }
    auto fracture = scene3d::fracture_of(world, entity, destructible->pieces, destructible->seed);
    if (!fracture) {
        return Error{.code = fracture.error().code,
                     .message = "can't break: " + fracture.error().message};
    }
    const std::vector<scene3d::FracturePiece>& pieces = fracture.value()->pieces;
    if (pieces.empty()) {
        return Error{.code = ErrorCode::InvalidArgument,
                     .message = "can't break: no cell holds any of its mesh"};
    }

    const world::Tree3D tree(world);
    const world::Transform3D place = placed(world, tree, entity);
    const world::Affine3D global = world::affine(place);
    double volume = 0.0;
    Vec3 middle;
    for (const scene3d::FracturePiece& piece : pieces) {
        volume += piece.volume;
        const auto share = static_cast<float>(piece.volume);
        middle =
            middle + Vec3{piece.center.x * share, piece.center.y * share, piece.center.z * share};
    }
    if (volume > 0.0) {
        const auto whole = static_cast<float>(volume);
        middle = {middle.x / whole, middle.y / whole, middle.z / whole};
    }
    const Vec3 center = global.apply(middle);
    const Vec3 away_from = from.value_or(center);

    // What the pieces take from the body.
    world::RigidBody3D base;
    float mass = destructible->mass;
    Vec3 linear;
    Vec3 angular;
    if (rigid != nullptr) {
        base = *rigid;
        mass = rigid->mass;
        linear = rigid->linear_velocity;
        angular = rigid->angular_velocity;
    } else {
        base.collision_layer = fixed->collision_layer;
        base.collision_mask = fixed->collision_mask;
        base.friction = fixed->friction;
        base.bounce = fixed->bounce;
    }
    base.freeze = false;
    base.sleeping = false;
    base.inertia = {};
    base.lock_rotation = false;
    base.constant_force = {};
    base.constant_torque = {};
    const world::MeshInstance3D instance = ecs.get<const world::MeshInstance3D>(entity);
    std::optional<world::StandardMaterial3D> material;
    if (const auto* standard = ecs.try_get<const world::StandardMaterial3D>(entity);
        standard != nullptr) {
        material = *standard;
    }
    auto primitive = scene3d::primitive_of(world, entity);
    const auto shape = primitive ? primitive.value() : std::nullopt;
    const Destructible3D settings = *destructible;
    const std::string id(world.id_of(entity));

    std::vector<entt::entity> made;
    made.reserve(pieces.size());
    json::Array ids;
    for (std::size_t i = 0; i < pieces.size(); ++i) {
        const scene3d::FracturePiece& piece = pieces[i];
        auto created = create_piece(world, id, i + 1);
        if (!created) {
            for (const entt::entity done : made) {
                world.destroy(done);
            }
            return created.error();
        }
        const entt::entity out = created.value();
        made.push_back(out);
        ids.emplace_back(std::string(world.id_of(out)));
        ecs.emplace<world::Transform3D>(out, place);
        copy_looks(ecs, out, instance, shape, material);
        const Vec3 at = global.apply(piece.center);
        const Vec3 arm = at - center;
        world::RigidBody3D body = base;
        body.mass = volume > 0.0 ? static_cast<float>(mass * (piece.volume / volume))
                                 : mass / static_cast<float>(pieces.size());
        body.linear_velocity = linear + cross(angular, arm);
        if (push != 0.0F) {
            const Vec3 out_dir = at - away_from;
            const float size = length(out_dir);
            if (size > 0.0F) {
                const float scale = push / size;
                body.linear_velocity =
                    body.linear_velocity +
                    Vec3{out_dir.x * scale, out_dir.y * scale, out_dir.z * scale};
            }
        }
        body.angular_velocity = angular;
        ecs.emplace<world::RigidBody3D>(out, body);
        world::CollisionShape3D hull;
        hull.shape = "convex_polygon";
        hull.points = piece.hull;
        ecs.emplace<world::CollisionShape3D>(out, std::move(hull));
        ecs.emplace<FracturePiece3D>(out, FracturePiece3D{.pieces = settings.pieces,
                                                          .seed = settings.seed,
                                                          .index = static_cast<int>(i),
                                                          .lifetime = settings.debris_lifetime});
    }
    world.destroy(entity);
    json::Value data;
    data["pieces"] = std::move(ids);
    world.emit("broken", id, std::move(data));
    return made;
}

namespace detail {

void break_and_age(world::World& world, Space& space, float seconds) {
    auto& ecs = world.ecs();
    // Bodies hit hard enough break, in the order the world keeps them.
    std::unordered_map<entt::entity, Space::Impact> impacts = space.take_impacts();
    if (!impacts.empty()) {
        std::vector<std::pair<entt::entity, float>> hit;
        for (const entt::entity entity : ecs.view<const Destructible3D>()) {
            if (const auto found = impacts.find(entity); found != impacts.end()) {
                hit.emplace_back(entity, found->second.impulse);
            }
        }
        for (const auto& [entity, impulse] : hit) {
            if (auto broken = break_body(world, entity); !broken) {
                log::warn("a Destructible3D can't break", {{"id", std::string(world.id_of(entity))},
                                                           {"error", broken.error().message}});
            }
        }
    }
    // Debris goes when its lifetime runs out.
    std::vector<entt::entity> gone;
    for (const auto [entity, piece] : ecs.view<FracturePiece3D>().each()) {
        if (piece.lifetime <= 0.0F) {
            continue;
        }
        piece.lifetime -= seconds;
        if (piece.lifetime <= 0.0F) {
            gone.push_back(entity);
        }
    }
    for (const entt::entity entity : gone) {
        world.destroy(entity);
    }
}

} // namespace detail

} // namespace slash::physics3d
