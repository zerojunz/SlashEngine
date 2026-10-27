#pragma once

#include "fsr2.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

// What every device shares to upscale a Forward+ frame's 3D with AMD's FidelityFX Super Resolution
// 3.1's upscaler (FidelityFX SDK 1.1.4), the scaling_3d mode "fsr3": the passes' uniforms, the
// sizes of their textures and the CPU twin of shaders/fsr3_*.hlsl, which the software device uses.
// It takes what FSR 2 takes, from the same prepass at the same render size with the same jittered
// camera, with the settings Godot gives FSR 2, and its last pass is FSR 2's sharpening pass, which
// FSR 3's matches. In place of FSR 2's depth clip and locks it measures how much each pixel's
// shading changed since the frame before (a pyramid of luma differences at half the render size),
// counts the frames each render pixel has gathered, and keeps four frames of luma to tell a
// flicker from a change.
namespace slash::render::detail {

// Whether a scene's frame upscales its 3D with FSR 3 rather than FSR 2.
[[nodiscard]] inline bool uses_fsr3(const Scene3D& scene) {
    return uses_fsr2(scene) && scene.fsr2.has_value() && scene.fsr2->fsr3;
}

// The passes' uniforms, fsr3.hlsl's Fsr3 constant buffer: FSR 2's, which its sharpening pass
// reads, then the frame before's jitter in render pixels.
struct Fsr3Uniforms {
    Fsr2Uniforms fsr2{};
    Float4 previous{};
};
static_assert(sizeof(Fsr3Uniforms) == 160);

// A frame's uniforms: FSR 2's for the frame, with options.x and options.y 0 (the reduce pass
// sets the level it draws in x), and the frame before's jitter in clip space, as camera_jitter.
[[nodiscard]] Fsr3Uniforms fsr3_uniforms(const Fsr2Frame& frame,
                                         const std::array<float, 2>& previous_jitter);

// The size of the textures at half the render size (the farthest depths, the shading change and
// the shading change pyramid's first level): the render size halved, at least 1 x 1.
[[nodiscard]] std::array<std::uint32_t, 2> fsr3_half_size(std::uint32_t render_width,
                                                          std::uint32_t render_height);
// The levels of the shading change pyramid the shading change pass reads.
inline constexpr std::uint32_t kFsr3ShadingLevels = 3;
// A level's size: the half size halved level times, at least 1 x 1.
[[nodiscard]] std::array<std::uint32_t, 2>
fsr3_level_size(std::uint32_t render_width, std::uint32_t render_height, std::uint32_t level);

// What FSR 3 keeps between a target's frames, rows top to bottom: at the display size the
// accumulate pass's color and lock (RGBA, half floats), and at the render size the frames each
// pixel gathered (8-bit), the luma (a half float) and the last four lumas (RGBA, half floats). A
// reset frame starts them at 0.
struct Fsr3History {
    std::vector<float> color;
    std::vector<float> accumulation;
    std::vector<float> luma;
    std::vector<float> luma_history;
};

// The CPU twin of the passes: the frame (Fsr2Inputs, as FSR 2 takes it) upscaled into out (RGBA
// at the display size, each value rounded to a half float, as the GPU stores it), and the history
// moved on.
void run_fsr3(const Fsr3Uniforms& uniforms, const Fsr2Inputs& inputs, Fsr3History& history,
              std::span<float> out);

} // namespace slash::render::detail
