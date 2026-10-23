#include "slash/render/image.hpp"

#include "checks.hpp"
#include "image_codecs.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
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
