#pragma once

#include "slash/render/mesh.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

// A lightmap's light traced on a device that traces rays (RenderDevice::ray_tracing), as Godot's
// LightmapperRD traces it on the GPU: scene3d's lightmapper (src/lightmapper.cpp, its CPU twin)
// rasters the atlas, then hands the device its triangles, lights, environment and texels for
// lm_compute.glsl's MODE_DIRECT_LIGHT, MODE_BOUNCE_LIGHT and MODE_LIGHT_PROBES, and denoises,
// dilates and blends seams in what comes back.
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
    // Light probes: points in the world, the rays each sends (16 to 8192), and the probes' space
    // as three columns, which turns their rays into the world's.
    std::span<const std::array<float, 3>> probe_points;
    std::uint32_t probe_rays = 512;
    std::array<std::array<float, 3>, 3> probe_space{{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
};

// What comes back: each texel's direct light for bounces and its light (direct from lights that
// bake whole, plus the bounces), and each probe's light as nine spherical harmonics' rgb.
struct LightmapTraced {
    std::vector<std::array<float, 4>> light_source;
    std::vector<std::array<float, 4>> accum;
    std::vector<std::array<std::array<float, 3>, 9>> probe_sh;
};

} // namespace slash::render
