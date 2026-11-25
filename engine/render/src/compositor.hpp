#pragma once

#include "slash/core/result.hpp"
#include "slash/render/compositor.hpp"
#include "slash/render/device.hpp"
#include "slash/render/mesh.hpp"
#include "slash/render/shader.hpp"

#include <cstdint>
#include <functional>
#include <span>
#include <vector>

// What every device shares to draw a clustered frame's compositor effects
// (slash/render/compositor.hpp): which effects a frame draws at each point, their checks and
// uniforms, and the CPU twin of a compositor shader's pass, which the software device runs.
//
// At a point with effects, each device copies the world drawn so far into a texture of its own (the
// screen the effect reads) and draws one triangle over the world with the effect's shader, the
// depth and the prepass's normals bound beside it, then goes on drawing over the result.
namespace slash::render::detail {

// Whether a scene's frame draws compositor effects: in the clustered renderer, when it is no
// probe's capture and isn't path traced, and has any.
[[nodiscard]] bool uses_effects(const Scene3D& scene);

// The scene's effects that run at a point, in order.
[[nodiscard]] std::vector<const CompositorEffect3D*> effects_at(const Scene3D& scene,
                                                                EffectCallback point);

// Whether any of the scene's effects reads the prepass's normals and roughness, so the frame
// draws the prepass.
[[nodiscard]] bool effects_need_normals(const Scene3D& scene);

// Checks a scene's effects: at most kMaxCompositorEffects, each with a compositor shader made on
// the device (is_compositor) and textures it has (has_texture) or none.
[[nodiscard]] Result<void> check_effects(const Scene3D& scene,
                                         const std::function<bool(ShaderId)>& is_compositor,
                                         const std::function<bool(TextureId)>& has_texture);

// An effect's uniforms for a screen of width by height pixels, normal_roughness when the prepass
// drew its normals.
[[nodiscard]] CompositorUniforms compositor_uniforms(const Scene3D& scene,
                                                     const CompositorEffect3D& effect,
                                                     std::uint32_t width, std::uint32_t height,
                                                     bool normal_roughness);

// What the CPU twin reads at a pixel: the screen as half floats (RGBA, a row after another), the
// depth, and the prepass's normals and roughness as the GPU stores them (RGBA8 over 255), or empty.
struct CompositorScreen {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::span<const std::array<float, 4>> color;
    std::span<const float> depth;
    std::span<const std::array<float, 4>> normal_roughness;
};

// The texture params' texels at a uv, filtered as their sampler does, by param slot.
using CompositorParamSampler = std::function<std::array<float, 4>(std::uint32_t, float, float)>;

// Runs a compositor shader's CPU module (CompiledShader::cpu_fragment, loaded) over every pixel
// of the screen in 2x2 quads, as GPUs shade pixels, writing each pixel's new color into out (as
// many as the screen has).
void run_compositor(ShaderVm& fragment, const CompositorUniforms& uniforms,
                    const CompositorScreen& screen, const CompositorParamSampler& params,
                    std::span<std::array<float, 4>> out);

} // namespace slash::render::detail
