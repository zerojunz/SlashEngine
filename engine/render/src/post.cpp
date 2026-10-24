#include "post_shading.hpp"

#include "slash/core/math.hpp"
#include "slash/render/image.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>

namespace slash::render::detail {

namespace {

// x to the power y for x of 0 or more, the same bits on every platform.
float power(float x, float y) {
    if (!(x > 0.0F)) {
        return 0.0F;
    }
    return slash::exp(y * slash::ln(x));
}

constexpr float kLn2 = std::numbers::ln2_v<float>;

// The 3x3 matrix whose rows are r, times the column c, as HLSL's mul(float3x3(r...), c).
Float3 rows_times(const std::array<Float3, 3>& r, const Float3& c) {
    Float3 out{};
    for (std::size_t i = 0; i < 3; ++i) {
        out.at(i) = (r.at(i)[0] * c[0]) + (r.at(i)[1] * c[1]) + (r.at(i)[2] * c[2]);
    }
    return out;
}

// The row c times the 3x3 matrix whose rows are r, as HLSL's mul(c, float3x3(r...)).
Float3 times_rows(const Float3& c, const std::array<Float3, 3>& r) {
    Float3 out{};
    for (std::size_t j = 0; j < 3; ++j) {
        out.at(j) = (c[0] * r[0].at(j)) + (c[1] * r[1].at(j)) + (c[2] * r[2].at(j));
    }
    return out;
}

Float3 tonemap_reinhard(const Float3& color, float white) {
    const float white_squared = white * white;
    Float3 out{};
    for (std::size_t i = 0; i < 3; ++i) {
        const float c = color.at(i);
        const float white_squared_color = white_squared * c;
        out.at(i) = (white_squared_color + (c * c)) / (white_squared_color + white_squared);
    }
    return out;
}

Float3 tonemap_filmic(const Float3& color, float white) {
    constexpr float kA = 0.22F * 2.0F * 2.0F;
    constexpr float kB = 0.30F * 2.0F;
    constexpr float kC = 0.10F;
    constexpr float kD = 0.20F;
    constexpr float kE = 0.01F;
    constexpr float kF = 0.30F;
    const auto curve = [](float x) {
        return (((x * ((kA * x) + (kC * kB))) + (kD * kE)) / ((x * ((kA * x) + kB)) + (kD * kF))) -
               (kE / kF);
    };
    const float mapped_white = curve(white);
    return {curve(color[0]) / mapped_white, curve(color[1]) / mapped_white,
            curve(color[2]) / mapped_white};
}

Float3 tonemap_aces(const Float3& color, float white) {
    constexpr float kBias = 1.8F;
    constexpr float kA = 0.0245786F;
    constexpr float kB = 0.000090537F;
    constexpr float kC = 0.983729F;
    constexpr float kD = 0.432951F;
    constexpr float kE = 0.238081F;
    const std::array<Float3, 3> rgb_to_rrt{
        {{0.59719F * kBias, 0.35458F * kBias, 0.04823F * kBias},
         {0.07600F * kBias, 0.90834F * kBias, 0.01566F * kBias},
         {0.02840F * kBias, 0.13383F * kBias, 0.83777F * kBias}}};
    const std::array<Float3, 3> odt_to_rgb{{{1.60475F, -0.53108F, -0.07367F},
                                            {-0.10208F, 1.10813F, -0.00605F},
                                            {-0.00327F, -0.07276F, 1.07602F}}};
    const auto curve = [](float x) { return ((x * (x + kA)) - kB) / ((x * ((kC * x) + kD)) + kE); };
    const Float3 rrt = rows_times(rgb_to_rrt, color);
    const Float3 mapped = rows_times(odt_to_rgb, {curve(rrt[0]), curve(rrt[1]), curve(rrt[2])});
    const float mapped_white = curve(white * kBias);
    return {mapped[0] / mapped_white, mapped[1] / mapped_white, mapped[2] / mapped_white};
}

float agx_contrast_approx(float x) {
    const float x2 = x * x;
    const float x4 = x2 * x2;
    return (0.021F * x) + (4.0111F * x2) - (25.682F * x2 * x) + (70.359F * x4) -
           (74.778F * x4 * x) + (27.069F * x4 * x2);
}

Float3 tonemap_agx(const Float3& color) {
    const std::array<Float3, 3> inset{
        {{0.54490813676363087053F, 0.14044005884001287035F, 0.088827411851915368603F},
         {0.37377945959812267119F, 0.75410959864013760045F, 0.17887712465043811023F},
         {0.081384976686407536266F, 0.10543358536857773485F, 0.73224999956948382528F}}};
    const std::array<Float3, 3> outset{
        {{1.9645509602733325934F, -0.29932243390911083839F, -0.16436833806080403409F},
         {-0.85585845117807513559F, 1.3264510741502356555F, -0.23822464068860595117F},
         {-0.10886710826831608324F, -0.027084020983874825605F, 1.402665347143271889F}}};
    constexpr float kMinEv = -12.4739311883324F;
    constexpr float kMaxEv = 4.02606881166759F;
    const Float3 floored{std::max(color[0], 2e-10F), std::max(color[1], 2e-10F),
                         std::max(color[2], 2e-10F)};
    const Float3 inset_color = times_rows(floored, inset);
    Float3 curved{};
    for (std::size_t i = 0; i < 3; ++i) {
        const float log2 = std::clamp(slash::ln(inset_color.at(i)) / kLn2, kMinEv, kMaxEv);
        const float encoded = (log2 - kMinEv) / (kMaxEv - kMinEv);
        curved.at(i) = power(agx_contrast_approx(encoded), 2.4F);
    }
    return times_rows(curved, outset);
}

float smoothstep(float low, float high, float x) {
    const float t = std::clamp((x - low) / (high - low), 0.0F, 1.0F);
    return t * t * (3.0F - (2.0F * t));
}

float lerp(float a, float b, float t) {
    return a + ((b - a) * t);
}

Float4 tap(const Image& image, float u, float v) {
    return sample_image(image, Filter::Linear, false, u, v);
}

Float4 add4(const Float4& a, const Float4& b, float weight = 1.0F) {
    return {a[0] + (b[0] * weight), a[1] + (b[1] * weight), a[2] + (b[2] * weight),
            a[3] + (b[3] * weight)};
}

Float4 scale4(const Float4& a, float s) {
    return {a[0] * s, a[1] * s, a[2] * s, a[3] * s};
}

Float4 five_taps(const Image& image, float u, float v, float hx, float hy) {
    Float4 color = scale4(tap(image, u, v), 4.0F);
    color = add4(color, tap(image, u - hx, v - hy));
    color = add4(color, tap(image, u + hx, v + hy));
    color = add4(color, tap(image, u - hx, v + hy));
    color = add4(color, tap(image, u + hx, v - hy));
    return color;
}

Float4 eight_taps(const Image& image, float u, float v, float hx, float hy) {
    Float4 color = tap(image, u - (hx * 2.0F), v);
    color = add4(color, tap(image, u - hx, v + hy), 2.0F);
    color = add4(color, tap(image, u, v + (hy * 2.0F)));
    color = add4(color, tap(image, u + hx, v + hy), 2.0F);
    color = add4(color, tap(image, u + (hx * 2.0F), v));
    color = add4(color, tap(image, u + hx, v - hy), 2.0F);
    color = add4(color, tap(image, u, v - (hy * 2.0F)));
    color = add4(color, tap(image, u - hx, v - hy), 2.0F);
    return scale4(color, 1.0F / 12.0F);
}

Float3 lut_3d(const Image& lut, float size, const Float3& color) {
    Float3 texel{};
    for (std::size_t i = 0; i < 3; ++i) {
        texel.at(i) = std::clamp((color.at(i) * size) - 0.5F, 0.0F, size - 1.0F);
    }
    float slice = std::min(std::floor(texel[2]), size - 2.0F);
    float blend = texel[2] - slice;
    if (size < 2.0F) {
        slice = 0.0F;
        blend = 0.0F;
    }
    const float width = size * size;
    const float v = (texel[1] + 0.5F) / size;
    const Float4 low = tap(lut, (texel[0] + 0.5F + (slice * size)) / width, v);
    const Float4 high = tap(lut, (texel[0] + 0.5F + ((slice + 1.0F) * size)) / width, v);
    return {lerp(low[0], high[0], blend), lerp(low[1], high[1], blend),
            lerp(low[2], high[2], blend)};
}

std::uint8_t to_byte(float value) {
    return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F));
}

