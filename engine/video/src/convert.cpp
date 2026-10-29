#include "slash/video/video.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace slash::video {
namespace {

constexpr int kShift = 16;
constexpr std::int64_t kHalf = std::int64_t{1} << (kShift - 1);

struct Weights {
    double kr = 0.2126;
    double kb = 0.0722;
};

Weights weights(Matrix matrix) {
    switch (matrix) {
    case Matrix::Bt601:
        return {0.299, 0.114};
    case Matrix::Bt2020:
        return {0.2627, 0.0593};
    case Matrix::Bt709:
    case Matrix::Identity:
        break;
    }
    return {0.2126, 0.0722};
}

std::int64_t fixed(double value) {
    return std::llround(value * static_cast<double>(std::int64_t{1} << kShift));
}

std::uint8_t clamp_byte(std::int64_t value) {
    return static_cast<std::uint8_t>(std::clamp<std::int64_t>(value, 0, 255));
}

int sample(const void* plane, std::ptrdiff_t stride, std::uint32_t x, std::uint32_t y, bool wide) {
    const auto* row =
        static_cast<const std::uint8_t*>(plane) + (static_cast<std::ptrdiff_t>(y) * stride);
    if (wide) {
        std::uint16_t value = 0;
        std::memcpy(&value, row + (static_cast<std::size_t>(x) * 2), sizeof(value));
        return value;
    }
    return row[x];
}

} // namespace

void to_rgba(const YuvPicture& picture, render::Image& out) {
    out.width = picture.width;
    out.height = picture.height;
    out.pixels.resize(static_cast<std::size_t>(picture.width) * picture.height * 4);
    out.blocks.clear();
    out.block_format = render::TextureFormat::Rgba8;
    const bool wide = picture.bits > 8;
    const int extra = picture.bits - 8;
    const auto max = static_cast<double>((1 << picture.bits) - 1);
    // Y' and C' in 0..255 units from the coded samples, as offsets and scales.
    const std::int64_t y_offset = picture.full_range ? 0 : (16 << extra);
    const std::int64_t c_offset = 128 << extra;
    const double y_scale = picture.full_range ? 255.0 / max : 255.0 / (219 << extra);
    const double c_scale = picture.full_range ? 255.0 / max : 255.0 / (224 << extra);
    const Weights w = weights(picture.matrix);
    const double kg = 1.0 - w.kr - w.kb;
    const std::int64_t ky = fixed(y_scale);
    const std::int64_t cr_r = fixed(2.0 * (1.0 - w.kr) * c_scale);
    const std::int64_t cb_b = fixed(2.0 * (1.0 - w.kb) * c_scale);
    const std::int64_t cb_g = fixed(2.0 * w.kb * (1.0 - w.kb) / kg * c_scale);
    const std::int64_t cr_g = fixed(2.0 * w.kr * (1.0 - w.kr) / kg * c_scale);
    const unsigned sx = picture.ss_x ? 1U : 0U;
    const unsigned sy = picture.ss_y ? 1U : 0U;
    for (std::uint32_t y = 0; y < picture.height; ++y) {
        for (std::uint32_t x = 0; x < picture.width; ++x) {
            const std::int64_t luma =
                sample(picture.planes[0], picture.strides[0], x, y, wide) - y_offset;
            std::int64_t u = 0;
            std::int64_t v = 0;
            if (!picture.monochrome) {
                u = sample(picture.planes[1], picture.strides[1], x >> sx, y >> sy, wide) -
                    c_offset;
                v = sample(picture.planes[2], picture.strides[1], x >> sx, y >> sy, wide) -
                    c_offset;
            }
            std::uint8_t* pixel =
                &out.pixels[((static_cast<std::size_t>(y) * picture.width) + x) * 4];
            if (picture.matrix == Matrix::Identity) {
                // GBR: Y is green, U blue and V red, at Y's range.
                pixel[0] = clamp_byte(((v + c_offset - y_offset) * ky + kHalf) >> kShift);
                pixel[1] = clamp_byte((luma * ky + kHalf) >> kShift);
                pixel[2] = clamp_byte(((u + c_offset - y_offset) * ky + kHalf) >> kShift);
            } else {
                const std::int64_t base = luma * ky;
                pixel[0] = clamp_byte((base + (v * cr_r) + kHalf) >> kShift);
                pixel[1] = clamp_byte((base - (u * cb_g) - (v * cr_g) + kHalf) >> kShift);
                pixel[2] = clamp_byte((base + (u * cb_b) + kHalf) >> kShift);
            }
            pixel[3] = 255;
        }
    }
}

