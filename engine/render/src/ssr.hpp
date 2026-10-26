#pragma once

#include "post_shading.hpp"
#include "ssao.hpp"
#include "taa.hpp"

#include "slash/render/mesh.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

// What every device shares to work out a Forward+ frame's screen-space reflections, as Godot 4.6's
// SSEffects does: the passes of shaders/ssr.hlsl, the textures they draw, their uniforms, and
// their CPU twins, which the software device uses. From the prepass's depth (halved first at half
// size, keeping each 2x2 pixels' nearest) a pyramid of the nearest depths is made (Hi-Z). Each
// pixel's reflected ray steps across the pyramid's cells, down a level where it might hit and up
// one where it can't, and where it lands reads the frame before, moved to where it is now; rougher
// pixels pick blurrier levels of a pyramid of the reflections, which the meshes' color pass puts
// over the sky's.
//
// Godot's compute shaders sample with its hardware samplers and read past the screen's edges as
// Vulkan does; these passes load texels and filter them in code, and keep every load inside its
// texture, so every device draws the same values.
namespace slash::render::detail {

// Whether a scene's frame draws SSR: in Forward+ only, as Godot's.
[[nodiscard]] inline bool uses_ssr(const Scene3D& scene) {
    return scene.method == RenderMethod::ForwardPlus && scene.ssr.enabled;
}

// Whether a scene's frame draws Forward+'s normal-roughness prepass, which SSAO and SSR read, and
// with TAA its motion vectors.
[[nodiscard]] inline bool uses_prepass(const Scene3D& scene) {
    return uses_ssao(scene) || uses_ssr(scene) || uses_taa(scene);
}

// ssr.hlsl's passes.
enum class SsrPass : std::uint8_t {
    DownsampleDepth,   // half size: Hi-Z's first level, the nearest depth of each 2x2 pixels, as
                       // screen_space_reflection_downsample.glsl
    DownsampleNormals, // and the normal and roughness of the pixel that was nearest
    CopyDepth,         // full size: Hi-Z's first level, the depth buffer
    Hiz,               // a level of Hi-Z from the one before, as screen_space_reflection_hiz.glsl
    CopyLevels,        // copies a pyramid's levels into one texture with mipmaps (GPU devices)
    Trace,             // the reflection and its mip level, as screen_space_reflection.glsl
    FilterRows,        // a level of the reflections, its 7 taps along rows, as
                       // screen_space_reflection_filter.glsl's apply_gaus_horz
    FilterColumns,     // and along columns, as its apply_gaus_vert
    Resolve,           // half size: the reflections at the frame's size, as
                       // screen_space_reflection_resolve.glsl
    Mip,               // full size: each pixel's reflection at its mip level, as the scene shader
                       // reads it with RESOLVE_SSR
    LastFrame,         // the frame's color, kept for the next frame's reflections, as
                       // copy_internal_texture_to_last_frame
};

// The textures SSR draws, by index: Hi-Z's levels, the reflections' levels, the filter's rows at
// each level (from level 1), the normals and roughness at half size, the mip levels and the result.
inline constexpr std::uint32_t kSsrMaxLevels = 16;
inline constexpr std::uint32_t kSsrHiz = 0;    // + level
inline constexpr std::uint32_t kSsrColor = 16; // + level
inline constexpr std::uint32_t kSsrRows = 32;  // + level
inline constexpr std::uint32_t kSsrNormalsHalf = 48;
inline constexpr std::uint32_t kSsrMipLevel = 49;
inline constexpr std::uint32_t kSsrFinal = 50;
inline constexpr std::uint32_t kSsrTextures = 51;
// What else a pass reads or draws: the prepass's depth and normal-roughness buffers, Hi-Z's and
// the reflections' levels in one texture with mipmaps each, the frame before (kept between
// frames, at SSR's size), the frame's color, or nothing.
inline constexpr std::uint32_t kSsrDepthBuffer = 51;
inline constexpr std::uint32_t kSsrNormals = 52;
inline constexpr std::uint32_t kSsrHizMips = 53;
inline constexpr std::uint32_t kSsrColorMips = 54;
inline constexpr std::uint32_t kSsrLastFrame = 55;
inline constexpr std::uint32_t kSsrWorld = 56;
inline constexpr std::uint32_t kSsrUnbound = 57;

// The texture formats: Hi-Z's floats, the normals' bytes, the reflections' half floats, the
// filter rows' floats (as Godot's shared memory holds them) and the mip levels' byte.
enum class SsrFormat : std::uint8_t { R32F, RGBA8, RGBA16F, RGBA32F, R8 };
[[nodiscard]] SsrFormat ssr_format(std::uint32_t texture);
// How many channels a texture of the format has.
[[nodiscard]] std::uint32_t ssr_channels(SsrFormat format);

// ssr.hlsl's texture slots: a source (the depth buffer, a level of Hi-Z, of the reflections or of
// the filter's rows, or the frame's color), the normal-roughness buffer (at SSR's size for the
// trace), Hi-Z's mipmaps, the frame before, the reflections' mipmaps, the mip levels, and Hi-Z's
// first level and the normals at half size, for the resolve. All are loaded, never sampled, since
// WebGPU filters no 32-bit floats.
inline constexpr std::uint32_t kSsrSlots = 8;

// One of SSR's passes: what it draws (a texture index, or for CopyLevels kSsrHizMips or
// kSsrColorMips), from what (a texture index for each slot, or kSsrUnbound) and at which level.
struct SsrStep {
    SsrPass pass = SsrPass::CopyDepth;
    std::uint32_t level = 0;
    std::uint32_t target = 0;
    std::array<std::uint32_t, kSsrSlots> slots{};
};

// A frame's SSR: the size of each texture it draws (0 x 0 for those it doesn't), its size (the
// frame's, or half), the levels of its pyramids, and its passes in order, as Godot's
// ssr_allocate_buffers and screen_space_reflection. The frame before is kept at its size.
struct SsrPlan {
    std::array<PostSize, kSsrTextures> sizes{};
    PostSize size{};
    std::uint32_t levels = 1;
    std::vector<SsrStep> steps;
};
[[nodiscard]] SsrPlan ssr_plan(const SsrSettings& settings, std::uint32_t width,
                               std::uint32_t height);
// The pass that keeps the frame's color for the next frame, drawn after the meshes into the frame
// before (kSsrLastFrame): copied at full size, averaged in 2x2 pixels at half.
[[nodiscard]] SsrStep ssr_last_frame_step();

// A camera as the passes see it: Godot's corrected projection (rows top to bottom, as a texture
// coordinate's y runs) and the world to the camera's space.
struct SsrCamera {
    Mat4 projection{};
    Mat4 camera{};
};
[[nodiscard]] SsrCamera ssr_camera(const Scene3D& scene);

// The passes' uniforms: b0 in space3. Godot's push constants and scene data for its shaders,
// gathered.
struct SsrUniforms {
    // The pass, the level it draws, flags (kSsrOddWidth and kSsrOddHeight for a source of odd
    // width or height, kSsrOrthogonal) and the levels of the pyramids.
    std::array<std::uint32_t, 4> options{};
    // The pass's screen size (the level's for Hi-Z and the filter, SSR's for the trace, the
    // frame's for the resolve, the mip pass and the frame before), then its source's size.
    std::array<std::uint32_t, 4> sizes{};
    // The frame's width and height, then SSR's.
    std::array<std::uint32_t, 4> frame{};
    // The ray's most steps.
    std::array<std::uint32_t, 4> steps{};
    Float4 fade{}; // distance_fade (fade out), curve_fade_in, depth_tolerance
    Mat4 projection{};
    Mat4 inv_projection{};
    // The frame's clip space to the frame before's.
    Mat4 reprojection{};
};
static_assert(sizeof(SsrUniforms) == 272);
inline constexpr std::uint32_t kSsrOddWidth = 1;
inline constexpr std::uint32_t kSsrOddHeight = 2;
inline constexpr std::uint32_t kSsrOrthogonal = 4;

// A pass's uniforms, for a frame of width x height seen as camera, whose frame before was seen
// as last.
[[nodiscard]] SsrUniforms ssr_uniforms(const Ssr3D& ssr, const SsrStep& step,
                                       const SsrCamera& camera, const SsrCamera& last,
                                       std::uint32_t width, std::uint32_t height);

// A texture of floats, its rows top to bottom, channels values a texel, each rounded as the GPU
// stores it in the texture's format.
struct SsrImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t channels = 1;
    std::vector<float> values;
};

