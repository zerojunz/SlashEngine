#pragma once

#include "post_shading.hpp"

#include "slash/render/device.hpp"
#include "slash/render/mesh.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// What every device shares to post-process a Forward+ frame's 3D world, as Godot's Forward+
// renderer does: the uniforms of shaders/forward_post.hlsl, glow's levels, auto exposure's
// luminance textures, and the CPU twins of its passes, which the software device uses. The sky
// and meshes draw linear colors into a texture of half floats; with auto exposure, the luminance
// passes measure its brightness into a 1 x 1 texture of floats; glow blurs it into up to seven
// levels, and the tonemap pass draws it over the frame's target.
namespace slash::render::detail {

// forward_post.hlsl's passes: glow's first level from the world, each next level from the one
// before, then the tonemap pass over the frame's target; and auto exposure's luminance passes,
// the first from the world, each next from the one before, and the last into 1 x 1 moving from
// the last frame's brightness.
enum class ForwardPass : std::uint8_t {
    GlowFirst,
    GlowNext,
    Tonemap,
    LuminanceFirst,
    Luminance,
    LuminanceFinal
};

// The textures each pass binds: its source, glow's seven levels, the color correction, the
// measured brightness (the last frame's, for the last luminance pass) and the luminance texture a
// luminance pass after the first reads. The last two are floats, which are only loaded.
inline constexpr std::uint32_t kForwardPostTextures = 4 + kMaxGlowLevels;
inline constexpr std::uint32_t kBrightnessSlot = 2 + kMaxGlowLevels;
inline constexpr std::uint32_t kLuminanceSourceSlot = 3 + kMaxGlowLevels;

// The options flag for auto exposure, beside 1 for glow and 2 for adjustments, and the darkest
// measured brightness the colors are exposed by.
inline constexpr std::uint32_t kForwardAutoExposure = 16;
inline constexpr float kMinLuminance = 0.0001F;

// The passes' uniforms: b0 in space3.
struct ForwardPostUniforms {
    Float4 target{};      // the pass's target's width and height, then one over them
    Float4 glow{};        // strength, bloom, HDR threshold, HDR scale
    Float4 glow_first{};  // exposure, luminance cap, level 0's width and height
    Float4 levels_low{};  // levels 0 to 3's weights, 0 for those not added
    Float4 levels_high{}; // levels 4 to 6's weights, then the intensity
    Float4 tonemap{};     // exposure, white, the 3D LUT's size
    Float4 tonemapper{};  // the tonemapper's parameters
    Float4 adjust{};      // brightness, contrast, saturation
    // Auto exposure's scale, the brightness's least and most, and how far it moves this frame
    Float4 exposure{};
    Float4 source{}; // a luminance pass's source's width and height
    // The pass, the tonemapper, 1 for glow | 2 for adjustments | the correction times 4 |
    // kForwardAutoExposure, and the glow blend mode.
    std::array<std::uint32_t, 4> options{};
};
static_assert(sizeof(ForwardPostUniforms) == 176);

// A texture of half floats, RGBA, its rows top to bottom, each value already rounded to half
// precision, as the GPU devices keep the world and glow's levels.
struct HdrImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<float> pixels;
};

// value rounded to the nearest half float, ties to even, as a GPU stores it.
[[nodiscard]] float round_half(float value);

// A frame's clear color in the world's texture: linear, by the exact sRGB curve, so a frame with
// nothing over it comes back out of the tonemap pass the color it was.
[[nodiscard]] Float4 forward_clear(const Color& clear);

// Glow's levels for a frame of width x height, as Godot's RenderSceneBuffersRD sizes its second
// blur texture's mipmaps: half the frame's size, then half again for each next one, at least 1 x 1,
// and how many of them the scene's glow draws (to the last level of more than 0.01, at least one).
struct GlowPlan {
    std::uint32_t count = 0;
    std::array<PostSize, kMaxGlowLevels> sizes{};
};
[[nodiscard]] GlowPlan forward_glow_plan(const PostProcess& post, std::uint32_t width,
                                         std::uint32_t height);

// Auto exposure's luminance textures for a frame of width x height, as Godot's
// Luminance::LuminanceBuffers: the frame's size divided by 8, rounded down and at least 1, again
// and again to 1 x 1. Each luminance pass draws the next.
[[nodiscard]] std::vector<PostSize> luminance_plan(std::uint32_t width, std::uint32_t height);

// A pass's uniforms, for a target of target_width x target_height in a frame of frame_width x
// frame_height. The tonemap pass reads a color correction texture of lut_width x lut_height, when
// the scene has one (0 x 0 for none).
[[nodiscard]] ForwardPostUniforms
forward_post_uniforms(const PostProcess& post, ForwardPass pass, std::uint32_t target_width,
                      std::uint32_t target_height, std::uint32_t frame_width,
                      std::uint32_t frame_height, std::uint32_t lut_width = 0,
                      std::uint32_t lut_height = 0);
// A luminance pass's uniforms, from a source of source's size into a target of target's.
[[nodiscard]] ForwardPostUniforms luminance_uniforms(const PostProcess& post, ForwardPass pass,
                                                     PostSize source, PostSize target);

// A texture of one float a texel, its rows top to bottom, as auto exposure's luminance textures.
struct LuminanceImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<float> values;
};

// The CPU twins of forward_post.hlsl's passes. A glow pass draws over all of target from source,
// rounding to half floats. The tonemap pass draws over all of target from the world, glow's levels
// (as many as the plan computed, or none) and the color correction texture (empty when the
// uniforms don't use it), rounding to bytes as a GPU writes them. Both expose by luminance, the
// measured brightness, when the uniforms ask for auto exposure. A luminance pass draws over all
// of target from source: the world for the first, the luminance texture before for the rest, and
// for the last also previous, the last frame's brightness.
void run_forward_glow_pass(const ForwardPostUniforms& uniforms, const HdrImage& source,
                           HdrImage& target, float luminance = 1.0F);
void run_forward_tonemap_pass(const ForwardPostUniforms& uniforms, const HdrImage& world,
                              std::span<const HdrImage> levels, const Image& lut, Image& target,
                              float luminance = 1.0F);
void run_luminance_pass(const ForwardPostUniforms& uniforms, const HdrImage& world,
                        LuminanceImage& target);
void run_luminance_pass(const ForwardPostUniforms& uniforms, const LuminanceImage& source,
                        float previous, LuminanceImage& target);

} // namespace slash::render::detail
