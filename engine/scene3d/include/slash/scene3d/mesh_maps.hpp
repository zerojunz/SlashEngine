#pragma once

#include "slash/core/jobs.hpp"
#include "slash/core/result.hpp"
#include "slash/render/device.hpp"
#include "slash/render/mesh.hpp"
#include "slash/scene3d/model.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Mesh maps: what a model's shape says about each texel of its texture, baked into its texture
// coordinates, for texture graphs and paint stacks to mask with: where dirt gathers (ambient
// occlusion), where edges wear (curvature), where light passes through (thickness), what the sky
// reaches, which way the surface faces, where it is, and which part and material it belongs to.
// From a detailed (high) mesh onto a simple (low) one, the height and normal maps carry the
// detail over. Rays trace on the GPU where the device traces rays, and on the CPU, its twin,
// elsewhere.
namespace slash::scene3d {

enum class MeshMap : std::uint8_t {
    Ao,          // "ao": the share of the hemisphere above a texel that meets nothing nearby
    Curvature,   // "curvature": 0.5 flat, toward 1 on convex edges and 0 in concave ones
    Thickness,   // "thickness": how far rays into the mesh go before leaving it
    Sky,         // "sky": the share of the hemisphere above a texel that reaches the sky
    WorldNormal, // "world_normal": the normal in the model's space, as rgb = n / 2 + 0.5
    Position,    // "position": the point in the model's box, from its min (0) to its max (1)
    PartId,      // "part_id": a color for each part of the model
    MaterialId,  // "material_id": a color for each material
    Height,      // "height": how far the high mesh lies above (over 0.5) or below the low one
    Normal,      // "normal": the high mesh's normal in the low one's tangent frame, OpenGL style
};

// Every map, in the order bakes list them.
inline constexpr std::array<MeshMap, 10> kMeshMaps{
    MeshMap::Ao,          MeshMap::Curvature, MeshMap::Thickness, MeshMap::Sky,
    MeshMap::WorldNormal, MeshMap::Position,  MeshMap::PartId,    MeshMap::MaterialId,
    MeshMap::Height,      MeshMap::Normal};

[[nodiscard]] std::string_view mesh_map_name(MeshMap map);
[[nodiscard]] std::optional<MeshMap> mesh_map_named(std::string_view name);
// Whether a map needs a high mesh to bake from: the height and normal maps.
[[nodiscard]] bool mesh_map_needs_high(MeshMap map);

// A model's triangles in its own space: each part's mesh where its node puts it at rest, its
// normals and tangents turned with it, and each triangle's part and material.
struct MeshMapMesh {
    std::vector<render::Vertex3D> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<std::uint32_t> parts;     // a triangle's part, into part_names
    std::vector<std::uint32_t> materials; // a triangle's material, into material_names
    std::vector<std::string> part_names;
    std::vector<std::string> material_names; // the model's, then "default" for surfaces with none
};
[[nodiscard]] MeshMapMesh mesh_map_mesh(const Model& model);

struct MeshMapSettings {
    std::uint32_t width = 1024;
    std::uint32_t height = 1024;
    // The maps to bake, in any order; empty for every map the meshes allow.
    std::vector<MeshMap> maps;
    // Bake into the second texture coordinates (Vertex3D::uv2) rather than the first.
    bool uv2 = false;
    // The rays each texel traces for each of ambient occlusion, sky and thickness.
    std::uint32_t rays = 32;
    // Distances in the model's units; 0 for a share of the size of the meshes' box (its
    // diagonal): how far an occluding surface may be (a quarter), the most thickness measured
    // (half), how far around a point curvature looks (a fiftieth), and how far out from the low
    // mesh and in a ray looks for the high one (a twentieth).
    float occlusion_distance = 0.0F;
    float thickness_distance = 0.0F;
    float curvature_radius = 0.0F;
    float cage_distance = 0.0F;
    // The texels each island of the maps grows into empty texels around it, so filtering and
    // mipmaps don't bleed the background in.
    std::uint32_t padding = 4;
    // A device that traces rays (RenderDevice::ray_tracing) traces them on the GPU; without one,
    // or where it can't, the CPU does.
    render::RenderDevice* device = nullptr;
};

struct MeshMapImage {
    MeshMap map = MeshMap::Ao;
    // RGBA8: gray maps in rgb alike, alpha 255 where a triangle or padding covers a texel and 0
    // elsewhere.
    render::Image image;
    // The red channel's least, mean and greatest over the texels a triangle covers, 0 to 1.
    std::array<float, 3> stats{};
};

struct MeshMapBake {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<MeshMapImage> maps; // in kMeshMaps' order
    float coverage = 0.0F;          // the share of texels a triangle covers
    float overlap = 0.0F;           // the share of covered texels more than one triangle covers
    float missed = 0.0F;            // the share of covered texels whose ray met no high triangle
    // The position map's box, and the height map's reach either way, in the model's units.
    Vec3 min;
    Vec3 max;
    float height_range = 0.0F;
    bool traced_on_gpu = false;
};

// The id maps' color for an id: hues a golden ratio apart, the same on every machine.
[[nodiscard]] std::array<std::uint8_t, 3> mesh_map_id_color(std::uint32_t id);

// Bakes the maps of low, from high when given (it must be for the height and normal maps): the
// other maps then come from the high mesh's surface where each texel's ray meets it. Fails for
// sizes over 8192, a mesh without triangles, or more than 2^24 triangles.
[[nodiscard]] Result<MeshMapBake> bake_mesh_maps(const MeshMapMesh& low, const MeshMapMesh* high,
                                                 const MeshMapSettings& settings,
                                                 jobs::Pool* pool = nullptr);

} // namespace slash::scene3d
