#pragma once

#include "slash/core/result.hpp"
#include "slash/render/device.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Image files to and from Image, for textures and captures. PNG and BMP read, through SDL's own
// loaders, and KTX2 files of RGBA8, BC7 or ASTC 4x4 texels; PNG and KTX2 write.
namespace slash::render {

// Decodes a PNG, BMP or KTX2 file's bytes to straight-alpha RGBA. A KTX2 file of BC7 or ASTC
// blocks keeps them too, in blocks; only its first mip level is read. Images wider or taller than
// 16384 pixels, the largest texture every GPU takes, are refused.
[[nodiscard]] Result<Image> decode_image(std::string_view bytes);

// Whether the bytes are a KTX2 file, Khronos' container for textures as GPUs take them.
[[nodiscard]] bool is_ktx2(std::string_view bytes);
// Decodes a KTX2 file's bytes, as decode_image does.
[[nodiscard]] Result<Image> decode_ktx2(std::string_view bytes);
// A KTX2 file of the image: its blocks when it has them, else its pixels as RGBA8. One mip level,
// not supercompressed.
[[nodiscard]] Result<std::string> encode_ktx2(const Image& image);

// The file an export ships in place of a texture, its texels in one format as a KTX2 file:
// "ui/icon.png" in BC7 is "ui/icon.png.bc7.ktx2". Games read the form their GPU samples.
[[nodiscard]] std::string texture_form_path(std::string_view texture, TextureFormat format);

// Texels of a format decoded to straight-alpha RGBA, as GPUs decode them: BC7 and ASTC blocks
// exactly as their formats specify, so the same on every machine; RGBA8 as it is.
[[nodiscard]] Result<std::vector<std::uint8_t>> decode_texels(TextureFormat format,
                                                              std::uint32_t width,
                                                              std::uint32_t height,
                                                              std::span<const std::uint8_t> texels);

// The image resized to width by height pixels with a Lanczos filter of three lobes, as Godot's
// Image::resize with INTERPOLATE_LANCZOS, with colors weighted by alpha so transparent pixels
// don't darken the edges. The same pixels on every machine. Blocks are left out.
[[nodiscard]] Image resize_image(const Image& image, std::uint32_t width, std::uint32_t height);

// Encodes an image as a PNG file's bytes.
[[nodiscard]] Result<std::string> encode_png(const Image& image);

// The image's color at a texture coordinate, from (0, 0) at its top left to (1, 1), as RGBA from
// 0 to 1, filtered as a GPU samples it. Outside 0..1 the edge pixels stretch on, or with repeat
// the image tiles. An empty image is white.
[[nodiscard]] std::array<float, 4> sample_image(const Image& image, Filter filter, bool repeat,
                                                float u, float v);

} // namespace slash::render
