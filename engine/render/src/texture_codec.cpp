// Compressed texture formats: names, decoding BC7 and ASTC 4x4 blocks as GPUs decode them, and
// KTX2 files, Khronos' container for textures in the formats GPUs take.

#include "checks.hpp"

#include "slash/render/image.hpp"

#include <array>
#include <astcenc.h>
#include <bc7decomp.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace slash::render {
namespace {

constexpr std::uint32_t kBlock = 4;      // texels on each side of a block
constexpr std::size_t kBlockTexels = 16; // texels a block holds
constexpr std::size_t kBlockBytes = 16;  // bytes a block takes in BC7 and ASTC 4x4
constexpr std::size_t kTexelBytes = 4;   // bytes an RGBA8 texel takes

// KTX2's identifier, then its header and index: the Vulkan format, sizes, counts, the
// supercompression scheme and where the data format descriptor and key/value data are.
constexpr std::array<std::uint8_t, 12> kKtx2Identifier{0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32,
                                                       0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A};
constexpr std::size_t kKtx2HeaderBytes = 80;
constexpr std::size_t kKtx2LevelBytes = 24;
// The Vulkan formats KTX2 names textures by.
constexpr std::uint32_t kVkRgba8Unorm = 37;
constexpr std::uint32_t kVkRgba8Srgb = 43;
constexpr std::uint32_t kVkBc7Unorm = 145;
constexpr std::uint32_t kVkBc7Srgb = 146;
constexpr std::uint32_t kVkAstc4x4Unorm = 157;
constexpr std::uint32_t kVkAstc4x4Srgb = 158;
// Khronos Data Format colour models.
constexpr std::uint32_t kDfModelRgbsda = 1;
constexpr std::uint32_t kDfModelBc7 = 134;
constexpr std::uint32_t kDfModelAstc = 162;

std::uint32_t read32(std::string_view bytes, std::size_t at) {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        value |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[at + i])) << (8 * i);
    }
    return value;
}

std::uint64_t read64(std::string_view bytes, std::size_t at) {
    return std::uint64_t{read32(bytes, at)} | (std::uint64_t{read32(bytes, at + 4)} << 32U);
}

void write32(std::string& out, std::uint32_t value) {
    for (std::size_t i = 0; i < 4; ++i) {
        out.push_back(static_cast<char>((value >> (8 * i)) & 0xFFU));
    }
}

void write64(std::string& out, std::uint64_t value) {
    write32(out, static_cast<std::uint32_t>(value & 0xFFFFFFFFU));
    write32(out, static_cast<std::uint32_t>(value >> 32U));
}

Error ktx2_error(const std::string& message) {
    return make_error(ErrorCode::InvalidArgument,
                      "not a KTX2 texture SlashEngine reads: " + message);
}

struct AstcContextDeleter {
    void operator()(astcenc_context* context) const { astcenc_context_free(context); }
};
using AstcContext = std::unique_ptr<astcenc_context, AstcContextDeleter>;

// One decoding context a thread, as astc-encoder asks: making one builds tables.
Result<astcenc_context*> astc_decoder() {
    thread_local AstcContext context;
    if (context == nullptr) {
        astcenc_config config{};
        astcenc_error status =
            astcenc_config_init(ASTCENC_PRF_LDR, kBlock, kBlock, 1, ASTCENC_PRE_FASTEST,
                                ASTCENC_FLG_DECOMPRESS_ONLY, &config);
        astcenc_context* made = nullptr;
        if (status == ASTCENC_SUCCESS) {
            status = astcenc_context_alloc(&config, 1, &made, nullptr);
        }
        if (status != ASTCENC_SUCCESS) {
            return make_error(ErrorCode::PlatformFailure,
                              std::string("could not start the ASTC decoder: ") +
                                  astcenc_get_error_string(status));
        }
        context.reset(made);
    }
    return context.get();
}

