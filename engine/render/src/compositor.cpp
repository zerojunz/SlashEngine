#include "compositor.hpp"

#include "mesh_shading.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace slash::render::detail {

bool uses_effects(const Scene3D& scene) {
    return scene.method == RenderMethod::ForwardPlus && !scene.capture && !scene.path_trace &&
           !scene.effects.empty();
}

std::vector<const CompositorEffect3D*> effects_at(const Scene3D& scene, EffectCallback point) {
    std::vector<const CompositorEffect3D*> out;
    if (!uses_effects(scene)) {
        return out;
    }
    for (const CompositorEffect3D& effect : scene.effects) {
        if (effect.callback == point) {
            out.push_back(&effect);
        }
    }
    return out;
}

bool effects_need_normals(const Scene3D& scene) {
    return uses_effects(scene) &&
           std::ranges::any_of(scene.effects, &CompositorEffect3D::normal_roughness);
}

Result<void> check_effects(const Scene3D& scene, const std::function<bool(ShaderId)>& is_compositor,
                           const std::function<bool(TextureId)>& has_texture) {
    if (scene.effects.size() > kMaxCompositorEffects) {
        return make_error(ErrorCode::InvalidArgument,
                          "a scene takes up to " + std::to_string(kMaxCompositorEffects) +
                              " compositor effects, not " + std::to_string(scene.effects.size()));
    }
    for (std::size_t i = 0; i < scene.effects.size(); ++i) {
        const CompositorEffect3D& effect = scene.effects[i];
        if (!is_compositor(effect.shader)) {
            return make_error(ErrorCode::InvalidArgument,
                              "compositor effect " + std::to_string(i) + "'s shader " +
                                  std::to_string(static_cast<std::uint32_t>(effect.shader)) +
                                  " isn't a compositor shader made on this device");
        }
        for (const TextureId texture : effect.textures) {
            if (texture != TextureId::None && !has_texture(texture)) {
                return make_error(ErrorCode::NotFound,
                                  "compositor effect " + std::to_string(i) + " names no texture " +
                                      std::to_string(static_cast<std::uint32_t>(texture)));
            }
        }
        for (const auto& constant : effect.constants) {
            if (!std::ranges::all_of(constant, [](float v) { return std::isfinite(v); })) {
                return make_error(ErrorCode::InvalidArgument, "compositor effect " +
                                                                  std::to_string(i) +
                                                                  "'s constants are finite");
            }
        }
    }
    return {};
}

CompositorUniforms compositor_uniforms(const Scene3D& scene, const CompositorEffect3D& effect,
                                       std::uint32_t width, std::uint32_t height,
                                       bool normal_roughness) {
    const auto to_floats = [](const std::array<double, 16>& m) {
        Mat4 out{};
        for (std::size_t i = 0; i < out.size(); ++i) {
            out.at(i) = static_cast<float>(m.at(i));
        }
        return out;
    };
    CompositorUniforms out;
    out.constants = effect.constants;
    out.projection = scene.projection;
    out.inv_projection = to_floats(inverse_matrix(scene.projection));
    out.view = scene.camera;
    out.inv_view = to_floats(inverse_matrix(scene.camera));
    const float w = static_cast<float>(std::max(width, 1U));
    const float h = static_cast<float>(std::max(height, 1U));
    out.screen = {1.0F / w, 1.0F / h, w, h};
    out.options = {normal_roughness ? 1U : 0U, 0U, 0U, 0U};
    return out;
}

void run_compositor(ShaderVm& fragment, const CompositorUniforms& uniforms,
                    const CompositorScreen& screen, const CompositorParamSampler& params,
                    std::span<std::array<float, 4>> out) {
    using cpu_compositor::kDepthBinding;
    using cpu_compositor::kFirstParamBinding;
    using cpu_compositor::kNormalBinding;
    using cpu_compositor::kScreenBinding;
    const auto w = static_cast<std::int32_t>(screen.width);
    const auto h = static_cast<std::int32_t>(screen.height);
    const auto inside = [&](std::int32_t x, std::int32_t y) {
        return x >= 0 && y >= 0 && x < w && y < h;
    };
    const auto at = [&](std::int32_t x, std::int32_t y) {
        return (static_cast<std::size_t>(y) * screen.width) + static_cast<std::size_t>(x);
    };
    ShaderVm::Textures textures;
    textures.fetch = [&](std::uint32_t binding, std::int32_t x,
                         std::int32_t y) -> ShaderVm::Float4 {
        if (!inside(x, y)) {
            return {0.0F, 0.0F, 0.0F, 0.0F};
        }
        if (binding == kScreenBinding) {
            return screen.color[at(x, y)];
        }
        if (binding == kDepthBinding) {
            return {screen.depth.empty() ? 0.0F : screen.depth[at(x, y)], 0.0F, 0.0F, 1.0F};
        }
        if (binding == kNormalBinding && !screen.normal_roughness.empty()) {
            return screen.normal_roughness[at(x, y)];
        }
        return {0.0F, 0.0F, 0.0F, 0.0F};
    };
    textures.sample = [&](std::uint32_t binding, float u, float v) -> ShaderVm::Float4 {
        if (binding >= kFirstParamBinding && params) {
            return params(binding - kFirstParamBinding, u, v);
        }
        return {0.0F, 0.0F, 0.0F, 0.0F};
    };
    textures.size = [&](std::uint32_t binding) -> std::array<std::uint32_t, 2> {
        if (binding < kFirstParamBinding) {
            return {screen.width, screen.height};
        }
        return {1U, 1U};
    };
    const std::array<std::span<const std::byte>, 1> buffers{std::as_bytes(std::span(&uniforms, 1))};
    for (std::int32_t y = 0; y < h; y += 2) {
        for (std::int32_t x = 0; x < w; x += 2) {
            ShaderVm::Lanes lanes;
            lanes.count = 4;
            lanes.quad = true;
            for (std::uint32_t lane = 0; lane < 4; ++lane) {
                const auto px = static_cast<float>(x + static_cast<std::int32_t>(lane & 1U));
                const auto py = static_cast<float>(y + static_cast<std::int32_t>(lane >> 1U));
                lanes.inputs.at(lane).at(cpu_compositor::kFragCoord) = {px + 0.5F, py + 0.5F, 0.0F,
                                                                        0.0F};
            }
            fragment.run(lanes, buffers, textures);
            for (std::uint32_t lane = 0; lane < 4; ++lane) {
                const std::int32_t px = x + static_cast<std::int32_t>(lane & 1U);
                const std::int32_t py = y + static_cast<std::int32_t>(lane >> 1U);
                if (!inside(px, py)) {
                    continue;
                }
                // A pixel that discards keeps its color.
                out[at(px, py)] = lanes.discarded.at(lane)
                                      ? screen.color[at(px, py)]
                                      : lanes.outputs.at(lane).at(cpu_compositor::kOutColor);
            }
        }
    }
}

} // namespace slash::render::detail
