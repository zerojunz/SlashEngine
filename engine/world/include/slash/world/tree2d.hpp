#pragma once

#include "slash/core/math.hpp"
#include "slash/world/components.hpp"

#include <cstdint>
#include <entt/entity/entity.hpp>
#include <string>
#include <unordered_map>
#include <vector>

namespace slash::world {

class World;

// A 2D affine transform, as Godot's Transform2D: a point p goes to x * p.x + y * p.y + origin.
struct Affine2D {
    Vec2 x{1.0F, 0.0F};
    Vec2 y{0.0F, 1.0F};
    Vec2 origin;

    [[nodiscard]] Vec2 apply(Vec2 p) const;
    // This transform after o, as Godot's operator*: a parent's times its child's.
    [[nodiscard]] Affine2D operator*(const Affine2D& o) const;
    // The transform that undoes this one, as Godot's affine_inverse; itself if it can't be undone.
    [[nodiscard]] Affine2D inverse() const;

    friend bool operator==(const Affine2D&, const Affine2D&) = default;
};

// A Transform2D's matrix, as Godot's Transform2D(rotation, scale, skew, position). Its children
// play no part. The same bits on every platform.
[[nodiscard]] Affine2D affine(const Transform2D& transform);
// A matrix's parts as Godot's get_origin, get_rotation, get_scale and get_skew, with no
// children. The same bits on every platform.
[[nodiscard]] Transform2D parts(const Affine2D& matrix);

// Where 2D entities are in the world, as Godot's tree of Node2Ds: an entity in another's
// Transform2D children is placed in its parent's space. A child listed by two parents keeps the
// first in the order entities were made; a parent that is its own descendant is left a root.
// Make one, use it and drop it: it reads the parents as they were when it was made, and the
// transforms as they are when asked.
class Tree2D {
public:
    explicit Tree2D(const World& world);

    // The entity whose children list it, or null for a root.
    [[nodiscard]] entt::entity parent(entt::entity entity) const;
    // Whether any entity has a parent: with none, every global transform is the entity's own.
    [[nodiscard]] bool nested() const { return !parents_.empty(); }
    // The entity's Transform2D after its parents', as Godot's get_global_transform: a root's
    // own exactly (with no children), and Transform2D{} for an entity without one.
    [[nodiscard]] Transform2D global(entt::entity entity) const;
    [[nodiscard]] Affine2D global_affine(entt::entity entity) const;
    // Moves the entity so its global position is this, as Godot's set_global_position.
    void set_global_position(World& world, entt::entity entity, Vec2 position) const;
    // Turns the entity so its global rotation is this, as Godot's set_global_rotation: its own
    // rotation is this less its parent's global rotation.
    void set_global_rotation(World& world, entt::entity entity, float rotation) const;
    // Where the entity draws among others of the same z_index, lowest first: the roots in the
    // order entities were made, each followed by its children in list order, theirs after
    // them. An entity with no Transform2D is a root.
    [[nodiscard]] std::uint64_t draw_key(entt::entity entity) const;

private:
    const World* world_;
    std::unordered_map<entt::entity, entt::entity> parents_;
    std::unordered_map<entt::entity, std::uint64_t> keys_; // nested entities' draw keys
};

// A mistake in the entities' Transform2D or CanvasLayer children, for scene validate: path is
// the JSON Pointer of the bad entry in the scene. An entity in a CanvasLayer's children has no
// parent and is in no other layer.
struct TreeProblem {
    std::string path;
    std::string message;
};
[[nodiscard]] std::vector<TreeProblem> check_tree(const World& world);

} // namespace slash::world
