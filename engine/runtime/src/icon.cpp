#include "slash/runtime/icon.hpp"

#include "slash/core/log.hpp"
#include "slash/render/image.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <utility>

namespace slash::runtime {
namespace {

// The distance from a point to the segment from a to b.
double segment_distance(double x, double y, double ax, double ay, double bx, double by) {
    const double dx = bx - ax;
    const double dy = by - ay;
    const double t =
        std::clamp((((x - ax) * dx) + ((y - ay) * dy)) / ((dx * dx) + (dy * dy)), 0.0, 1.0);
    const double px = ax + (t * dx) - x;
    const double py = ay + (t * dy) - y;
    return std::sqrt((px * px) + (py * py));
}

std::uint8_t mix(std::uint8_t from, std::uint8_t to, double amount) {
    return static_cast<std::uint8_t>(
        std::floor(static_cast<double>(from) + ((static_cast<double>(to) - from) * amount) + 0.5));
}

} // namespace

render::Image default_icon(std::uint32_t size) {
    render::Image icon{.width = size, .height = size, .pixels = {}};
    icon.pixels.resize(static_cast<std::size_t>(size) * size * 4);
    const auto side = static_cast<double>(size);
    // Full bleed, as phones' masks and macOS's rounded square expect; the slash, from lower left
    // to upper right with round ends, stays inside every platform's mask.
    const double half_width = side * 0.075;
    const double ax = side * 0.36;
    const double ay = side * 0.74;
    const double bx = side * 0.64;
    const double by = side * 0.26;
    for (std::uint32_t y = 0; y < size; ++y) {
        // Lighter at the top: #2b3170 to #161a3d.
        const double fall = (static_cast<double>(y) + 0.5) / side;
        const std::uint8_t r = mix(0x2b, 0x16, fall);
        const std::uint8_t g = mix(0x31, 0x1a, fall);
        const std::uint8_t b = mix(0x70, 0x3d, fall);
        for (std::uint32_t x = 0; x < size; ++x) {
            const double distance = segment_distance(static_cast<double>(x) + 0.5,
                                                     static_cast<double>(y) + 0.5, ax, ay, bx, by);
            // How much of the pixel the slash covers, for smooth edges.
            const double cover = std::clamp(half_width - distance + 0.5, 0.0, 1.0);
            std::uint8_t* pixel = &icon.pixels[((static_cast<std::size_t>(y) * size) + x) * 4];
            pixel[0] = mix(r, 0xff, cover);
            pixel[1] = mix(g, 0xff, cover);
            pixel[2] = mix(b, 0xff, cover);
            pixel[3] = 0xff;
        }
    }
    return icon;
}

Result<render::Image> decode_icon(std::string_view bytes) {
    auto image = render::decode_image(bytes);
    if (!image) {
        return image.error();
    }
    if (image.value().width != image.value().height) {
        return make_error(ErrorCode::InvalidArgument, "icons are square, and this one is " +
                                                          std::to_string(image.value().width) +
                                                          "x" +
                                                          std::to_string(image.value().height));
    }
    image.value().blocks.clear();
    image.value().block_format = render::TextureFormat::Rgba8;
    return image;
}

render::Image window_icon(std::string_view path,
                          const std::function<Result<std::string>(std::string_view path)>& read) {
    if (!path.empty()) {
        auto bytes = read(path);
        auto icon = bytes ? decode_icon(bytes.value()) : Result<render::Image>(bytes.error());
        if (icon) {
            if (icon.value().width <= kWindowIconSize) {
                return std::move(icon).value();
            }
            return render::resize_image(icon.value(), kWindowIconSize, kWindowIconSize);
        }
        log::warn("the icon did not load", {{"icon", path}, {"error", icon.error().message}});
    }
    return default_icon(kWindowIconSize);
}

} // namespace slash::runtime