Float4 post_pixel(const PostUniforms& uniforms, const Image& source, const Image& glow,
                  const Image& lut, float u, float v) {
    const float hx = uniforms.pixel[2] * 0.5F;
    const float hy = uniforms.pixel[3] * 0.5F;
    const auto pass = static_cast<PostPass>(uniforms.options[0]);
    if (pass == PostPass::GlowFilter) {
        const Float4 taps = scale4(five_taps(source, u, v, hx, hy), 1.0F / 8.0F);
        const float brightest = std::max(taps[0], std::max(taps[1], taps[2]));
        const float feedback =
            std::max(smoothstep(uniforms.glow[2], uniforms.glow[2] + uniforms.glow[3], brightest),
                     uniforms.glow[1]);
        const float cap = uniforms.grade[1];
        return {std::min(taps[0] * feedback, cap), std::min(taps[1] * feedback, cap),
                std::min(taps[2] * feedback, cap), 1.0F};
    }
    if (pass == PostPass::GlowDownsample) {
        return scale4(five_taps(source, u, v, hx, hy), 1.0F / 8.0F);
    }
    if (pass == PostPass::GlowUpsample) {
        return eight_taps(source, u, v, hx, hy);
    }

    Float4 color = tap(source, u, v);
    const std::uint32_t flags = uniforms.options[2];
    if ((flags & 1U) != 0) {
        const Float4 bloom = eight_taps(glow, u, v, hx, hy);
        for (std::size_t i = 0; i < 3; ++i) {
            const float g = std::clamp(bloom.at(i) * uniforms.glow[0], 0.0F, 1.0F);
            color.at(i) = std::max(color.at(i) + g - (color.at(i) * g), 0.0F);
        }
    }
    Float3 rgb =
        linear_to_srgb(tonemap(srgb_to_linear({color[0], color[1], color[2]}), uniforms.grade[0],
                               static_cast<Tonemapper>(uniforms.options[1])));
    if ((flags & 2U) != 0) {
        for (std::size_t i = 0; i < 3; ++i) {
            rgb.at(i) = lerp(0.0F, rgb.at(i), uniforms.adjust[0]);
            rgb.at(i) = lerp(0.5F, rgb.at(i), uniforms.adjust[1]);
        }
        const float gray = (rgb[0] + rgb[1] + rgb[2]) * 0.33333F;
        for (std::size_t i = 0; i < 3; ++i) {
            rgb.at(i) = lerp(gray, rgb.at(i), uniforms.adjust[2]);
        }
        const std::uint32_t correction = flags >> 2U;
        if (correction == kCorrection1D) {
            for (std::size_t i = 0; i < 3; ++i) {
                rgb.at(i) = tap(lut, rgb.at(i), 0.5F).at(i);
            }
        } else if (correction == kCorrection3D) {
            rgb = lut_3d(lut, uniforms.grade[2], rgb);
        }
    }
    return {rgb[0], rgb[1], rgb[2], color[3]};
}

} // namespace

