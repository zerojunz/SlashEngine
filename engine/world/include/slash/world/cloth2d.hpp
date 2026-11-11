#pragma once

// 2D cloth's sheet: where a Cloth2D's points are at rest, each one's share of its max distance,
// and where 2D physics last put them, which physics2d gives and the canvas draws (canvas doesn't
// depend on physics2d).

#include "slash/core/math.hpp"
#include "slash/world/components.hpp"
#include "slash/world/tree2d.hpp"
#include "slash/world/world.hpp"

#include <array>
#include <functional>
#include <vector>

namespace slash::world {

// How many points a sheet has across and down, at least 2 each.
[[nodiscard]] int cloth_columns(const Cloth2D& cloth);
[[nodiscard]] int cloth_rows(const Cloth2D& cloth);

// The sheet's points where its entity's global transform puts them at rest, row by row from the
// top: its top edge's middle at offset, hanging down the entity's y.
[[nodiscard]] std::vector<Vec2> cloth_rest_2d(const Cloth2D& cloth, const Affine2D& global);

// Each point's share of max_distance, 0 to 1, by max_distance_map.
[[nodiscard]] std::vector<float> cloth_shares_2d(const Cloth2D& cloth);

// A sheet's edges: from each point, row by row, to the point right of it, then to the one below
// it. A Cloth2D's torn edges are numbered in this order.
struct ClothGrid2D {
    int columns = 2;
    int rows = 2;
    std::vector<std::array<int, 2>> edges; // the two points each edge joins
    std::vector<int> across;               // each point's edge to the one right of it, or -1
    std::vector<int> down;                 // each point's edge to the one below it, or -1
    std::vector<bool> torn;                // by edge, from the Cloth2D's torn
};
[[nodiscard]] ClothGrid2D cloth_grid_2d(const Cloth2D& cloth);

// Whether each of the sheet's triangles still draws: two a cell, row by row, first the one with
// the cell's top and left sides, then the one with its right and bottom sides. A triangle with a
// torn side doesn't.
[[nodiscard]] std::vector<bool> cloth_triangles_2d(const ClothGrid2D& grid);

// Where physics put a sheet's points after its last step and the step before, in the world.
struct ClothPoints2D {
    std::vector<Vec2> previous;
    std::vector<Vec2> current;
};
using Cloth2DSource = std::function<const ClothPoints2D*(const World&, entt::entity)>;
void set_cloth_2d_source(World& world, Cloth2DSource source);
// The entity's points, or null when physics hasn't stepped its sheet.
[[nodiscard]] const ClothPoints2D* cloth_points_2d(const World& world, entt::entity entity);

} // namespace slash::world
