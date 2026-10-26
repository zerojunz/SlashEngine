#include "ssao.hpp"

#include "forward_post.hpp"

#include "slash/core/math.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <span>
#include <vector>

namespace slash::render::detail {

namespace {

constexpr float kPi = std::numbers::pi_v<float>;

// The index of column x of row y in rows of the given width.
std::size_t at_index(int x, int y, int width) {
    return (static_cast<std::size_t>(y) * static_cast<std::size_t>(width)) +
           static_cast<std::size_t>(x);
}

// ssao.glsl's sample_pattern: each sample's offset in the disk, its weight, and log2 of its
// distance from the center.
constexpr std::array<Float4, 32> kSamplePattern{{
    {0.78488064F, 0.56661671F, 1.500000F, -0.126083F},
    {0.26022232F, -0.29575172F, 1.500000F, -1.064030F},
    {0.10459357F, 0.08372527F, 1.110000F, -2.730563F},
    {-0.68286800F, 0.04963045F, 1.090000F, -0.498827F},
    {-0.13570161F, -0.64190155F, 1.250000F, -0.532765F},
    {-0.26193795F, -0.08205118F, 0.670000F, -1.783245F},
    {-0.61177456F, 0.66664219F, 0.710000F, -0.044234F},
    {0.43675563F, 0.25119025F, 0.610000F, -1.167283F},
    {0.07884444F, 0.86618668F, 0.640000F, -0.459002F},
    {-0.12790935F, -0.29869005F, 0.600000F, -1.729424F},
    {-0.04031125F, 0.02413622F, 0.600000F, -4.792042F},
    {0.16201244F, -0.52851415F, 0.790000F, -1.067055F},
    {-0.70991218F, 0.47301072F, 0.640000F, -0.335236F},
    {0.03277707F, -0.22349690F, 0.600000F, -1.982384F},
    {0.68921727F, 0.36800742F, 0.630000F, -0.266718F},
    {0.29251814F, 0.37775412F, 0.610000F, -1.422520F},
    {-0.12224089F, 0.96582592F, 0.600000F, -0.426142F},
    {0.11071457F, -0.16131058F, 0.600000F, -2.165947F},
    {0.46562141F, -0.59747696F, 0.600000F, -0.189760F},
    {-0.51548797F, 0.11804193F, 0.600000F, -1.246800F},
    {0.89141309F, -0.42090443F, 0.600000F, 0.028192F},
    {-0.32402530F, -0.01591529F, 0.600000F, -1.543018F},
    {0.60771245F, 0.41635221F, 0.600000F, -0.605411F},
    {0.02379565F, -0.08239821F, 0.600000F, -3.809046F},
    {0.48951152F, -0.23657045F, 0.600000F, -1.189011F},
    {-0.17611565F, -0.81696892F, 0.600000F, -0.513724F},
    {-0.33930185F, -0.20732205F, 0.600000F, -1.698047F},
    {-0.91974425F, 0.05403209F, 0.600000F, 0.062246F},
    {-0.15064627F, -0.14949332F, 0.600000F, -1.896062F},
    {0.53180975F, -0.35210401F, 0.600000F, -0.758838F},
    {0.41487166F, 0.81442589F, 0.600000F, -0.505648F},
    {-0.24106961F, -0.32721516F, 0.600000F, -1.665244F},
}};

// ssao.glsl's num_taps for qualities 0 to 2, and its constants.
constexpr std::array<int, 3> kTaps{3, 5, 12};
constexpr int kAdaptiveBaseTaps = 5;
constexpr int kMaxTaps = 32;
constexpr float kHaloingReduction = 0.6F;
constexpr float kNormalEdgeThreshold = 0.5F;
constexpr float kMipOffset = -4.3F;
constexpr float kLn2 = std::numbers::ln2_v<float>;

using Float2 = std::array<float, 2>;

struct Int2 {
    int x = 0;
    int y = 0;
};

// x to the power y for x of 0 or more, the same bits on every platform.
float power(float x, float y) {
    if (!(x > 0.0F)) {
        return 0.0F;
    }
    return slash::exp(y * slash::ln(x));
}

float log2(float x) {
    return slash::ln(x) / kLn2;
}

// Rounds halfway cases to even, as HLSL's round.
float round_even(float x) {
    return std::nearbyint(x);
}

std::uint8_t to_byte(float value) {
    return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F));
}

// value as the texture's format keeps it.
float stored(SsaoFormat format, float value) {
    switch (format) {
    case SsaoFormat::R16F:
        return round_half(value);
    case SsaoFormat::RG8:
    case SsaoFormat::R8:
        return static_cast<float>(to_byte(value)) / 255.0F;
    case SsaoFormat::R32F:
        break;
    }
    return value;
}

// a / b rounded down, for b above 0.
int floor_div(int a, int b) {
    const int q = a / b;
    return (a % b != 0 && (a < 0) != (b < 0)) ? q - 1 : q;
}

// A texel index in a texture size texels across, mirrored at its edges and repeating, as a
// MIRRORED_REPEAT sampler addresses it.
int mirror(int i, int size) {
    const int period = 2 * size;
    int t = i % period;
    if (t < 0) {
        t += period;
    }
    return t < size ? t : period - 1 - t;
}

float texel(const SsaoImage& image, int x, int y, std::uint32_t channel = 0) {
    const std::size_t at =
        (((static_cast<std::size_t>(y) * image.width) + static_cast<std::size_t>(x)) *
         image.channels) +
        channel;
    return image.values[at];
}

// A texel with its index kept inside the texture, as a CLAMP_TO_EDGE sampler addresses it.
float clamped(const SsaoImage& image, int x, int y, std::uint32_t channel = 0) {
    return texel(image, std::clamp(x, 0, static_cast<int>(image.width) - 1),
                 std::clamp(y, 0, static_cast<int>(image.height) - 1), channel);
}

float mirrored(const SsaoImage& image, int x, int y, std::uint32_t channel = 0) {
    return texel(image, mirror(x, static_cast<int>(image.width)),
                 mirror(y, static_cast<int>(image.height)), channel);
}

