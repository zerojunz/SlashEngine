#pragma once

#include "slash/core/result.hpp"
#include "slash/render/device.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Image files to and from Image, for textures and captures. PNG and BMP read, through SDL's own
// loaders, JPEG through stb_image's decoder, and KTX2 files of RGBA8, BC7 or ASTC 4x4 texels; PNG
// and KTX2 write. 16-bit gray PNG files, as heightmaps are, read and write as Gray16, and
// Radiance HDR files, as lightmaps are, as HdrImage.
namespace slash::render {

// The suffixes of the image files decode_image reads, as texture paths end.
inline constexpr std::array<std::string_view, 5> kImageSuffixes{".png", ".bmp", ".jpg", ".jpeg",
                                                                ".ktx2"};
// Whether a path ends in one of kImageSuffixes.
[[nodiscard]] bool is_image_path(std::string_view path);

// Decodes a PNG, BMP, JPEG or KTX2 file's bytes to straight-alpha RGBA. A KTX2 file of BC7 or
// ASTC blocks keeps them too, in blocks; only its first mip level is read. Images wider or taller
// than 16384 pixels, the largest texture every GPU takes, are refused. JPEG files decode with
// integer math, to the same pixels on every machine.
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

// A grid of 16-bit values, rows from the top, as a heightmap's PNG file holds them.
struct Gray16 {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint16_t> values; // width * height

    bool operator==(const Gray16&) const = default;
};
// Decodes a PNG file to 16-bit gray values, through stb_image's decoder, the same on every
// machine: 8-bit samples widen to 16 bits (times 257) and colors become gray by their luminance.
// Images wider or taller than 16384 pixels are refused.
[[nodiscard]] Result<Gray16> decode_gray16(std::string_view bytes);
// Encodes the values as a 16-bit gray PNG file, the same bytes on every machine.
[[nodiscard]] Result<std::string> encode_gray16_png(const Gray16& image);

// An image of linear light beyond 1, three floats a pixel (red, green, blue), rows from the top,
// as a baked lightmap holds it.
struct HdrImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<float> rgb; // width * height * 3

    bool operator==(const HdrImage&) const = default;
};
// Decodes a Radiance HDR file (.hdr): RGBE pixels, in flat or run-length encoded scanlines, top
// row first ("-Y height +X width"), as Greg Ward's rgbe2float decodes them. Images wider or
// taller than 16384 pixels are refused.
[[nodiscard]] Result<HdrImage> decode_hdr(std::string_view bytes);
// Encodes the image as a Radiance HDR file of flat RGBE scanlines, the same bytes on every
// machine. Values below 0 and NaN are written as 0, and values past 1e38 as 1e38.
[[nodiscard]] Result<std::string> encode_hdr(const HdrImage& image);

// The ambient light a scene's lit surfaces take from each direction they face, as a panorama
// width by height texels, linear: its ambient color times its energy, mixed with its sky's
// radiance at the blurriest level by Radiance3D's ambient_mix where Radiance3D.ambient is set, as
// the devices light a surface by its normal. Godot's LightmapGI takes its environment so
// (environment_bake_panorama) for rays that leave the scene. Texel (x, y) holds the direction at
// angle 2 pi (x + 0.5) / width about y from +z toward +x and pi (y + 0.5) / height down from +y,
// as Godot's lightmapper looks a direction up. sky_texel samples the sky's texture at a panorama
// coordinate, as a device would.
[[nodiscard]] HdrImage
ambient_panorama(const Scene3D& scene, std::uint32_t width, std::uint32_t height,
                 const std::function<std::array<float, 4>(const std::array<float, 2>&)>& sky_texel);

// The image's color at a texture coordinate, from (0, 0) at its top left to (1, 1), as RGBA from
// 0 to 1, filtered as a GPU samples it. Outside 0..1 the edge pixels stretch on, or with repeat
// the image tiles. An empty image is white.
[[nodiscard]] std::array<float, 4> sample_image(const Image& image, Filter filter, bool repeat,
                                                float u, float v);

} // namespace slash::render
