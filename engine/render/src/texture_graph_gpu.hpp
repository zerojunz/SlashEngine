// Texture graphs on the GPU devices: what shaders/texture_graph.hlsl takes for a pass, shared by
// the GPU and WebGPU devices.

#pragma once

#include "slash/render/texture_graph.hpp"

#include <array>
#include <cmath>
#include <cstdint>

namespace slash::render::detail {

// texture_graph.hlsl's threads a group, across and down.
inline constexpr std::uint32_t kGraphGroup = 8;

// texture_graph.hlsl's Params.
struct GraphUniforms {
    // The op, the width and height, 1 when the program tiles.
    std::array<std::uint32_t, 4> head{};
    // A bit per input the pass reads, the image's width and height.
    std::array<std::uint32_t, 4> more{};
    std::array<float, kGraphParams> params{};
};
static_assert(sizeof(GraphUniforms) == 224);

// A pass's uniforms in program.
[[nodiscard]] inline GraphUniforms graph_uniforms(const TextureGraphProgram& program,
                                                  const GraphPass& pass) {
    GraphUniforms uniforms;
    uniforms.head = {static_cast<std::uint32_t>(pass.op), program.width, program.height,
                     program.tiling ? 1U : 0U};
    for (std::uint32_t k = 0; k < kGraphInputs; ++k) {
        if (pass.in.at(k) != kNoGraphSlot) {
            uniforms.more[0] |= 1U << k;
        }
    }
    if (pass.op == GraphOp::Image) {
        const GraphImage& image =
            program.images.at(static_cast<std::size_t>(std::lround(pass.params[0])));
        uniforms.more[1] = image.width;
        uniforms.more[2] = image.height;
    }
    uniforms.params = pass.params;
    return uniforms;
}

// The image a pass samples, or none.
[[nodiscard]] inline const GraphImage* graph_image(const TextureGraphProgram& program,
                                                   const GraphPass& pass) {
    if (pass.op != GraphOp::Image) {
        return nullptr;
    }
    return &program.images.at(static_cast<std::size_t>(std::lround(pass.params[0])));
}

// A pass's dispatch: groups across and down.
[[nodiscard]] inline std::array<std::uint32_t, 2> graph_groups(const TextureGraphProgram& program) {
    return {(program.width + kGraphGroup - 1) / kGraphGroup,
            (program.height + kGraphGroup - 1) / kGraphGroup};
}

} // namespace slash::render::detail
