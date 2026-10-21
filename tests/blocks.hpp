#pragma once

#include "slash/render/device.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

// Compressed blocks written by hand, so tests know exactly what GPUs decode them to.
namespace slash::test {

using Rgba = std::array<std::uint8_t, 4>;

// A BC7 block of one color: mode 6 with both endpoints the color and every index 0. Mode 6 keeps
// 7 bits a channel and one shared low bit, so every channel of the color must be odd, or every
// one even.
inline std::array<std::uint8_t, 16> bc7_block(Rgba color) {
    std::array<std::uint8_t, 16> block{};
    std::size_t bit = 0;
    const auto put = [&](std::uint32_t value, std::size_t bits) {
        for (std::size_t i = 0; i < bits; ++i, ++bit) {
            if (((value >> i) & 1U) != 0) {
                block.at(bit / 8) |= static_cast<std::uint8_t>(1U << (bit % 8));
            }
        }
    };
    put(1U << 6U, 7); // mode 6
    for (const std::uint8_t channel : color) {
        put(channel >> 1U, 7); // endpoint 0
        put(channel >> 1U, 7); // endpoint 1
    }
    put(color[0] & 1U, 1); // the endpoints' low bits
    put(color[0] & 1U, 1);
    return block; // the indices are all 0
}

// An ASTC block of one color: a void-extent block, its color as 16-bit UNORM channels.
inline std::array<std::uint8_t, 16> astc_block(Rgba color) {
    std::array<std::uint8_t, 16> block{0xFC, 0xFD, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    for (std::size_t i = 0; i < 4; ++i) {
        block.at(8 + (i * 2)) = color.at(i); // v * 257: the low byte...
        block.at(9 + (i * 2)) = color.at(i); // ...and the high
    }
    return block;
}

// A texture of 2x2 blocks in format, each one color: top left, top right, bottom left, bottom
// right. It is 8x8 texels.
inline std::vector<std::uint8_t> quadrant_blocks(render::TextureFormat format,
                                                 const std::array<Rgba, 4>& colors) {
    std::vector<std::uint8_t> out;
    for (const Rgba& color : colors) {
        const auto block =
            format == render::TextureFormat::Bc7 ? bc7_block(color) : astc_block(color);
        out.insert(out.end(), block.begin(), block.end());
    }
    return out;
}

// The same quadrants as RGBA8 texels.
inline std::vector<std::uint8_t> quadrant_pixels(const std::array<Rgba, 4>& colors) {
    std::vector<std::uint8_t> out;
    for (std::size_t y = 0; y < 8; ++y) {
        for (std::size_t x = 0; x < 8; ++x) {
            const Rgba& color = colors.at(((y / 4) * 2) + (x / 4));
            out.insert(out.end(), color.begin(), color.end());
        }
    }
    return out;
}

} // namespace slash::test
