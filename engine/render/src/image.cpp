#include "slash/render/image.hpp"

#include "checks.hpp"

#include <SDL3/SDL.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>

namespace slash::render {
namespace {

// RGBA in memory order on every machine, as Image keeps it.
constexpr SDL_PixelFormat kRgba = SDL_PIXELFORMAT_RGBA32;

struct SurfaceDeleter {
    void operator()(SDL_Surface* surface) const { SDL_DestroySurface(surface); }
};
using SurfacePtr = std::unique_ptr<SDL_Surface, SurfaceDeleter>;

} // namespace

Result<Image> decode_image(std::string_view bytes) {
    SDL_IOStream* stream = SDL_IOFromConstMem(bytes.data(), bytes.size());
    if (stream == nullptr) {
        return make_error(ErrorCode::PlatformFailure,
                          std::string("could not read the image: ") + SDL_GetError());
    }
    // Closes the stream, whether or not the image loads.
    const SurfacePtr loaded(SDL_LoadSurface_IO(stream, true));
    if (loaded == nullptr) {
        return make_error(ErrorCode::InvalidArgument,
                          std::string("not a PNG or BMP image SDL can read: ") + SDL_GetError());
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

} // namespace slash::render