// The prepass's buffers: depth (a float a pixel, reversed: 1 at the near plane) and normals
// (RGBA bytes, as the GPU writes them, scaled to 0 to 1); the frame before, at SSR's size; and
// for the last frame's pass, the frame's color (four floats a pixel).
struct SsrInputs {
    std::span<const float> depth;
    const SsrImage* normals = nullptr;
    const SsrImage* last_frame = nullptr;
    const SsrImage* world = nullptr;
};

// The CPU twin of one pass: draws over all of target from textures (indexed as the plan's, the
// ones the pass doesn't read empty) and inputs, rounding as the GPU stores each format.
void run_ssr_pass(const SsrUniforms& uniforms, const SsrStep& step,
                  std::span<const SsrImage> textures, const SsrInputs& inputs, SsrImage& target);
// Every pass of the plan, with textures made at their sizes, and the result: each pixel's
// reflection at the frame's size, in RGB, and how much of the sky's it replaces, in A.
[[nodiscard]] SsrImage run_ssr(const Ssr3D& ssr, const SsrCamera& camera, const SsrCamera& last,
                               std::uint32_t width, std::uint32_t height, const SsrInputs& inputs);
// The frame before for the next frame: world (four floats a pixel, width x height) as the last
// frame's pass keeps it, at SSR's size for settings.
[[nodiscard]] SsrImage ssr_last_frame(const SsrSettings& settings, std::uint32_t width,
                                      std::uint32_t height, std::span<const float> world);

} // namespace slash::render::detail
