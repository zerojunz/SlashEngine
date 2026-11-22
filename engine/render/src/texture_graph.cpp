#include "slash/render/texture_graph.hpp"

#include "slash/core/jobs.hpp"
#include "slash/core/math.hpp"

#include <FastNoiseLite.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <string>

// The CPU twin of shaders/texture_graph.hlsl. Built with -ffp-contract=off, so the same on every
// machine; a change to an op goes in the shader too.
namespace slash::render {
namespace {

using Color = std::array<float, 4>;

constexpr std::array<std::string_view, 29> kOpNames{
    "constant",
    "noise",
    "gradient",
    "shape",
    "bricks",
    "checker",
    "waves",
    "image",
    "levels",
    "invert",
    "clamp",
    "gradient_map",
    "hsv",
    "blur",
    "sharpen",
    "warp",
    "transform",
    "edge_detect",
    "grayscale",
    "threshold",
    "posterize",
    "merge",
    "swizzle",
    "normal_from_height",
    "ao_from_height",
    "curvature_from_height",
    "blend",
    "math",
    "remap",
};

constexpr float kTurn = 6.28318530717958647692F;

float clamp01(float v) {
    return std::clamp(v, 0.0F, 1.0F);
}

float fract(float v) {
    return v - std::floor(v);
}

float mix(float a, float b, float t) {
    return a + (b - a) * t;
}

float smoothstep(float from, float to, float s) {
    if (from == to) {
        return s < from ? 0.0F : 1.0F;
    }
    const float t = clamp01((s - from) / (to - from));
    return t * t * (3.0F - 2.0F * t);
}

float luminance(const Color& c) {
    return 0.2126F * c[0] + 0.7152F * c[1] + 0.0722F * c[2];
}

// PCG's hash of a whole number: the shader's pcg_hash.
std::uint32_t hash(std::uint32_t v) {
    const std::uint32_t state = v * 747796405U + 2891336453U;
    const std::uint32_t word = ((state >> ((state >> 28U) + 4U)) ^ state) * 277803737U;
    return (word >> 22U) ^ word;
}

float hash01(std::uint32_t v) {
    return static_cast<float>(hash(v) >> 8U) * (1.0F / 16777216.0F);
}

int to_int(float v) {
    return static_cast<int>(std::lround(v));
}

// A program's slots and how to read them.
struct Slots {
    const TextureGraphProgram& program;
    std::vector<std::vector<float>>& data;
    int width = 0;
    int height = 0;

