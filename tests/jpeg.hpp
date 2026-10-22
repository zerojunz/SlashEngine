#pragma once

#include "jpeg_write.h"

#include "slash/render/device.hpp"

#include <string>

namespace slash::test {

// An image as a JPEG file of a quality from 1 to 100, made with stb_image_write's encoder.
inline std::string make_jpeg(const render::Image& image, int quality = 100) {
    std::string out;
    const auto write = [](void* context, void* data, int size) {
        static_cast<std::string*>(context)->append(static_cast<const char*>(data),
                                                   static_cast<std::size_t>(size));
    };
    (void)slash_test_write_jpeg(image.pixels.data(), static_cast<int>(image.width),
                                static_cast<int>(image.height), quality, write, &out);
    return out;
}

} // namespace slash::test