// A linearly filtered sample at a texture coordinate, with clamped edges, as Godot's default
// sampler takes it.
float bilinear(const SsaoImage& image, Float2 uv, std::uint32_t channel = 0) {
    const float px = (uv[0] * static_cast<float>(image.width)) - 0.5F;
    const float py = (uv[1] * static_cast<float>(image.height)) - 0.5F;
    const float bx = std::floor(px);
    const float by = std::floor(py);
    const float fx = px - bx;
    const float fy = py - by;
    const int x = static_cast<int>(bx);
    const int y = static_cast<int>(by);
    const float t00 = clamped(image, x, y, channel);
    const float t10 = clamped(image, x + 1, y, channel);
    const float t01 = clamped(image, x, y + 1, channel);
    const float t11 = clamped(image, x + 1, y + 1, channel);
    const float top = t00 + ((t10 - t00) * fx);
    const float bottom = t01 + ((t11 - t01) * fx);
    return top + ((bottom - top) * fy);
}

float dot3(const Float3& a, const Float3& b) {
    return (a[0] * b[0]) + (a[1] * b[1]) + (a[2] * b[2]);
}

float dot4(const Float4& a, const Float4& b) {
    return (a[0] * b[0]) + (a[1] * b[1]) + (a[2] * b[2]) + (a[3] * b[3]);
}

// ss_effects_downsample.glsl's screen_space_to_view_space_depth: a depth buffer value as the
// distance in front of the camera.
float view_depth(const SsaoUniforms& u, float depth) {
    if (u.depth[2] != 0.0F) {
        const float d = (depth * 2.0F) - 1.0F;
        return -((d * (u.depth[1] - u.depth[0])) - (u.depth[1] + u.depth[0])) / 2.0F;
    }
    return u.depth[0] / (u.depth[1] - depth);
}

// mip_smart_average: four depths averaged, those far behind the nearest weighed less.
float smart_average(float s00, float s01, float s10, float s11, float radius_sq) {
    const float closest = std::min(std::min(s00, s01), std::min(s10, s11));
    const float falloff_sq = -1.0F / radius_sq;
    const Float4 depths{s00, s01, s10, s11};
    Float4 weights{};
    for (std::size_t i = 0; i < 4; ++i) {
        const float d = depths.at(i) - closest;
        weights.at(i) = std::clamp((d * d * falloff_sq) + 1.0F, 0.0F, 1.0F);
    }
    return dot4(weights, depths) / dot4(weights, {1.0F, 1.0F, 1.0F, 1.0F});
}

// The depth pass: a slice's linear depth at a level, the level's 2^level x 2^level texels of
// level 0 averaged in pairs of rows and columns, a level at a time, as the downsample's mipmaps
// are. Level 0's texels are depth buffer pixels a step apart, kept inside the buffer.
float depth_pass(const SsaoUniforms& u, const SsaoInputs& inputs, int x, int y) {
    const std::uint32_t slice = u.options[2];
    const std::uint32_t level = u.options[3];
    const int n = 1 << level;
    const int step = static_cast<int>(u.layout[0]);
    const int ox = static_cast<int>(slice % 2U) * (step / 2);
    const int oy = static_cast<int>(slice / 2U) * (step / 2);
    const int width = static_cast<int>(u.frame[0]);
    const int height = static_cast<int>(u.frame[1]);
    std::array<float, 64> values{};
    for (int j = 0; j < n; ++j) {
        for (int i = 0; i < n; ++i) {
            const int px = std::clamp((((x * n) + i) * step) + ox, 0, width - 1);
            const int py = std::clamp((((y * n) + j) * step) + oy, 0, height - 1);
            const float depth = inputs.depth[(static_cast<std::size_t>(py) * u.frame[0]) +
                                             static_cast<std::size_t>(px)];
            values.at(at_index(i, j, n)) = view_depth(u, depth);
        }
    }
    for (int size = n; size > 1; size /= 2) {
        const int half = size / 2;
        for (int j = 0; j < half; ++j) {
            for (int i = 0; i < half; ++i) {
                const auto at = [&](int ci, int cj) { return values.at(at_index(ci, cj, size)); };
                values.at(at_index(i, j, half)) =
                    smart_average(at(2 * i, 2 * j), at(2 * i, (2 * j) + 1), at((2 * i) + 1, 2 * j),
                                  at((2 * i) + 1, (2 * j) + 1), u.depth[3]);
            }
        }
    }
    return values[0];
}

// --- The gather, as ssao.glsl ---

struct GatherTextures {
    std::array<const SsaoImage*, 4> depth{};
    const SsaoImage* normals = nullptr;
    const SsaoImage* base = nullptr;
    const SsaoImage* importance = nullptr;
    const SsaoImage* sum = nullptr;
};

Float3 ndc_to_view(const SsaoUniforms& u, Float2 p, float depth) {
    const float x = (u.ndc[0] * p[0]) + u.ndc[2];
    const float y = (u.ndc[1] * p[1]) + u.ndc[3];
    if (u.depth[2] != 0.0F) {
        return {x, y, depth};
    }
    return {x * depth, y * depth, depth};
}

Float3 load_normal(const SsaoUniforms& u, const SsaoImage& normals, int x, int y) {
    const int cx = std::clamp(x, 0, static_cast<int>(u.frame[0]) - 1);
    const int cy = std::clamp(y, 0, static_cast<int>(u.frame[1]) - 1);
    Float3 n{};
    for (std::uint32_t c = 0; c < 3; ++c) {
        n.at(c) = (texel(normals, cx, cy, c) * 2.0F) - 1.0F;
    }
    const float length = std::sqrt(dot3(n, n));
    return {n[0] / length, n[1] / length, -(n[2] / length)};
}

float pixel_obscurance(const SsaoUniforms& u, const Float3& normal, const Float3& delta,
                       float falloff_sq) {
    const float length_sq = dot3(delta, delta);
    const float n_dot_d = dot3(normal, delta) / std::sqrt(length_sq);
    const float falloff = std::max(0.0F, (length_sq * falloff_sq) + 1.0F);
    return std::max(0.0F, n_dot_d - u.fade[2]) * falloff;
}

// The depth level a sample reads: the nearest mipmap to lod, as a sampler with nearest mipmaps
// picks it, within the levels made.
int sample_level(const SsaoUniforms& u, float lod) {
    if (!(lod > 0.0F)) {
        return 0;
    }
    const float level = std::ceil(lod + 0.5F) - 1.0F;
    return static_cast<int>(std::min(level, static_cast<float>(u.layout[3] - 1U)));
}

