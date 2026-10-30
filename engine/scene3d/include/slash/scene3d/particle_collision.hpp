#pragma once

#include "slash/core/jobs.hpp"
#include "slash/core/math.hpp"
#include "slash/core/result.hpp"
#include "slash/render/image.hpp"
#include "slash/render/particles3d.hpp"
#include "slash/scene3d/skeleton.hpp"
#include "slash/world/tree3d.hpp"
#include "slash/world/world.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace slash::asset {
class Assets;
}

// What GPUParticles3D's particles collide with besides spheres and boxes, as Godot 4.6's
// GPUParticlesCollisionSDF3D and GPUParticlesCollisionHeightField3D: a signed distance field baked
// from the meshes in a box, and the ground under a box drawn from above.
namespace slash::scene3d {

struct Problem; // scene3d.hpp's

// What a GPUParticlesCollisionSDF3D's texture ends in: a 16-bit gray PNG whose samples are half
// floats' bits, its slices of cells stacked down as Godot's bake image stacks them.
inline constexpr std::string_view kParticleSdfSuffix = ".sdf.png";

// The cells along x, y and z of an SDF of a box of size at resolution, as Godot's
// get_estimated_cell_size: resolution along the longest side, the others as many whole cells
// as fit, at least 1; zeros for a resolution that isn't "16", "32", "64", "128", "256" or "512".
[[nodiscard]] std::array<std::uint32_t, 3> particle_sdf_cells(Vec3 size,
                                                              std::string_view resolution);

// The texels across x and z of a height field of a box of size at resolution, as Godot's particles
// storage sizes its texture: resolution along the longer side, the other in proportion; zeros for
// a resolution that isn't "256", "512" or "1024".
[[nodiscard]] std::array<std::uint32_t, 2> particle_height_field_size(Vec3 size,
                                                                      std::string_view resolution);

// What particles.bake_sdf makes: the distances, the meshes and triangles they were measured to.
struct ParticleSdfBake {
    render::ParticleSdf3D sdf;
    std::size_t meshes = 0;
    std::size_t triangles = 0;
    std::vector<Problem> problems;
};

// Bakes a GPUParticlesCollisionSDF3D entity's distances, as Godot's
// GPUParticlesCollisionSDF3D::bake: the visible MeshInstance3Ds on a layer of bake_mask under its
// parent (everything for a root), with CSG roots and GridMaps there, whose triangles touch its box
// of whole cells, read through models; each cell's distance to the nearest triangle, less than 0
// within thickness cells behind one, through a bounding volume hierarchy, cells on pool's threads
// where given. The same distances on every platform. Refuses an entity without the component, a
// resolution that isn't one, an SDF of more than render::kMaxParticleSdfCells3D cells and a box
// with no triangles.
[[nodiscard]] Result<ParticleSdfBake> bake_particle_sdf(const world::World& world,
                                                        entt::entity entity,
                                                        const ModelLoader& models,
                                                        jobs::Pool* pool = nullptr);

// An SDF's .sdf.png file: width by height times depth samples, slice z under slice z - 1.
[[nodiscard]] Result<std::string> encode_particle_sdf(const render::ParticleSdf3D& sdf);

// An SDF's distances from its texture cut into slices_horizontal by slices_vertical slices, as
// particle_field cuts a vector field's. Refuses slices of no cells and more than
// render::kMaxParticleSdfCells3D cells.
[[nodiscard]] Result<render::ParticleSdf3D>
particle_sdf(const render::Gray16& texture, int slices_horizontal, int slices_vertical);

// A GPUParticlesCollisionHeightField3D entity's heights, as Godot draws them
// (render_particle_collider_heightfield): an orthographic camera half the box's height above its
// origin looks down its -y with -z up, and each texel takes the highest point at its center of the
// opaque triangles of what draws on a layer of heightfield_mask (MeshInstance3Ds, CSG roots,
// GridMaps on layer 1 and HTerrains), clipped to the box, read through models. Empty for an entity
// without the component or with a resolution that isn't one.
[[nodiscard]] render::ParticleHeightField3D
particle_height_field(const world::World& world, const world::Tree3D& tree, entt::entity entity,
                      const ModelLoader& models, std::vector<Problem>& problems);

// Registers the "particle_sdf" asset type: .sdf.png files as render::Gray16. add_asset_types
// calls it.
void add_particle_sdf_type(asset::Assets& assets);

// Each GPUParticlesCollisionSDF3D's and GPUParticlesCollisionHeightField3D's problems, for scene
// validate and exports: a resolution or update_mode that isn't one, a size that isn't positive, a
// thickness below 0, and a texture that isn't a .sdf.png file, can't be read or can't be cut into
// its slices.
[[nodiscard]] std::vector<Problem> check_particle_colliders(const world::World& world);

} // namespace slash::scene3d
