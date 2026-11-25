// Culling a frame's mesh draws on the GPU, as Unity's GPU-driven pipeline does before its indirect
// draws, and drawing runs of them as instances: shaders/mesh_cull.hlsl tests each draw's box
// against the camera's view in a thread of its own, then lists each run's draws in view and writes
// the run's indirect arguments, as many instances as draws in view, which
// shaders/mesh_instanced.hlsl draws by. A run is draws of one mesh and material next to each other
// in the camera's order (mesh_draw_order), drawn as instances of one draw. cull_meshes is its CPU
// twin; box_in_view is what the software device skips draws by.

#pragma once

#include "mesh_shading.hpp"

#include "slash/render/device.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace slash::render::detail {

// mesh_cull.hlsl's threads a group.
inline constexpr std::uint32_t kCullThreads = 64;
// A draw's record: its box's corners' place in clip space (the camera's view projection times
// its transform, column-major), its box's low and high corners, and its indices' first and
// count.
inline constexpr std::uint32_t kCullRecordWords = 24;
// A run's indirect arguments, as SDL's and WebGPU's indexed indirect draws read them: the
// indices, the instances (its draws in view), the first index, the vertex offset and the first
// instance.
inline constexpr std::uint32_t kCullArgsWords = 5;
// A draw's Instance as mesh_instanced.hlsl reads it: model, normal_model, uv1, lightmap_uv and
// previous_model, a float4 of a matrix's columns each, as MeshInstanceUniforms holds them.
inline constexpr std::uint32_t kInstanceFloat4s = 14;
// How far past the view's edges, as a share of w, a box still counts as in view, so the GPU's
// rounding, TAA's and FSR 2's jitter and the CPU twin never cull what shows.
inline constexpr float kCullMargin = 1.0F / 64.0F;

// mesh_cull.hlsl's Params: the draws, the runs, the places in the order, and the pass (0 tests
// the draws, 1 lists the runs').
struct CullUniforms {
    std::array<std::uint32_t, 4> counts{};
};
static_assert(sizeof(CullUniforms) == 16);
static_assert(sizeof(MeshInstanceUniforms) == std::size_t{kInstanceFloat4s} * 16);

// mesh_instanced.hlsl's InstanceBase: where a run's draws in view start in mesh_cull.hlsl's
// outputs, in x.
struct InstanceBaseUniforms {
    std::array<std::uint32_t, 4> base{};
};
static_assert(sizeof(InstanceBaseUniforms) == 16);

// Draws next to each other in the camera's order that a device draws as instances of one draw:
// first is the first's place in the order, count how many.
struct CullRun {
    std::uint32_t first = 0;
    std::uint32_t count = 0;
};

// Splits order into runs: a draw starts a run of its own unless same(the draw before, it).
template <typename Same>
[[nodiscard]] std::vector<CullRun> cull_runs(std::span<const std::size_t> order, const Same& same) {
    std::vector<CullRun> runs;
    for (std::size_t place = 0; place < order.size(); ++place) {
        if (runs.empty() || !same(order[place - 1], order[place])) {
            runs.push_back({.first = static_cast<std::uint32_t>(place), .count = 0});
        }
        ++runs.back().count;
    }
    return runs;
}

// Appends a draw's record to words. A draw with keep set (one whose vertex() moves its corners,
// so its box says nothing) is never culled.
void push_cull_record(std::vector<std::uint32_t>& words, const Mat4& view_projection,
                      const Mat4& transform, const Bounds& bounds, IndexRange range, bool keep);

// Whether the box from low to high, placed in clip space by clip, may show: false only when all
// of its corners are past one edge of the view (or nearer than its near plane, or farther than
// its far one) by more than the margin.
[[nodiscard]] bool box_in_view(const Mat4& clip, const Float3& low, const Float3& high);

// Appends, after the draws' records, each run's first place and count, then the order: what
// mesh_cull.hlsl reads after the records.
void push_cull_runs(std::vector<std::uint32_t>& words, std::span<const CullRun> runs,
                    std::span<const std::size_t> order);

// Where mesh_cull.hlsl's outputs keep the draws' flags, and the runs' draws in view (a run's
// starting at its first place after it), with draws draws and runs runs; and how many words they
// take with places places in the order.
[[nodiscard]] constexpr std::uint32_t cull_flags_at(std::uint32_t runs) {
    return runs * kCullArgsWords;
}
[[nodiscard]] constexpr std::uint32_t cull_list_at(std::uint32_t draws, std::uint32_t runs) {
    return cull_flags_at(runs) + draws;
}
[[nodiscard]] constexpr std::uint32_t cull_output_words(std::uint32_t draws, std::uint32_t runs,
                                                        std::uint32_t places) {
    return cull_list_at(draws, runs) + places;
}

// The CPU twin of mesh_cull.hlsl's two passes over words (draws' records, then what
// push_cull_runs appends): its outputs. With keep, no draw is culled, as a device draws when it
// can't cull.
[[nodiscard]] std::vector<std::uint32_t> cull_meshes(std::span<const std::uint32_t> words,
                                                     std::uint32_t draws, std::uint32_t runs,
                                                     bool keep = false);

// Appends a draw's Instance as mesh_instanced.hlsl reads it.
void push_instance(std::vector<MeshInstanceUniforms>& instances, const Mat4& transform,
                   const Material3D& material, const Mat4& previous);

// The groups a dispatch over count draws takes.
[[nodiscard]] std::uint32_t cull_groups(std::uint32_t count);

} // namespace slash::render::detail