Result<std::vector<std::uint8_t>> decode_bc7(std::uint32_t width, std::uint32_t height,
                                             std::span<const std::uint8_t> blocks) {
    std::vector<std::uint8_t> rgba(std::size_t{width} * height * kTexelBytes);
    const std::uint32_t across = width / kBlock;
    std::array<bc7decomp::color_rgba, kBlockTexels> texels{};
    for (std::uint32_t by = 0; by < height / kBlock; ++by) {
        for (std::uint32_t bx = 0; bx < across; ++bx) {
            const std::size_t block = (std::size_t{by} * across) + bx;
            // An invalid block decodes as transparent black, as GPUs decode it. (color_rgba's
            // default constructor leaves it unset.)
            if (!bc7decomp::unpack_bc7(blocks.data() + (block * kBlockBytes), texels.data())) {
                texels.fill(bc7decomp::color_rgba(0, 0, 0, 0));
            }
            for (std::uint32_t y = 0; y < kBlock; ++y) {
                const std::size_t row = ((std::size_t{by} * kBlock) + y) * width;
                std::memcpy(rgba.data() + ((row + (std::size_t{bx} * kBlock)) * kTexelBytes),
                            &texels.at(std::size_t{y} * kBlock), kBlock * kTexelBytes);
            }
        }
    }
    return rgba;
}

Result<std::vector<std::uint8_t>> decode_astc(std::uint32_t width, std::uint32_t height,
                                              std::span<const std::uint8_t> blocks) {
    auto context = astc_decoder();
    if (!context) {
        return context.error();
    }
    std::vector<std::uint8_t> rgba(std::size_t{width} * height * kTexelBytes);
    void* slice = rgba.data();
    astcenc_image image{
        .dim_x = width, .dim_y = height, .dim_z = 1, .data_type = ASTCENC_TYPE_U8, .data = &slice};
    constexpr astcenc_swizzle kRgba{
        .r = ASTCENC_SWZ_R, .g = ASTCENC_SWZ_G, .b = ASTCENC_SWZ_B, .a = ASTCENC_SWZ_A};
    const astcenc_error status =
        astcenc_decompress_image(context.value(), blocks.data(), blocks.size(), &image, &kRgba, 0);
    astcenc_decompress_reset(context.value());
    if (status != ASTCENC_SUCCESS) {
        return make_error(ErrorCode::InvalidArgument,
                          std::string("could not decode the ASTC blocks: ") +
                              astcenc_get_error_string(status));
    }
    return rgba;
}

// The Khronos data format descriptor KTX2 files carry: one basic block, describing RGBA8's four
// channels or a compressed format's one 128-bit block.
std::string data_format_descriptor(TextureFormat format) {
    const bool compressed = is_compressed(format);
    const std::uint32_t samples = compressed ? 1 : 4;
    const std::uint32_t block_size = 24 + (16 * samples);
    std::string out;
    write32(out, 4 + block_size);           // dfdTotalSize
    write32(out, 0);                        // vendor 0 (Khronos), descriptor type 0 (basic)
    write32(out, 2U | (block_size << 16U)); // version 2, then the block's size
    std::uint32_t model = kDfModelRgbsda;
    if (format == TextureFormat::Bc7) {
        model = kDfModelBc7;
    } else if (format == TextureFormat::Astc4x4) {
        model = kDfModelAstc;
    }
    // Model, BT.709 primaries, linear transfer, straight alpha.
    write32(out, model | (1U << 8U) | (1U << 16U));
    // Texel block dimensions less one: 4x4 for blocks, 1x1 for RGBA8.
    write32(out, compressed ? 3U | (3U << 8U) : 0U);
    write32(out, compressed ? kBlockBytes : kTexelBytes); // bytes in plane 0
    write32(out, 0);                                      // planes 4 to 7
    if (compressed) {
        // The block's 128 bits as one sample, its channel 0 (BC7 color, ASTC data).
        write32(out, 127U << 16U);
        write32(out, 0);
        write32(out, 0);
        write32(out, 0xFFFFFFFFU);
        return out;
    }
    // R, G, B and A, a byte each; channel 15 is alpha.
    constexpr std::array<std::uint32_t, 4> kChannels{0, 1, 2, 15};
    for (std::uint32_t i = 0; i < 4; ++i) {
        write32(out, (i * 8U) | (7U << 16U) | (kChannels.at(i) << 24U));
        write32(out, 0);
        write32(out, 0);
        write32(out, 255);
    }
    return out;
}

} // namespace

