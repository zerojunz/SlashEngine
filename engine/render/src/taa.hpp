#pragma once

#include "mesh_shading.hpp"
#include "post_shading.hpp"

#include "slash/render/device.hpp"
#include "slash/render/mesh.hpp"

#include <array>
#include <cstdint>
#include <span>

// What every device shares for a Forward+ frame's temporal antialiasing, as Godot 4.6's use_taa:
// the camera's jitter, the motion vectors the prepass writes, and the resolve pass of
// shaders/taa.hlsl, with its CPU twin, which the software device uses. Each frame of a target is
// drawn with its projection moved across a pixel by the next of 16 points of a Halton sequence
// (Godot's RendererSceneCull::render_camera), the prepass writes how far each opaque pixel moved
// since the frame before in texture coordinates (Godot's MOTION_VECTORS), and the resolve blends
// the frame with the ones before it, moved by those motion vectors and clipped to the colors around
// each pixel (Godot's taa_resolve.glsl, from Spartan Engine's TAA). Godot's compute shader runs as
// a pass that draws one triangle over its target; it loads texels and filters in code, keeping
// every load inside its texture, so every device draws the same values.
namespace slash::render::detail {

// Whether a scene's frame uses TAA: in Forward+ only, as Godot's, and never a probe's capture.
[[nodiscard]] inline bool uses_taa(const Scene3D& scene) {
    return scene.method == RenderMethod::ForwardPlus && scene.use_taa && !scene.capture;
}

// The jitter's points, as Godot's default for TAA.
inline constexpr std::uint32_t kTaaPhases = 16;
// The textures the resolve pass reads, as TaaInputs lists them.
inline constexpr std::uint32_t kTaaSlots = 5;

// Godot's get_halton_value: the index's point of the Halton sequence of base, from -1 to 1.
[[nodiscard]] float halton(std::uint32_t index, std::uint32_t base);
// The jitter of a target's frame-th frame with TAA, in clip space: its point of the sequence (bases
// 2 and 3) over the target's size, so at most half a pixel each way.
[[nodiscard]] std::array<float, 2> taa_jitter(std::uint64_t frame, std::uint32_t width,
                                              std::uint32_t height);
// projection moved by jitter in clip space, as Godot's Projection::add_jitter_offset on the
// correction it applies to its camera's projection: jitter times w added to x and y.
[[nodiscard]] Mat4 jittered(const Mat4& projection, const std::array<float, 2>& jitter);

// The mesh passes' camera for a frame with TAA: the scene's, jittered, with the frame before's
// world to clip space (previous, without its jitter) for the motion vectors.
[[nodiscard]] MeshCameraUniforms
taa_camera_uniforms(const Scene3D& scene, const std::array<float, 2>& jitter, const Mat4& previous);

// The motion vector of a pixel, as Godot's scene shader works it out from where its surface is in
// clip space now and was in the frame before, each without its frame's jitter: the move from now
// to before in texture coordinates (x right, y down). It is the prepass's second target.
[[nodiscard]] std::array<float, 2> motion_vector(const Float4& clip, const Float4& previous);

// taa.hlsl's uniforms: b0 in space3, Godot's push constants.
struct TaaUniforms {
    std::array<std::uint32_t, 4> size{}; // the frame's width and height
    // How many texels the motion may change by and the history still count (2.5), and the
    // variance clipping's box size for the frame's height, as Godot's TAA::resolve sets them.
    Float4 params{};
};
static_assert(sizeof(TaaUniforms) == 32);
[[nodiscard]] TaaUniforms taa_uniforms(std::uint32_t width, std::uint32_t height);

// What the resolve reads, each width x height with rows top to bottom: the frame's color (RGBA,
// half floats), the prepass's depth (reversed: 1 at the near plane), its motion vectors and the
// frame before's (two half floats a pixel), and the history, the frame before's result (RGBA).
struct TaaInputs {
    std::span<const float> color;
    std::span<const float> depth;
    std::span<const float> velocity;
    std::span<const float> last_velocity;
    std::span<const float> history;
};
// The CPU twin of the resolve pass: the frame blended with the history into out (RGBA, each value
// rounded to a half float, as the GPU stores it).
void run_taa(const TaaUniforms& uniforms, const TaaInputs& inputs, std::span<float> out);

} // namespace slash::render::detail
