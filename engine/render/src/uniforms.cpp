#include "uniforms.hpp"

#include <algorithm>

namespace slash::render::detail {

bool uses_material_shader(const Material& material) {
    return !material.program.code.empty() || material.shading.mode != LightMode::Unshaded ||
           material.field.pixel_range != 0.0F;
}

ProgramUniforms program_uniforms(const Material& material, const std::array<float, 4>& texel) {
    ProgramUniforms uniforms;
    std::ranges::copy(material.program.code, uniforms.code.begin());
    std::ranges::copy(material.program.constants, uniforms.constants.begin());
    uniforms.texel = texel;
    uniforms.counts.at(0) = static_cast<std::uint32_t>(material.program.code.size());
    const Shading& shading = material.shading;
    uniforms.shading = {static_cast<std::int32_t>(shading.mode),
                        static_cast<std::int32_t>(shading.light_mask), shading.z_index,
                        (shading.normal_map != TextureId::None ? 1 : 0) |
                            (shading.specular_map != TextureId::None ? 2 : 0)};
    uniforms.specular = shading.specular;
    uniforms.field = {material.field.pixel_range, material.field.outline, 0.0F, 0.0F};
    uniforms.layer = {shading.layer, 0, 0, 0};
    uniforms.ambient = shading.ambient;
    const std::array<float, 6> to_lights =
        shading.to_lights.value_or(std::array{1.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F});
    uniforms.to_lights_x = {to_lights[0], to_lights[1], to_lights[2], 0.0F};
    uniforms.to_lights_y = {to_lights[3], to_lights[4], to_lights[5], 0.0F};
    return uniforms;
}

VertexProgramUniforms vertex_program_uniforms(const Material& material,
                                              const std::array<float, 4>& texel) {
    VertexProgramUniforms uniforms;
    const std::size_t count = std::min(material.program.constants.size(), kMaxConstants);
    std::copy_n(material.program.constants.begin(), count, uniforms.constants.begin());
    uniforms.texel = texel;
    return uniforms;
}

LightsUniforms lights_uniforms(const Lighting& lighting) {
    LightsUniforms out;
    const std::size_t count = std::min(lighting.lights.size(), kMaxLights);
    out.count.at(0) = static_cast<std::uint32_t>(count);
    for (std::size_t i = 0; i < count; ++i) {
        const Light& light = lighting.lights[i];
        LightUniform& u = out.lights.at(i);
        const bool point = light.kind == LightKind::Point;
        const auto& m = light.texture_transform;
        u.color = light.color;
        u.position = {light.position[0], light.position[1], light.height, point ? 0.0F : 1.0F};
        u.texture_u = {m[0], m[1], m[2], 0.0F};
        u.texture_v = {m[3], m[4], m[5], 0.0F};
        u.atlas = light.atlas_rect;
        u.shadow = {static_cast<float>(light.shadow_row), light.shadow_far,
                    1.0F + light.shadow_smooth, light.shadow_reach};
        u.shadow_origin = {light.shadow_origin[0], light.shadow_origin[1], light.shadow_span, 0.0F};
        u.shadow_color = light.shadow_color;
        std::uint32_t taps = 0;
        if (light.shadow_filter == ShadowFilter::Pcf5) {
            taps = 2;
        } else if (light.shadow_filter == ShadowFilter::Pcf13) {
            taps = 6;
        }
        u.flags = {static_cast<std::uint32_t>(light.blend), taps, light.item_cull_mask, 0};
        u.range = {light.z_min, light.z_max, light.layer_min, light.layer_max};
    }
    return out;
}

} // namespace slash::render::detail