std::string_view texture_format_name(TextureFormat format) {
    switch (format) {
    case TextureFormat::Rgba8:
        return "rgba8";
    case TextureFormat::Bc7:
        return "bc7";
    case TextureFormat::Astc4x4:
        return "astc_4x4";
    }
    return "rgba8";
}

std::optional<TextureFormat> parse_texture_format(std::string_view name) {
    for (const TextureFormat format :
         {TextureFormat::Rgba8, TextureFormat::Bc7, TextureFormat::Astc4x4}) {
        if (name == texture_format_name(format)) {
            return format;
        }
    }
    return std::nullopt;
}

Result<std::vector<std::uint8_t>> decode_texels(TextureFormat format, std::uint32_t width,
                                                std::uint32_t height,
                                                std::span<const std::uint8_t> texels) {
    const TextureDesc desc{.width = width,
                           .height = height,
                           .filter = Filter::Linear,
                           .render_target = false,
                           .format = format};
    if (auto checked = detail::check_texture_desc(desc); !checked) {
        return checked.error();
    }
    if (auto checked = detail::check_pixels(desc, texels, false); !checked) {
        return checked.error();
    }
    switch (format) {
    case TextureFormat::Rgba8:
        return std::vector<std::uint8_t>(texels.begin(), texels.end());
    case TextureFormat::Bc7:
        return decode_bc7(width, height, texels);
    case TextureFormat::Astc4x4:
        return decode_astc(width, height, texels);
    }
    return std::vector<std::uint8_t>(texels.begin(), texels.end());
}

bool is_ktx2(std::string_view bytes) {
    return bytes.size() >= kKtx2Identifier.size() &&
           std::memcmp(bytes.data(), kKtx2Identifier.data(), kKtx2Identifier.size()) == 0;
}