// The texel of a depth level a sample offset from pixel id reads: where its texture coordinate,
// (id + offset + 0.75) * half_screen_pixel_size, falls in the level, in integers.
int sample_texel(int id, int offset, std::uint32_t multiplier, int size, std::uint32_t frame) {
    const int numerator = ((4 * (id + offset)) + 3) * static_cast<int>(multiplier) * size;
    return floor_div(numerator, 2 * static_cast<int>(frame));
}

struct Gather {
    const SsaoUniforms& u;
    const GatherTextures& t;
    int quality = 0;
    Int2 id;
    Float2 screen{}; // normalized_screen_pos
    Float3 center{}; // pix_center_pos
    Float3 normal{};
    float falloff_sq = 0.0F;
    float obscurance_sum = 0.0F;
    float weight_sum = 0.0F;

    void tap_inner(Int2 offset, float mip, float weight_mod) {
        const int level = sample_level(u, mip);
        const SsaoImage& depth = *t.depth.at(static_cast<std::size_t>(level));
        const int sx =
            sample_texel(id.x, offset.x, u.layout[1], static_cast<int>(depth.width), u.frame[0]);
        const int sy =
            sample_texel(id.y, offset.y, u.layout[1], static_cast<int>(depth.height), u.frame[1]);
        const float z = mirrored(depth, sx, sy);
        const Float2 uv{(static_cast<float>(offset.x) * u.pixel[0]) + screen[0],
                        (static_cast<float>(offset.y) * u.pixel[1]) + screen[1]};
        const Float3 hit = ndc_to_view(u, uv, z);
        const Float3 delta{hit[0] - center[0], hit[1] - center[1], hit[2] - center[2]};
        const float obscurance = pixel_obscurance(u, normal, delta, falloff_sq);
        float weight = 1.0F;
        if (quality >= 1) {
            float reduce = std::max(0.0F, -delta[2]);
            reduce = std::clamp((reduce * u.adaptive[0]) + 2.0F, 0.0F, 1.0F);
            weight = (kHaloingReduction * reduce) + (1.0F - kHaloingReduction);
        }
        weight *= weight_mod;
        obscurance_sum += obscurance * weight;
        weight_sum += weight;
    }

    void tap(int index, const Float4& rotation, float mip_offset, float weight_mod) {
        const Float4& sample = kSamplePattern.at(static_cast<std::size_t>(index));
        // A row vector times the matrix whose columns are (x, y) and (z, w), as GLSL's vec2 *
        // mat2; offsets far off screen are kept within what an int holds.
        const float fx = (sample[0] * rotation[0]) + (sample[1] * rotation[1]);
        const float fy = (sample[0] * rotation[2]) + (sample[1] * rotation[3]);
        const Int2 offset{static_cast<int>(std::clamp(round_even(fx), -32768.0F, 32768.0F)),
                          static_cast<int>(std::clamp(round_even(fy), -32768.0F, 32768.0F))};
        weight_mod *= sample[2];
        const float mip = quality < 2 ? 0.0F : sample[3] + mip_offset;
        tap_inner(offset, mip, weight_mod);
        tap_inner({-offset.x, -offset.y}, mip, weight_mod);
    }
};

Float4 calculate_edges(float center, float left, float right, float top, float bottom) {
    Float4 edges{left - center, right - center, top - center, bottom - center};
    const Float4 adjusted{edges[0] + edges[1], edges[1] + edges[0], edges[2] + edges[3],
                          edges[3] + edges[2]};
    for (std::size_t i = 0; i < 4; ++i) {
        const float e = std::min(std::abs(edges.at(i)), std::abs(adjusted.at(i)));
        edges.at(i) = std::clamp(1.3F - (e / (center * 0.040F)), 0.0F, 1.0F);
    }
    return edges;
}

float pack_edges(const Float4& edges) {
    Float4 rounded{};
    for (std::size_t i = 0; i < 4; ++i) {
        rounded.at(i) = round_even(std::clamp(edges.at(i), 0.0F, 1.0F) * 3.05F);
    }
    return dot4(rounded, {64.0F / 255.0F, 16.0F / 255.0F, 4.0F / 255.0F, 1.0F / 255.0F});
}

