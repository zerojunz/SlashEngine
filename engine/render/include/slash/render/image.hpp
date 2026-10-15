#pragma once

#include "slash/core/result.hpp"
#include "slash/render/device.hpp"

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

} // namespace slash::render
