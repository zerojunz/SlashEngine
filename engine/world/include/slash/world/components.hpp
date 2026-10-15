#pragma once

#include "slash/core/math.hpp"
#include "slash/core/reflect.hpp"

#include <string>
#include <vector>

// Components every SlashEngine world understands. Names follow Godot (Node2D and Node3D carry
// the same transforms).
namespace slash::world {

class ComponentRegistry;

// Position, rotation and scale in 2D. Units are pixels, and y points down, as in Godot.
struct Transform2D {
    Vec2 position;
    float rotation = 0.0F; // radians, clockwise
    Vec2 scale{1.0F, 1.0F};
};
SLASH_COMPONENT(Transform2D, position, rotation, scale)

// Position, rotation and scale in 3D. Units are meters, and y points up, as in Godot.
struct Transform3D {
    Vec3 position;
    Quat rotation;
    Vec3 scale{1.0F, 1.0F, 1.0F};
};
SLASH_COMPONENT(Transform3D, position, rotation, scale)

// Ties an entity to the storyboard entries it stands for, such as the character it plays or the
// place it is, so a brief for the scene includes them and checks catch a misspelled ID.
struct StoryRef {
    std::vector<std::string> ids;
};
SLASH_COMPONENT(StoryRef, ids)

// Registers the components above, with their docs.
void register_builtin_components(ComponentRegistry& registry);

} // namespace slash::world