// generate_SSAO_shadows_internal and main: the pixel's occlusion and packed edges, or for the
// adaptive base pass its occlusion and weight.
Float2 gather_pass(const SsaoUniforms& u, const GatherTextures& t, SsaoPass pass, int x, int y) {
    const bool base = pass == SsaoPass::GatherBase;
    const bool adaptive = pass == SsaoPass::GatherAdaptive;
    const int quality = static_cast<int>(u.options[1]);
    const std::uint32_t slice = u.options[2];
    const SsaoImage& depth0 = *t.depth[0];
    const float pix_z = mirrored(depth0, x, y);
    const float left_z = mirrored(depth0, x - 1, y);
    const float top_z = mirrored(depth0, x, y - 1);
    const float right_z = mirrored(depth0, x + 1, y);
    const float bottom_z = mirrored(depth0, x, y + 1);

    Gather g{.u = u, .t = t, .quality = quality, .id = {x, y}};
    g.screen = {(static_cast<float>(x) * u.pixel[0]) + u.pixel[2],
                (static_cast<float>(y) * u.pixel[1]) + u.pixel[3]};
    g.center = ndc_to_view(u, g.screen, pix_z);
    const int full_x = (x * 2 * static_cast<int>(u.layout[1])) + static_cast<int>(slice % 2U);
    const int full_y = (y * 2 * static_cast<int>(u.layout[1])) + static_cast<int>(slice / 2U);
    g.normal = load_normal(u, *t.normals, full_x, full_y);
    const Float3 beside =
        ndc_to_view(u, {g.screen[0] + u.pixel[0], g.screen[1] + u.pixel[1]}, g.center[2]);
    const Float2 pixel_size{beside[0] - g.center[0], beside[1] - g.center[1]};

    // calculate_radius_parameters
    const float center_length = std::sqrt(dot3(g.center, g.center));
    const float too_close = (std::clamp(center_length * u.fade[3], 0.0F, 1.0F) * 0.8F) + 0.2F;
    const float radius = u.shadow[0] * too_close;
    float lookup_radius = (0.85F * radius) / pixel_size[0];
    g.falloff_sq = -1.0F / (radius * radius);

    if (!base && quality >= 1) {
        float border = std::min(std::min(g.screen[0], 1.0F - g.screen[0]),
                                std::min(g.screen[1], 1.0F - g.screen[1]));
        border = std::clamp((10.0F * border) + 0.6F, 0.0F, 1.0F);
        lookup_radius *= border;
    }
    const auto random_index = static_cast<std::uint32_t>((y * 2) + x) % 5U;
    const Float4& scale = u.rotation.at((slice * 5U) + random_index);
    const Float4 rotation{scale[0] * lookup_radius, scale[1] * lookup_radius,
                          scale[2] * lookup_radius, scale[3] * lookup_radius};

    Float4 edges{1.0F, 1.0F, 1.0F, 1.0F};
    g.center = {g.center[0] * 0.99F, g.center[1] * 0.99F, g.center[2] * 0.99F};
    if (!base && quality >= 1) {
        edges = calculate_edges(pix_z, left_z, right_z, top_z, bottom_z);
    }
    if (!base && quality >= 1 && quality != 4) {
        const Float3 dir{g.center[0] / g.center[2], g.center[1] / g.center[2], 1.0F};
        const auto delta = [&](float dx, float dy, float z) {
            const float along = z - g.center[2];
            return Float3{dx + (dir[0] * along), dy + (dir[1] * along), dir[2] * along};
        };
        const float detail_falloff = 4.0F * g.falloff_sq;
        const Float4 extra{
            pixel_obscurance(u, g.normal, delta(-pixel_size[0], 0.0F, left_z), detail_falloff),
            pixel_obscurance(u, g.normal, delta(pixel_size[0], 0.0F, right_z), detail_falloff),
            pixel_obscurance(u, g.normal, delta(0.0F, -pixel_size[1], top_z), detail_falloff),
            pixel_obscurance(u, g.normal, delta(0.0F, pixel_size[1], bottom_z), detail_falloff)};
        g.obscurance_sum += u.adaptive[3] * dot4(extra, edges);
    }
    if (!base && quality >= 2) {
        const std::array<Int2, 4> around{{{-2, 0}, {2, 0}, {0, -2}, {0, 2}}};
        for (std::size_t i = 0; i < 4; ++i) {
            const Float3 neighbor =
                load_normal(u, *t.normals, full_x + around.at(i).x, full_y + around.at(i).y);
            edges.at(i) *= std::clamp(dot3(g.normal, neighbor) + kNormalEdgeThreshold, 0.0F, 1.0F);
        }
    }
    const float mip_offset = quality < 2 ? 0.0F : log2(lookup_radius) + kMipOffset;

    if (!adaptive) {
        const int taps = base ? kAdaptiveBaseTaps : kTaps.at(static_cast<std::size_t>(quality));
        for (int i = 0; i < taps; ++i) {
            g.tap(i, rotation, mip_offset, 1.0F);
        }
    } else {
        const Float2 uv{g.screen[0] + u.pass_offset[2], g.screen[1] + u.pass_offset[3]};
        float importance = bilinear(*t.importance, uv);
        g.obscurance_sum *= (static_cast<float>(kAdaptiveBaseTaps) / static_cast<float>(kMaxTaps)) +
                            ((importance * static_cast<float>(kMaxTaps - kAdaptiveBaseTaps)) /
                             static_cast<float>(kMaxTaps));
        const float base_occlusion = texel(*t.base, x, y, 0);
        const float base_weight = texel(*t.base, x, y, 1);
        g.weight_sum += base_weight * static_cast<float>(kAdaptiveBaseTaps * 4);
        g.obscurance_sum += base_occlusion * g.weight_sum;
        const float average_importance = texel(*t.sum, 0, 0) * u.adaptive[1];
        const float limiter = std::clamp(u.adaptive[2] / average_importance, 0.0F, 1.0F);
        importance *= limiter;
        float extra_samples = static_cast<float>(kMaxTaps - kAdaptiveBaseTaps) * importance;
        extra_samples += 0.5F;
        const auto count = static_cast<int>(extra_samples);
        const int to = std::min(kMaxTaps, count + kAdaptiveBaseTaps);
        for (int i = kAdaptiveBaseTaps; i < to; ++i) {
            extra_samples -= 1.0F;
            const float weight_mod = std::clamp(extra_samples * (1.0F / 3.0F), 0.0F, 1.0F);
            g.tap(i, rotation, mip_offset, weight_mod);
        }
    }

    if (base) {
        return {g.obscurance_sum / g.weight_sum,
                g.weight_sum / (static_cast<float>(kAdaptiveBaseTaps) * 4.0F)};
    }
    float obscurance = g.obscurance_sum / g.weight_sum;
    float fade = std::clamp((g.center[2] * u.fade[0]) + u.fade[1], 0.0F, 1.0F);
    if (quality >= 1) {
        const float edge_fade = std::clamp((1.0F - edges[0] - edges[1]) * 0.35F, 0.0F, 1.0F) +
                                std::clamp((1.0F - edges[2] - edges[3]) * 0.35F, 0.0F, 1.0F);
        fade *= std::clamp(1.0F - edge_fade, 0.0F, 1.0F);
    }
    obscurance = u.shadow[1] * obscurance;
    obscurance = std::min(obscurance, u.shadow[3]);
    obscurance *= fade;
    float occlusion = 1.0F - obscurance;
    occlusion = power(std::clamp(occlusion, 0.0F, 1.0F), u.shadow[2]);
    if (quality == 0) {
        edges = {1.0F, 1.0F, 1.0F, 1.0F};
    }
    return {occlusion, pack_edges(edges)};
}

// --- The importance map, as ssao_importance_map.glsl ---

float importance_generate(const SsaoUniforms& u, std::span<const SsaoImage* const> slices, int x,
                          int y) {
    float low = 1.0F;
    float high = 0.0F;
    for (const SsaoImage* slice : slices) {
        for (int j = 0; j < 2; ++j) {
            for (int i = 0; i < 2; ++i) {
                float v = clamped(*slice, (2 * x) + i, (2 * y) + j);
                v = u.importance[2] * v;
                v = 1.0F - v;
                v = power(std::clamp(v, 0.0F, 1.0F), u.importance[3]);
                high = std::max(high, v);
                low = std::min(low, v);
            }
        }
    }
    return power(std::clamp((high - low) * 2.0F, 0.0F, 1.0F), 0.8F);
}

