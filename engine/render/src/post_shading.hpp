#pragma once

#include "checks.hpp"

#include "slash/core/result.hpp"
#include "slash/render/device.hpp"
#include "slash/render/mesh.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

// What every device shares to post-process a frame's 3D world (PostProcess, in mesh.hpp): the
// uniforms of shaders/post.hlsl, the sizes of glow's textures, and the CPU twins of its passes and
// of tonemap.hlsl, which the software device and shade_mesh use.
namespace slash::render::detail {

using Float4 = std::array<float, 4>;
using Float3 = std::array<float, 3>;

// Glow's textures, each half the size of the last, as Godot's Compatibility renderer has them.
inline constexpr std::size_t kGlowLevels = 4;

// post.hlsl's passes: glow's filter into its first texture, downsampling into each next one,
// upsampling back into each one before, then the post pass over the frame's target.
enum class PostPass : std::uint8_t { GlowFilter, GlowDownsample, GlowUpsample, Post };

// How the post pass reads a color correction texture.
inline constexpr std::uint32_t kCorrection1D = 1;
inline constexpr std::uint32_t kCorrection3D = 2;

// The post passes' uniforms: b0 in space3.
struct PostUniforms {
    Float4 pixel{};  // one over the target's size, twice
    Float4 glow{};   // intensity, bloom, HDR threshold, HDR scale
    Float4 grade{};  // white, the luminance cap, the 3D LUT's size
    Float4 adjust{}; // brightness, contrast, saturation
    // The pass, the tonemapper, 1 for glow | 2 for adjustments | the correction times 4, 0.
    std::array<std::uint32_t, 4> options{};
};
static_assert(sizeof(PostUniforms) == 80);

// Whether a Compatibility scene draws through the post passes: with glow or adjustments.
[[nodiscard]] bool post_processes(const PostProcess& post);

// How a scene's sky and meshes finish their colors, the w of their tonemap uniforms: 1 exposed,
// tonemapped and as sRGB; 0 as sRGB for post.hlsl to tonemap; 2 linear for forward_post.hlsl,
// which draws every Forward+ frame.
[[nodiscard]] float color_stage(const Scene3D& scene);

struct PostSize {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};
// Glow's textures for a target of width x height: half its size, then half again for each next
// one, at least 4 x 4, as Godot's RenderSceneBuffersGLES3::check_glow_buffers sizes them.
[[nodiscard]] std::array<PostSize, kGlowLevels> glow_sizes(std::uint32_t width,
                                                           std::uint32_t height);

// How the post pass reads a color correction texture of this size: kCorrection1D for one texel
// tall, kCorrection3D for as wide as its height squared, 0 for neither.
[[nodiscard]] std::uint32_t correction_kind(std::uint32_t width, std::uint32_t height);

// A pass's uniforms, for a target of width x height. The post pass reads a color correction
// texture of lut_width x lut_height, when the scene has one (0 x 0 for none).
[[nodiscard]] PostUniforms post_uniforms(const PostProcess& post, PostPass pass,
                                         std::uint32_t width, std::uint32_t height,
                                         std::uint32_t lut_width = 0, std::uint32_t lut_height = 0);

// tonemap.hlsl's helpers, the same bits on every platform.
[[nodiscard]] Float3 srgb_to_linear(const Float3& color);
[[nodiscard]] Float3 linear_to_srgb(const Float3& color);
// Linear colors in, linear colors out, as apply_tonemapping.
[[nodiscard]] Float3 tonemap(const Float3& color, float white, Tonemapper tonemapper);

// The CPU twin of post.hlsl: draws the pass over all of target from source, and for the post pass
// from glow and the color correction texture too (either empty when the uniforms don't use it).
// Colors are rounded to bytes as a GPU writes them.
void run_post_pass(const PostUniforms& uniforms, const Image& source, const Image& glow,
                   const Image& lut, Image& target);

// Checks a scene's post-processing: a tonemapper and glow blend mode that are one, finite
// numbers, glow levels of at least 0, and a color correction texture the device has (known) and
// that is a lookup texture (size_of gives its width and height).
template <typename Known, typename SizeOf>
[[nodiscard]] Result<void> check_post(const PostProcess& post, const Known& known,
                                      const SizeOf& size_of) {
    if (static_cast<std::size_t>(post.tonemapper) >= kTonemapperNames.size()) {
        return make_error(ErrorCode::InvalidArgument, "a scene's tonemapper is one of five");
    }
    if (static_cast<std::size_t>(post.glow_blend_mode) >= kGlowBlendModeNames.size()) {
        return make_error(ErrorCode::InvalidArgument, "a scene's glow blend mode is one of five");
    }
    for (const float value :
         {post.exposure, post.white, post.agx_contrast, post.glow_intensity, post.glow_strength,
          post.glow_mix, post.glow_bloom, post.glow_hdr_threshold, post.glow_hdr_scale,
          post.glow_hdr_luminance_cap, post.brightness, post.contrast, post.saturation}) {
        if (!std::isfinite(value)) {
            return make_error(ErrorCode::InvalidArgument,
                              "a scene's tonemap, glow and adjustment numbers are finite");
        }
    }
    for (const float level : post.glow_levels) {
        if (!std::isfinite(level) || level < 0.0F) {
            return make_error(ErrorCode::InvalidArgument,
                              "a scene's glow levels are finite and at least 0");
        }
    }
    for (const float value : {post.auto_exposure_scale, post.auto_exposure_min,
                              post.auto_exposure_max, post.auto_exposure_adjust}) {
        if (!std::isfinite(value)) {
            return make_error(ErrorCode::InvalidArgument,
                              "a scene's auto exposure numbers are finite");
        }
    }
    if (post.color_correction == TextureId::None) {
        return {};
    }
    if (!known(post.color_correction)) {
        return unknown_texture(post.color_correction);
    }
    const auto [width, height] = size_of(post.color_correction);
    if (correction_kind(width, height) == 0) {
        return make_error(ErrorCode::InvalidArgument,
                          "a color correction texture is 1 texel tall, or as wide as its height "
                          "squared, not " +
                              std::to_string(width) + "x" + std::to_string(height));
    }
    return {};
}

} // namespace slash::render::detail
