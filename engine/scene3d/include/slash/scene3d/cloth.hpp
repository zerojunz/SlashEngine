#pragma once

// Character cloth, as Unreal's Chaos Cloth: the part of an entity's model a Cloth3D names, laid
// out for 3D physics to simulate on the model's skin, and where that skin puts it each tick.
// physics3d makes a Jolt soft body of it with skinned constraints; the renderer draws the
// surfaces where the soft body's points are (SoftBodyPose::surfaces).

#include "slash/core/math.hpp"
#include "slash/core/result.hpp"
#include "slash/scene3d/model.hpp"
#include "slash/scene3d/scene3d.hpp"
#include "slash/world/components.hpp"
#include "slash/world/tree3d.hpp"
#include "slash/world/world.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace slash::scene3d {

// The surfaces of a model a Cloth3D's section names, with their vertices one after another.
struct ClothSection {
    std::shared_ptr<const Model> model;
    std::string key; // the model's path and version, and the section: a change makes it again
    // Each surface: its part and its index in the part's mesh, and where its vertices start.
    struct Piece {
        std::size_t part = 0;
        std::size_t surface = 0;
        std::size_t first = 0;
        std::size_t count = 0;
    };
    std::vector<Piece> pieces;
    // Each vertex at rest in the model's space, its color, and the joints that carry it (into
    // the list cloth_joints gives) with their weights, at most four adding up to 1.
    std::vector<Vec3> positions;
    std::vector<std::array<float, 4>> colors;
    std::vector<std::array<std::pair<std::uint32_t, float>, 4>> weights;
    std::vector<std::uint32_t> indices; // its triangles, into positions
    // What each joint is: a skin's joint, or the node of a part no skin deforms.
    struct Joint {
        int skin = -1;
        std::size_t index = 0; // the skin's joint, or the part
    };
    std::vector<Joint> joints;
    std::vector<world::Affine3D> rest; // where each joint puts the vertices at rest
};

// What cloth_section would make the entity's section from: its model's path and version and the
// section's name, or "" when it draws no model. A change means the section changed.
[[nodiscard]] std::string cloth_key(const world::World& world, entt::entity entity);

// The section of the model an entity's MeshInstance3D draws that its Cloth3D names, its model
// read through the world's assets (else its file reader); an error when the entity draws no
// model or the section has no triangles.
[[nodiscard]] Result<std::shared_ptr<const ClothSection>> cloth_section(const world::World& world,
                                                                        entt::entity entity);

// Where the model's skin puts each of the section's joints now, in the model's space: the model
// posed as it draws (its clips, Skeleton3D and modifiers), a skin's joints as their nodes place
// them times their inverse binds, and a part's node as it moved from rest.
[[nodiscard]] std::vector<world::Affine3D> cloth_joints(const world::World& world,
                                                        const world::Tree3D& tree,
                                                        entt::entity entity,
                                                        const ClothSection& section);

// Each vertex's share of max_distance, 0 to 1, by the Cloth3D's max_distance_map.
[[nodiscard]] std::vector<float> cloth_shares(const world::Cloth3D& cloth,
                                              const ClothSection& section);

// What keeps each Cloth3D from being made: a MeshInstance3D drawing a model is needed, and its
// section must name one of the model's materials with triangles.
[[nodiscard]] std::vector<Problem> check_cloths(const world::World& world);

} // namespace slash::scene3d
