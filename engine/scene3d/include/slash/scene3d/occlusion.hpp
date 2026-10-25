#pragma once

#include "slash/core/math.hpp"
#include "slash/core/result.hpp"
#include "slash/scene3d/model.hpp"
#include "slash/scene3d/skeleton.hpp"
#include "slash/world/components.hpp"
#include "slash/world/tree3d.hpp"

#include <array>
#include <cstdint>
#include <entt/entity/entity.hpp>
#include <vector>

namespace slash::world {
class World;
}

// Occlusion culling, as Godot 4.6's: OccluderInstance3D's shapes (scene/3d/
// occluder_instance_3d.cpp) and its raycasting culler (modules/raycast/raycast_occlusion_cull.cpp
// over servers/rendering/renderer_scene_occlusion_cull.h). Each frame a ray is cast from the camera
// through each texel of a small depth buffer into the occluders' triangles; a mesh whose bounds lie
// behind every texel they cover is hidden. Godot casts with Embree, which SlashEngine doesn't
// build: OccluderScene is a bounding volume hierarchy over the triangles, casting on the CPU with
// the same results in any order, so frames cull the same on every platform.
namespace slash::scene3d {

struct Problem; // scene3d.hpp's

// An occluder's triangles, as Godot's Occluder3D keeps them: every three indices a triangle.
struct OccluderMesh {
    std::vector<Vec3> vertices;
    std::vector<int> indices;
};

// The triangles of an OccluderInstance3D's shape in its own space, as Godot's _update_arrays:
// QuadOccluder3D's and BoxOccluder3D's (size), SphereOccluder3D's rings (radius),
// PolygonOccluder3D's polygon turned clockwise and cut into triangles, or ArrayOccluder3D's
// vertices and indices. An error for a polygon that can't be cut or indices out of range.
[[nodiscard]] Result<OccluderMesh> occluder_mesh(const world::OccluderInstance3D& occluder);

// Each OccluderInstance3D that can't be used, for scene validate: as Godot's configuration
// warnings, a shape with fewer than three points or indices, a polygon that can't be cut into
// triangles and indices out of range.
[[nodiscard]] std::vector<Problem> check_occluders(const world::World& world);

// The triangles of every visible OccluderInstance3D, placed in the world by tree.
[[nodiscard]] std::vector<std::array<Vec3, 3>> occluder_triangles(const world::World& world,
                                                                  const world::Tree3D& tree);

// Occluders' triangles in a bounding volume hierarchy, for casting rays into them as Godot's
// Embree scene. Faces block rays from either side, as Embree's do without back face culling.
class OccluderScene {
public:
    // Builds the hierarchy over triangles, or none when it holds the same triangles already.
    void build(std::vector<std::array<Vec3, 3>> triangles);
    [[nodiscard]] bool empty() const noexcept { return triangles_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return triangles_.size(); }
    // How far along dir (of unit length) from origin the nearest triangle is, between near and
    // far; far when none is.
    [[nodiscard]] float cast(Vec3 origin, Vec3 dir, float near, float far) const;

private:
    struct Node {
        Bounds box;
        std::uint32_t first = 0; // a leaf's first triangle in order_, or an inner node's first
                                 // child, which its second follows
        std::uint32_t count = 0; // a leaf's triangles; 0 for an inner node
    };

    std::vector<std::array<Vec3, 3>> triangles_; // as built from
    std::vector<std::array<Vec3, 3>> order_;     // in the leaves' order
    std::vector<Node> nodes_;
};

// A camera an occlusion buffer is cast from: where it is, its projection (render::perspective or
// orthogonal's), its near and far planes and whether it is orthogonal.
struct OcclusionCamera {
    world::Affine3D transform;
    std::array<float, 16> projection{};
    float near = 0.05F;
    float far = 4000.0F;
    bool orthogonal = false;
};

// The number of threads Godot's buffer is sized for: Godot multiplies
// rendering/occlusion_culling/occlusion_rays_per_thread by its worker threads, and SlashEngine by
// this, so the buffer is the same size on every machine.
inline constexpr int kOcclusionThreads = 8;

// Godot's hierarchical depth buffer of the distances its rays travel, each level half the last's
// size keeping the farthest of the texels under it, down to one texel.
class OcclusionBuffer {
public:
    // The buffer's size for a viewport of width by height pixels, as RendererViewport::_draw_3d:
    // rays_per_thread times kOcclusionThreads texels, at least one for every 32x32 pixels and at
    // most one for every 2x2, in the viewport's aspect.
    [[nodiscard]] static std::array<int, 2> size_for(int width, int height, int rays_per_thread);

    // Makes the buffer width by height texels, with every level, as HZBuffer::resize; empty when
    // either is 0.
    void resize(int width, int height);
    [[nodiscard]] bool empty() const noexcept { return sizes_.empty(); }
    [[nodiscard]] std::size_t levels() const noexcept { return sizes_.size(); }
    [[nodiscard]] std::array<int, 2> size(std::size_t level = 0) const { return sizes_.at(level); }
    // The distance a level's texel holds, x from the left and y from the bottom.
    [[nodiscard]] float depth(std::size_t level, int x, int y) const;

    // Casts the buffer's rays from camera into scene and fills its levels, as
    // RaycastOcclusionCull::buffer_update: through each texel's middle on the near plane, moved by
    // Godot's jitter for frame when jitter is on (a pattern of nine frames, two thirds of a texel
    // across).
    void update(const OccluderScene& scene, const OcclusionCamera& camera, std::uint64_t frame,
                bool jitter);

    // Whether the box (in the world) is hidden from the camera of the last update, as Godot's
    // HZBuffer::_is_occluded: its nearest point's distance lies beyond every texel its corners'
    // rectangle covers, at the finest level that takes at most 512 texels in all. With jitter, a
    // box seen in one frame isn't hidden again until nine frames have passed (timeout, which
    // starts at 0), as Godot's occlusion timers, so the jitter doesn't make meshes flicker.
    [[nodiscard]] bool occluded(const Bounds& box, std::uint64_t& timeout) const;

private:
    [[nodiscard]] bool hidden(const Bounds& box) const;

    std::vector<std::array<int, 2>> sizes_;
    std::vector<std::vector<float>> levels_;
    OcclusionCamera camera_;
    world::Affine3D inverse_;
    std::uint64_t frame_ = 0;
    bool jitter_ = false;
};

// An OccluderInstance3D's shape baked from the meshes around it, as Godot's
// OccluderInstance3D::bake_scene: every visible MeshInstance3D whose layers meet its bake_mask and
// whose material is opaque, read through models, placed in the occluder's space, each surface
// simplified by meshoptimizer to bake_simplification_distance (Godot's _bake_surface) and its
// repeated points and triangles merged away (SurfaceTool::strip_mesh_arrays). What can't be read
// is a problem.
[[nodiscard]] OccluderMesh bake_occluder(const world::World& world, entt::entity occluder,
                                         const ModelLoader& models, std::vector<Problem>& problems);

} // namespace slash::scene3d
