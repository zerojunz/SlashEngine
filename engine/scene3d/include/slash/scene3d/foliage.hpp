#pragma once

// Foliage that blocks: each Foliage3D whose types have collision is one static body of its
// instances' shapes, as a GridMap's cells' shapes are, kept until the instances or a type change.

#include "slash/scene3d/gridmap.hpp"

#include <vector>

namespace slash::world {
class World;
}

namespace slash::scene3d {

// Keeps each Foliage3D's shapes in the world's context until its instances or types change, so
// physics rebuilds its body only then. Called by canvas::install.
void install_foliage_collision(world::World& world);

// Each Foliage3D with an instance that blocks, as a GridMap's body: each shape's cell is
// [group, instance, 0], so a ray that hits one says which instance it hit.
[[nodiscard]] std::vector<GridBody> foliage_bodies(const world::World& world);

} // namespace slash::scene3d
