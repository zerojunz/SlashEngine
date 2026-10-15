#pragma once

#include "slash/core/result.hpp"
#include "slash/render/device.hpp"

#include <array>
#include <string>
#include <string_view>

// Image files to and from Image, for textures and captures. PNG and BMP read, through SDL's own
// loaders; PNG writes.
namespace slash::render {

// Decodes a PNG or BMP file's bytes to straight-alpha RGBA. Images wider or taller than 16384
// pixels, the largest texture every GPU takes, are refused.
[[nodiscard]] Result<Image> decode_image(std::string_view bytes);

// Encodes an image as a PNG file's bytes.
[[nodiscard]] Result<std::string> encode_png(const Image& image);

// The image's color at a texture coordinate, from (0, 0) at its top left to (1, 1), as RGBA from
// 0 to 1, filtered as a GPU samples it. Outside 0..1 the edge pixels stretch on, or with repeat
// the image tiles. An empty image is white.
[[nodiscard]] std::array<float, 4> sample_image(const Image& image, Filter filter, bool repeat,
                                                float u, float v);

} // namespace slash::render
