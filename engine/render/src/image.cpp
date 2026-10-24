#include "slash/render/image.hpp"

#include "checks.hpp"
#include "image_codecs.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <string>

namespace slash::render {
namespace {

// RGBA in memory order on every machine, as Image keeps it.
constexpr SDL_PixelFormat kRgba = SDL_PIXELFORMAT_RGBA32;

struct SurfaceDeleter {
    void operator()(SDL_Surface* surface) const { SDL_DestroySurface(surface); }
};
using SurfacePtr = std::unique_ptr<SDL_Surface, SurfaceDeleter>;

// A texel index on one axis of a size-texel image, clamped to its edge or wrapped around.
std::uint32_t texel_index(float texel, std::uint32_t size, bool repeat) {
    const auto count = static_cast<float>(size);
    if (repeat) {
        const float wrapped = texel - (count * std::floor(texel / count));
        return std::min(static_cast<std::uint32_t>(wrapped), size - 1);
    }
    return static_cast<std::uint32_t>(std::clamp(texel, 0.0F, count - 1.0F));
}

// Whether the bytes start as a JPEG file does: a start of image marker, then another marker.
bool is_jpeg(std::string_view bytes) {
    return bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xFF &&
           static_cast<unsigned char>(bytes[1]) == 0xD8 &&
           static_cast<unsigned char>(bytes[2]) == 0xFF;
}

Result<Image> decode_jpeg(std::string_view bytes) {
    if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return make_error(ErrorCode::InvalidArgument, "the JPEG file is too large");
    }
    int width = 0;
    int height = 0;
    unsigned char* pixels = slash_decode_jpeg(reinterpret_cast<const unsigned char*>(bytes.data()),
                                              static_cast<int>(bytes.size()), &width, &height);
    if (pixels == nullptr) {
        // stb_image refuses images past STBI_MAX_DIMENSIONS, the largest texture size.
        return make_error(ErrorCode::InvalidArgument,
                          std::string("not a JPEG image it can read: ") + slash_image_error() +
                              "; textures are at most " + std::to_string(detail::kMaxTextureSize) +
                              " pixels on each side");
    }
    Image image;
    image.width = static_cast<std::uint32_t>(width);
    image.height = static_cast<std::uint32_t>(height);
    image.pixels.assign(pixels, pixels + (std::size_t{image.width} * image.height * 4));
    slash_free_image(pixels);
    return image;
}

} // namespace

bool is_image_path(std::string_view path) {
    return std::ranges::any_of(kImageSuffixes,
                               [&](std::string_view suffix) { return path.ends_with(suffix); });
}

Result<Image> decode_image(std::string_view bytes) {
    if (is_ktx2(bytes)) {
        return decode_ktx2(bytes);
    }
    if (is_jpeg(bytes)) {
        return decode_jpeg(bytes);
    }
    SDL_IOStream* stream = SDL_IOFromConstMem(bytes.data(), bytes.size());
    if (stream == nullptr) {
        return make_error(ErrorCode::PlatformFailure,
                          std::string("could not read the image: ") + SDL_GetError());
    }
    // Closes the stream, whether or not the image loads.
    const SurfacePtr loaded(SDL_LoadSurface_IO(stream, true));
    if (loaded == nullptr) {
        return make_error(ErrorCode::InvalidArgument,
                          std::string("not a PNG, BMP, JPEG or KTX2 image: ") + SDL_GetError());
    }
    if (loaded->w > static_cast<int>(detail::kMaxTextureSize) ||
        loaded->h > static_cast<int>(detail::kMaxTextureSize)) {
        return make_error(ErrorCode::InvalidArgument,
                          "the image is " + std::to_string(loaded->w) + "x" +
                              std::to_string(loaded->h) + "; textures are at most " +
                              std::to_string(detail::kMaxTextureSize) + " pixels on each side");
    }
    const SurfacePtr rgba(SDL_ConvertSurface(loaded.get(), kRgba));
    if (rgba == nullptr) {
        return make_error(ErrorCode::PlatformFailure,
                          std::string("could not convert the image: ") + SDL_GetError());
    }
    Image image;
    image.width = static_cast<std::uint32_t>(rgba->w);
    image.height = static_cast<std::uint32_t>(rgba->h);
    const std::size_t row = std::size_t{image.width} * 4;
    image.pixels.resize(row * image.height);
    const auto* source = static_cast<const std::uint8_t*>(rgba->pixels);
    for (std::uint32_t y = 0; y < image.height; ++y) {
        std::memcpy(image.pixels.data() + (row * y),
                    source + (static_cast<std::size_t>(rgba->pitch) * y), row);
    }
    return image;
}