bool post_processes(const PostProcess& post) {
    return post.glow || post.adjustments;
}

float color_stage(const Scene3D& scene) {
    if (scene.method == RenderMethod::ForwardPlus) {
        return 2.0F;
    }
    return post_processes(scene.post) ? 0.0F : 1.0F;
}

std::array<PostSize, kGlowLevels> glow_sizes(std::uint32_t width, std::uint32_t height) {
    std::array<PostSize, kGlowLevels> out{};
    PostSize size{.width = width, .height = height};
    for (PostSize& level : out) {
        size = {.width = std::max(size.width >> 1U, 4U), .height = std::max(size.height >> 1U, 4U)};
        level = size;
    }
    return out;
}

std::uint32_t correction_kind(std::uint32_t width, std::uint32_t height) {
    if (width == 0 || height == 0) {
        return 0;
    }
    if (height == 1) {
        return kCorrection1D;
    }
    if (std::uint64_t{height} * height == width) {
        return kCorrection3D;
    }
    return 0;
}

PostUniforms post_uniforms(const PostProcess& post, PostPass pass, std::uint32_t width,
                           std::uint32_t height, std::uint32_t lut_width,
                           std::uint32_t lut_height) {
    PostUniforms out;
    const float x = 1.0F / static_cast<float>(std::max(width, 1U));
    const float y = 1.0F / static_cast<float>(std::max(height, 1U));
    out.pixel = {x, y, x, y};
    out.glow = {post.glow_intensity, post.glow_bloom, post.glow_hdr_threshold, post.glow_hdr_scale};
    const std::uint32_t correction =
        post.color_correction == TextureId::None ? 0 : correction_kind(lut_width, lut_height);
    out.grade = {post.white, post.glow_hdr_luminance_cap,
                 correction == kCorrection3D ? static_cast<float>(lut_height) : 0.0F, 0.0F};
    out.adjust = {post.brightness, post.contrast, post.saturation, 0.0F};
    std::uint32_t flags = 0;
    if (post.glow) {
        flags |= 1U;
    }
    if (post.adjustments) {
        flags |= 2U | (correction << 2U);
    }
    out.options = {static_cast<std::uint32_t>(pass), static_cast<std::uint32_t>(post.tonemapper),
                   flags, 0};
    return out;
}

