#pragma once

#include "slash/core/math.hpp"
#include "slash/core/result.hpp"

#include <cstddef>
#include <span>
#include <vector>

// Polygon helpers for shapes Box2D can't take whole: its polygons are convex with at most 8
// points.
namespace slash::physics2d::detail {

using Polygon = std::vector<Vec2>;

// The convex hull of points, counter-clockwise (in y-up terms), without collinear points. Fewer
// than 3 points if they don't span an area.
[[nodiscard]] Polygon convex_hull(std::span<const Vec2> points);

// Splits a convex polygon into fans of at most max_points points.
[[nodiscard]] std::vector<Polygon> split_convex(std::span<const Vec2> hull, std::size_t max_points);

// Splits a simple polygon, wound either way, into convex polygons of at most max_points points.
// Fails if it crosses itself or has no area.
[[nodiscard]] Result<std::vector<Polygon>> decompose(std::span<const Vec2> points,
                                                     std::size_t max_points);

} // namespace slash::physics2d::detail