Result<std::string> encode_png(const Image& image) {
    if (image.width == 0 || image.height == 0 ||
        image.pixels.size() != std::size_t{image.width} * image.height * 4) {
        return make_error(ErrorCode::InvalidArgument,
                          "the image needs width * height * 4 bytes of pixels");
    }
    // SDL only reads the pixels, though the surface type is not const.
    const SurfacePtr surface(SDL_CreateSurfaceFrom(
        static_cast<int>(image.width), static_cast<int>(image.height), kRgba,
        const_cast<std::uint8_t*>(image.pixels.data()), static_cast<int>(image.width * 4)));
    SDL_IOStream* stream = SDL_IOFromDynamicMem();
    if (surface == nullptr || stream == nullptr) {
        if (stream != nullptr) {
            SDL_CloseIO(stream);
        }
        return make_error(ErrorCode::PlatformFailure,
                          std::string("could not encode the image: ") + SDL_GetError());
    }
    if (!SDL_SavePNG_IO(surface.get(), stream, false)) {
        SDL_CloseIO(stream);
        return make_error(ErrorCode::PlatformFailure,
                          std::string("could not encode the image: ") + SDL_GetError());
    }
    const Sint64 size = SDL_TellIO(stream);
    const auto* data = static_cast<const char*>(SDL_GetPointerProperty(
        SDL_GetIOProperties(stream), SDL_PROP_IOSTREAM_DYNAMIC_MEMORY_POINTER, nullptr));
    std::string png;
    if (data != nullptr && size > 0) {
        png.assign(data, static_cast<std::size_t>(size));
    }
    SDL_CloseIO(stream);
    if (png.empty()) {
        return make_error(ErrorCode::PlatformFailure, "could not encode the image");
    }
    return png;
}

Result<Gray16> decode_gray16(std::string_view bytes) {
    if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return make_error(ErrorCode::InvalidArgument, "the PNG file is too large");
    }
    int width = 0;
    int height = 0;
    unsigned short* values =
        slash_decode_gray16(reinterpret_cast<const unsigned char*>(bytes.data()),
                            static_cast<int>(bytes.size()), &width, &height);
    if (values == nullptr) {
        return make_error(ErrorCode::InvalidArgument,
                          std::string("not a PNG image it can read: ") + slash_image_error() +
                              "; images are at most " + std::to_string(detail::kMaxTextureSize) +
                              " pixels on each side");
    }
    Gray16 image;
    image.width = static_cast<std::uint32_t>(width);
    image.height = static_cast<std::uint32_t>(height);
    image.values.assign(values, values + (std::size_t{image.width} * image.height));
    slash_free_image(values);
    return image;
}

Result<std::string> encode_gray16_png(const Gray16& image) {
    if (image.width == 0 || image.height == 0 || image.width > detail::kMaxTextureSize ||
        image.height > detail::kMaxTextureSize ||
        image.values.size() != std::size_t{image.width} * image.height) {
        return make_error(ErrorCode::InvalidArgument,
                          "the image needs width * height values, at most " +
                              std::to_string(detail::kMaxTextureSize) + " on each side");
    }
    int size = 0;
    unsigned char* png = slash_encode_gray16_png(image.values.data(), static_cast<int>(image.width),
                                                 static_cast<int>(image.height), &size);
    if (png == nullptr) {
        return make_error(ErrorCode::PlatformFailure, "could not encode the image");
    }
    std::string bytes(reinterpret_cast<const char*>(png), static_cast<std::size_t>(size));
    std::free(png); // NOLINT(cppcoreguidelines-no-malloc): the C encoder allocates with malloc
    return bytes;
}

namespace {

// Greg Ward's float2rgbe: the largest component's exponent, shared, and each mantissa in 8 bits.
std::array<std::uint8_t, 4> to_rgbe(float red, float green, float blue) {
    const auto clean = [](float v) {
        constexpr float kLargest = 1e38F;
        return std::isnan(v) || v < 0.0F ? 0.0F : std::min(v, kLargest);
    };
    const double r = clean(red);
    const double g = clean(green);
    const double b = clean(blue);
    const double largest = std::max({r, g, b});
    if (largest < 1e-32) {
        return {0, 0, 0, 0};
    }
    int exponent = 0;
    const double scale = std::frexp(largest, &exponent) * 256.0 / largest;
    return {static_cast<std::uint8_t>(r * scale), static_cast<std::uint8_t>(g * scale),
            static_cast<std::uint8_t>(b * scale), static_cast<std::uint8_t>(exponent + 128)};
}

} // namespace

