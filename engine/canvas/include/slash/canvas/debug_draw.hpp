#pragma once

#include "slash/canvas/light.hpp"
#include "slash/core/math.hpp"
#include "slash/render/device.hpp"
#include "slash/world/world.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// Collision shapes drawn as Godot draws them with Debug > Visible Collision Shapes (its
// --debug-collisions flag), for the debug overlay and frame.capture.
namespace slash::canvas {

// Godot's debug/shapes/collision/shape_color.
inline constexpr Vec4 kCollisionColor{0.0F, 0.6F, 0.7F, 0.42F};

struct CollisionShapes {
    std::vector<render::Vertex2D> vertices; // triangles in world pixels, to draw untextured
    std::size_t shapes = 0;                 // CollisionShape2D and CollisionPolygon2D drawn
};

// Every CollisionShape2D and CollisionPolygon2D in the world, placed as 2D physics places them
// (moved and turned by the entity's global transform, or transform_of's version of it, and
// scaled by its scale, radii by the larger axis), in the order entities were made, as Godot's
// CollisionShape2D and CollisionPolygon2D draw themselves:
//   - A shape fills with the color and has an opaque outline one pixel wide, with Godot's 24
//     points round a circle and 26 round a capsule; a segment is a line 3 pixels wide.
//   - A CollisionPolygon2D fills with the color and has an opaque Color(0.9, 0.2, 0.0) outline.
//   - A disabled one is gray, as bright as the color's brightest channel, at half its alpha.
//   - One-way collision adds Godot's arrow down the shape's +y: the color inverted for a shape
//     (darker when disabled), the color made opaque for a polygon.
[[nodiscard]] CollisionShapes collision_shapes(const world::World& world,
                                               const TransformOf& transform_of = {},
                                               Vec4 color = kCollisionColor);

// Triangles that fill a simple polygon wound either way, by ear clipping: indices into points,
// three to a triangle. Empty when it has no area, or crosses itself so that no ear is left.
[[nodiscard]] std::vector<std::uint32_t> triangulate(std::span<const Vec2> points);

} // namespace slash::canvas
