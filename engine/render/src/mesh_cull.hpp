// Culling a frame's mesh draws on the GPU, as Unity's GPU-driven pipeline does before its
// indirect draws: shaders/mesh_cull.hlsl tests each draw's box against the camera's view in a
// thread of its own and writes the draw's indirect arguments, with no instances where the box is
// out of view. cull_draws is its CPU twin, which the software and OpenGL devices skip draws by.

#pragma once

#include "mesh_shading.hpp"

#include "slash/render/device.hpp"

#include <array>
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
// A draw's indirect arguments, as SDL's and WebGPU's indexed indirect draws read them: the
// indices, the instances (0 when culled), the first index, the vertex offset and the first
// instance.
inline constexpr std::uint32_t kCullArgsWords = 5;
// How far past the view's edges, as a share of w, a box still counts as in view, so the GPU's
// rounding, TAA's and FSR 2's jitter and the CPU twin never cull what shows.
inline constexpr float kCullMargin = 1.0F / 64.0F;

// mesh_cull.hlsl's Params: the draws.
struct CullUniforms {
    std::array<std::uint32_t, 4> counts{};
};
static_assert(sizeof(CullUniforms) == 16);

// Appends a draw's record to words. A draw with keep set (one whose vertex() moves its corners,
// so its box says nothing) is never culled.
void push_cull_record(std::vector<std::uint32_t>& words, const Mat4& view_projection,
                      const Mat4& transform, const Bounds& bounds, IndexRange range, bool keep);

// Whether the box from low to high, placed in clip space by clip, may show: false only when all
// of its corners are past one edge of the view (or nearer than its near plane, or farther than
// its far one) by more than the margin.
[[nodiscard]] bool box_in_view(const Mat4& clip, const Float3& low, const Float3& high);

// The CPU twin of mesh_cull.hlsl: each record's indirect arguments.
[[nodiscard]] std::vector<std::uint32_t> cull_draws(std::span<const std::uint32_t> records);

// The groups a dispatch over count draws takes.
[[nodiscard]] std::uint32_t cull_groups(std::uint32_t count);

} // namespace slash::render::detail
