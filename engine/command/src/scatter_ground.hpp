#pragma once

#include "slash/command/session.hpp"
#include "slash/core/result.hpp"
#include "slash/navigation/navigation.hpp"
#include "slash/procedural/scatter.hpp"
#include "slash/world/tree2d.hpp"
#include "slash/world/tree3d.hpp"

#include <array>
#include <set>
#include <string>
#include <vector>

// The ground scatter.run and pcg.run sample, and placing what they make under one parent
// (scatter.cpp).
namespace slash::command {

// A place in the parent's space, as a Transform3D's or Transform2D's fields: the parent's
// inverse after it.
[[nodiscard]] world::Transform3D local_3d(const world::Affine3D& to_local,
                                          const procedural::ScatterPlace& place);
[[nodiscard]] world::Transform2D local_2d(const world::Affine2D& to_local,
                                          const procedural::ScatterPlace& place);

// The rectangle of an area as two triangles, at z 0 in 2D and on y 0 in 3D.
void add_area(const std::array<float, 4>& area, bool flat,
              std::vector<procedural::ScatterTriangle>& out);

// The 3D ground of the entities in on and those under them, from the geometry navigation bakes
// read.
[[nodiscard]] std::vector<procedural::ScatterTriangle>
ground_3d(const std::vector<navigation::EntityGeometry>& geometry, const world::Tree3D& tree,
          const std::set<entt::entity>& on);

// The 2D ground: the rectangles of the cells of the TileMapLayers in on whose key or tile is in
// tiles (any, when it is empty), in world space. path is where on is, for errors.
[[nodiscard]] Result<std::vector<procedural::ScatterTriangle>>
ground_2d(Session& session, const std::vector<std::string>& tiles,
          const std::vector<entt::entity>& on, const std::string& path);

// The entities under the parent, and theirs, so a run replaces what the one before made.
void collect_descendants(const world::World& world, entt::entity entity,
                         std::vector<entt::entity>& out);

} // namespace slash::command
