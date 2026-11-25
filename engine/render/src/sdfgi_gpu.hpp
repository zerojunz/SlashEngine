// SDFGI on the GPU devices: what shaders/sdfgi.hlsl, sdfgi_voxelize.hlsl and sdfgi_gather.hlsl
// take (their uniforms, the words of the items buffer and how each target's buffers are laid
// out) and the dispatches a frame runs, in the order the CPU twin (src/sdfgi.cpp's update_sdfgi)
// does the same work. The GPU and WebGPU devices both follow it; keep the three in step.

#pragma once

#include "sdfgi.hpp"

#include "slash/render/device.hpp"
#include "slash/render/mesh.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace slash::render::detail {

// Whether a frame draws SDFGI on a GPU device: in the clustered renderer, and not into a reflection
// probe's cube, as the software device.
[[nodiscard]] inline bool uses_sdfgi(const Scene3D& scene) {
    return scene.method == RenderMethod::ForwardPlus && scene.sdfgi.enabled && !scene.capture;
}

// sdfgi.hlsl's threads a group, and how a dispatch's index runs: id.x + id.y * 65536.
inline constexpr std::uint32_t kSdfgiThreads = 64;
inline constexpr std::uint32_t kSdfgiGroupsAcross = 65536 / kSdfgiThreads;

// The items buffer (sdfgi_common.hlsl): kSdfgiItemCascade words a cascade (its offset, cell
// size, position, 1 over its cell size, then where its cells, fields, probes, probe offsets and
// history start), the lights from kSdfgiItemLights (kSdfgiItemLight words each: type, color,
// direction, position, attenuation, radius, the spot's cosine and 1 over its attenuation), then
// each cascade's probes' ray offsets.
inline constexpr std::uint32_t kSdfgiItemCascade = 16;
inline constexpr std::uint32_t kSdfgiItemLights = kSdfgiItemCascade * kMaxSdfgiCascades;
inline constexpr std::uint32_t kSdfgiItemLight = 16;
inline constexpr std::uint32_t kSdfgiProbeCount =
    kSdfgiProbeAxis * kSdfgiProbeAxis * kSdfgiProbeAxis;
inline constexpr std::uint32_t kSdfgiProbeImageTexels =
    (kSdfgiOctSize + 2) * kSdfgiProbeAxis * kSdfgiProbeAxis * (kSdfgiOctSize + 2) * kSdfgiProbeAxis;

// sdfgi.hlsl's passes (step.x).
enum class SdfgiPass : std::uint8_t {
    Clear = 0,
    Resolve = 1,
    EdtInit = 2,
    Edt = 3,
    Store = 4,
    Occlusion = 5,
    OcclusionStore = 6,
    Light = 7,
    LightMerge = 8,
    LightGather = 9,
    Integrate = 10,
    ProbeStore = 11,
    ScrollCopy = 12,
    ScrollMove = 13,
};
// The buffers a clear writes (its step.z).
enum class SdfgiBuffer : std::uint8_t {
    Cells = 0,
    Fields = 1,
    Probes = 2,
    History = 3,
    Scratch = 4
};
// probes.w and the gather's grid.z.
inline constexpr std::uint32_t kSdfgiFlagOcclusion = 1;
inline constexpr std::uint32_t kSdfgiFlagSkyLight = 2;

// sdfgi.hlsl's Params.
struct SdfgiUniforms {
    std::array<std::uint32_t, 4> step{};   // the pass, the cascade, its number, its threads
    std::array<std::uint32_t, 4> grid{};   // the cells a side, the cascades, the light's share
    std::array<std::uint32_t, 4> probes{}; // the history's frames, this frame's, rays, flags
    std::array<std::int32_t, 4> shift{};   // how far it moved; w 1 when its cells scroll
    std::array<std::uint32_t, 4> range{};  // a clear's start, count and value; w the lights
    Float4 params{};                       // the y scale, the bounce feedback, the probe bias
    Float4 ambient{};                      // the ambient light; w 1 when the sky lights
    std::array<Float4, 3> sky_rows{};      // the world to the sky; energy [0].w, share [1].w
};
static_assert(sizeof(SdfgiUniforms) == 160);