    [[nodiscard]] int wrap_x(int x) const {
        if (program.tiling) {
            x %= width;
            return x < 0 ? x + width : x;
        }
        return std::clamp(x, 0, width - 1);
    }
    [[nodiscard]] int wrap_y(int y) const {
        if (program.tiling) {
            y %= height;
            return y < 0 ? y + height : y;
        }
        return std::clamp(y, 0, height - 1);
    }
    [[nodiscard]] Color at(std::uint32_t slot, int x, int y) const {
        if (slot == kNoGraphSlot) {
            return {1.0F, 1.0F, 1.0F, 1.0F};
        }
        const std::size_t i =
            (static_cast<std::size_t>(wrap_y(y)) * static_cast<std::size_t>(width) +
             static_cast<std::size_t>(wrap_x(x))) *
            4;
        const std::vector<float>& d = data[slot];
        return {d[i], d[i + 1], d[i + 2], d[i + 3]};
    }
    // Bilinear at a UV point.
    [[nodiscard]] Color sample(std::uint32_t slot, float u, float v) const {
        const float fx = u * static_cast<float>(width) - 0.5F;
        const float fy = v * static_cast<float>(height) - 0.5F;
        const float x0f = std::floor(fx);
        const float y0f = std::floor(fy);
        const float tx = fx - x0f;
        const float ty = fy - y0f;
        const int x0 = static_cast<int>(x0f);
        const int y0 = static_cast<int>(y0f);
        const Color a = at(slot, x0, y0);
        const Color b = at(slot, x0 + 1, y0);
        const Color c = at(slot, x0, y0 + 1);
        const Color d = at(slot, x0 + 1, y0 + 1);
        Color out{};
        for (std::size_t k = 0; k < 4; ++k) {
            out[k] = mix(mix(a[k], b[k], tx), mix(c[k], d[k], tx), ty);
        }
        return out;
    }
};

Color image_sample(const GraphImage& image, bool tiling, float u, float v) {
    const auto w = static_cast<int>(image.width);
    const auto h = static_cast<int>(image.height);
    const auto texel = [&](int x, int y) {
        if (tiling) {
            x = ((x % w) + w) % w;
            y = ((y % h) + h) % h;
        } else {
            x = std::clamp(x, 0, w - 1);
            y = std::clamp(y, 0, h - 1);
        }
        const std::size_t i = (static_cast<std::size_t>(y) * static_cast<std::size_t>(w) +
                               static_cast<std::size_t>(x)) *
                              4;
        return Color{image.texels[i], image.texels[i + 1], image.texels[i + 2],
                     image.texels[i + 3]};
    };
    const float fx = u * static_cast<float>(w) - 0.5F;
    const float fy = v * static_cast<float>(h) - 0.5F;
    const float x0f = std::floor(fx);
    const float y0f = std::floor(fy);
    const float tx = fx - x0f;
    const float ty = fy - y0f;
    const int x0 = static_cast<int>(x0f);
    const int y0 = static_cast<int>(y0f);
    const Color a = texel(x0, y0);
    const Color b = texel(x0 + 1, y0);
    const Color c = texel(x0, y0 + 1);
    const Color d = texel(x0 + 1, y0 + 1);
    Color out{};
    for (std::size_t k = 0; k < 4; ++k) {
        out[k] = mix(mix(a[k], b[k], tx), mix(c[k], d[k], tx), ty);
    }
    return out;
}

// The ramp in a pass's params, as Godot's Gradient with linear interpolation in sRGB.
Color ramp(const std::array<float, kGraphParams>& p, float offset) {
    const int count = std::clamp(to_int(p[kGraphRampAt]), 0, static_cast<int>(kMaxGraphRampPoints));
    const auto point = [&](int i) {
        const std::size_t at = kGraphRampAt + 1 + static_cast<std::size_t>(i) * 5;
        return std::array<float, 5>{p[at], p[at + 1], p[at + 2], p[at + 3], p[at + 4]};
    };
    if (count == 0) {
        return {offset, offset, offset, 1.0F};
    }
    const auto color = [](const std::array<float, 5>& q) { return Color{q[1], q[2], q[3], q[4]}; };
    if (offset <= point(0)[0]) {
        return color(point(0));
    }
    for (int i = 1; i < count; ++i) {
        const auto b = point(i);
        if (offset < b[0]) {
            const auto a = point(i - 1);
            const float t = b[0] > a[0] ? (offset - a[0]) / (b[0] - a[0]) : 0.0F;
            Color out{};
            for (std::size_t k = 0; k < 4; ++k) {
                out[k] = mix(a[k + 1], b[k + 1], t);
            }
            return out;
        }
    }
    return color(point(count - 1));
}

// Godot's FastNoiseLite resource, set up from a pass's params.
struct GraphNoise {
    FastNoiseLite noise;
    FastNoiseLite warp;
    bool warped = false;
    float ox = 0.0F;
    float oy = 0.0F;
    float oz = 0.0F;

    explicit GraphNoise(const std::array<float, kGraphParams>& p) {
        noise.SetNoiseType(static_cast<FastNoiseLite::NoiseType>(to_int(p[0])));
        noise.SetSeed(to_int(p[1]));
        noise.SetFrequency(p[2]);
        noise.SetFractalType(static_cast<FastNoiseLite::FractalType>(to_int(p[3])));
        noise.SetFractalOctaves(to_int(p[4]));
        noise.SetFractalLacunarity(p[5]);
        noise.SetFractalGain(p[6]);
        noise.SetFractalWeightedStrength(p[7]);
        noise.SetFractalPingPongStrength(p[8]);
        noise.SetCellularDistanceFunction(
            static_cast<FastNoiseLite::CellularDistanceFunction>(to_int(p[9])));
        noise.SetCellularReturnType(static_cast<FastNoiseLite::CellularReturnType>(to_int(p[10])));
        noise.SetCellularJitter(p[11]);
        warped = p[12] > 0.5F;
        warp.SetDomainWarpType(static_cast<FastNoiseLite::DomainWarpType>(to_int(p[13])));
        warp.SetSeed(to_int(p[1]));
        warp.SetDomainWarpAmp(p[14]);
        warp.SetFrequency(p[15]);
        // Godot's _convert_domain_warp_fractal_type_enum: none, progressive, independent.
        const int fractal = to_int(p[16]);
        FastNoiseLite::FractalType warp_fractal = FastNoiseLite::FractalType_None;
        if (fractal == 1) {
            warp_fractal = FastNoiseLite::FractalType_DomainWarpProgressive;
        } else if (fractal == 2) {
            warp_fractal = FastNoiseLite::FractalType_DomainWarpIndependent;
        }
        warp.SetFractalType(warp_fractal);
        warp.SetFractalOctaves(to_int(p[17]));
        warp.SetFractalLacunarity(p[18]);
        warp.SetFractalGain(p[19]);
        ox = p[20];
        oy = p[21];
        oz = p[22];
    }