float importance_blur(const SsaoUniforms& u, const SsaoImage& source, bool second, int x, int y) {
    const float hx = u.importance[0];
    const float hy = u.importance[1];
    const Float2 uv{(static_cast<float>(x) + 0.5F) * hx * 2.0F,
                    (static_cast<float>(y) + 0.5F) * hy * 2.0F};
    const std::array<Float2, 4> first_offsets{
        {{-hx * 3.0F, -hy}, {hx, -hy * 3.0F}, {hx * 3.0F, hy}, {-hx, hy * 3.0F}}};
    const std::array<Float2, 4> second_offsets{
        {{-hx, -hy * 3.0F}, {hx * 3.0F, -hy}, {hx, hy * 3.0F}, {-hx * 3.0F, hy}}};
    const std::array<Float2, 4>& offsets = second ? second_offsets : first_offsets;
    Float4 values{};
    for (std::size_t i = 0; i < 4; ++i) {
        values.at(i) = bilinear(source, {uv[0] + offsets.at(i)[0], uv[1] + offsets.at(i)[1]});
    }
    return dot4(values, {0.25F, 0.25F, 0.25F, 0.25F});
}

// The load counter's row: the importance map's bytes at every third texel of every third row,
// summed, as PROCESS_MAPB adds them up.
float importance_row(const SsaoImage& importance, int y) {
    float sum = 0.0F;
    for (int x = 0; x < static_cast<int>(importance.width); x += 3) {
        sum += std::floor((std::clamp(texel(importance, x, y * 3), 0.0F, 1.0F) * 255.0F) + 0.5F);
    }
    return sum;
}

// --- The blur, as ssao_blur.glsl ---

Float4 unpack_edges(float packed, float sharpness) {
    const auto bits = static_cast<std::uint32_t>(packed * 255.5F);
    Float4 edges{
        static_cast<float>((bits >> 6U) & 3U) / 3.0F, static_cast<float>((bits >> 4U) & 3U) / 3.0F,
        static_cast<float>((bits >> 2U) & 3U) / 3.0F, static_cast<float>(bits & 3U) / 3.0F};
    for (float& edge : edges) {
        edge = std::clamp(edge + sharpness, 0.0F, 1.0F);
    }
    return edges;
}

Float2 blur_pass(const SsaoUniforms& u, const SsaoImage& source, SsaoPass pass, int x, int y) {
    const float sharpness = u.blur[0];
    if (pass == SsaoPass::BlurPlain) {
        const float hx = u.blur[1] * 0.5F;
        const float hy = u.blur[2] * 0.5F;
        const Float2 uv{(static_cast<float>(x) + 0.5F) * u.blur[1],
                        (static_cast<float>(y) + 0.5F) * u.blur[2]};
        const float center_occlusion = bilinear(source, uv, 0);
        const float center_edges = bilinear(source, uv, 1);
        const Float4 values{
            bilinear(source, {uv[0] - (hx * 3.0F), uv[1] - hy}),
            bilinear(source, {uv[0] + hx, uv[1] - (hy * 3.0F)}),
            bilinear(source, {uv[0] - hx, uv[1] + (hy * 3.0F)}),
            bilinear(source, {uv[0] + (hx * 3.0F), uv[1] + hy}),
        };
        return {dot4(values, {0.2F, 0.2F, 0.2F, 0.2F}) + (center_occlusion * 0.2F), center_edges};
    }
    const bool wide = pass == SsaoPass::BlurWide;
    const int reach = wide ? 2 : 1;
    const float packed = mirrored(source, x, y, 1);
    Float4 edges = unpack_edges(packed, sharpness);
    const float center = mirrored(source, x, y, 0);
    const float left = mirrored(source, x - reach, y, 0);
    const float top = mirrored(source, x, y - reach, 0);
    const float right = mirrored(source, x + reach, y, 0);
    const float bottom = mirrored(source, x, y + reach, 0);
    if (wide) {
        edges[0] *= unpack_edges(mirrored(source, x - 2, y, 1), sharpness)[1];
        edges[2] *= unpack_edges(mirrored(source, x, y - 2, 1), sharpness)[3];
        edges[1] *= unpack_edges(mirrored(source, x + 2, y, 1), sharpness)[0];
        edges[3] *= unpack_edges(mirrored(source, x, y + 2, 1), sharpness)[2];
    }
    float sum_weight = wide ? 0.8F : 0.5F;
    float sum = center * sum_weight;
    const std::array<float, 4> values{left, right, top, bottom};
    for (std::size_t i = 0; i < 4; ++i) {
        sum += edges.at(i) * values.at(i);
        sum_weight += edges.at(i);
    }
    return {sum / sum_weight, packed};
}

// --- Putting the slices back together, as ssao_interleave.glsl ---

float interleave_pass(const SsaoUniforms& u, std::span<const SsaoImage* const> slices,
                      SsaoPass pass, int x, int y) {
    const float px = 1.0F / static_cast<float>(u.frame[0]);
    const float py = 1.0F / static_cast<float>(u.frame[1]);
    const Float2 uv{(static_cast<float>(x) + 0.5F) * px, (static_cast<float>(y) + 0.5F) * py};
    if (pass == SsaoPass::InterleaveHalf) {
        const float a = bilinear(*slices[0], uv);
        const float d = bilinear(*slices[3], uv);
        return (a + d) * 0.5F;
    }
    if (pass == SsaoPass::InterleavePlain) {
        const float a = bilinear(*slices[0], uv);
        const float b = bilinear(*slices[1], uv);
        const float c = bilinear(*slices[2], uv);
        const float d = bilinear(*slices[3], uv);
        return (a + b + c + d) * 0.25F;
    }
    const int mx = x % 2;
    const int my = y % 2;
    const int center_index = mx + (my * 2);
    const int horizontal_index = (1 - mx) + (my * 2);
    const int vertical_index = mx + ((1 - my) * 2);
    const int diagonal_index = (1 - mx) + ((1 - my) * 2);
    const auto modifier = static_cast<int>(u.layout[2]);
    const SsaoImage& center = *slices[static_cast<std::size_t>(center_index)];
    const float occlusion = texel(center, x / modifier, y / modifier, 0);
    const Float4 edges = unpack_edges(texel(center, x / modifier, y / modifier, 1), u.blur[0]);
    const auto fmx = static_cast<float>(mx);
    const auto fmy = static_cast<float>(my);
    const float fmxe = edges[1] - edges[0];
    const float fmye = edges[3] - edges[2];
    const auto at = [&](float ox, float oy) {
        return Float2{(static_cast<float>(x) + 0.5F + ox) * px,
                      (static_cast<float>(y) + 0.5F + oy) * py};
    };
    const float horizontal = bilinear(*slices[static_cast<std::size_t>(horizontal_index)],
                                      at(fmx + fmxe - 0.5F, 0.5F - fmy));
    const float vertical = bilinear(*slices[static_cast<std::size_t>(vertical_index)],
                                    at(0.5F - fmx, fmy - 0.5F + fmye));
    const float diagonal = bilinear(*slices[static_cast<std::size_t>(diagonal_index)],
                                    at(fmx - 0.5F + fmxe, fmy - 0.5F + fmye));
    Float4 weights{1.0F, (edges[0] + edges[1]) * 0.5F, (edges[2] + edges[3]) * 0.5F, 0.0F};
    weights[3] = (weights[1] + weights[2]) * 0.5F;
    const float weight_sum = dot4(weights, {1.0F, 1.0F, 1.0F, 1.0F});
    return dot4({occlusion, horizontal, vertical, diagonal}, weights) / weight_sum;
}

