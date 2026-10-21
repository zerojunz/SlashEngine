#include "slash/core/math.hpp"
#include "slash/render/image.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <vector>

namespace slash::render {
namespace {

// Lanczos with three lobes, as Godot's Image::resize with INTERPOLATE_LANCZOS.
constexpr double kLobes = 3.0;

double sinc(double x) {
    const double pi_x = std::numbers::pi * x;
    return static_cast<double>(slash::cos_sin(static_cast<float>(pi_x)).y) / pi_x;
}

double lanczos(double x) {
    if (x == 0.0) {
        return 1.0;
    }
    if (x <= -kLobes || x >= kLobes) {
        return 0.0;
    }
    return sinc(x) * sinc(x / kLobes);
}

// The source texels one output texel on an axis reads, and how much of each.
struct Taps {
    std::uint32_t first = 0;
    std::vector<float> weights;
};

// For each of to texels on an axis of from texels. Shrinking widens the filter by the scale, so
// every source texel counts; growing keeps it three texels wide.
std::vector<Taps> taps(std::uint32_t from, std::uint32_t to) {
    const double scale = static_cast<double>(from) / static_cast<double>(to);
    const double stretch = std::max(scale, 1.0);
    const double support = kLobes * stretch;
    std::vector<Taps> out(to);
    for (std::uint32_t i = 0; i < to; ++i) {
        const double center = (static_cast<double>(i) + 0.5) * scale;
        const auto first = static_cast<std::int64_t>(std::floor(center - support));
        const auto last = static_cast<std::int64_t>(std::ceil(center + support));
        // Texels past the edges repeat the edge texel, so their weight goes to it.
        const auto low = std::clamp<std::int64_t>(first, 0, from - 1);
        const auto high = std::clamp<std::int64_t>(last, 0, from - 1);
        std::vector<double> weights(static_cast<std::size_t>(high - low + 1), 0.0);
        double total = 0.0;
        for (std::int64_t texel = first; texel <= last; ++texel) {
            const double weight = lanczos((static_cast<double>(texel) + 0.5 - center) / stretch);
            const std::int64_t clamped = std::clamp<std::int64_t>(texel, 0, from - 1);
            weights[static_cast<std::size_t>(clamped - low)] += weight;
            total += weight;
        }
        out[i].first = static_cast<std::uint32_t>(low);
        out[i].weights.reserve(weights.size());
        for (const double weight : weights) {
            out[i].weights.push_back(static_cast<float>(weight / total));
        }
    }
    return out;
}

} // namespace

Image resize_image(const Image& image, std::uint32_t width, std::uint32_t height) {
    Image out{.width = width, .height = height, .pixels = {}};
    if (image.width == 0 || image.height == 0 || width == 0 || height == 0) {
        out.pixels.assign(static_cast<std::size_t>(width) * height * 4, 0);
        return out;
    }
    if (image.width == width && image.height == height) {
        out.pixels = image.pixels;
        return out;
    }
    constexpr std::size_t kChannels = 4;
    // Colors weighted by their alpha, so transparent texels' colors don't bleed into the edges.
    std::vector<float> premultiplied(image.pixels.size());
    for (std::size_t i = 0; i < image.pixels.size(); i += kChannels) {
        const float alpha = static_cast<float>(image.pixels[i + 3]) / 255.0F;
        for (std::size_t c = 0; c < 3; ++c) {
            premultiplied[i + c] = static_cast<float>(image.pixels[i + c]) * alpha;
        }
        premultiplied[i + 3] = static_cast<float>(image.pixels[i + 3]);
    }

    // Across, then down.
    const std::vector<Taps> across = taps(image.width, width);
    std::vector<float> rows(static_cast<std::size_t>(width) * image.height * kChannels, 0.0F);
    for (std::uint32_t y = 0; y < image.height; ++y) {
        const std::size_t row = static_cast<std::size_t>(y) * image.width;
        for (std::uint32_t x = 0; x < width; ++x) {
            const Taps& tap = across[x];
            float* to = &rows[((static_cast<std::size_t>(y) * width) + x) * kChannels];
            for (std::size_t k = 0; k < tap.weights.size(); ++k) {
                const float* from = &premultiplied[(row + tap.first + k) * kChannels];
                for (std::size_t c = 0; c < kChannels; ++c) {
                    to[c] += from[c] * tap.weights[k];
                }
            }
        }
    }
    const std::vector<Taps> down = taps(image.height, height);
    out.pixels.resize(static_cast<std::size_t>(width) * height * kChannels);
    for (std::uint32_t y = 0; y < height; ++y) {
        const Taps& tap = down[y];
        for (std::uint32_t x = 0; x < width; ++x) {
            float sum[kChannels] = {0.0F, 0.0F, 0.0F, 0.0F};
            for (std::size_t k = 0; k < tap.weights.size(); ++k) {
                const float* from = &rows[(((tap.first + k) * width) + x) * kChannels];
                for (std::size_t c = 0; c < kChannels; ++c) {
                    sum[c] += from[c] * tap.weights[k];
                }
            }
            // Lanczos rings past the source's range: clamp, then undo the alpha weighting.
            const float alpha = std::clamp(sum[3], 0.0F, 255.0F);
            std::uint8_t* to = &out.pixels[((static_cast<std::size_t>(y) * width) + x) * kChannels];
            for (std::size_t c = 0; c < 3; ++c) {
                const float color = alpha > 0.0F ? sum[c] * 255.0F / alpha : 0.0F;
                to[c] =
                    static_cast<std::uint8_t>(std::floor(std::clamp(color, 0.0F, 255.0F) + 0.5F));
            }
            to[3] = static_cast<std::uint8_t>(std::floor(alpha + 0.5F));
        }
    }
    return out;
}

} // namespace slash::render