Float3 srgb_to_linear(const Float3& c) {
    Float3 out{};
    for (std::size_t i = 0; i < 3; ++i) {
        const float v = c.at(i);
        out.at(i) = v * ((v * ((v * 0.305306011F) + 0.682171111F)) + 0.012522878F);
    }
    return out;
}

Float3 linear_to_srgb(const Float3& c) {
    Float3 out{};
    for (std::size_t i = 0; i < 3; ++i) {
        out.at(i) =
            std::max((1.055F * power(std::max(c.at(i), 0.0F), 0.416666667F)) - 0.055F, 0.0F);
    }
    return out;
}

Float3 tonemap(const Float3& color, float white, Tonemapper tonemapper) {
    const auto positive = [](const Float3& c) {
        return Float3{std::max(c[0], 0.0F), std::max(c[1], 0.0F), std::max(c[2], 0.0F)};
    };
    switch (tonemapper) {
    case Tonemapper::Linear:
        return color;
    case Tonemapper::Reinhard:
        return tonemap_reinhard(positive(color), white);
    case Tonemapper::Filmic:
        return tonemap_filmic(positive(color), white);
    case Tonemapper::Aces:
        return tonemap_aces(positive(color), white);
    case Tonemapper::Agx:
        break;
    }
    return tonemap_agx(color);
}

void run_post_pass(const PostUniforms& uniforms, const Image& source, const Image& glow,
                   const Image& lut, Image& target) {
    target.pixels.resize(std::size_t{target.width} * target.height * 4);
    for (std::uint32_t y = 0; y < target.height; ++y) {
        for (std::uint32_t x = 0; x < target.width; ++x) {
            const float u = (static_cast<float>(x) + 0.5F) * uniforms.pixel[0];
            const float v = (static_cast<float>(y) + 0.5F) * uniforms.pixel[1];
            const Float4 color = post_pixel(uniforms, source, glow, lut, u, v);
            const std::size_t at = ((std::size_t{y} * target.width) + x) * 4;
            for (std::size_t i = 0; i < 4; ++i) {
                target.pixels[at + i] = to_byte(color.at(i));
            }
        }
    }
}

} // namespace slash::render::detail
