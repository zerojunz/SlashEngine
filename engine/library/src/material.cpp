#include "slash/library/material.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

namespace slash::library {

namespace {

// Rec. 709 luminance of a pixel, 0 to 255.
int brightness(const render::Image& image, std::uint32_t x, std::uint32_t y) {
    const std::size_t at = (static_cast<std::size_t>(y) * image.width + x) * 4;
    return (54 * image.pixels[at] + 183 * image.pixels[at + 1] + 19 * image.pixels[at + 2]) >> 8;
}

// The difference of two pixels' colors, summed over red, green and blue.
std::int64_t difference(const render::Image& image, std::size_t a, std::size_t b) {
    std::int64_t sum = 0;
    for (std::size_t c = 0; c < 3; ++c) {
        sum += std::abs(static_cast<int>(image.pixels[a * 4 + c]) -
                        static_cast<int>(image.pixels[b * 4 + c]));
    }
    return sum;
}

// The integer square root, rounded down.
std::int64_t isqrt(std::int64_t value) {
    std::int64_t root = 0;
    std::int64_t bit = std::int64_t{1} << 62;
    while (bit > value) {
        bit >>= 2;
    }
    while (bit != 0) {
        if (value >= root + bit) {
            value -= root + bit;
            root = (root >> 1) + bit;
        } else {
            root >>= 1;
        }
        bit >>= 2;
    }
    return root;
}

render::Image blank(const render::Image& like) {
    render::Image out;
    out.width = like.width;
    out.height = like.height;
    out.pixels.resize(static_cast<std::size_t>(like.width) * like.height * 4);
    return out;
}

// How much a copy whose seam runs down the middle counts at x, out of size: nothing at the
// middle, all of it at the edges. The copy whose seam is at the edges takes the rest.
std::int64_t edge_weight(std::uint32_t x, std::uint32_t size) {
    const std::int64_t from_middle = std::abs(2 * static_cast<std::int64_t>(x) + 1 - size);
    return std::min<std::int64_t>(from_middle, size);
}

} // namespace

double seam_ratio(const render::Image& image) {
    const std::uint32_t w = image.width;
    const std::uint32_t h = image.height;
    if (w < 2 || h < 2) {
        return 0.0;
    }
    std::int64_t inside = 0;
    std::int64_t pairs = 0;
    for (std::uint32_t y = 0; y < h; ++y) {
        for (std::uint32_t x = 0; x < w; ++x) {
            const std::size_t at = static_cast<std::size_t>(y) * w + x;
            if (x + 1 < w) {
                inside += difference(image, at, at + 1);
                ++pairs;
            }
            if (y + 1 < h) {
                inside += difference(image, at, at + w);
                ++pairs;
            }
        }
    }
    std::int64_t across = 0;
    for (std::uint32_t y = 0; y < h; ++y) {
        across += difference(image, static_cast<std::size_t>(y) * w,
                             static_cast<std::size_t>(y) * w + w - 1);
    }
    for (std::uint32_t x = 0; x < w; ++x) {
        across += difference(image, x, static_cast<std::size_t>(h - 1) * w + x);
    }
    if (inside == 0) {
        return across == 0 ? 0.0 : 99.0;
    }
    // across / (w + h) against inside / pairs, in hundredths.
    const std::int64_t num = across * pairs * 100;
    const std::int64_t den = inside * (static_cast<std::int64_t>(w) + h);
    const std::int64_t hundredths = (num + den / 2) / den;
    return static_cast<double>(std::min<std::int64_t>(hundredths, 9900)) / 100.0;
}

render::Image make_seamless(const render::Image& image) {
    render::Image out = blank(image);
    const std::uint32_t w = image.width;
    const std::uint32_t h = image.height;
    const std::int64_t total = static_cast<std::int64_t>(w) * h;
    for (std::uint32_t y = 0; y < h; ++y) {
        const std::int64_t moved_y = edge_weight(y, h);
        const std::array<std::int64_t, 2> wy{h - moved_y, moved_y};
        const std::array<std::uint32_t, 2> ys{y, (y + h / 2) % h};
        for (std::uint32_t x = 0; x < w; ++x) {
            const std::int64_t moved_x = edge_weight(x, w);
            const std::array<std::int64_t, 2> wx{w - moved_x, moved_x};
            const std::array<std::uint32_t, 2> xs{x, (x + w / 2) % w};
            std::array<std::int64_t, 3> sum{};
            for (std::size_t j = 0; j < 2; ++j) {
                for (std::size_t i = 0; i < 2; ++i) {
                    const std::int64_t weight = wx[i] * wy[j];
                    const std::size_t from = (static_cast<std::size_t>(ys[j]) * w + xs[i]) * 4;
                    for (std::size_t c = 0; c < 3; ++c) {
                        sum[c] += weight * image.pixels[from + c];
                    }
                }
            }
            const std::size_t to = (static_cast<std::size_t>(y) * w + x) * 4;
            for (std::size_t c = 0; c < 3; ++c) {
                out.pixels[to + c] = static_cast<std::uint8_t>((sum[c] + total / 2) / total);
            }
            out.pixels[to + 3] = 255;
        }
    }
    return out;
}

render::Image normal_map(const render::Image& image) {
    render::Image out = blank(image);
    const std::uint32_t w = image.width;
    const std::uint32_t h = image.height;
    // How flat a slope of one full brightness step a pixel reads, against Sobel's sums.
    constexpr std::int64_t kDepth = 512;
    for (std::uint32_t y = 0; y < h; ++y) {
        const std::uint32_t up = (y + h - 1) % h;
        const std::uint32_t down = (y + 1) % h;
        for (std::uint32_t x = 0; x < w; ++x) {
            const std::uint32_t left = (x + w - 1) % w;
            const std::uint32_t right = (x + 1) % w;
            const auto at = [&](std::uint32_t px, std::uint32_t py) {
                return static_cast<std::int64_t>(brightness(image, px, py));
            };
            const std::int64_t dx = at(right, up) + 2 * at(right, y) + at(right, down) -
                                    at(left, up) - 2 * at(left, y) - at(left, down);
            const std::int64_t dy = at(left, down) + 2 * at(x, down) + at(right, down) -
                                    at(left, up) - 2 * at(x, up) - at(right, up);
            // Height rising to the right tilts the normal left; rising down the image tilts it
            // up the image, which is +y.
            const std::array<std::int64_t, 3> n{-dx, dy, kDepth};
            const std::int64_t length =
                std::max<std::int64_t>(isqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]), 1);
            const std::size_t to = (static_cast<std::size_t>(y) * w + x) * 4;
            for (std::size_t c = 0; c < 3; ++c) {
                // 128 + 127 * n / length, rounded to nearest.
                const std::int64_t scaled = 127 * n[c];
                const std::int64_t step =
                    (scaled >= 0 ? scaled + length / 2 : scaled - length / 2) / length;
                out.pixels[to + c] =
                    static_cast<std::uint8_t>(std::clamp<std::int64_t>(128 + step, 1, 255));
            }
            out.pixels[to + 3] = 255;
        }
    }
    return out;
}

render::Image roughness_map(const render::Image& image) {
    render::Image out = blank(image);
    for (std::uint32_t y = 0; y < image.height; ++y) {
        for (std::uint32_t x = 0; x < image.width; ++x) {
            const int dark = 255 - brightness(image, x, y);
            const auto value = static_cast<std::uint8_t>(140 + (dark * 100 + 127) / 255);
            const std::size_t to = (static_cast<std::size_t>(y) * image.width + x) * 4;
            out.pixels[to] = value;
            out.pixels[to + 1] = value;
            out.pixels[to + 2] = value;
            out.pixels[to + 3] = 255;
        }
    }
    return out;
}

} // namespace slash::library
