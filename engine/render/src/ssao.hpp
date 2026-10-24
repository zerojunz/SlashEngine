#pragma once

#include "post_shading.hpp"

#include "slash/render/mesh.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

// What every device shares to work out a Forward+ frame's screen-space ambient occlusion, as
// Godot 4.6's SSEffects does with Intel's ASSAO: the passes of shaders/ssao.hlsl, the textures
// they draw, their uniforms, and their CPU twins, which the software device uses. A prepass draws
// the opaque meshes' depth and view-space normals; the depth is made linear and split into four
// slices of every other pixel (deinterleaved), each at up to four levels; each slice's occlusion is
// gathered from its depths, blurred along edges and put back together at the frame's size, which
// the meshes' color pass reads.
//
// Godot's compute shaders sample with its hardware samplers; these passes load texels and filter
// them in code, so every device draws the same values: a nearest sample's texel and a gather's
// footprint are worked out in integers (as Godot's would be for a frame of even width and
// height), and bilinear samples are weighed in floats.
namespace slash::render::detail {

// Whether a scene's frame draws SSAO: in Forward+ only, as Godot's.
[[nodiscard]] inline bool uses_ssao(const Scene3D& scene) {
    return scene.method == RenderMethod::ForwardPlus && scene.ssao.enabled;
}

// ssao.hlsl's passes.
enum class SsaoPass : std::uint8_t {
    Depth,              // a slice's linear depth at a level, as ss_effects_downsample.glsl
    Gather,             // occlusion and edges, as ssao.glsl
    GatherBase,         // ultra's first pass: occlusion from five samples, and their weight
    GatherAdaptive,     // ultra's second: up to 32 samples where the importance map asks
    ImportanceGenerate, // the importance map, as ssao_importance_map.glsl's GENERATE_MAP
    ImportanceA,        // its PROCESS_MAPA blur
    ImportanceB,        // and PROCESS_MAPB
    ImportanceRows,     // PROCESS_MAPB's load counter: the sum of every third texel, by row
    ImportanceSum,      // and of the rows
    BlurPlain,          // ssao_blur.glsl's MODE_NON_SMART
    BlurSmart,          // MODE_SMART
    BlurWide,           // MODE_WIDE
    InterleavePlain,    // ssao_interleave.glsl's default mode
    InterleaveSmart,    // MODE_SMART
    InterleaveHalf,     // MODE_HALF
};

// The textures SSAO draws, by index: the linear depth's four slices at up to four levels (slice
// * 4 + level), the occlusion's four slices and the four it blurs back and forth with, the
// importance map and the one its blur draws, the load counter's rows and sum, and the result.
inline constexpr std::uint32_t kSsaoDepthLevels = 4;
inline constexpr std::uint32_t kSsaoAo = 16;   // + slice
inline constexpr std::uint32_t kSsaoPong = 20; // + slice
inline constexpr std::uint32_t kSsaoImportance = 24;
inline constexpr std::uint32_t kSsaoImportancePong = 25;
inline constexpr std::uint32_t kSsaoRows = 26;
inline constexpr std::uint32_t kSsaoSum = 27;
inline constexpr std::uint32_t kSsaoFinal = 28;
inline constexpr std::uint32_t kSsaoTextures = 29;
// What else a pass reads: the prepass's depth and normal-roughness buffers, or nothing.
inline constexpr std::uint32_t kSsaoDepthBuffer = 29;
inline constexpr std::uint32_t kSsaoNormals = 30;
inline constexpr std::uint32_t kSsaoUnbound = 31;

[[nodiscard]] constexpr std::uint32_t ssao_depth(std::uint32_t slice, std::uint32_t level) {
    return (slice * kSsaoDepthLevels) + level;
}

// The texture formats: the depth's half floats, the occlusion's two bytes (the occlusion and its
// packed edges, or a base pass's occlusion and weight), the importance maps' and the result's
// byte, and the load counter's floats.
enum class SsaoFormat : std::uint8_t { R16F, RG8, R8, R32F };
[[nodiscard]] SsaoFormat ssao_format(std::uint32_t texture);
// How many channels a texture of the format has.
[[nodiscard]] std::uint32_t ssao_channels(SsaoFormat format);

// ssao.hlsl's texture slots: a slice's four depth levels (the depth buffer, for the depth pass),
// the normal-roughness buffer, an adaptive gather's base pass, the importance map, the load
// counter (its rows, for the sum pass), and four slices of occlusion. The depth buffer, its
// levels and the load counter are floats, which are only loaded, since WebGPU filters no floats.
inline constexpr std::uint32_t kSsaoSlots = 12;

// One of SSAO's passes: what it draws, from what (a texture index for each slot, or
// kSsaoUnbound) and with which slice and level.
struct SsaoStep {
    SsaoPass pass = SsaoPass::Depth;
    std::uint32_t slice = 0;
    std::uint32_t level = 0;
    std::uint32_t target = 0;
    std::array<std::uint32_t, kSsaoSlots> slots{};
};

// A frame's SSAO: the size of each texture it draws (0 x 0 for those it doesn't), the levels of
// depth it makes, and its passes in order, as Godot's downsample_depth and generate_ssao.
struct SsaoPlan {
    std::array<PostSize, kSsaoTextures> sizes{};
    std::uint32_t levels = 1;
    std::vector<SsaoStep> steps;
};
[[nodiscard]] SsaoPlan ssao_plan(const SsaoSettings& settings, std::uint32_t width,
                                 std::uint32_t height);

// The passes' uniforms: b0 in space3. Godot's push constants for each of its shaders, gathered.
struct SsaoUniforms {
    // The pass, Godot's shader quality (one less than the setting, at least 0), the slice and the
    // depth level.
    std::array<std::uint32_t, 4> options{};
    // The frame's width and height, and the depth's at level 0.
    std::array<std::uint32_t, 4> frame{};
    // The occlusion slices' width and height, and the importance map's.
    std::array<std::uint32_t, 4> buffers{};
    // The depth pass's step between source pixels (2, or 4 at half size), the gather's size
    // multiplier (1, or 2 at half size), the interleave's size modifier (2, or 4), and the levels
    // of depth made (1, or 4 from high quality on).
    std::array<std::uint32_t, 4> layout{};
    Float4 pixel{};       // half_screen_pixel_size, and it times 0.75 (half_screen_pixel_size_x025)
    Float4 ndc{};         // NDC_to_view_mul, then NDC_to_view_add
    Float4 shadow{};      // radius, intensity, shadow_power, shadow_clamp
    Float4 fade{};        // fade_out_mul, fade_out_add, horizon_angle_threshold,
                          // inv_radius_near_limit
    Float4 adaptive{};    // neg_inv_radius, load_counter_avg_div, adaptive_sample_limit,
                          // detail_intensity
    Float4 pass_offset{}; // pass_coord_offset, then pass_uv_offset
    // The depth's z_near and z_far (for a perspective projection, its linearizing multiplier and
    // addend), 1 for an orthogonal projection, and the mipmaps' radius_sq.
    Float4 depth{};
    Float4 blur{};       // edge_sharpness (1 - sharpness), then the blur's half_screen_pixel_size
    Float4 importance{}; // the importance map's half_screen_pixel_size, intensity and power
    std::array<Float4, 20> rotation{}; // ss_effects.cpp's rotation_matrices
};
static_assert(sizeof(SsaoUniforms) == 528);

// A pass's uniforms, for a frame of width x height seen through projection (the Scene3D's).
[[nodiscard]] SsaoUniforms ssao_uniforms(const Ssao3D& ssao, const SsaoStep& step,
                                         const std::array<float, 16>& projection,
                                         std::uint32_t width, std::uint32_t height);

// A texture of floats, its rows top to bottom, channels values a texel, each rounded as the GPU
// stores it in the texture's format: half floats for depth, bytes for occlusion and importance.
struct SsaoImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t channels = 1;
    std::vector<float> values;
};

// The prepass's buffers: depth (a float a pixel, reversed: 1 at the near plane) and normals
// (RGBA bytes, as the GPU writes them, scaled to 0 to 1).
struct SsaoInputs {
    std::span<const float> depth;
    const SsaoImage* normals = nullptr;
};

// The CPU twin of one pass: draws over all of target from textures (indexed as the plan's, the
// ones the pass doesn't read empty) and inputs, rounding as the GPU stores each format.
void run_ssao_pass(const SsaoUniforms& uniforms, const SsaoStep& step,
                   std::span<const SsaoImage> textures, const SsaoInputs& inputs,
                   SsaoImage& target);
// Every pass of the plan, with textures made at their sizes, and the result: the occlusion at
// each pixel of the frame, from 0 (fully occluded) to 1.
[[nodiscard]] SsaoImage run_ssao(const Ssao3D& ssao, const std::array<float, 16>& projection,
                                 std::uint32_t width, std::uint32_t height,
                                 const SsaoInputs& inputs);

} // namespace slash::render::detail
