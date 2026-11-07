// Godot 4.6's SDFGI (Scene3D::sdfgi) on the CPU: a research spike, after RendererRD's
// GI::SDFGI (environment/gi.cpp) and its shaders (sdfgi_preprocess.glsl, sdfgi_direct_light.glsl,
// sdfgi_integrate.glsl and gi.glsl's sdfgi_process), which the software device runs. Each step
// names the pass it follows and says where it differs.

#pragma once

#include "mesh_shading.hpp"
#include "path_trace.hpp"
#include "ray_shadows.hpp"

#include "slash/core/jobs.hpp"
#include "slash/render/mesh.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace slash::render::detail {

// Godot's SDFGI constants: a probe every cascade_size / 16 cells (17 a side), probes' light as
// 16 spherical harmonics, kept over the frames as integers of 10 bits under the point, and stored
// as octahedral maps 6 texels across with a border of 1.
inline constexpr std::uint32_t kSdfgiProbeDivisor = 16;
inline constexpr std::uint32_t kSdfgiProbeAxis = kSdfgiProbeDivisor + 1;
inline constexpr std::uint32_t kSdfgiShSize = 16;
inline constexpr std::uint32_t kSdfgiOctSize = 6;
inline constexpr int kSdfgiHistoryBits = 10;

// A solid cell, as sdfgi_preprocess.glsl's ProcessVoxel holds it: its index in the cascade, its
// albedo (5 bits a channel, as Godot's), the light it gives off, which sides geometry faces (bit k
// for aniso direction k: +x, +y, +z, -x, -y, -z), and which of its 26 neighbors are empty cells
// that take its light, so light filters between cells.
struct SdfgiCell {
    std::uint32_t index = 0;
    Float3 albedo{};
    Float3 emission{};
    std::uint32_t facing = 0;
    std::uint32_t neighbors = 0;
};

// A cascade: its place (its center in cells, Godot's cascade.position) and cell size, its cells'
// distance field (Godot's R8 texels: 0 in a solid cell, else 1 more than the distance to the
// nearest one, rounded, up to 255), its cells' light (RGB9E5, decoded) and how it splits over the
// six directions (Godot's aniso0 and aniso1, 8 bits each), its solid cells, and its probes'
// history, averages and octahedral irradiance and radiance (Godot's lightprobe texture, RGB9E5,
// (6 + 2) * 17 * 17 texels by (6 + 2) * 17, x fastest).
// What the triangles touching a cell sum to, kept so a cascade that moves makes only the cells
// that came in (Godot's scroll): albedo and emission sums, how many triangles, the sides they face.
struct SdfgiCellSum {
    Float3 albedo{};
    Float3 emission{};
    std::uint32_t count = 0;
    std::uint32_t facing = 0;
};

struct SdfgiCascade {
    std::array<std::int32_t, 3> position{};
    float cell_size = 0.0F;
    std::vector<SdfgiCellSum> sums;
    std::vector<std::uint8_t> sdf;
    std::vector<Float3> light;
    std::vector<std::array<float, 6>> aniso;
    std::vector<SdfgiCell> cells;
    std::vector<std::array<std::int32_t, 4>> history; // [layer][probe * 16 + i], layer fastest last
    std::vector<std::array<std::int32_t, 4>> average; // [probe * 16 + i]
    std::vector<Float3> irradiance;
    std::vector<Float3> radiance;
    bool all_lights_dirty = true;
};

// A target's SDFGI, kept between frames.
struct SdfgiState {
    Sdfgi3D settings;
    std::vector<SdfgiCascade> cascades;
    std::uint64_t geometry = 0; // the path scene's key its cells were made from
    std::uint32_t render_pass = 0;
    std::uint32_t frame = 0;
};

// The cells' and probes' world: positions with y times y_mult, as Godot's.
[[nodiscard]] float sdfgi_y_mult(const Sdfgi3D& settings);
// Where a cascade's cell 0 starts, in that world.
[[nodiscard]] Float3 sdfgi_offset(const SdfgiCascade& cascade, std::uint32_t size);

// Brings a target's SDFGI up to date for a frame seen from camera (in the world), as Godot's
// sdfgi_update, render_region, update_light, update_probes and store_probes run in a frame: the
// cascades follow the camera (moving by 8 cells at a time); a cascade that moved keeps the cells
// it still holds and voxelizes the slab that came in, as Godot scrolls it, and all of them are
// voxelized whole when the frame's geometry changed (paths' key) or settings did, each with its
// distance field made again; the solid cells' light is worked out (a
// frames_to_update_light share of them a frame, all after a cascade is made again); and the
// probes trace their rays. Work is shared out over pool's threads when there is one; the result
// doesn't depend on how many.
void update_sdfgi(SdfgiState& state, const Scene3D& scene, const Float3& camera,
                  const PathScene& paths, const PathTextures& textures, jobs::Pool* pool);

// Each pixel's SDFGI light, as gi.glsl's sdfgi_process works it out for the GI buffers: ambient
// light (RGB, then how much it covers) and reflections (RGB, then how much), eight floats a pixel
// in rows from the top, times energy; zero where a pixel has no surface or no cascade holds it.
// depth and normals are the prepass's, as run_ray_gi takes them.
[[nodiscard]] std::vector<float> run_sdfgi(const SdfgiState& state, const Float3& camera,
                                           const RayShadowFrame& frame,
                                           std::span<const float> depth,
                                           std::span<const float> normals, jobs::Pool* pool);

// Godot's RGB9E5 (rgbe_encode in its SDFGI shaders), and back, as its textures filter them.
[[nodiscard]] std::uint32_t sdfgi_rgbe_encode(const Float3& color);
[[nodiscard]] Float3 sdfgi_rgbe_decode(std::uint32_t rgbe);

} // namespace slash::render::detail
