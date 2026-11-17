// Skinning on the GPU devices: what shaders/skin.hlsl takes (its uniforms and the words of a
// skin's buffers and a frame's poses), shared by the GPU and WebGPU devices.

#pragma once

#include "slash/core/result.hpp"
#include "slash/render/mesh.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace slash::render::detail {

// skin.hlsl's threads a group; a dispatch's index runs id.x + id.y * 65536.
inline constexpr std::uint32_t kSkinThreads = 64;
inline constexpr std::uint32_t kSkinWeightWords = 12;
inline constexpr std::uint32_t kSkinShapeWords = 9;
inline constexpr std::uint32_t kSkinJointWords = 12;

// skin.hlsl's Params.
struct SkinUniforms {
    // The vertices, the joints, the blend shapes, 1 when the shapes at their weights turn normals
    // or tangents.
    std::array<std::uint32_t, 4> counts{};
    // Where the mesh's joints and shape weights start in the poses, 1 when the skin has weights.
    std::array<std::uint32_t, 4> at{};
};
static_assert(sizeof(SkinUniforms) == 32);

// A skin as the device keeps it: the words of its buffers, and which shapes turn normals or
// tangents.
struct SkinWords {
    std::vector<std::uint32_t> rest;    // 15 a vertex, as Vertex3D
    std::vector<std::uint32_t> weights; // kSkinWeightWords a vertex; empty without weights
    std::vector<std::uint32_t> shapes;  // kSkinShapeWords a vertex, each shape's in turn
    std::vector<bool> turns;            // by shape
    std::uint32_t vertex_count = 0;
    std::uint32_t shape_count = 0;
};

// Checks a skin (as many weights as vertices, or none; shapes' offsets as many or none) and lays
// it out.
[[nodiscard]] Result<SkinWords> skin_words(const SkinDesc& desc);

// Appends a mesh's joints and shape weights to poses, and returns the uniforms of its dispatch.
[[nodiscard]] SkinUniforms skin_pose(const SkinWords& skin, std::span<const SkinJoint> joints,
                                     std::span<const float> shape_weights,
                                     std::vector<std::uint32_t>& poses);

// The groups a dispatch over count vertices runs, x then y.
[[nodiscard]] std::array<std::uint32_t, 2> skin_groups(std::uint32_t count);

} // namespace slash::render::detail