Result<HdrImage> decode_hdr(std::string_view bytes) {
    std::size_t at = 0;
    const auto line = [&]() -> std::optional<std::string_view> {
        const std::size_t end = bytes.find('\n', at);
        if (end == std::string_view::npos) {
            return std::nullopt;
        }
        const std::string_view text = bytes.substr(at, end - at);
        at = end + 1;
        return text;
    };
    const auto bad = [](const std::string& why) {
        return make_error(ErrorCode::InvalidArgument,
                          "not a Radiance HDR file it can read: " + why);
    };
    const auto magic = line();
    if (!magic || (*magic != "#?RADIANCE" && *magic != "#?RGBE")) {
        return bad("it doesn't start with #?RADIANCE");
    }
    while (true) {
        const auto header = line();
        if (!header) {
            return bad("its header doesn't end");
        }
        if (header->empty()) {
            break;
        }
        if (header->starts_with("FORMAT=") && *header != "FORMAT=32-bit_rle_rgbe") {
            return bad("its pixels are " + std::string(header->substr(7)) +
                       ", not 32-bit_rle_rgbe");
        }
    }
    const auto size = line();
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    {
        // "-Y height +X width": rows from the top, pixels from the left.
        std::string_view text = size.value_or("");
        const auto number = [&text](std::string_view before) -> std::uint64_t {
            if (!text.starts_with(before)) {
                return 0;
            }
            text.remove_prefix(before.size());
            std::uint64_t value = 0;
            std::size_t digits = 0;
            while (digits < text.size() && digits < 6 && text[digits] >= '0' &&
                   text[digits] <= '9') {
                value = (value * 10) + static_cast<std::uint64_t>(text[digits] - '0');
                ++digits;
            }
            text.remove_prefix(digits);
            return digits == 0 ? 0 : value;
        };
        const std::uint64_t h = number("-Y ");
        const std::uint64_t w = number(" +X ");
        if (w == 0 || h == 0 || !text.empty()) {
            return bad("its size isn't \"-Y height +X width\"");
        }
        if (w > detail::kMaxTextureSize || h > detail::kMaxTextureSize) {
            return bad("images are at most " + std::to_string(detail::kMaxTextureSize) +
                       " pixels on each side");
        }
        width = static_cast<std::uint32_t>(w);
        height = static_cast<std::uint32_t>(h);
    }
    HdrImage image{.width = width, .height = height, .rgb = {}};
    image.rgb.resize(std::size_t{width} * height * 3);
    std::vector<std::uint8_t> row(std::size_t{width} * 4);
    const auto byte = [&]() -> std::optional<std::uint8_t> {
        if (at >= bytes.size()) {
            return std::nullopt;
        }
        return static_cast<std::uint8_t>(bytes[at++]);
    };
    for (std::uint32_t y = 0; y < height; ++y) {
        const bool rle = width >= 8 && width < 32768 && at + 4 <= bytes.size() && bytes[at] == 2 &&
                         bytes[at + 1] == 2 && static_cast<std::uint8_t>(bytes[at + 2]) < 128;
        if (rle) {
            const std::uint32_t length =
                (static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[at + 2])) << 8U) |
                static_cast<std::uint8_t>(bytes[at + 3]);
            if (length != width) {
                return bad("a scanline is " + std::to_string(length) + " pixels, not " +
                           std::to_string(width));
            }
            at += 4;
            // Each component's run of the scanline in turn.
            for (std::size_t channel = 0; channel < 4; ++channel) {
                std::size_t x = 0;
                while (x < width) {
                    const auto count = byte();
                    if (!count || *count == 0) {
                        return bad("it ends early");
                    }
                    if (*count > 128) {
                        const std::size_t run = *count - 128U;
                        const auto value = byte();
                        if (!value || x + run > width) {
                            return bad("a run is past the end of its scanline");
                        }
                        for (std::size_t i = 0; i < run; ++i) {
                            row[((x + i) * 4) + channel] = *value;
                        }
                        x += run;
                    } else {
                        if (x + *count > width) {
                            return bad("a run is past the end of its scanline");
                        }
                        for (std::size_t i = 0; i < *count; ++i) {
                            const auto value = byte();
                            if (!value) {
                                return bad("it ends early");
                            }
                            row[((x + i) * 4) + channel] = *value;
                        }
                        x += *count;
                    }
                }
            }
        } else {
            if (at + row.size() > bytes.size()) {
                return bad("it ends early");
            }
            for (std::size_t i = 0; i < row.size(); ++i) {
                row[i] = static_cast<std::uint8_t>(bytes[at + i]);
            }
            at += row.size();
        }
        for (std::size_t x = 0; x < width; ++x) {
            const std::uint8_t* rgbe = &row[x * 4];
            float* out = &image.rgb[((std::size_t{y} * width) + x) * 3];
            if (rgbe[3] == 0) {
                out[0] = out[1] = out[2] = 0.0F;
                continue;
            }
            const auto scale =
                static_cast<float>(std::ldexp(1.0, static_cast<int>(rgbe[3]) - (128 + 8)));
            out[0] = static_cast<float>(rgbe[0]) * scale;
            out[1] = static_cast<float>(rgbe[1]) * scale;
            out[2] = static_cast<float>(rgbe[2]) * scale;
        }
    }
    return image;
}