    // Godot's FastNoiseLite::get_noise_2d: the offset added, the domain warped, then the noise.
    [[nodiscard]] float at(float x, float y) const {
        x += ox;
        y += oy;
        if (warped) {
            warp.DomainWarp(x, y);
        }
        return noise.GetNoise(x, y);
    }
};

// One noise value as Godot's Noise::_generate_seamless_image places it, without its 8-bit
// steps: the larger image's quadrants swapped, its skirt blended over the middle seams.
float seamless_noise(const GraphNoise& noise, int x, int y, int width, int height, float sx,
                     float sy, float skirt) {
    const int skirt_w = static_cast<int>(std::max(1.0F, static_cast<float>(width) * skirt));
    const int skirt_h = static_cast<int>(std::max(1.0F, static_cast<float>(height) * skirt));
    const int half_w = width / 2;
    const int half_h = height / 2;
    const int src_w = width + skirt_w;
    const int src_h = height + skirt_h;
    // The source image at (x, y) read as Godot's img_buff with each mode's modulo.
    const auto src = [&](int px, int py, bool alt_x, bool alt_y) {
        const int mx = alt_x ? width : src_w;
        const int my = alt_y ? height : src_h;
        const int sxp = (px + half_w) % mx;
        const int syp = (py + half_h) % my;
        return noise.at(static_cast<float>(sxp) * sx, static_cast<float>(syp) * sy);
    };
    const auto weight = [](int at, int edge, int sk) {
        return 1.0F -
               smoothstep(0.1F, 0.9F, static_cast<float>(at - edge) / static_cast<float>(sk));
    };
    const bool in_x = x >= half_w && x < half_w + skirt_w;
    const bool in_y = y >= half_h && y < half_h + skirt_h;
    if (in_x && in_y) {
        const float wx = weight(x, half_w, skirt_w);
        const float wy = weight(y, half_h, skirt_h);
        const float top = mix(src(x, y, false, false), src(x, y, true, false), wx);
        const float bottom = mix(src(x, y, false, true), src(x, y, true, true), wx);
        return mix(top, bottom, wy);
    }
    const float base = src(x, y, true, true);
    if (in_x) {
        return mix(base, src(x, y, true, false), weight(x, half_w, skirt_w));
    }
    if (in_y) {
        return mix(base, src(x, y, false, true), weight(y, half_h, skirt_h));
    }
    return base;
}

// GradientTexture2D::_get_gradient_offset_at.
float gradient_offset(const std::array<float, kGraphParams>& p, float u, float v) {
    const float fx = p[1];
    const float fy = p[2];
    const float tx = p[3];
    const float ty = p[4];
    if (fx == tx && fy == ty) {
        return 0.0F;
    }
    float pos = 0.0F;
    const int fill = to_int(p[0]);
    if (fill == 1) {
        const float dx = u - fx;
        const float dy = v - fy;
        const float rx = tx - fx;
        const float ry = ty - fy;
        pos = std::sqrt(dx * dx + dy * dy) / std::sqrt(rx * rx + ry * ry);
    } else if (fill == 2) {
        const float dx = std::abs(u - fx);
        const float dy = std::abs(v - fy);
        const float rx = std::abs(tx - fx);
        const float ry = std::abs(ty - fy);
        pos = std::max(dx, dy) / std::max(rx, ry);
    } else {
        const float rx = tx - fx;
        const float ry = ty - fy;
        pos = ((u - fx) * rx + (v - fy) * ry) / (rx * rx + ry * ry);
    }
    const int repeat = to_int(p[5]);
    if (repeat == 1) {
        pos = fract(pos);
    } else if (repeat == 2) {
        pos = std::abs(pos - 2.0F * std::floor(pos * 0.5F + 0.5F));
    } else {
        pos = clamp01(pos);
    }
    return pos;
}

// Rotation by a share of a turn, as the shader's: cos and sin from slash's own polynomial so
// both agree to the bit.
std::array<float, 2> cos_sin_turns(float turns) {
    const Vec2f cs = slash::cos_sin(turns * kTurn);
    return {cs.x, cs.y};
}

float shape_value(const std::array<float, kGraphParams>& p, float u, float v) {
    const float cells_x = std::max(1.0F, p[1]);
    const float cells_y = std::max(1.0F, p[2]);
    float x = fract(u * cells_x) - 0.5F;
    float y = fract(v * cells_y) - 0.5F;
    const auto [c, s] = cos_sin_turns(p[6]);
    const float rx = x * c + y * s;
    const float ry = -x * s + y * c;
    x = rx;
    y = ry;
    const float size = std::max(1e-4F, p[3]) * 0.5F;
    const float soft = std::max(1e-4F, p[5] * size);
    float d = 0.0F; // distance outside the edge
    switch (to_int(p[0])) {
    case 1:
        d = std::max(std::abs(x), std::abs(y)) - size;
        break;
    case 2: {
        const float sides = std::max(3.0F, std::floor(p[4]));
        const float angle = slash::atan2(x, -y);
        const float sector = kTurn / sides;
        const float a = angle - sector * std::floor(angle / sector + 0.5F);
        d = slash::cos_sin(a).x * std::sqrt(x * x + y * y) - size * slash::cos_sin(sector * 0.5F).x;
        break;
    }
    case 3: {
        const float r = std::sqrt(x * x + y * y);
        const float half = std::max(1e-4F, p[7]) * size * 0.5F;
        d = std::abs(r - (size - half)) - half;
        break;
    }
    default:
        d = std::sqrt(x * x + y * y) - size;
        break;
    }
    return 1.0F - smoothstep(-soft, 0.0F, d);
}

Color bricks(const std::array<float, kGraphParams>& p, float u, float v) {
    const float across = std::max(1.0F, p[0]);
    const float down = std::max(1.0F, p[1]);
    const float by = v * down;
    const float row = std::floor(by);
    const float bx = u * across + p[3] * row;
    const float col = std::floor(bx);
    const float fx = bx - col;
    const float fy = by - row;
    // Distance to the nearest edge as a share of the brick, with the brick's aspect.
    const float aspect = (down / across);
    const float ex = std::min(fx, 1.0F - fx) * (1.0F / std::max(aspect, 1e-4F));
    const float ey = std::min(fy, 1.0F - fy);
    const float edge = std::min(ex * aspect, ey);
    const float mortar = p[2] * 0.5F;
    const float bevel = std::max(1e-4F, p[4]);
    const float height = clamp01((edge - mortar) / bevel);
    const auto wrapped_col = static_cast<std::uint32_t>(
        static_cast<int>(col) -
        static_cast<int>(across) * static_cast<int>(std::floor(col / across)));
    const auto row_id = static_cast<std::uint32_t>(
        static_cast<int>(row) - static_cast<int>(down) * static_cast<int>(std::floor(row / down)));
    const std::uint32_t id = wrapped_col + row_id * 4096U;
    const auto seed = static_cast<std::uint32_t>(to_int(p[5]));
    return {edge > mortar ? height : 0.0F, hash01(id ^ hash(seed)),
            static_cast<float>(id % 1024U) / 1023.0F, 1.0F};
}

float waves(const std::array<float, kGraphParams>& p, float u, float v) {
    const auto [c, s] = cos_sin_turns(p[1]);
    const float t = (u * c + v * s) * p[0] + p[3];
    const int kind = to_int(p[2]);
    if (kind == 1) {
        return 1.0F - std::abs(fract(t) * 2.0F - 1.0F);
    }
    if (kind == 2) {
        return fract(t) < 0.5F ? 1.0F : 0.0F;
    }
    return 0.5F + 0.5F * slash::cos_sin(fract(t) * kTurn).y;
}

Color hsv_adjust(Color c, float hue, float sat, float val) {
    const float r = c[0];
    const float g = c[1];
    const float b = c[2];
    const float maxc = std::max({r, g, b});
    const float minc = std::min({r, g, b});
    const float delta = maxc - minc;
    float h = 0.0F;
    if (delta > 0.0F) {
        if (maxc == r) {
            h = (g - b) / delta;
        } else if (maxc == g) {
            h = 2.0F + (b - r) / delta;
        } else {
            h = 4.0F + (r - g) / delta;
        }
        h = fract(h / 6.0F);
    }
    float s = maxc > 0.0F ? delta / maxc : 0.0F;
    float value = maxc;
    h = fract(h + hue);
    s = clamp01(s * sat);
    value = value * val;
    const float h6 = h * 6.0F;
    const float sector = std::floor(h6);
    const float f = h6 - sector;
    const float pv = value * (1.0F - s);
    const float qv = value * (1.0F - s * f);
    const float tv = value * (1.0F - s * (1.0F - f));
    switch (static_cast<int>(sector) % 6) {
    case 0:
        return {value, tv, pv, c[3]};
    case 1:
        return {qv, value, pv, c[3]};
    case 2:
        return {pv, value, tv, c[3]};
    case 3:
        return {pv, qv, value, c[3]};
    case 4:
        return {tv, pv, value, c[3]};
    default:
        return {value, pv, qv, c[3]};
    }
}

float blend_channel(int mode, float a, float b) {
    switch (mode) {
    case 1:
        return a + b;
    case 2:
        return a - b;
    case 3:
        return a * b;
    case 4:
        return 1.0F - (1.0F - a) * (1.0F - b);
    case 5:
        return a < 0.5F ? 2.0F * a * b : 1.0F - 2.0F * (1.0F - a) * (1.0F - b);
    case 6:
        return std::min(a, b);
    case 7:
        return std::max(a, b);
    case 8:
        return std::abs(a - b);
    case 9: // Pegtop's soft light
        return (1.0F - 2.0F * b) * a * a + 2.0F * b * a;
    case 10:
        return b > 0.0F ? a / b : 1.0F;
    default:
        return b;
    }
}

float math_channel(int op, float a, float b) {
    switch (op) {
    case 1:
        return a - b;
    case 2:
        return a * b;
    case 3:
        return b != 0.0F ? a / b : 0.0F;
    case 4:
        return std::min(a, b);
    case 5:
        return std::max(a, b);
    case 6:
        return a > 0.0F ? slash::exp(b * slash::ln(a)) : 0.0F;
    case 7:
        return std::abs(a);
    case 8:
        return fract(a);
    case 9:
        return 1.0F - a;
    default:
        return a + b;
    }
}

float channel_of(const Color& c, int which) {
    if (which >= 0 && which < 4) {
        return c[static_cast<std::size_t>(which)];
    }
    return which == 5 ? 1.0F : 0.0F;
}

// One pixel of a pass.
Color run_pixel(const GraphPass& pass, const Slots& slots, const std::vector<GraphNoise>& noises,
                std::size_t noise_index, int x, int y) {
    const auto& p = pass.params;
    const float u = (static_cast<float>(x) + 0.5F) / static_cast<float>(slots.width);
    const float v = (static_cast<float>(y) + 0.5F) / static_cast<float>(slots.height);
    const auto in = [&](std::size_t k) { return slots.at(pass.in.at(k), x, y); };
    switch (pass.op) {
    case GraphOp::Constant:
        return {p[0], p[1], p[2], p[3]};
    case GraphOp::Noise: {
        const GraphNoise& noise = noises.at(noise_index);
        const float sx = p[23] / static_cast<float>(slots.width);
        const float sy = p[24] / static_cast<float>(slots.height);
        const float n = p[25] > 0.5F
                            ? seamless_noise(noise, x, y, slots.width, slots.height, sx, sy, p[26])
                            : noise.at(static_cast<float>(x) * sx, static_cast<float>(y) * sy);
        const float g = n * 0.5F + 0.5F;
        return {g, g, g, 1.0F};
    }
    case GraphOp::Gradient:
        return ramp(p, gradient_offset(p, u, v));
    case GraphOp::Shape: {
        const float g = shape_value(p, u, v);
        return {g, g, g, 1.0F};
    }
    case GraphOp::Bricks:
        return bricks(p, u, v);
    case GraphOp::Checker: {
        const int cx = static_cast<int>(std::floor(u * std::max(1.0F, p[0])));
        const int cy = static_cast<int>(std::floor(v * std::max(1.0F, p[1])));
        const float g = ((cx + cy) & 1) != 0 ? 1.0F : 0.0F;
        return {g, g, g, 1.0F};
    }
    case GraphOp::Waves: {
        const float g = waves(p, u, v);
        return {g, g, g, 1.0F};
    }
    case GraphOp::Image: {
        const auto index = static_cast<std::size_t>(to_int(p[0]));
        return image_sample(slots.program.images.at(index), slots.program.tiling, u, v);
    }
    case GraphOp::Levels: {
        Color c = in(0);
        const float range = std::max(1e-6F, p[1] - p[0]);
        const float gamma = std::max(1e-4F, p[2]);
        for (std::size_t k = 0; k < 3; ++k) {
            const float t = clamp01((c[k] - p[0]) / range);
            float curved = t;
            if (gamma != 1.0F) {
                curved = t > 0.0F ? slash::exp(slash::ln(t) / gamma) : 0.0F;
            }
            c[k] = mix(p[3], p[4], curved);
        }
        return c;
    }
    case GraphOp::Invert: {
        Color c = in(0);
        for (std::size_t k = 0; k < 3; ++k) {
            c[k] = 1.0F - c[k];
        }
        return c;
    }
    case GraphOp::Clamp: {
        Color c = in(0);
        for (float& k : c) {
            k = clamp01(k);
        }
        return c;
    }
    case GraphOp::GradientMap:
        return ramp(p, clamp01(luminance(in(0))));
    case GraphOp::Hsv:
        return hsv_adjust(in(0), p[0], p[1], p[2]);
    case GraphOp::Blur: {
        const int radius = std::clamp(to_int(p[0]), 0, 256);
        const bool across = to_int(p[1]) == 0;
        Color sum{};
        float total = 0.0F;
        for (int d = -radius; d <= radius; ++d) {
            const float t = static_cast<float>(d) / static_cast<float>(radius + 1);
            const float w0 = 1.0F - t * t;
            const float w = w0 * w0;
            const Color c =
                across ? slots.at(pass.in[0], x + d, y) : slots.at(pass.in[0], x, y + d);
            for (std::size_t k = 0; k < 4; ++k) {
                sum[k] += c[k] * w;
            }
            total += w;
        }
        for (float& k : sum) {
            k /= total;
        }
        return sum;
    }
    case GraphOp::Sharpen: {
        const std::uint32_t s = pass.in[0];
        const Color c = slots.at(s, x, y);
        const Color l = slots.at(s, x - 1, y);
        const Color r = slots.at(s, x + 1, y);
        const Color t = slots.at(s, x, y - 1);
        const Color b = slots.at(s, x, y + 1);
        Color out = c;
        for (std::size_t k = 0; k < 3; ++k) {
            out[k] = c[k] + p[0] * (c[k] - (l[k] + r[k] + t[k] + b[k]) * 0.25F);
        }
        return out;
    }
    case GraphOp::Warp: {
        float du = 0.0F;
        float dv = 0.0F;
        if (to_int(p[1]) == 1) {
            const auto [c, s] = cos_sin_turns(p[2]);
            const float g = slots.at(pass.in[1], x, y)[0];
            du = c * g * p[0];
            dv = s * g * p[0];
        } else {
            const std::uint32_t m = pass.in[1];
            const float gx = luminance(slots.at(m, x + 1, y)) - luminance(slots.at(m, x - 1, y));
            const float gy = luminance(slots.at(m, x, y + 1)) - luminance(slots.at(m, x, y - 1));
            du = gx * 0.5F * p[0];
            dv = gy * 0.5F * p[0];
        }
        return slots.sample(pass.in[0], u + du, v + dv);
    }
    case GraphOp::Transform: {
        const auto [c, s] = cos_sin_turns(p[2]);
        const float cu = u - 0.5F;
        const float cv = v - 0.5F;
        const float ru = cu * c + cv * s;
        const float rv = -cu * s + cv * c;
        float su = (ru * p[0]) + 0.5F + p[3];
        float sv = (rv * p[1]) + 0.5F + p[4];
        if (slots.program.tiling) {
            su = fract(su);
            sv = fract(sv);
        }
        return slots.sample(pass.in[0], su, sv);
    }
    case GraphOp::EdgeDetect: {
        const std::uint32_t s = pass.in[0];
        const auto l = [&](int dx, int dy) { return luminance(slots.at(s, x + dx, y + dy)); };
        const float gx =
            (l(1, -1) + 2.0F * l(1, 0) + l(1, 1)) - (l(-1, -1) + 2.0F * l(-1, 0) + l(-1, 1));
        const float gy =
            (l(-1, 1) + 2.0F * l(0, 1) + l(1, 1)) - (l(-1, -1) + 2.0F * l(0, -1) + l(1, -1));
        const float g = std::sqrt(gx * gx + gy * gy) * p[0];
        return {g, g, g, 1.0F};
    }
    case GraphOp::Grayscale: {
        const Color c = in(0);
        const float g = luminance(c);
        return {g, g, g, c[3]};
    }
    case GraphOp::Threshold: {
        const Color c = in(0);
        const float g = smoothstep(p[0] - p[1], p[0] + p[1], luminance(c));
        return {g, g, g, c[3]};
    }
    case GraphOp::Posterize: {
        Color c = in(0);
        const float steps = std::max(1.0F, std::floor(p[0]));
        for (std::size_t k = 0; k < 3; ++k) {
            c[k] = std::floor(clamp01(c[k]) * steps + 0.5F) / steps;
        }
        return c;
    }
    case GraphOp::Merge: {
        Color out{};
        for (std::size_t k = 0; k < 4; ++k) {
            out[k] =
                pass.in.at(k) == kNoGraphSlot ? p.at(4 + k) : channel_of(in(k), to_int(p.at(k)));
        }
        return out;
    }
    case GraphOp::Channels: {
        const Color c = in(0);
        return {channel_of(c, to_int(p[0])), channel_of(c, to_int(p[1])),
                channel_of(c, to_int(p[2])), channel_of(c, to_int(p[3]))};
    }
    case GraphOp::NormalFromHeight: {
        const std::uint32_t s = pass.in[0];
        const float dx = (slots.at(s, x + 1, y)[0] - slots.at(s, x - 1, y)[0]) * 0.5F * p[0];
        const float dy = (slots.at(s, x, y + 1)[0] - slots.at(s, x, y - 1)[0]) * 0.5F * p[0];
        // Green up: a height rising down the image tilts the normal toward -y in UV, +y in
        // tangent space.
        const float nx = -dx;
        const float ny = dy;
        const float len = std::sqrt(nx * nx + ny * ny + 1.0F);
        return {0.5F + 0.5F * nx / len, 0.5F + 0.5F * ny / len, 0.5F + 0.5F / len, 1.0F};
    }
    case GraphOp::AoFromHeight: {
        const std::uint32_t s = pass.in[0];
        const float h = slots.at(s, x, y)[0];
        const int radius = std::clamp(to_int(p[0]), 1, 64);
        constexpr std::array<std::array<int, 2>, 8> kDirs{
            {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {-1, -1}, {1, -1}, {-1, 1}}};
        float occlusion = 0.0F;
        for (const auto& dir : kDirs) {
            float most = 0.0F;
            for (int step = 1; step <= radius; ++step) {
                const float rise = slots.at(s, x + dir[0] * step, y + dir[1] * step)[0] - h;
                most = std::max(most, rise * static_cast<float>(radius) / static_cast<float>(step));
            }
            occlusion += clamp01(most);
        }
        const float ao = clamp01(1.0F - occlusion * 0.125F * p[1]);
        return {ao, ao, ao, 1.0F};
    }
    case GraphOp::CurvatureFromHeight: {
        const std::uint32_t s = pass.in[0];
        const float lap = slots.at(s, x + 1, y)[0] + slots.at(s, x - 1, y)[0] +
                          slots.at(s, x, y + 1)[0] + slots.at(s, x, y - 1)[0] -
                          4.0F * slots.at(s, x, y)[0];
        const float g = clamp01(0.5F - lap * p[0]);
        return {g, g, g, 1.0F};
    }
    case GraphOp::Blend: {
        const Color a = in(0);
        const Color b = in(1);
        const float mask = pass.in[2] == kNoGraphSlot ? 1.0F : in(2)[0];
        const float t = clamp01(p[1] * mask * b[3]);
        const int mode = to_int(p[0]);
        Color out = a;
        for (std::size_t k = 0; k < 3; ++k) {
            out[k] = mix(a[k], blend_channel(mode, a[k], b[k]), t);
        }
        return out;
    }
    case GraphOp::Math: {
        const Color a = in(0);
        const Color b = pass.in[1] == kNoGraphSlot ? Color{p[2], p[3], p[4], p[5]} : in(1);
        const int op = to_int(p[0]);
        Color out{};
        for (std::size_t k = 0; k < 4; ++k) {
            out[k] = math_channel(op, a[k], b[k]);
        }
        return out;
    }
    case GraphOp::Remap: {
        Color c = in(0);
        const float range = p[1] - p[0];
        for (std::size_t k = 0; k < 3; ++k) {
            float t = range != 0.0F ? (c[k] - p[0]) / range : 0.0F;
            if (p[4] > 0.5F) {
                t = clamp01(t);
            }
            c[k] = mix(p[2], p[3], t);
        }
        return c;
    }
    }
    return {0.0F, 0.0F, 0.0F, 1.0F};
}

} // namespace

std::string_view graph_op_name(GraphOp op) {
    const auto i = static_cast<std::size_t>(op);
    return i < kOpNames.size() ? kOpNames.at(i) : std::string_view("unknown");
}

std::uint64_t texture_graph_bytes(const TextureGraphProgram& program) {
    std::uint64_t bytes = std::uint64_t{program.slots} * program.width * program.height * 16U;
    for (const GraphImage& image : program.images) {
        bytes += image.texels.size() * sizeof(float);
    }
    return bytes;
}

Result<void> check_texture_graph(const TextureGraphProgram& program) {
    if (program.width == 0 || program.height == 0 || program.width > kMaxGraphSize ||
        program.height > kMaxGraphSize) {
        return make_error(ErrorCode::InvalidArgument, "a texture graph bakes at 1 to " +
                                                          std::to_string(kMaxGraphSize) +
                                                          " pixels a side");
    }
    if (program.slots == 0 || program.slots > kMaxGraphSlots) {
        return make_error(ErrorCode::InvalidArgument,
                          "a texture graph has 1 to " + std::to_string(kMaxGraphSlots) + " slots");
    }
    if (program.passes.empty() || program.passes.size() > kMaxGraphPasses) {
        return make_error(ErrorCode::InvalidArgument, "a texture graph has 1 to " +
                                                          std::to_string(kMaxGraphPasses) +
                                                          " passes");
    }
    if (program.images.size() > kMaxGraphImages) {
        return make_error(ErrorCode::InvalidArgument, "a texture graph samples at most " +
                                                          std::to_string(kMaxGraphImages) +
                                                          " images");
    }
    for (const GraphImage& image : program.images) {
        if (image.width == 0 || image.height == 0 ||
            image.texels.size() != std::size_t{image.width} * image.height * 4) {
            return make_error(ErrorCode::InvalidArgument,
                              "a texture graph's image has width x height x 4 floats");
        }
    }
    if (texture_graph_bytes(program) > kMaxGraphBytes) {
        return make_error(ErrorCode::InvalidArgument,
                          "a texture graph's slots and images take more than 1 GiB; bake it "
                          "smaller or with fewer nodes");
    }
    std::vector<bool> written(program.slots, false);
    for (std::size_t i = 0; i < program.passes.size(); ++i) {
        const GraphPass& pass = program.passes[i];
        if (static_cast<std::size_t>(pass.op) >= kOpNames.size()) {
            return make_error(ErrorCode::InvalidArgument,
                              "pass " + std::to_string(i) + " has an unknown op");
        }
        if (pass.out >= program.slots) {
            return make_error(ErrorCode::InvalidArgument,
                              "pass " + std::to_string(i) + " writes a slot past the last");
        }
        for (const std::uint32_t in : pass.in) {
            if (in == kNoGraphSlot) {
                continue;
            }
            if (in >= program.slots || !written[in]) {
                return make_error(ErrorCode::InvalidArgument,
                                  "pass " + std::to_string(i) +
                                      " reads a slot no earlier pass wrote");
            }
            if (in == pass.out) {
                return make_error(ErrorCode::InvalidArgument,
                                  "pass " + std::to_string(i) + " reads the slot it writes");
            }
        }
        if (pass.op == GraphOp::Image &&
            static_cast<std::size_t>(to_int(pass.params[0])) >= program.images.size()) {
            return make_error(ErrorCode::InvalidArgument,
                              "pass " + std::to_string(i) + " samples an image the program lacks");
        }
        written[pass.out] = true;
    }
    return {};
}

Result<std::vector<GraphTexels>> run_texture_graph(const TextureGraphProgram& program,
                                                   std::span<const std::uint32_t> outputs,
                                                   jobs::Pool* pool) {
    if (auto checked = check_texture_graph(program); !checked) {
        return checked.error();
    }
    for (const std::uint32_t slot : outputs) {
        if (slot >= program.slots) {
            return make_error(ErrorCode::InvalidArgument, "an output names a slot past the last");
        }
    }
    const auto width = static_cast<int>(program.width);
    const auto height = static_cast<int>(program.height);
    const std::size_t pixels = std::size_t{program.width} * program.height;
    std::vector<std::vector<float>> data(program.slots);
    Slots slots{program, data, width, height};
    // Each noise pass's FastNoiseLite objects, made once.
    std::vector<GraphNoise> noises;
    std::vector<std::size_t> noise_of(program.passes.size(), 0);
    for (std::size_t i = 0; i < program.passes.size(); ++i) {
        if (program.passes[i].op == GraphOp::Noise) {
            noise_of[i] = noises.size();
            noises.emplace_back(program.passes[i].params);
        }
    }
    for (std::size_t i = 0; i < program.passes.size(); ++i) {
        const GraphPass& pass = program.passes[i];
        std::vector<float> out(pixels * 4);
        const auto row = [&](std::size_t y) {
            for (int x = 0; x < width; ++x) {
                const Color c = run_pixel(pass, slots, noises, noise_of[i], x, static_cast<int>(y));
                const std::size_t at = (y * program.width + static_cast<std::size_t>(x)) * 4;
                out[at] = c[0];
                out[at + 1] = c[1];
                out[at + 2] = c[2];
                out[at + 3] = c[3];
            }
        };
        if (pool != nullptr) {
            pool->parallel_for(program.height, row);
        } else {
            for (std::size_t y = 0; y < program.height; ++y) {
                row(y);
            }
        }
        data[pass.out] = std::move(out);
    }
    std::vector<GraphTexels> result;
    result.reserve(outputs.size());
    for (const std::uint32_t slot : outputs) {
        result.push_back(data[slot].empty() ? GraphTexels(pixels * 4, 0.0F) : data[slot]);
    }
    return result;
}

} // namespace slash::render
