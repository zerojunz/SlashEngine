#pragma once

#include "mesh_shading.hpp"

#include "slash/render/device.hpp"
#include "slash/render/mesh.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

// What every device shares to upscale a Forward+ frame's 3D with AMD's FidelityFX Super Resolution
// 2.2, as Godot 4.6's scaling_3d mode "fsr2" runs it: the render size, the camera's jitter, the
// passes' uniforms, and the CPU twin of the seven passes of shaders/fsr2_*.hlsl, which the software
// device uses. The 3D draws at the frame's size times the scale with its camera moved across a
// render pixel by the next point of a Halton sequence each frame, as with TAA; FSR 2 then rebuilds
// the frame at its full size from that and the frames before, moved by the prepass's motion
// vectors, and the post passes draw at the full size. Godot's settings: HDR color, reversed depth,
// motion vectors at the render size, no reactive mask or auto exposure of FSR 2's own.
namespace slash::render::detail {

// Whether a scene's frame upscales its 3D with FSR 2: in Forward+ only, at a scale under 1, as
// Godot's (which falls back to bilinear scaling otherwise, done by the canvas renderer here), and
// never for a probe's capture. TAA is off in a frame with FSR 2, as Godot turns it off.
[[nodiscard]] inline bool uses_fsr2(const Scene3D& scene) {
    return scene.method == RenderMethod::ForwardPlus && scene.fsr2.has_value() &&
           scene.fsr2->scale < 1.0F && !scene.capture;
}

// A frame's render size with FSR 2: its size times the scale, rounded down, at least 1, as
// Godot's RendererViewport sizes it.
[[nodiscard]] std::array<std::uint32_t, 2>
fsr2_render_size(const Scene3D& scene, std::uint32_t width, std::uint32_t height);
// ffxFsr2GetJitterPhaseCount, as Godot works it out: 8 times the display's width over the
// render's, squared, rounded down.
[[nodiscard]] std::uint32_t fsr2_phase_count(std::uint32_t render_width,
                                             std::uint32_t display_width);
// The camera's jitter in clip space for its frame-th frame of a sequence of phases points: the
// frame's point of the Halton sequence (bases 2 and 3) over the render size, so at most half a
// render pixel each way, as Godot's RendererSceneCull::render_camera.
[[nodiscard]] std::array<float, 2> camera_jitter(std::uint64_t frame, std::uint32_t phases,
                                                 std::uint32_t width, std::uint32_t height);

// The passes' uniforms, fsr2.hlsl's Fsr2 constant buffer.
struct Fsr2Uniforms {
    std::array<std::int32_t, 4> sizes{}; // the render size, then the display size
    Float4 jitter_scale{};               // the jitter in render pixels, then render over display
    Float4 view_depth{};                 // FSR 2's deviceToViewDepth
    // The exposure (0 for none), the jitter's phases, the frame's index (0 after a reset) and
    // RCAS's sharpness, the lobe's scale (FsrRcasCon).
    Float4 frame{};
    // The shading change mip's size, a pass's step (the reconstruct pass: 0 clears and 1
    // scatters; the sharpening pass: 1 copies), and 1 for an orthogonal projection.
    std::array<std::int32_t, 4> options{};
};
static_assert(sizeof(Fsr2Uniforms) == 80);

// What a frame's uniforms are made from.
struct Fsr2Frame {
    std::uint32_t render_width = 0;
    std::uint32_t render_height = 0;
    std::uint32_t display_width = 0;
    std::uint32_t display_height = 0;
    std::array<float, 2> jitter{}; // in clip space, as camera_jitter
    Mat4 projection{};             // the camera's, without the jitter
    float sharpness = 0.2F;        // Godot's scaling_3d/fsr_sharpness: 0 sharpest, 2 none
    std::uint32_t frame_index = 0; // frames since the target's last reset
    float phases = 0.0F;           // the jitter's phases, as FSR 2 eases them (Fsr2History)
};
[[nodiscard]] Fsr2Uniforms fsr2_uniforms(const Fsr2Frame& frame);

// The shading change mip's size for a render size: 1/32 of it, at least 1 x 1.
[[nodiscard]] std::array<std::uint32_t, 2> fsr2_luma_mip_size(std::uint32_t render_width,
                                                              std::uint32_t render_height);

// What FSR 2 keeps between a target's frames, besides the textures: how many frames since its
// last reset, and the jitter's phases, which follow a change of size one a frame, as AMD's.
struct Fsr2Clock {
    std::uint32_t frames = 0;
    float phases = 0.0F;
    bool primed = false;

    // The next frame's index and phases for a sequence of phases points: a reset starts them
    // again.
    void advance(std::uint32_t target_phases);
};

// What the CPU twin keeps between a target's frames, rows top to bottom: at the display size the
// accumulate pass's color and temporal reactive factor (RGBA, half floats), the locks (two half
// floats) and the luma history (RGBA, 8-bit), and at the render size the dilate pass's motion
// vectors (two half floats). A reset frame starts them at 0.
struct Fsr2History {
    std::vector<float> color;
    std::vector<float> lock_status;
    std::vector<float> luma_history;
    std::vector<float> dilated_motion;
};

// What a frame gives FSR 2, at the render size with rows top to bottom: the world (RGBA, half
// floats), the prepass's depth (reversed: 1 at the near plane) and its motion vectors (two half
// floats a pixel, in texture coordinates).
struct Fsr2Inputs {
    std::span<const float> color;
    std::span<const float> depth;
    std::span<const float> motion;
};

// The CPU twin of the passes: the frame upscaled into out (RGBA at the display size, each value
// rounded to a half float, as the GPU stores it), and the history moved on.
void run_fsr2(const Fsr2Uniforms& uniforms, const Fsr2Inputs& inputs, Fsr2History& history,
              std::span<float> out);

} // namespace slash::render::detail
