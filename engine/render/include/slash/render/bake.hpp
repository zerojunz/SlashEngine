// Parts of this file are ported from Godot Engine (MIT); see THIRD_PARTY.md.
#pragma once

#include "slash/core/result.hpp"
#include "slash/render/mesh.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

// A lightmap's light traced on a device that traces rays (RenderDevice::ray_tracing), on the GPU:
// scene3d's lightmapper (src/lightmapper.cpp, its CPU twin) rasters the atlas, then hands the
// device its triangles, lights, environment and texels for the lightmap light shader's direct
// light, bounce light and light probes modes, and denoises, dilates and blends seams in what comes
// back.
namespace slash::render {

// A corner of a triangle a bake traces: in the world, with its normal and its place in the
// atlas, from 0 to 1.
struct BakeVertex {
    std::array<float, 3> position{};
    float u = 0.0F;
    std::array<float, 3> normal{};
    float v = 0.0F;
};

// A triangle's slice of the atlas and the faces it culls.
struct BakeTriangle {
    std::uint32_t slice = 0;
    CullMode cull = CullMode::Back;
};

// A light a bake traces, as LightmapperRD's LightBaseData.
struct BakeLight {
    LightType type = LightType::Directional;
    std::array<float, 3> position{};
    std::array<float, 3> direction{};
    std::array<float, 3> color{};
    float energy = 1.0F;
    float indirect_energy = 1.0F;
    float size = 0.0F;
    float range = 0.0F;
    float attenuation = 1.0F;
    float cos_spot_angle = 0.0F;
    float inv_spot_attenuation = 1.0F;
    float shadow_blur = 1.0F;
    bool static_bake = false; // its light goes into the lightmap, not only into bounces
};

// What a trace bakes: a lightmap's light, or one of the passes of a mesh map bake
// (trace_mesh_maps): each texel's ambient occlusion, sky and thickness, or where its ray from the
// cage meets the triangles.
enum class BakeKind : std::uint8_t { Lightmap, MeshOcclusion, MeshHits };

// What a bake's light is traced over. Each atlas layer holds a float4 a texel, slice by slice and
// row by row: the materials' albedo (alpha too) and emission, and each texel's place in the world
// and normal, where the raster put them (a normal of zero length marks an empty texel).
struct LightmapTrace {
    std::vector<BakeVertex> vertices;
    std::vector<std::uint32_t> indices; // three a triangle
    std::vector<BakeTriangle> triangles;
    std::vector<BakeLight> lights;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t slices = 0;
    std::span<const std::array<float, 4>> albedo;
    std::span<const std::array<float, 4>> emission;
    std::span<const std::array<float, 4>> position;
    std::span<const std::array<float, 4>> normal;
    // The environment's panorama, rgb, repeating; one texel when it has none.
    std::uint32_t environment_width = 0;
    std::uint32_t environment_height = 0;
    std::span<const std::array<float, 4>> environment;
    // scene3d::LightmapSettings, and the world's size.
    std::uint32_t rays = 128;
    std::uint32_t bounces = 3;
    float bounce_indirect_energy = 1.0F;
    bool use_texture_for_bounces = true;
    float bias = 0.0005F;
    float supersampling = 1.0F;
    float exposure = 1.0F;
    float world_size = 1.0F;
    // How the work is split: the rays a bounce pass traces (each pass seeded afresh, so this
    // changes the noise), the texels a dispatch lights at most (region_size squared) and the rays a
    // probe pass traces.
    std::uint32_t rays_per_pass = 32;
    std::uint32_t region_texels = 512U * 512U;
    std::uint32_t rays_per_probe_pass = 64;
    // Light probes: points in the world, the rays each sends (16 to 8192), and the probes' space
    // as three columns, which turns their rays into the world's.
    std::span<const std::array<float, 3>> probe_points;
    std::uint32_t probe_rays = 512;
    std::array<std::array<float, 3>, 3> probe_space{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
    // A mesh map bake's pass, which trace_mesh_maps sets with how far its rays reach.
    BakeKind kind = BakeKind::Lightmap;
    float occlusion_distance = 1.0F;
    float thickness_distance = 1.0F;
    float cage_distance = 0.0F;
};

// What comes back: each texel's direct light for bounces and its light (direct from lights that
// bake whole, plus the bounces), and each probe's light as nine spherical harmonics' rgb.
struct LightmapTraced {
    std::vector<std::array<float, 4>> light_source;
    std::vector<std::array<float, 4>> accum;
    std::vector<std::array<std::array<float, 3>, 9>> probe_sh;
};

// A mesh map bake's rays (scene3d's bake_mesh_maps, its CPU twin), traced on the GPU: from each
// texel of a width by height map that a triangle covers, at its point and normal, against the
// triangles, which every ray meets from either side.
struct MeshMapTrace {
    std::vector<BakeVertex> vertices; // their positions and normals
    std::vector<std::uint32_t> indices;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    // A float4 a texel, row by row: its point (xyz), and its normal, of zero length for a texel
    // no triangle covers.
    std::span<const std::array<float, 4>> position;
    std::span<const std::array<float, 4>> normal;
    // MeshOcclusion: rays cosine-weighted about the normal (about its opposite for thickness),
    // rays of each kind a texel. MeshHits: the one ray from cage_distance out along the normal
    // to cage_distance in.
    BakeKind kind = BakeKind::MeshOcclusion;
    std::uint32_t rays = 32;
    float bias = 0.0001F;            // how near a hit may be
    float occlusion_distance = 1.0F; // how far an occluding surface may be
    float thickness_distance = 1.0F; // the most thickness measured
    float sky_distance = 1.0F;       // how far a ray goes to reach the sky
    float cage_distance = 0.0F;
    std::uint32_t region_texels = 512U * 512U; // the texels a dispatch traces at most
};

// A float4 a texel. MeshOcclusion: the share of rays that leave unoccluded, that reach the sky
// above (going up), and the mean distance through the mesh over thickness_distance. MeshHits:
// the triangle met plus 1 (0 for none, a float), the hit's barycentrics y and z, and how far
// from the cage it is.
struct MeshMapTraced {
    std::vector<std::array<float, 4>> texels;
};

class RenderDevice;

// Traces a mesh map bake's rays on the device's GPU (RenderDevice::trace_lightmap), waiting for
// them. An error with ErrorCode::NotFound on a device that doesn't trace rays.
[[nodiscard]] Result<MeshMapTraced> trace_mesh_maps(RenderDevice& device,
                                                    const MeshMapTrace& trace);

} // namespace slash::render
