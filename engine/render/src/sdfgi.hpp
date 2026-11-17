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
#include <optional>
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

// A cascade: its place (its center in cells, Godot's cascade.position) and cell size, and its
// cells and probes as words, laid out as the GPU devices' buffers hold them (x fastest, then y,
// then z), so the compute shaders that port these passes (shaders/sdfgi.hlsl) read and write the
// same numbers:
//
// - cells: two words a cell. The first is the cell word: a solid cell has kSdfgiSolid set, its
//   albedo as Godot's 5 bits a channel (red in bits 0 to 4, green 5 to 9, blue 10 to 14) and the
//   sides geometry faces from bit 15 (bit 15 + k for aniso direction k: +x, +y, +z, -x, -y, -z);
//   an empty cell holds 1 + which of its 26 neighbors it takes its light from, or 0, so light
//   filters between cells (numbered as MODE_STORE numbers them: offsets -1 to 1 in x, then y,
//   then z, z fastest, the cell itself left out). The second is a solid cell's emission (RGB9E5).
// - field: two words a cell. The first holds the light's shares over aniso directions 0 to 3 as
//   bytes (Godot's aniso0), the second those of 4 and 5 (aniso1) in its low bytes and the distance
//   field in bits 16 to 23 (Godot's R8 texels: 0 in a solid cell, else 1 more than the distance
//   to the nearest one, rounded, up to 255).
// - light: a word a cell, its light as RGB9E5.
// - occlusion: with use_occlusion, a word a cell: how much of the cell each of the eight nearest
//   probes sees, by the probe's world parity (bit 0 x, 1 y, 2 z), 4 bits each (nibble i at bit
//   4 * i), as Godot's occlusion texture holds them.
// - history: the probes' spherical harmonics over the last frames_to_converge frames, as Godot's
//   RGBA16I history texture: [layer][probe * 16 + i], two words each, red and green then blue and
//   alpha, as 16-bit integers (low half first); average their sum, four words each.
// - irradiance and radiance: the probes' octahedral maps (Godot's lightprobe texture, RGB9E5),
//   (6 + 2) * 17 * 17 texels by (6 + 2) * 17, x fastest.
inline constexpr std::uint32_t kSdfgiSolid = 1U << 31U;
inline constexpr std::uint32_t kSdfgiFacingShift = 15;

struct SdfgiCascade {
    std::array<std::int32_t, 3> position{};
    float cell_size = 0.0F;
    std::vector<std::uint32_t> cells;
    std::vector<std::uint32_t> field;
    std::vector<std::uint32_t> light;
    std::vector<std::uint32_t> occlusion;
    std::vector<std::uint32_t> history;
    std::vector<std::int32_t> average;
    std::vector<std::uint32_t> irradiance;
    std::vector<std::uint32_t> radiance;
    bool all_lights_dirty = true;
};

// A cell's distance field byte, cell word and whether it is solid.
[[nodiscard]] std::uint32_t sdfgi_sdf(const SdfgiCascade& cascade, std::size_t cell);
[[nodiscard]] inline bool sdfgi_solid(const SdfgiCascade& cascade, std::size_t cell) {
    return (cascade.cells[cell * 2] & kSdfgiSolid) != 0;
}
// A solid cell's albedo (0 to 1 a channel, in steps of 1/31) and the sides it faces.
[[nodiscard]] Float3 sdfgi_albedo(std::uint32_t cell_word);
[[nodiscard]] inline std::uint32_t sdfgi_facing(std::uint32_t cell_word) {
    return (cell_word & kSdfgiSolid) != 0 ? (cell_word >> kSdfgiFacingShift) & 0x3FU : 0U;
}

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

// What a frame's update does to a cascade, which the CPU twin and the GPU devices both follow.
struct SdfgiCascadePlan {
    // Whether its cells are made again; with scrolled, it moved by that many cells and keeps the
    // cells it still holds, moved, as Godot's scroll renders only the region that came in.
    bool voxelize = false;
    std::optional<std::array<std::int32_t, 3>> scrolled;
    // What happens to its probes: kept, cleared, or moved by probe_shift whole probes.
    enum class Probes : std::uint8_t { Keep, Clear, Scroll };
    Probes probes = Probes::Keep;
    std::array<std::int32_t, 3> probe_shift{};
    // Its solid cells lit this frame: those whose index is light_offset modulo light_increment.
    std::uint32_t light_increment = 1;
    std::uint32_t light_offset = 0;
};
struct SdfgiFramePlan {
    std::vector<SdfgiCascadePlan> cascades;
    std::uint32_t size = 0;
    float y_mult = 1.0F;
    std::uint32_t history_size = 1;
    // Which of the history's frames the probes' rays write this frame.
    std::uint32_t history_index = 0;
};

// Godot's sdfgi_update for a frame seen from camera (in the world): settles the settings, makes
// the cascades again when they changed, moves each cascade with the camera by whole steps, marks
// them all to be voxelized when geometry (a key of what they are made of) changed, and counts the
// frame. The cascades' places and cell sizes change here; their cells and probes don't.
[[nodiscard]] SdfgiFramePlan plan_sdfgi_frame(SdfgiState& state, const Scene3D& scene,
                                              const Float3& camera, std::uint64_t geometry);

// Each of a cascade's probes' offset along its rays' spiral (sdfgi_integrate.glsl's
// hashf3 of the probe's place in the world), as integrate works it out: kSdfgiProbeAxis^3 of them,
// x fastest. Devices without doubles can't make Godot's sine bits, so the CPU hands them over.
[[nodiscard]] std::vector<float> sdfgi_probe_offsets(const SdfgiCascade& cascade,
                                                     std::uint32_t size);

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

// sdfgi_preprocess.glsl's MODE_OCCLUSION and MODE_STORE: from each probe, how much of each cell
// within a probe's spacing of it the probe sees, flooded out from the probe through the empty
// cells, solid cells taking their empty neighbors', and empty cells next to solid ones by how many
// of those face the probe. Reads the cells' sums; fills cascade.occlusion.
void make_sdfgi_occlusion(SdfgiCascade& cascade, std::uint32_t size, jobs::Pool* pool);
// A cell's occlusion from the probes of a parity, 0 to 1, in steps of 1/15.
[[nodiscard]] float sdfgi_occlusion(const SdfgiCascade& cascade, std::size_t cell,
                                    std::uint32_t parity);

// Godot's RGB9E5 (rgbe_encode in its SDFGI shaders), and back, as its textures filter them.
[[nodiscard]] std::uint32_t sdfgi_rgbe_encode(const Float3& color);
[[nodiscard]] Float3 sdfgi_rgbe_decode(std::uint32_t rgbe);

} // namespace slash::render::detail