PostSize depth_base(const SsaoSettings& settings, std::uint32_t width, std::uint32_t height) {
    const PostSize full{.width = (width + 1) / 2, .height = (height + 1) / 2};
    if (!settings.half_size) {
        return full;
    }
    return {.width = std::max(full.width >> 1U, 1U), .height = std::max(full.height >> 1U, 1U)};
}

PostSize buffer_size(const SsaoSettings& settings, std::uint32_t width, std::uint32_t height) {
    if (settings.half_size) {
        return {.width = (width + 3) / 4, .height = (height + 3) / 4};
    }
    return {.width = (width + 1) / 2, .height = (height + 1) / 2};
}

PostSize half_buffer_size(const SsaoSettings& settings, std::uint32_t width, std::uint32_t height) {
    if (settings.half_size) {
        return {.width = (width + 7) / 8, .height = (height + 7) / 8};
    }
    return {.width = (width + 3) / 4, .height = (height + 3) / 4};
}

SsaoStep make_step(SsaoPass pass, std::uint32_t target, std::uint32_t slice = 0,
                   std::uint32_t level = 0) {
    SsaoStep step{.pass = pass, .slice = slice, .level = level, .target = target};
    step.slots.fill(kSsaoUnbound);
    return step;
}

} // namespace

SsaoFormat ssao_format(std::uint32_t texture) {
    if (texture < kSsaoAo) {
        return SsaoFormat::R16F;
    }
    if (texture < kSsaoImportance) {
        return SsaoFormat::RG8;
    }
    if (texture == kSsaoRows || texture == kSsaoSum) {
        return SsaoFormat::R32F;
    }
    return SsaoFormat::R8;
}

std::uint32_t ssao_channels(SsaoFormat format) {
    return format == SsaoFormat::RG8 ? 2 : 1;
}

SsaoPlan ssao_plan(const SsaoSettings& settings, std::uint32_t width, std::uint32_t height) {
    SsaoPlan plan;
    const SsaoQuality quality = settings.quality;
    const bool very_low = quality == SsaoQuality::VeryLow;
    const bool ultra = quality == SsaoQuality::Ultra;
    // Godot's use_mips: the gather reads depth mipmaps from high quality on.
    plan.levels = quality > SsaoQuality::Medium ? kSsaoDepthLevels : 1;
    const PostSize base = depth_base(settings, width, height);
    const PostSize buffer = buffer_size(settings, width, height);
    const PostSize half_buffer = half_buffer_size(settings, width, height);
    // Very low reads only the first and last slices.
    const auto skipped = [&](std::uint32_t slice) {
        return very_low && (slice == 1 || slice == 2);
    };
    for (std::uint32_t slice = 0; slice < 4; ++slice) {
        plan.sizes.at(kSsaoAo + slice) = buffer;
        plan.sizes.at(kSsaoPong + slice) = buffer;
        if (skipped(slice)) {
            continue;
        }
        for (std::uint32_t level = 0; level < plan.levels; ++level) {
            const std::uint32_t texture = ssao_depth(slice, level);
            plan.sizes.at(texture) = {.width = std::max(base.width >> level, 1U),
                                      .height = std::max(base.height >> level, 1U)};
            SsaoStep step = make_step(SsaoPass::Depth, texture, slice, level);
            step.slots[0] = kSsaoDepthBuffer;
            plan.steps.push_back(step);
        }
    }
    plan.sizes.at(kSsaoFinal) = {.width = width, .height = height};
    const auto gather = [&](SsaoPass pass, std::uint32_t target_base) {
        for (std::uint32_t slice = 0; slice < 4; ++slice) {
            if (skipped(slice)) {
                continue;
            }
            SsaoStep step = make_step(pass, target_base + slice, slice);
            for (std::uint32_t level = 0; level < plan.levels; ++level) {
                step.slots.at(level) = ssao_depth(slice, level);
            }
            step.slots[4] = kSsaoNormals;
            if (pass == SsaoPass::GatherAdaptive) {
                step.slots[5] = kSsaoPong + slice;
                step.slots[6] = kSsaoImportance;
                step.slots[7] = kSsaoSum;
            }
            plan.steps.push_back(step);
        }
    };
    if (ultra) {
        plan.sizes.at(kSsaoImportance) = half_buffer;
        plan.sizes.at(kSsaoImportancePong) = half_buffer;
        plan.sizes.at(kSsaoRows) = {.width = 1, .height = (half_buffer.height + 2) / 3};
        plan.sizes.at(kSsaoSum) = {.width = 1, .height = 1};
        gather(SsaoPass::GatherBase, kSsaoPong);
        SsaoStep generate = make_step(SsaoPass::ImportanceGenerate, kSsaoImportance);
        for (std::uint32_t slice = 0; slice < 4; ++slice) {
            generate.slots.at(8 + slice) = kSsaoPong + slice;
        }
        plan.steps.push_back(generate);
        SsaoStep first = make_step(SsaoPass::ImportanceA, kSsaoImportancePong);
        first.slots[6] = kSsaoImportance;
        plan.steps.push_back(first);
        SsaoStep second = make_step(SsaoPass::ImportanceB, kSsaoImportance);
        second.slots[6] = kSsaoImportancePong;
        plan.steps.push_back(second);
        SsaoStep rows = make_step(SsaoPass::ImportanceRows, kSsaoRows);
        rows.slots[6] = kSsaoImportance;
        plan.steps.push_back(rows);
        SsaoStep sum = make_step(SsaoPass::ImportanceSum, kSsaoSum);
        sum.slots[7] = kSsaoRows;
        plan.steps.push_back(sum);
        gather(SsaoPass::GatherAdaptive, kSsaoAo);
    } else {
        gather(SsaoPass::Gather, kSsaoAo);
    }
    const std::uint32_t blur_passes = very_low ? 1 : settings.blur_passes;
    for (std::uint32_t pass = 0; pass < blur_passes; ++pass) {
        SsaoPass kind = SsaoPass::BlurPlain;
        if (!very_low) {
            kind = pass + 2 < blur_passes ? SsaoPass::BlurWide : SsaoPass::BlurSmart;
        }
        const bool even = pass % 2 == 0;
        for (std::uint32_t slice = 0; slice < 4; ++slice) {
            if (skipped(slice)) {
                continue;
            }
            SsaoStep step = make_step(kind, (even ? kSsaoPong : kSsaoAo) + slice, slice);
            step.slots[8] = (even ? kSsaoAo : kSsaoPong) + slice;
            plan.steps.push_back(step);
        }
    }
    SsaoPass interleave = SsaoPass::InterleaveSmart;
    if (very_low) {
        interleave = SsaoPass::InterleaveHalf;
    } else if (quality == SsaoQuality::Low) {
        interleave = SsaoPass::InterleavePlain;
    }
    const std::uint32_t source = !very_low && settings.blur_passes % 2 == 0 ? kSsaoAo : kSsaoPong;
    SsaoStep last = make_step(interleave, kSsaoFinal);
    for (std::uint32_t slice = 0; slice < 4; ++slice) {
        last.slots.at(8 + slice) = source + slice;
    }
    plan.steps.push_back(last);
    return plan;
}

