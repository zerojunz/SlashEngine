#pragma once

// Hair and fur over a model's skin, as Unreal's Groom component (slash/world/hair.hpp has the
// groom files and the strand solver): a HairInstance3D's groom grown over the surfaces of its
// model that its section names, its guide strands where the skin puts them each tick, and the
// meshes its strands, cards or shells draw as.

#include "slash/core/math.hpp"
#include "slash/core/result.hpp"
#include "slash/render/device.hpp"
#include "slash/render/mesh.hpp"
#include "slash/scene3d/cloth.hpp"
#include "slash/scene3d/scene3d.hpp"
#include "slash/world/hair.hpp"
#include "slash/world/tree3d.hpp"
#include "slash/world/world.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace slash::scene3d {

// Where a strand's root is on the section: a triangle (its first index into the section's
// indices) and a point in it, with what is there at rest in the model's space and the joints
// that carry it.
struct HairRoot {
    std::uint32_t triangle = 0;
    Vec3 bary;
    Vec3 position;
    Vec3 normal;
    Vec2 uv;
    std::array<std::pair<std::uint32_t, float>, 4> weights{};
};

// A groom grown over an entity's model.
struct HairGrowth3D {
    std::shared_ptr<const ClothSection> section;
    std::shared_ptr<const world::Groom> groom;
    std::string key;
    int points = 0; // per strand, root included
    std::vector<HairRoot> guides;
    std::vector<Vec3> rest; // the guides' points at rest, in the model's space, points each
    // A strand drawn: it follows its guide, from its own root (offset from the guide's at rest),
    // at its share of the guide's length.
    struct Strand {
        std::uint32_t guide = 0;
        Vec3 offset;
        float length = 1.0F;
        float phase = 0.0F;
        float clump = 0.0F;
        Vec4 color{1.0F, 1.0F, 1.0F, 1.0F};
    };
    std::vector<Strand> strands;
    std::vector<std::uint32_t> vertex_guides; // each section vertex's nearest guide
    std::vector<float> vertex_lengths;        // and the fur's length there (shells)
    real_t area = 0.0F;                       // the section's, at rest
};

// What hair_growth would grow an entity's hair from: its model, section, groom and maps, with
// their versions; "" when it can't be grown. A change means the growth changed.
[[nodiscard]] std::string hair_key(const world::World& world, entt::entity entity);

// The groom a HairInstance3D names grown over its model: guides spread over the section by
// area, the strands drawn by the groom's density, each kept where its density map says, sized
// by its length map and tinted by its color map; an error when it can't be.
[[nodiscard]] Result<std::shared_ptr<const HairGrowth3D>> hair_growth(const world::World& world,
                                                                      entt::entity entity);

// Where the skin puts each guide's root now, in the world: the blend of its joints' moves from
// rest, after the entity's global transform. The guides' groomed points there, and where they
// are once a guide's frame carries its rest points.
[[nodiscard]] std::vector<world::Affine3D> hair_frames(const world::World& world,
                                                       const world::Tree3D& tree,
                                                       entt::entity entity,
                                                       const HairGrowth3D& growth);
[[nodiscard]] std::vector<Vec3> hair_targets(const HairGrowth3D& growth,
                                             std::span<const world::Affine3D> frames);

// The strands (or cards) as ribbons, from the guides' points in the world: strands face eye,
// cards the surface they grow from. Indices are into vertices, three a triangle.
void hair_ribbons(const HairGrowth3D& growth, std::span<const Vec3> guides,
                  std::span<const world::Affine3D> frames, Vec3 eye,
                  std::vector<render::Vertex3D>& vertices, std::vector<std::uint32_t>& indices);

// The section's vertices where the skin puts them now, in the world, with their normals.
void hair_skin(const world::World& world, const world::Tree3D& tree, entt::entity entity,
               const HairGrowth3D& growth, std::vector<Vec3>& positions,
               std::vector<Vec3>& normals);

// One layer of fur shells: the skinned section raised along its normals to layer + 1 of
// shell_count of the fur's length, moved as its nearest guide's tip has moved from its groomed
// place (more toward the top), its texture coordinates scaled by shell_tiling. Indices are the
// section's.
void hair_shell(const HairGrowth3D& growth, std::span<const Vec3> positions,
                std::span<const Vec3> normals, std::span<const Vec3> guides,
                std::span<const Vec3> targets, int layer, std::vector<render::Vertex3D>& out);

// The fur's pattern for shells, the same for every groom: a strand in some texels, alpha its
// height (a shell at h of the fur's length shows those taller than h), none in the rest.
[[nodiscard]] render::Image hair_shell_image();

// What keeps each HairInstance3D from growing: a groom file that can't be read or is 2D, no
// model to grow on, or a section with no triangles.
[[nodiscard]] std::vector<Problem> check_hair(const world::World& world);

} // namespace slash::scene3d
