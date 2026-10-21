// Compressing texels into BC7 with bc7enc_rdo and into ASTC 4x4 with astc-encoder. Both are
// built without SIMD and with -ffp-contract=off, so the same texels give the same blocks on
// every machine.

#include "slash/render/texture_encoder.hpp"

#include "checks.hpp"

#include <array>
#include <astcenc.h>
#include <bc7enc.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace slash::render {
namespace {

constexpr std::uint32_t kBlock = 4;      // texels on each side of a block
constexpr std::size_t kBlockTexels = 16; // texels a block holds
constexpr std::size_t kBlockBytes = 16;  // bytes a block takes in BC7 and ASTC 4x4
constexpr std::size_t kTexelBytes = 4;   // bytes an RGBA8 texel takes

std::vector<std::uint8_t> encode_bc7(std::uint32_t width, std::uint32_t height,
                                     std::span<const std::uint8_t> rgba) {
    // bc7enc builds its tables once, before the first block.
    static std::once_flag tables;
    std::call_once(tables, [] { bc7enc_compress_block_init(); });
    bc7enc_compress_block_params params{};
    bc7enc_compress_block_params_init(&params);
    // Every channel counts the same: sprites' alpha edges matter as much as their colors.
    bc7enc_compress_block_params_init_linear_weights(&params);
    const std::uint32_t across = width / kBlock;
    const std::uint32_t down = height / kBlock;
    std::vector<std::uint8_t> blocks(std::size_t{across} * down * kBlockBytes);
    std::array<std::uint8_t, kBlockTexels * kTexelBytes> texels{};
    for (std::uint32_t by = 0; by < down; ++by) {
        for (std::uint32_t bx = 0; bx < across; ++bx) {
            for (std::uint32_t y = 0; y < kBlock; ++y) {
                const std::size_t row = ((std::size_t{by} * kBlock) + y) * width;
                std::memcpy(&texels.at(std::size_t{y} * kBlock * kTexelBytes),
                            rgba.data() + ((row + (std::size_t{bx} * kBlock)) * kTexelBytes),
                            kBlock * kTexelBytes);
            }
            const std::size_t block = (std::size_t{by} * across) + bx;
            bc7enc_compress_block(blocks.data() + (block * kBlockBytes), texels.data(), &params);
        }
    }
    return blocks;
}

struct AstcContextDeleter {
    void operator()(astcenc_context* context) const { astcenc_context_free(context); }
};

Result<std::vector<std::uint8_t>> encode_astc(std::uint32_t width, std::uint32_t height,
                                              std::span<const std::uint8_t> rgba) {
    // One context a thread, as astc-encoder asks: making one builds tables.
    thread_local std::unique_ptr<astcenc_context, AstcContextDeleter> context;
    if (context == nullptr) {
        astcenc_config config{};
        astcenc_error status =
            astcenc_config_init(ASTCENC_PRF_LDR, kBlock, kBlock, 1, ASTCENC_PRE_MEDIUM, 0, &config);
        astcenc_context* made = nullptr;
        if (status == ASTCENC_SUCCESS) {
            status = astcenc_context_alloc(&config, 1, &made, nullptr);
        }
        if (status != ASTCENC_SUCCESS) {
            return make_error(ErrorCode::PlatformFailure,
                              std::string("could not start the ASTC encoder: ") +
                                  astcenc_get_error_string(status));
        }
        context.reset(made);
    }
    std::vector<std::uint8_t> blocks(std::size_t{width / kBlock} * (height / kBlock) * kBlockBytes);
    // astc-encoder reads the texels but takes them as a mutable pointer.
    void* slice =
        const_cast<std::uint8_t*>(rgba.data()); // NOLINT(cppcoreguidelines-pro-type-const-cast)
    astcenc_image image{
        .dim_x = width, .dim_y = height, .dim_z = 1, .data_type = ASTCENC_TYPE_U8, .data = &slice};
    constexpr astcenc_swizzle kRgba{
        .r = ASTCENC_SWZ_R, .g = ASTCENC_SWZ_G, .b = ASTCENC_SWZ_B, .a = ASTCENC_SWZ_A};
    const astcenc_error status =
        astcenc_compress_image(context.get(), &image, &kRgba, blocks.data(), blocks.size(), 0);
    astcenc_compress_reset(context.get());
    if (status != ASTCENC_SUCCESS) {
        return make_error(ErrorCode::PlatformFailure, std::string("could not encode ASTC: ") +
                                                          astcenc_get_error_string(status));
    }
    return blocks;
}

} // namespace

Result<std::vector<std::uint8_t>> encode_texels(TextureFormat format, std::uint32_t width,
                                                std::uint32_t height,
                                                std::span<const std::uint8_t> rgba) {
    const TextureDesc desc{.width = width,
                           .height = height,
                           .filter = Filter::Linear,
                           .render_target = false,
                           .format = format};
    if (auto checked = detail::check_texture_desc(desc); !checked) {
        return checked.error();
    }
    if (auto checked = detail::check_pixels(
            {.width = width, .height = height, .filter = Filter::Linear, .render_target = false},
            rgba, false);
        !checked) {
        return checked.error();
    }
    switch (format) {
    case TextureFormat::Rgba8:
        break;
    case TextureFormat::Bc7:
        return encode_bc7(width, height, rgba);
    case TextureFormat::Astc4x4:
        return encode_astc(width, height, rgba);
    }
    return std::vector<std::uint8_t>(rgba.begin(), rgba.end());
}

} // namespace slash::render
