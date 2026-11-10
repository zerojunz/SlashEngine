#pragma once

// 2D cloth's sheet: where a Cloth2D's points are at rest, each one's share of its max distance,
// and where 2D physics last put them, which physics2d gives and the canvas draws (canvas doesn't
// depend on physics2d).

#include "slash/core/math.hpp"
#include "slash/world/components.hpp"
#include "slash/world/tree2d.hpp"
#include "slash/world/world.hpp"

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
