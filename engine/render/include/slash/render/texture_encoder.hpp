#pragma once

#include "slash/core/result.hpp"
#include "slash/render/device.hpp"

#include <cstdint>
#include <span>
#include <vector>

// Compressing straight-alpha RGBA texels into the formats GPUs sample: BC7 with bc7enc_rdo and
// ASTC 4x4 with astc-encoder. It is the slash::texture_encoder library, which tools and tests
// link; games only decode (decode_texels), so their builds leave the encoders out.
namespace slash::render {

// The texels in format: blocks of 4x4 texels for BC7 and ASTC, so width and height are
// multiples of 4; RGBA8 as it is. The same texels give the same blocks on every machine.
[[nodiscard]] Result<std::vector<std::uint8_t>> encode_texels(TextureFormat format,
                                                              std::uint32_t width,
                                                              std::uint32_t height,
                                                              std::span<const std::uint8_t> rgba);

} // namespace slash::render