Result<std::string> encode_hdr(const HdrImage& image) {
    if (image.width == 0 || image.height == 0 || image.width > detail::kMaxTextureSize ||
        image.height > detail::kMaxTextureSize ||
        image.rgb.size() != std::size_t{image.width} * image.height * 3) {
        return make_error(ErrorCode::InvalidArgument,
                          "the image needs width * height * 3 values, at most " +
                              std::to_string(detail::kMaxTextureSize) + " on each side");
    }
    std::string bytes = "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y " + std::to_string(image.height) +
                        " +X " + std::to_string(image.width) + "\n";
    const std::size_t header = bytes.size();
    bytes.resize(header + (std::size_t{image.width} * image.height * 4));
    for (std::size_t i = 0; i < std::size_t{image.width} * image.height; ++i) {
        const std::array<std::uint8_t, 4> rgbe =
            to_rgbe(image.rgb[i * 3], image.rgb[(i * 3) + 1], image.rgb[(i * 3) + 2]);
        for (std::size_t c = 0; c < 4; ++c) {
            bytes[header + (i * 4) + c] = static_cast<char>(rgbe[c]);
        }
    }
    return bytes;
}

std::array<float, 4> sample_image(const Image& image, Filter filter, bool repeat, float u,
                                  float v) {
    if (image.width == 0 || image.height == 0 ||
        image.pixels.size() != std::size_t{image.width} * image.height * 4) {
        return {1.0F, 1.0F, 1.0F, 1.0F};
    }
    const auto texel = [&](float x, float y) {
        const std::size_t at = ((std::size_t{texel_index(y, image.height, repeat)} * image.width) +
                                texel_index(x, image.width, repeat)) *
                               4;
        std::array<float, 4> color{};
        for (std::size_t channel = 0; channel < 4; ++channel) {
            color.at(channel) = static_cast<float>(image.pixels[at + channel]) / 255.0F;
        }
        return color;
    };
    // Texel centers are at half-texel coordinates, as on the GPU.
    const float x = (u * static_cast<float>(image.width)) - 0.5F;
    const float y = (v * static_cast<float>(image.height)) - 0.5F;
    if (filter == Filter::Nearest) {
        return texel(std::floor(x + 0.5F), std::floor(y + 0.5F));
    }
    const float x0 = std::floor(x);
    const float y0 = std::floor(y);
    const float tx = x - x0;
    const float ty = y - y0;
    const std::array<float, 4> top_left = texel(x0, y0);
    const std::array<float, 4> top_right = texel(x0 + 1.0F, y0);
    const std::array<float, 4> bottom_left = texel(x0, y0 + 1.0F);
    const std::array<float, 4> bottom_right = texel(x0 + 1.0F, y0 + 1.0F);
    std::array<float, 4> color{};
    for (std::size_t channel = 0; channel < 4; ++channel) {
        const float top =
            top_left.at(channel) + ((top_right.at(channel) - top_left.at(channel)) * tx);
        const float bottom =
            bottom_left.at(channel) + ((bottom_right.at(channel) - bottom_left.at(channel)) * tx);
        color.at(channel) = top + ((bottom - top) * ty);
    }
    return color;
}

} // namespace slash::render