Yuv420 to_yuv420(const render::Image& image, Matrix matrix) {
    Yuv420 out;
    out.width = image.width;
    out.height = image.height;
    const std::uint32_t cw = (image.width + 1) / 2;
    const std::uint32_t ch = (image.height + 1) / 2;
    out.y.resize(static_cast<std::size_t>(image.width) * image.height);
    out.u.resize(static_cast<std::size_t>(cw) * ch);
    out.v.resize(out.u.size());
    const Weights w = weights(matrix == Matrix::Identity ? Matrix::Bt709 : matrix);
    const double kg = 1.0 - w.kr - w.kb;
    // Limited range: Y' = 16 + 219/255 (Kr R + Kg G + Kb B), C' = 128 + 224/255 of the scaled
    // differences.
    const std::int64_t yr = fixed(219.0 / 255.0 * w.kr);
    const std::int64_t yg = fixed(219.0 / 255.0 * kg);
    const std::int64_t yb = fixed(219.0 / 255.0 * w.kb);
    const double cb_scale = 224.0 / 255.0 / (2.0 * (1.0 - w.kb));
    const double cr_scale = 224.0 / 255.0 / (2.0 * (1.0 - w.kr));
    const std::int64_t ur = fixed(-w.kr * cb_scale);
    const std::int64_t ug = fixed(-kg * cb_scale);
    const std::int64_t ub = fixed((1.0 - w.kb) * cb_scale);
    const std::int64_t vr = fixed((1.0 - w.kr) * cr_scale);
    const std::int64_t vg = fixed(-kg * cr_scale);
    const std::int64_t vb = fixed(-w.kb * cr_scale);
    const auto at = [&](std::uint32_t x, std::uint32_t y) {
        x = std::min(x, image.width - 1);
        y = std::min(y, image.height - 1);
        return &image.pixels[((static_cast<std::size_t>(y) * image.width) + x) * 4];
    };
    for (std::uint32_t y = 0; y < image.height; ++y) {
        for (std::uint32_t x = 0; x < image.width; ++x) {
            const std::uint8_t* p = at(x, y);
            out.y[(static_cast<std::size_t>(y) * image.width) + x] =
                clamp_byte(16 + ((p[0] * yr + p[1] * yg + p[2] * yb + kHalf) >> kShift));
        }
    }
    for (std::uint32_t y = 0; y < ch; ++y) {
        for (std::uint32_t x = 0; x < cw; ++x) {
            std::int64_t r = 0;
            std::int64_t g = 0;
            std::int64_t b = 0;
            for (std::uint32_t dy = 0; dy < 2; ++dy) {
                for (std::uint32_t dx = 0; dx < 2; ++dx) {
                    const std::uint8_t* p = at((x * 2) + dx, (y * 2) + dy);
                    r += p[0];
                    g += p[1];
                    b += p[2];
                }
            }
            // The mean of the four, kept in quarters until the end.
            const std::size_t i = (static_cast<std::size_t>(y) * cw) + x;
            out.u[i] = clamp_byte(128 + ((r * ur + g * ug + b * ub + (kHalf * 4)) >> (kShift + 2)));
            out.v[i] = clamp_byte(128 + ((r * vr + g * vg + b * vb + (kHalf * 4)) >> (kShift + 2)));
        }
    }
    return out;
}

} // namespace slash::video
