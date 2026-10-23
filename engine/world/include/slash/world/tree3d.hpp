#pragma once

#include "slash/core/math.hpp"
#include "slash/world/components.hpp"

#include <array>
#include <cstdint>
#include <entt/entity/entity.hpp>
#include <unordered_map>
#include <vector>

namespace slash::world {

class World;

// A 3D affine transform, as Godot's Transform3D: a point p goes to x * p.x + y * p.y + z * p.z +
// origin, where x, y and z are its basis's columns.
struct Affine3D {
    Vec3 x{1.0F, 0.0F, 0.0F};
    Vec3 y{0.0F, 1.0F, 0.0F};
    Vec3 z{0.0F, 0.0F, 1.0F};
    Vec3 origin;

    [[nodiscard]] Vec3 apply(Vec3 p) const;
    // A direction, turned and scaled but not moved.
    [[nodiscard]] Vec3 basis(Vec3 v) const;
    // This transform after o, as Godot's operator*: a parent's times its child's.
    [[nodiscard]] Affine3D operator*(const Affine3D& o) const;
    // The transform that undoes this one, as Godot's affine_inverse; itself if it can't be undone.
    [[nodiscard]] Affine3D inverse() const;
    // As a column-major 4x4 matrix, as the render device takes it.
    [[nodiscard]] std::array<float, 16> matrix() const;

    friend bool operator==(const Affine3D&, const Affine3D&) = default;
};

// A Transform3D's matrix, as Godot's Node3D builds it: its basis is the rotation (Basis::
// from_euler in YXZ order) times the scale, its origin the position. Children play no part.
// The same bits on every platform.
[[nodiscard]] Affine3D affine(const Transform3D& transform);

// A rotation's Euler angles (Transform3D::rotation) as a unit quaternion, for blending rotations.
// The same bits on every platform.
[[nodiscard]] Quat quaternion(Vec3 rotation);
// The basis of a unit quaternion, as Godot's Basis(Quaternion).
[[nodiscard]] Affine3D rotation_basis(const Quat& q);
// The Euler angles (Transform3D::rotation) of a rotation basis, as Godot's Basis::get_euler in
// its default YXZ order. The basis must be orthonormal. The same bits on every platform.
[[nodiscard]] Vec3 euler(const Affine3D& rotation);
// weight of the way from rotation a to b, the shorter way round: their blend normalized, which
// is near Godot's slerp for the small turns between ticks.
[[nodiscard]] Quat nlerp(const Quat& a, const Quat& b, float weight);

// Where 3D entities are in the world, as Godot's tree of Node3Ds: an entity in another's
// Transform3D children is placed in its parent's space. As Tree2D: a child listed by two parents
// keeps the first in the order entities were made, a parent that is its own descendant is left a
// root, and the tree reads the parents as they were when it was made.
class Tree3D {
public:
    explicit Tree3D(const World& world);

    // The entity whose children list it, or null for a root.
    [[nodiscard]] entt::entity parent(entt::entity entity) const;
    // The entity whose space it is placed in: its parent, or null for a root or a top_level
    // entity.
    [[nodiscard]] entt::entity transform_parent(entt::entity entity) const;
    // The entity's Transform3D after its parents', as Godot's get_global_transform; the identity
    // for an entity without one.
    [[nodiscard]] Affine3D global(entt::entity entity) const;
    // The same with some entities' own transforms replaced, such as by where they are part of the
    // way between two ticks: an entity in locals is placed by its transform there.
    using Locals = std::unordered_map<entt::entity, Affine3D>;
    [[nodiscard]] Affine3D global(entt::entity entity, const Locals& locals) const;
    // Whether it and every entity above it are visible, as Godot's is_visible_in_tree. An entity
    // without a Transform3D is visible.
    [[nodiscard]] bool visible(entt::entity entity) const;
    // The entities with a Transform3D in tree order, as Godot's: the roots in the order entities
    // were made, each followed by its children in list order, theirs after them.
    [[nodiscard]] const std::vector<entt::entity>& order() const { return order_; }

private:
    const World* world_;
    std::unordered_map<entt::entity, entt::entity> parents_;
    std::vector<entt::entity> order_;
};

// The transform with its basis made orthonormal, as Godot's Transform3D::orthonormalized:
// Gram-Schmidt on the columns, x first. The origin stays.
[[nodiscard]] Affine3D orthonormalized(Affine3D transform);

// A rotation basis whose -z axis faces target and whose y axis is as near up as it can be, as
// Godot's Basis::looking_at: with up along target, x is any direction across up. Its origin is
// zero.
[[nodiscard]] Affine3D looking_at(Vec3 target, Vec3 up = {0.0F, 1.0F, 0.0F});

// The camera the 3D world is seen through, as Godot's Viewport::get_camera_3d: the first
// Camera3D with current set in tree order, else the first one; cameras with no Transform3D come
// after those in the tree, in the order they were made. Null with none.
[[nodiscard]] entt::entity current_camera(const World& world, const Tree3D& tree);

} // namespace slash::world