SsaoUniforms ssao_uniforms(const Ssao3D& ssao, const SsaoStep& step,
                           const std::array<float, 16>& projection, std::uint32_t width,
                           std::uint32_t height) {
    const SsaoSettings& settings = ssao.settings;
    const auto quality = static_cast<std::uint32_t>(settings.quality);
    const bool half = settings.half_size;
    SsaoUniforms out;
    out.options = {static_cast<std::uint32_t>(step.pass), quality > 0 ? quality - 1 : 0, step.slice,
                   step.level};
    const PostSize base = depth_base(settings, width, height);
    const PostSize buffer = buffer_size(settings, width, height);
    const PostSize half_buffer = half_buffer_size(settings, width, height);
    out.frame = {width, height, base.width, base.height};
    out.buffers = {buffer.width, buffer.height, half_buffer.width, half_buffer.height};
    out.layout = {half ? 4U : 2U, half ? 2U : 1U, half ? 4U : 2U,
                  settings.quality > SsaoQuality::Medium ? kSsaoDepthLevels : 1U};

    const auto w = static_cast<float>(width);
    const auto h = static_cast<float>(height);
    float hx = 2.0F / w;
    float hy = 2.0F / h;
    if (half) {
        hx *= 2.0F;
        hy *= 2.0F;
    }
    out.pixel = {hx, hy, hx * 0.75F, hy * 0.75F};
    const float tan_x = 1.0F / projection[0];
    const float tan_y = 1.0F / projection[5];
    out.ndc = {tan_x * 2.0F, tan_y * -2.0F, tan_x * -1.0F, tan_y};

    float radius = ssao.radius;
    float near_limit = ssao.radius * 1.2F;
    if (settings.quality <= SsaoQuality::Low) {
        near_limit *= 1.5F;
        if (settings.quality == SsaoQuality::VeryLow) {
            radius *= 0.8F;
        }
    }
    near_limit /= tan_y;
    out.shadow = {radius, ssao.intensity, ssao.power, 0.98F};
    const float fade_range = settings.fadeout_to - settings.fadeout_from;
    out.fade = {-1.0F / fade_range, (settings.fadeout_from / fade_range) + 1.0F, ssao.horizon,
                1.0F / near_limit};
    out.adaptive = {-1.0F / radius,
                    9.0F / static_cast<float>(half_buffer.width * half_buffer.height * 255U),
                    settings.adaptive_target, ssao.detail};
    const auto column = static_cast<float>(step.slice % 2U);
    const auto row = static_cast<float>(step.slice >> 1U);
    out.pass_offset = {column, row, column / w, row / h};

    // Godot's downsample_depth: a perspective projection's depth made linear from its [3][2]
    // and [2][2] entries, an orthogonal one's from its near and far planes.
    const bool orthogonal = projection[15] == 1.0F;
    if (orthogonal) {
        const float z_near = (projection[14] - 1.0F) / projection[10];
        const float z_far = projection[14] / projection[10];
        out.depth = {z_near, z_far, 1.0F, 1.0F};
    } else {
        const float mul = -projection[14];
        float add = projection[10];
        if (mul * add < 0.0F) {
            add = -add;
        }
        out.depth = {mul, add, 0.0F, 1.0F};
    }
    out.blur = {1.0F - ssao.sharpness, 1.0F / static_cast<float>(buffer.width),
                1.0F / static_cast<float>(buffer.height), 0.0F};
    out.importance = {1.0F / static_cast<float>(buffer.width),
                      1.0F / static_cast<float>(buffer.height), ssao.intensity, ssao.power};

    // ss_effects.cpp's rotation_matrices: for each slice, five turns and scales of the sample
    // pattern, picked by pixel.
    constexpr int kSubPasses = 5;
    constexpr std::array<int, kSubPasses> kSubPassMap{0, 1, 4, 3, 2};
    for (int pass = 0; pass < 4; ++pass) {
        for (int sub = 0; sub < kSubPasses; ++sub) {
            const auto a = static_cast<float>(pass);
            const auto b = static_cast<float>(kSubPassMap.at(static_cast<std::size_t>(sub)));
            const float angle = (a + (b / static_cast<float>(kSubPasses))) * kPi * 0.5F;
            const Vec2 turn = slash::cos_sin(angle);
            const float scale =
                1.0F + ((a - 1.5F + ((b - ((kSubPasses - 1.0F) * 0.5F)) / kSubPasses)) * 0.07F);
            out.rotation.at(at_index(sub, pass, kSubPasses)) = {scale * turn.x, scale * -turn.y,
                                                                -scale * turn.y, -scale * turn.x};
        }
    }
    return out;
}