Result<Image> decode_ktx2(std::string_view bytes) {
    if (!is_ktx2(bytes) || bytes.size() < kKtx2HeaderBytes + kKtx2LevelBytes) {
        return ktx2_error("its header is cut short");
    }
    const std::uint32_t vk_format = read32(bytes, 12);
    const std::uint32_t width = read32(bytes, 20);
    const std::uint32_t height = read32(bytes, 24);
    const std::uint32_t depth = read32(bytes, 28);
    const std::uint32_t layers = read32(bytes, 32);
    const std::uint32_t faces = read32(bytes, 36);
    const std::uint32_t supercompression = read32(bytes, 44);
    std::optional<TextureFormat> format;
    switch (vk_format) {
    case kVkRgba8Unorm:
    case kVkRgba8Srgb:
        format = TextureFormat::Rgba8;
        break;
    case kVkBc7Unorm:
    case kVkBc7Srgb:
        format = TextureFormat::Bc7;
        break;
    case kVkAstc4x4Unorm:
    case kVkAstc4x4Srgb:
        format = TextureFormat::Astc4x4;
        break;
    default:
        break;
    }
    if (!format) {
        return ktx2_error("its Vulkan format " + std::to_string(vk_format) +
                          " is not RGBA8, BC7 or ASTC 4x4");
    }
    if (depth > 1 || layers > 1 || faces != 1) {
        return ktx2_error("it is a 3D texture, an array or a cube map, not one 2D texture");
    }
    if (supercompression != 0) {
        return ktx2_error("its levels are supercompressed (scheme " +
                          std::to_string(supercompression) + ")");
    }
    // The first level, the full-size one, is listed first.
    const std::uint64_t offset = read64(bytes, kKtx2HeaderBytes);
    const std::uint64_t length = read64(bytes, kKtx2HeaderBytes + 8);
    if (offset > bytes.size() || length > bytes.size() - offset) {
        return ktx2_error("its first level runs past the end of the file");
    }
    const TextureDesc desc{.width = width,
                           .height = height,
                           .filter = Filter::Linear,
                           .render_target = false,
                           .format = *format};
    if (auto checked = detail::check_texture_desc(desc); !checked) {
        return checked.error();
    }
    if (length != texture_bytes(desc)) {
        return ktx2_error("its first level is " + std::to_string(length) + " bytes, not the " +
                          std::to_string(texture_bytes(desc)) + " a " + std::to_string(width) +
                          "x" + std::to_string(height) + " " +
                          std::string(texture_format_name(*format)) + " texture takes");
    }
    const auto* first = reinterpret_cast<const std::uint8_t*>(bytes.data() + offset);
    const std::span<const std::uint8_t> texels(first, static_cast<std::size_t>(length));
    auto pixels = decode_texels(*format, width, height, texels);
    if (!pixels) {
        return pixels.error();
    }
    Image image{.width = width, .height = height, .pixels = std::move(pixels).value()};
    if (is_compressed(*format)) {
        image.block_format = *format;
        image.blocks.assign(texels.begin(), texels.end());
    }
    return image;
}

Result<std::string> encode_ktx2(const Image& image) {
    const bool compressed = is_compressed(image.block_format);
    const TextureDesc desc{.width = image.width,
                           .height = image.height,
                           .filter = Filter::Linear,
                           .render_target = false,
                           .format = image.block_format};
    if (auto checked = detail::check_texture_desc(desc); !checked) {
        return checked.error();
    }
    const std::vector<std::uint8_t>& texels = compressed ? image.blocks : image.pixels;
    if (auto checked = detail::check_pixels(desc, texels, false); !checked) {
        return checked.error();
    }
    std::uint32_t vk_format = kVkRgba8Unorm;
    if (image.block_format == TextureFormat::Bc7) {
        vk_format = kVkBc7Unorm;
    } else if (image.block_format == TextureFormat::Astc4x4) {
        vk_format = kVkAstc4x4Unorm;
    }
    const std::string dfd = data_format_descriptor(image.block_format);
    const std::size_t dfd_offset = kKtx2HeaderBytes + kKtx2LevelBytes;
    // Levels start on a multiple of the block size and of 4.
    const std::size_t align = compressed ? kBlockBytes : kTexelBytes;
    const std::size_t data_offset = ((dfd_offset + dfd.size() + align - 1) / align) * align;

    std::string out(reinterpret_cast<const char*>(kKtx2Identifier.data()), kKtx2Identifier.size());
    write32(out, vk_format);
    write32(out, 1); // typeSize: bytes a component takes, 1 for these formats
    write32(out, image.width);
    write32(out, image.height);
    write32(out, 0); // pixelDepth
    write32(out, 0); // layerCount
    write32(out, 1); // faceCount
    write32(out, 1); // levelCount
    write32(out, 0); // supercompressionScheme
    write32(out, static_cast<std::uint32_t>(dfd_offset));
    write32(out, static_cast<std::uint32_t>(dfd.size()));
    write32(out, 0); // no key/value data
    write32(out, 0);
    write64(out, 0); // no supercompression global data
    write64(out, 0);
    write64(out, data_offset);
    write64(out, texels.size());
    write64(out, texels.size());
    out += dfd;
    out.resize(data_offset, '\0');
    out.append(reinterpret_cast<const char*>(texels.data()), texels.size());
    return out;
}

} // namespace slash::render
