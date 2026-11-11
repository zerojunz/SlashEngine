#pragma once

#include "slash/core/math.hpp"
#include "slash/core/result.hpp"
#include "slash/scene3d/csg.hpp"
#include "slash/scene3d/scene3d.hpp"
#include "slash/world/components.hpp"

#include <cstdint>
#include <entt/entity/entity.hpp>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace slash::world {
class World;
}

// Destruction's geometry (Godot has none; Unreal's geometry collections are the model): a closed
// mesh cut into Voronoi cells around points a pattern places in its bounds, cut again as pieces
// break again, each cell's share made with
// Manifold's booleans, as CSG shapes are. physics3d breaks Destructible3D bodies into these
// pieces, and Renderer draws a FracturePiece3D's.
namespace slash::scene3d {

// One piece: what of the mesh lies in one cell.
struct FracturePiece {
    // Its surfaces, triangles and bounds in the mesh's space, as a CSG root's: the faces it cuts
    // through draw with the mesh's own material.
    CsgMesh mesh;
    std::vector<Vec3> hull; // its corners, in the order they first appear, for a convex shape
    Vec3 center;            // the center of its volume
    double volume = 0.0;    // cubic meters
    Vec3 low;               // its bounds
    Vec3 high;
    // The pieces it shares a face with, and the face's area in square meters, in order.
    std::vector<std::pair<int, float>> neighbors;
};

// A mesh's pieces, in the order of their cells' points; cells that hold none of the mesh are
// left out, so a piece's index is into this list.
struct Fracture {
    std::vector<FracturePiece> pieces;
    std::vector<Problem> problems; // why the mesh couldn't be cut
};

// Cuts the mesh of a CSGMesh3D tree (its model or primitive, with its material) by cuts, first
// to last: each earlier cut's piece (its index) is what the next one cuts, and the pieces are
// the last cut's (whose index is not used). A cut's cells grow around the points its pattern
// places in the bounds of what it cuts (world::fracture_sites). Children of the tree are left
// out.
[[nodiscard]] std::shared_ptr<const Fracture> fracture(const CsgTree& mesh,
                                                       std::span<const world::FractureCut> cuts);
// One voronoi cut into `cells` cells around points seeded by seed.
[[nodiscard]] std::shared_ptr<const Fracture> fracture(const CsgTree& mesh, int cells,
                                                       std::uint32_t seed);

// The cuts that made a piece, its own last: its cuts, or its one voronoi cut.
[[nodiscard]] std::vector<world::FractureCut> piece_cuts(const world::FracturePiece3D& piece);

// The fracture of what an entity's MeshInstance3D draws, its primitive mesh or model with its
// StandardMaterial3D, kept in the world (install_csg) while an entity with the same mesh and
// cuts uses it. Fails, saying why, when the entity draws nothing that can be cut.
[[nodiscard]] Result<std::shared_ptr<const Fracture>>
fracture_of(const world::World& world, entt::entity entity,
            std::span<const world::FractureCut> cuts);
[[nodiscard]] Result<std::shared_ptr<const Fracture>>
fracture_of(const world::World& world, entt::entity entity, int cells, std::uint32_t seed);

// The Destructible3Ds of a world that can't break, in the order their entities were made: those
// with no StaticBody3D or RigidBody3D, and those whose mesh can't be cut (fracture_of's reason).
[[nodiscard]] std::vector<Problem> check_destructibles(const world::World& world);

} // namespace slash::scene3d