void run_ssao_pass(const SsaoUniforms& uniforms, const SsaoStep& step,
                   std::span<const SsaoImage> textures, const SsaoInputs& inputs,
                   SsaoImage& target) {
    const SsaoFormat format = ssao_format(step.target);
    const std::uint32_t channels = ssao_channels(format);
    target.channels = channels;
    target.values.assign(static_cast<std::size_t>(target.width) * target.height * channels, 0.0F);
    const auto slot = [&](std::size_t i) -> const SsaoImage* {
        const std::uint32_t index = step.slots.at(i);
        if (index == kSsaoNormals) {
            return inputs.normals;
        }
        return index < textures.size() ? &textures[index] : nullptr;
    };
    GatherTextures gather;
    for (std::size_t level = 0; level < 4; ++level) {
        gather.depth.at(level) = slot(level);
    }
    gather.normals = slot(4);
    gather.base = slot(5);
    gather.importance = slot(6);
    gather.sum = slot(7);
    const std::array<const SsaoImage*, 4> slices{slot(8), slot(9), slot(10), slot(11)};
    for (std::uint32_t y = 0; y < target.height; ++y) {
        for (std::uint32_t x = 0; x < target.width; ++x) {
            const auto ix = static_cast<int>(x);
            const auto iy = static_cast<int>(y);
            Float2 out{};
            switch (step.pass) {
            case SsaoPass::Depth:
                out[0] = depth_pass(uniforms, inputs, ix, iy);
                break;
            case SsaoPass::Gather:
            case SsaoPass::GatherBase:
            case SsaoPass::GatherAdaptive:
                out = gather_pass(uniforms, gather, step.pass, ix, iy);
                break;
            case SsaoPass::ImportanceGenerate:
                out[0] = importance_generate(uniforms, slices, ix, iy);
                break;
            case SsaoPass::ImportanceA:
            case SsaoPass::ImportanceB:
                out[0] = importance_blur(uniforms, *gather.importance,
                                         step.pass == SsaoPass::ImportanceB, ix, iy);
                break;
            case SsaoPass::ImportanceRows:
                out[0] = importance_row(*gather.importance, iy);
                break;
            case SsaoPass::ImportanceSum:
                for (std::uint32_t row = 0; row < gather.sum->height; ++row) {
                    out[0] += texel(*gather.sum, 0, static_cast<int>(row));
                }
                break;
            case SsaoPass::BlurPlain:
            case SsaoPass::BlurSmart:
            case SsaoPass::BlurWide:
                out = blur_pass(uniforms, *slices[0], step.pass, ix, iy);
                break;
            case SsaoPass::InterleavePlain:
            case SsaoPass::InterleaveSmart:
            case SsaoPass::InterleaveHalf:
                out[0] = interleave_pass(uniforms, slices, step.pass, ix, iy);
                break;
            }
            const std::size_t at = ((static_cast<std::size_t>(y) * target.width) + x) * channels;
            for (std::uint32_t c = 0; c < channels; ++c) {
                target.values[at + c] = stored(format, out.at(c));
            }
        }
    }
}

SsaoImage run_ssao(const Ssao3D& ssao, const std::array<float, 16>& projection, std::uint32_t width,
                   std::uint32_t height, const SsaoInputs& inputs) {
    const SsaoPlan plan = ssao_plan(ssao.settings, width, height);
    std::vector<SsaoImage> textures(kSsaoTextures);
    for (std::uint32_t i = 0; i < kSsaoTextures; ++i) {
        SsaoImage& texture = textures[i];
        texture.width = plan.sizes.at(i).width;
        texture.height = plan.sizes.at(i).height;
        texture.channels = ssao_channels(ssao_format(i));
        texture.values.assign(
            static_cast<std::size_t>(texture.width) * texture.height * texture.channels, 0.0F);
    }
    for (const SsaoStep& step : plan.steps) {
        const SsaoUniforms uniforms = ssao_uniforms(ssao, step, projection, width, height);
        SsaoImage target = std::move(textures[step.target]);
        run_ssao_pass(uniforms, step, textures, inputs, target);
        textures[step.target] = std::move(target);
    }
    return std::move(textures[kSsaoFinal]);
}

} // namespace slash::render::detail

namespace slash::render {

SsaoSettings ssao_settings_from_json(const json::Value& value) {
    SsaoSettings out;
    if (!value.is_object()) {
        return out;
    }
    if (const json::Value* found = value.find("quality"); found != nullptr && found->is_string()) {
        for (std::size_t i = 0; i < kSsaoQualityNames.size(); ++i) {
            if (found->as_string() == kSsaoQualityNames.at(i)) {
                out.quality = static_cast<SsaoQuality>(i);
            }
        }
    }
    if (const json::Value* found = value.find("half_size"); found != nullptr && found->is_bool()) {
        out.half_size = found->as_bool();
    }
    const auto number = [&](std::string_view key, float low, float high, float& field) {
        if (const json::Value* found = value.find(key); found != nullptr && found->is_number()) {
            const double read = found->as_double();
            if (read >= low && read <= high) {
                field = static_cast<float>(read);
            }
        }
    };
    number("adaptive_target", 0.0F, 1.0F, out.adaptive_target);
    number("fadeout_from", 0.0F, 1.0e6F, out.fadeout_from);
    number("fadeout_to", 0.0F, 1.0e6F, out.fadeout_to);
    if (const json::Value* found = value.find("blur_passes");
        found != nullptr && found->is_number()) {
        const double read = found->as_double();
        if (read >= 0.0 && read <= 6.0 && read == std::floor(read)) {
            out.blur_passes = static_cast<std::uint32_t>(read);
        }
    }
    return out;
}

} // namespace slash::render