// sdfgi_voxelize.hlsl's Params, for one draw into one cascade.
struct SdfgiVoxelizeUniforms {
    std::array<Float4, 3> model_rows{};
    std::array<Float4, 3> normal_rows{};
    Float4 albedo{};
    Float4 emission{};
    std::array<std::uint32_t, 4> options{};
    Float4 uv1{};   // scale, then offset
    Float4 place{}; // the cascade's cell 0 in the cells' world, then 1 over its cell size
    std::array<std::uint32_t, 4> work{}; // the cells a side, first index, triangles, y scale bits
    std::array<std::int32_t, 4> shift{}; // how far it moved; w 1 when it keeps its cells
};
static_assert(sizeof(SdfgiVoxelizeUniforms) == 208);

// sdfgi_gather.hlsl's Gather.
struct SdfgiGatherUniforms {
    std::array<Float4, 4> to_world{}; // the prepass's clip space to the world, a row each
    std::array<Float4, 3> view_rows{};
    Float4 camera{};                     // the camera in the world, then the energy
    std::array<std::uint32_t, 4> grid{}; // the cells a side, the cascades, flags
    Float4 params{};                     // the y scale, the normal bias, width, height
};
static_assert(sizeof(SdfgiGatherUniforms) == 160);

// A target's buffers, in words, for cascades of size cells a side keeping history_size frames:
// each cascade's cells (its cells, then its occlusion), fields (its field, then its light),
// probes (averages, irradiance, radiance) and history in turn, and the scratch every pass shares.
struct SdfgiLayout {
    std::uint32_t cascades = 0;
    std::uint32_t size = 0;
    std::uint32_t history_size = 0;
    std::uint32_t cells = 0; // a cascade's words in each buffer
    std::uint32_t fields = 0;
    std::uint32_t probes = 0;
    std::uint32_t history = 0;
    std::uint32_t scratch = 0; // the scratch buffer's words
    [[nodiscard]] std::uint32_t cell_count() const { return size * size * size; }
    bool operator==(const SdfgiLayout&) const = default;
};
[[nodiscard]] SdfgiLayout sdfgi_layout(std::uint32_t cascades, std::uint32_t size,
                                       std::uint32_t history_size);

// The light a probe's ray takes where it leaves the cascades: the ambient light, or with sky the
// sky's radiance cube (which the device binds) turned by to_sky, times sky_energy, over the
// ambient light by sky_share, as ray_light.comp's environment.
struct SdfgiEnvironment {
    Float3 ambient{};
    bool sky = false;
    std::array<Float3, 3> to_sky{};
    float sky_energy = 1.0F;
    float sky_share = 1.0F;
};

// The items buffer for a frame: the cascades' places and sections, the lights and the probes'
// ray offsets.
[[nodiscard]] std::vector<std::uint32_t>
sdfgi_items(const SdfgiState& state, const SdfgiLayout& layout, std::span<const SdfgiLight> lights);

// One step of a frame's work: a dispatch of sdfgi.hlsl, the draws voxelized into a cascade
// (with voxelize's place and shift), or a copy of a cascade's new cells, which Resolve wrote
// over its field, into its cells.
struct SdfgiStep {
    enum class Kind : std::uint8_t { Dispatch, Voxelize, CopyCells };
    Kind kind = Kind::Dispatch;
    std::uint32_t cascade = 0;
    SdfgiUniforms uniforms{};
    SdfgiVoxelizeUniforms voxelize{}; // with Voxelize: place, work's size and y scale, shift
};

// A frame's steps for plan, as update_sdfgi does the work: each cascade's probes cleared or
// scrolled and its cells voxelized, resolved, given their distance field and occlusion; then
// every cascade's light; then every cascade's probes integrated and stored.
[[nodiscard]] std::vector<SdfgiStep>
sdfgi_steps(const SdfgiState& state, const SdfgiFramePlan& plan, const SdfgiLayout& layout,
            std::uint32_t light_count, const SdfgiEnvironment& environment);

// The groups a dispatch of threads runs, x then y.
[[nodiscard]] std::array<std::uint32_t, 2> sdfgi_groups(std::uint32_t threads);

// A draw's voxelize uniforms for a cascade's step: its model, normals' matrix, surface and
// texture coordinates' scale and offset, and its triangles from first_index.
[[nodiscard]] SdfgiVoxelizeUniforms
sdfgi_voxelize_uniforms(const SdfgiStep& step, const Mat4& model, const Mat4& normal_model,
                        const MeshSurfaceUniforms& surface, const Float4& uv1,
                        std::uint32_t first_index, std::uint32_t index_count);

// The gather's uniforms for a frame of frame's size seen from camera.
[[nodiscard]] SdfgiGatherUniforms
sdfgi_gather_uniforms(const SdfgiState& state, const Float3& camera, const RayShadowFrame& frame);

} // namespace slash::render::detail
