#pragma once

#include "slash/render/device.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

// The material shader's uniforms, laid out as its cbuffers (and their std140 blocks in GLSL), so
// the GPU and OpenGL devices fill them the same way.
namespace slash::render::detail {

// The Program cbuffer.
struct ProgramUniforms {
    std::array<Instruction, kMaxInstructions> code{};
    std::array<std::array<float, 4>, kMaxConstants> constants{};
    std::array<float, 4> texel{};
    std::array<std::uint32_t, 4> counts{};
    std::array<std::int32_t, 4> shading{};
    std::array<float, 4> specular{};
    std::array<float, 4> field{};
    std::array<std::int32_t, 4> layer{};
    std::array<float, 4> ambient{};
    std::array<float, 4> to_lights_x{};
    std::array<float, 4> to_lights_y{};
};
static_assert(sizeof(ProgramUniforms) == 1680);

// One light, laid out as the Light struct.
struct LightUniform {
    std::array<float, 4> color{};
    std::array<float, 4> position{};
    std::array<float, 4> texture_u{};
    std::array<float, 4> texture_v{};
    std::array<float, 4> atlas{};
    std::array<float, 4> shadow{};
    std::array<float, 4> shadow_origin{};
    std::array<float, 4> shadow_color{};
    std::array<std::uint32_t, 4> flags{};
    std::array<std::int32_t, 4> range{};
};
static_assert(sizeof(LightUniform) == 160);

// The Lights cbuffer.
struct LightsUniforms {
    std::array<std::uint32_t, 4> count{};
    std::array<LightUniform, kMaxLights> lights{};
};
static_assert(sizeof(LightsUniforms) == 16 + (160 * kMaxLights));

// The material shader's samplers: the draw's texture, the material's three, the draw's normal and
// specular maps, then the frame's light atlas and shadow map.
inline constexpr std::size_t kMaterialSamplers = kMaterialTextureSlots + 4;

// Whether a draw needs the material shader; the sprite shader draws the rest.
[[nodiscard]] bool uses_material_shader(const Material& material);

// A draw's Program cbuffer. texel is one over the draw's texture's width and height.
[[nodiscard]] ProgramUniforms program_uniforms(const Material& material,
                                               const std::array<float, 4>& texel);

// A frame's Lights cbuffer.
[[nodiscard]] LightsUniforms lights_uniforms(const Lighting& lighting);

} // namespace slash::render::detail
