#include "ssr.hpp"

#include "forward_post.hpp"
#include "mesh_shading.hpp"

#include "slash/core/json.hpp"
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

using Float2 = std::array<float, 2>;

// Godot's M_PI in screen_space_reflection.glsl.
constexpr float kPi = std::numbers::pi_v<float>;
constexpr float kLn2 = std::numbers::ln2_v<float>;
constexpr Float3 kLuminance{0.2126F, 0.7152F, 0.0722F};
// screen_space_reflection_filter.glsl's WEIGHTS.
constexpr std::array<float, 7> kFilterWeights{
    0.07130343198685299F, 0.1315141208431224F, 0.18987923288883812F, 0.21460642856237303F,
    0.18987923288883812F, 0.1315141208431224F, 0.07130343198685299F};
// The filter's rows reach this many rows past each edge of the level.
constexpr int kFilterBorder = 3;
// Mip levels are stored as a fraction of 14 in a byte, as Godot's.
constexpr float kMipScale = 14.0F;

// The index of column x of row y in rows of the given width.
std::size_t at_index(int x, int y, int width) {
    return (static_cast<std::size_t>(y) * static_cast<std::size_t>(width)) +
           static_cast<std::size_t>(x);
}

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

float saturate(float x) {
    return std::clamp(x, 0.0F, 1.0F);
}

float smoothstep(float edge0, float edge1, float x) {
    const float t = saturate((x - edge0) / (edge1 - edge0));
    return t * t * (3.0F - (2.0F * t));
}

std::uint8_t to_byte(float value) {
    return static_cast<std::uint8_t>(std::lround(saturate(value) * 255.0F));
}

// value as the texture's format keeps it.
float stored(SsrFormat format, float value) {
    switch (format) {
    case SsrFormat::RGBA16F:
        return round_half(value);
    case SsrFormat::RGBA8:
    case SsrFormat::R8:
        return static_cast<float>(to_byte(value)) / 255.0F;
    case SsrFormat::R32F:
    case SsrFormat::RGBA32F:
        break;
    }
    return value;
}

float dot3(const Float3& a, const Float3& b) {
    return (a[0] * b[0]) + (a[1] * b[1]) + (a[2] * b[2]);
}

Float3 add(const Float3& a, const Float3& b) {
    return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
}

Float3 sub(const Float3& a, const Float3& b) {
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}

Float3 scale(const Float3& a, float s) {
    return {a[0] * s, a[1] * s, a[2] * s};
}

float length(const Float3& a) {
    return std::sqrt(dot3(a, a));
}

Float3 normalize(const Float3& a) {
    return scale(a, 1.0F / length(a));
}

Float3 cross(const Float3& a, const Float3& b) {
    return {(a[1] * b[2]) - (a[2] * b[1]), (a[2] * b[0]) - (a[0] * b[2]),
            (a[0] * b[1]) - (a[1] * b[0])};
}

// GLSL's reflect: incident turned about the normal.
Float3 reflect(const Float3& incident, const Float3& normal) {
    return sub(incident, scale(normal, 2.0F * dot3(normal, incident)));
}

Float4 transform(const Mat4& m, const Float4& v) {
    Float4 out{};
    for (std::size_t row = 0; row < 4; ++row) {
        out.at(row) = (m.at(row) * v[0]) + (m.at(4 + row) * v[1]) + (m.at(8 + row) * v[2]) +
                      (m.at(12 + row) * v[3]);
    }
    return out;
}

float texel(const SsrImage& image, int x, int y, std::uint32_t channel) {
    const int cx = std::clamp(x, 0, static_cast<int>(image.width) - 1);
    const int cy = std::clamp(y, 0, static_cast<int>(image.height) - 1);
    return image
        .values[(at_index(cx, cy, static_cast<int>(image.width)) * image.channels) + channel];
}

// A texel's four channels (0 for those it doesn't have), its index kept inside the texture.
Float4 texel4(const SsrImage& image, int x, int y) {
    Float4 out{};
    for (std::uint32_t c = 0; c < std::min(image.channels, 4U); ++c) {
        out.at(c) = texel(image, x, y, c);
    }
    return out;
}

// A linearly filtered sample at a texture coordinate, with clamped edges, as Godot's samplers
// with REPEAT_DISABLED take it.
Float4 bilinear(const SsrImage& image, Float2 uv) {
    const float px = (uv[0] * static_cast<float>(image.width)) - 0.5F;
    const float py = (uv[1] * static_cast<float>(image.height)) - 0.5F;
    const float bx = std::floor(px);
    const float by = std::floor(py);
    const float fx = px - bx;
    const float fy = py - by;
    const int x = static_cast<int>(bx);
    const int y = static_cast<int>(by);
    const Float4 t00 = texel4(image, x, y);
    const Float4 t10 = texel4(image, x + 1, y);
    const Float4 t01 = texel4(image, x, y + 1);
    const Float4 t11 = texel4(image, x + 1, y + 1);
    Float4 out{};
    for (std::size_t c = 0; c < 4; ++c) {
        const float top = t00.at(c) + ((t10.at(c) - t00.at(c)) * fx);
        const float bottom = t01.at(c) + ((t11.at(c) - t01.at(c)) * fx);
        out.at(c) = top + ((bottom - top) * fy);
    }
    return out;
}

// A sample between mipmaps, as a sampler with LINEAR_WITH_MIPMAPS takes it: each of the two
// nearest levels filtered linearly, then mixed.
Float4 trilinear(std::span<const SsrImage* const> levels, Float2 uv, float lod) {
    const auto top = static_cast<float>(levels.size() - 1);
    const float clamped = std::clamp(lod, 0.0F, top);
    const float base = std::floor(clamped);
    const float fraction = clamped - base;
    const auto first = static_cast<std::size_t>(base);
    const std::size_t second = std::min(first + 1, levels.size() - 1);
    const Float4 a = bilinear(*levels[first], uv);
    if (fraction == 0.0F) {
        return a;
    }
    const Float4 b = bilinear(*levels[second], uv);
    Float4 out{};
    for (std::size_t c = 0; c < 4; ++c) {
        out.at(c) = a.at(c) + ((b.at(c) - a.at(c)) * fraction);
    }
    return out;
}

// A pixel's roughness from the normal-roughness buffer's alpha: Godot keeps dynamic objects'
// roughness flipped above half, which these never are.
float decode_roughness(float alpha) {
    float roughness = alpha;
    if (roughness > 0.5F) {
        roughness = 1.0F - roughness;
    }
    return roughness / (127.0F / 255.0F);
}

Float3 decode_normal(const Float4& texel) {
    return normalize(
        {(texel[0] * 2.0F) - 1.0F, (texel[1] * 2.0F) - 1.0F, (texel[2] * 2.0F) - 1.0F});
}

// The prepass's depth at a pixel, kept inside the buffer.
float depth_at(const SsrUniforms& u, const SsrInputs& inputs, int x, int y) {
    const auto width = static_cast<int>(u.frame[0]);
    const auto height = static_cast<int>(u.frame[1]);
    const int cx = std::clamp(x, 0, width - 1);
    const int cy = std::clamp(y, 0, height - 1);
    return inputs.depth[at_index(cx, cy, width)];
}

// --- The pyramid, as screen_space_reflection_downsample.glsl and _hiz.glsl ---

// The source pixels a texel of the next level covers, in the order Godot's shaders read them:
// 2x2, then the third column and row of a source of odd width or height.
std::vector<std::array<int, 2>> footprint(std::uint32_t flags) {
    std::vector<std::array<int, 2>> out{{0, 0}, {1, 0}, {0, 1}, {1, 1}};
    const bool odd_width = (flags & kSsrOddWidth) != 0U;
    const bool odd_height = (flags & kSsrOddHeight) != 0U;
    if (odd_width) {
        out.push_back({2, 0});
        out.push_back({2, 1});
    }
    if (odd_height) {
        out.push_back({0, 2});
        out.push_back({1, 2});
    }
    if (odd_width && odd_height) {
        out.push_back({2, 2});
    }
    return out;
}

// The downsample's winner: the nearest of the pixels a half-size texel covers (the first of
// equals), its depth and where it is.
struct Nearest {
    float depth = 0.0F;
    int x = 0;
    int y = 0;
};

Nearest downsample(const SsrUniforms& u, const SsrInputs& inputs, int x, int y) {
    Nearest out{.depth = 0.0F, .x = 2 * x, .y = 2 * y};
    bool first = true;
    for (const auto& [dx, dy] : footprint(u.options[2])) {
        const int sx = (2 * x) + dx;
        const int sy = (2 * y) + dy;
        const float depth = depth_at(u, inputs, sx, sy);
        if (first || out.depth < depth) {
            out = {.depth = depth, .x = sx, .y = sy};
        }
        first = false;
    }
    return out;
}

float hiz_pass(const SsrUniforms& u, const SsrImage& source, int x, int y) {
    float depth = 0.0F;
    bool first = true;
    for (const auto& [dx, dy] : footprint(u.options[2])) {
        const float sample = texel(source, (2 * x) + dx, (2 * y) + dy, 0);
        depth = first ? sample : std::max(depth, sample);
        first = false;
    }
    return depth;
}

// --- The trace, as screen_space_reflection.glsl ---

struct TraceTextures {
    const SsrImage* normals = nullptr;
    std::vector<const SsrImage*> hiz;
    const SsrImage* last_frame = nullptr;
};

Float3 view_position(const SsrUniforms& u, const Float3& screen) {
    const Float4 pos = transform(
        u.inv_projection, {(screen[0] * 2.0F) - 1.0F, (screen[1] * 2.0F) - 1.0F, screen[2], 1.0F});
    return {pos[0] / pos[3], pos[1] / pos[3], pos[2] / pos[3]};
}

Float3 screen_position(const SsrUniforms& u, const Float3& pos) {
    const Float4 clip = transform(u.projection, {pos[0], pos[1], pos[2], 1.0F});
    return {((clip[0] / clip[3]) * 0.5F) + 0.5F, ((clip[1] / clip[3]) * 0.5F) + 0.5F,
            clip[2] / clip[3]};
}

float linearize_depth(const SsrUniforms& u, float depth) {
    const Float4 pos = transform(u.inv_projection, {0.0F, 0.0F, depth, 1.0F});
    return pos[2] / pos[3];
}

float hiz_at(const TraceTextures& t, int level, int x, int y) {
    return texel(*t.hiz.at(static_cast<std::size_t>(level)), x, y, 0);
}

// compute_geometric_normal: the surface's normal from the depths beside the pixel, each side
// taken from whichever neighbor continues the surface best.
Float3 geometric_normal(const SsrUniforms& u, const TraceTextures& t, int x, int y, float depth_c,
                        const Float3& view_c) {
    const std::array<float, 4> h{hiz_at(t, 0, x - 1, y), hiz_at(t, 0, x - 2, y),
                                 hiz_at(t, 0, x + 1, y), hiz_at(t, 0, x + 2, y)};
    const std::array<float, 4> v{hiz_at(t, 0, x, y - 1), hiz_at(t, 0, x, y - 2),
                                 hiz_at(t, 0, x, y + 1), hiz_at(t, 0, x, y + 2)};
    const Float2 he{std::abs(((2.0F * h[0]) - h[1]) - depth_c),
                    std::abs(((2.0F * h[2]) - h[3]) - depth_c)};
    const Float2 ve{std::abs(((2.0F * v[0]) - v[1]) - depth_c),
                    std::abs(((2.0F * v[2]) - v[3]) - depth_c)};
    const int h_sign = he[0] < he[1] ? -1 : 1;
    const int v_sign = ve[0] < ve[1] ? -1 : 1;
    // The neighbor each side reads: h[0] or h[2], v[0] or v[2].
    const std::size_t h_index = h_sign < 0 ? 0 : 2;
    const std::size_t v_index = v_sign < 0 ? 0 : 2;
    const auto width = static_cast<float>(u.sizes[0]);
    const auto height = static_cast<float>(u.sizes[1]);
    const Float3 view_h =
        view_position(u, {(static_cast<float>(x + h_sign) + 0.5F) / width,
                          (static_cast<float>(y) + 0.5F) / height, h.at(h_index)});
    const Float3 view_v =
        view_position(u, {(static_cast<float>(x) + 0.5F) / width,
                          (static_cast<float>(y + v_sign) + 0.5F) / height, v.at(v_index)});
    const Float3 h_der = scale(sub(view_h, view_c), static_cast<float>(h_sign));
    const Float3 v_der = scale(sub(view_v, view_c), static_cast<float>(v_sign));
    return cross(v_der, h_der);
}

struct Traced {
    Float4 color{};
    float mip_level = 0.0F;
};

Traced trace_pass(const SsrUniforms& u, const TraceTextures& tex, int x, int y) {
    Traced out;
    const auto width = static_cast<float>(u.sizes[0]);
    const auto height = static_cast<float>(u.sizes[1]);
    const int mipmaps = static_cast<int>(u.options[3]);
    Float3 screen_pos{(static_cast<float>(x) + 0.5F) / width,
                      (static_cast<float>(y) + 0.5F) / height, hiz_at(tex, 0, x, y)};
    if (screen_pos[2] == 0.0F) {
        return out;
    }
    Float3 pos = view_position(u, screen_pos);
    const Float4 normal_roughness = texel4(*tex.normals, x, y);
    const Float3 normal = decode_normal(normal_roughness);
    const float roughness = decode_roughness(normal_roughness[3]);
    // Rough materials take no reflection, which saves the trace.
    if (roughness >= 0.7F) {
        return out;
    }
    const Float3 geom_normal = normalize(geometric_normal(u, tex, x, y, screen_pos[2], pos));
    // A small bias towards the geometry's normal keeps the ray off its own surface.
    pos = add(pos, scale(geom_normal, 1.0F - power(saturate(dot3(normal, geom_normal)), 8.0F)));
    screen_pos = screen_position(u, pos);
    const bool orthogonal = (u.options[2] & kSsrOrthogonal) != 0U;
    const Float3 view_dir = orthogonal ? Float3{0.0F, 0.0F, -1.0F} : normalize(pos);
    Float3 ray_dir = normalize(reflect(view_dir, normal));
    // A ray going straight into its own surface bounces off it.
    if (dot3(ray_dir, geom_normal) < 0.0F) {
        ray_dir = normalize(reflect(ray_dir, geom_normal));
    }
    Float3 end_pos = add(pos, ray_dir);
    // Clipped to the near plane, with a small bias so it doesn't go to infinity.
    if (end_pos[2] > 0.0F) {
        end_pos = sub(end_pos, scale(ray_dir, (end_pos[2] + 0.00001F) / ray_dir[2]));
    }
    const Float3 screen_end_pos = screen_position(u, end_pos);
    // Z normalized to -1 or +1, for parametric T tracing.
    Float3 screen_ray_dir = sub(screen_end_pos, screen_pos);
    screen_ray_dir = scale(screen_ray_dir, 1.0F / std::abs(screen_ray_dir[2]));
    const bool facing_camera = screen_ray_dir[2] >= 0.0F;
    // Where the ray leaves the screen.
    const Float2 t0{(0.0F - screen_pos[0]) / screen_ray_dir[0],
                    (0.0F - screen_pos[1]) / screen_ray_dir[1]};
    const Float2 t1{(1.0F - screen_pos[0]) / screen_ray_dir[0],
                    (1.0F - screen_pos[1]) / screen_ray_dir[1]};
    const float t_max = std::min(std::max(t0[0], t1[0]), std::max(t0[1], t1[1]));
    const Float2 cell_step{screen_ray_dir[0] < 0.0F ? -1.0F : 1.0F,
                           screen_ray_dir[1] < 0.0F ? -1.0F : 1.0F};
    const Float2 step_up{std::clamp(cell_step[0], 0.0F, 1.0F),
                         std::clamp(cell_step[1], 0.0F, 1.0F)};
    // Where the ray crosses into the next cell of a level count cells across and high.
    const auto edge = [&](const Float2& cell_index, const Float2& count) {
        const Float2 next{((cell_index[0] + step_up[0]) / count[0]) + (cell_step[0] * 0.000001F),
                          ((cell_index[1] + step_up[1]) / count[1]) + (cell_step[1] * 0.000001F)};
        return std::min((next[0] - screen_pos[0]) / screen_ray_dir[0],
                        (next[1] - screen_pos[1]) / screen_ray_dir[1]);
    };
    int cur_level = 0;
    auto cur_iteration = static_cast<int>(u.steps[0]);
    // The start moved to the next cell, so the ray doesn't hit itself at once.
    float t = edge({std::floor(screen_pos[0] * width), std::floor(screen_pos[1] * height)},
                   {width, height});
    while (cur_level >= 0 && cur_iteration > 0 && t < t_max) {
        const Float3 cur_screen_pos = add(screen_pos, scale(screen_ray_dir, t));
        const Float2 cell_count{static_cast<float>(std::max(1U, u.sizes[0] >> cur_level)),
                                static_cast<float>(std::max(1U, u.sizes[1] >> cur_level))};
        const Float2 cell_index{std::floor(cur_screen_pos[0] * cell_count[0]),
                                std::floor(cur_screen_pos[1] * cell_count[1])};
        const float cell_depth = hiz_at(tex, cur_level, static_cast<int>(cell_index[0]),
                                        static_cast<int>(cell_index[1]));
        // Z is -1 or 1, so this needs no division.
        const float depth_t = (cell_depth - screen_pos[2]) * screen_ray_dir[2];
        const float edge_t = edge(cell_index, cell_count);
        bool hit = facing_camera ? (t <= depth_t) : (depth_t <= edge_t);
        int mip_offset = hit ? -1 : 1;
        if (cur_level == 0) {
            const float z0 = linearize_depth(u, cell_depth);
            const float z1 = linearize_depth(u, cur_screen_pos[2]);
            if ((z0 - z1) > u.fade[2]) {
                hit = false;
                // The level stays, so it doesn't go down and up again and again.
                mip_offset = 0;
            }
        }
        if (hit) {
            if (!facing_camera) {
                t = std::max(t, depth_t);
            }
        } else {
            t = edge_t;
        }
        cur_level = std::min(cur_level + mip_offset, mipmaps - 1);
        --cur_iteration;
    }
    const Float3 cur_screen_pos = add(screen_pos, scale(screen_ray_dir, t));
    Float4 reprojected =
        transform(u.reprojection, {(cur_screen_pos[0] * 2.0F) - 1.0F,
                                   (cur_screen_pos[1] * 2.0F) - 1.0F, cur_screen_pos[2], 1.0F});
    const Float2 reprojected_uv{((reprojected[0] / reprojected[3]) * 0.5F) + 0.5F,
                                ((reprojected[1] / reprojected[3]) * 0.5F) + 0.5F};
    // How much the sample counts, rather than a hard yes or no, so invalid samples still give
    // mip levels for smoother changes of roughness.
    float validity = 1.0F;
    const int cur_x = static_cast<int>(cur_screen_pos[0] * width);
    const int cur_y = static_cast<int>(cur_screen_pos[1] * height);
    const float hit_depth = hiz_at(tex, 0, cur_x, cur_y);
    if (t >= t_max || hit_depth == 0.0F) {
        validity = 0.0F;
    }
    if (std::abs(screen_ray_dir[0] * t) < 2.0F / width &&
        std::abs(screen_ray_dir[1] * t) < 2.0F / height) {
        const Float4 hit_texel = texel4(*tex.normals, cur_x, cur_y);
        const Float3 hit_normal{(hit_texel[0] * 2.0F) - 1.0F, (hit_texel[1] * 2.0F) - 1.0F,
                                (hit_texel[2] * 2.0F) - 1.0F};
        if (dot3(ray_dir, hit_normal) >= 0.0F) {
            validity = 0.0F;
        }
    }
    const Float3 cur_pos = view_position(u, cur_screen_pos);
    const Float3 hit_pos = view_position(u, {cur_screen_pos[0], cur_screen_pos[1], hit_depth});
    const float delta = length(sub(cur_pos, hit_pos));
    const float confidence = 1.0F - smoothstep(0.0F, u.fade[2], delta);
    validity *= saturate(confidence * confidence);
    // Fades out toward an inner margin of the screen.
    const Float2 reprojected_pixel{reprojected_uv[0] * width, reprojected_uv[1] * height};
    const float margin = (width + height) * 0.05F;
    const Float2 margin_grad{reprojected_pixel[0] < width * 0.5F ? reprojected_pixel[0]
                                                                 : width - reprojected_pixel[0],
                             reprojected_pixel[1] < height * 0.5F ? reprojected_pixel[1]
                                                                  : height - reprojected_pixel[1]};
    const float margin_blend = smoothstep(0.0F, margin * margin, margin_grad[0] * margin_grad[1]);
    const float ray_len = std::sqrt((screen_ray_dir[0] * t * screen_ray_dir[0] * t) +
                                    (screen_ray_dir[1] * t * screen_ray_dir[1] * t));
    const float grad = ray_len;
    const float fade_in = u.fade[1] == 0.0F ? 1.0F : power(saturate(grad), u.fade[1]);
    const float fade_out = u.fade[0] == 0.0F ? 1.0F : power(saturate(1.0F - grad), u.fade[0]);
    float fade = fade_in * fade_out;
    // So precision doesn't fade a reflection that shouldn't, which strong specular light would
    // leak through.
    if (fade > 0.999F) {
        fade = 1.0F;
    }
    validity *= fade * margin_blend;
    if (validity > 0.0F) {
        const Float4 last = bilinear(*tex.last_frame, reprojected_uv);
        out.color = {last[0] * validity, last[1] * validity, last[2] * validity, validity};
        // Tone mapped, so rough reflections filter more smoothly across samples of different
        // brightness.
        const float luminance = dot3({out.color[0], out.color[1], out.color[2]}, kLuminance);
        for (std::size_t c = 0; c < 3; ++c) {
            out.color.at(c) /= 1.0F + luminance;
        }
    }
    float mip_level = 0.0F;
    if (roughness > 0.001F) {
        const float cone_angle = std::min(roughness, 0.999F) * kPi * 0.5F;
        const float cone_len = ray_len;
        const Vec2 turn = slash::cos_sin(cone_angle);
        // The opposite side of the cone's isosceles triangle.
        const float op_len = 2.0F * (turn.y / turn.x) * cone_len;
        // The sphere that fits inside the cone and ends at its end, which keeps blur from
        // bleeding past the reflection and stretches rough reflections a little.
        const float a = op_len;
        const float h = cone_len;
        const float a2 = a * a;
        const float fh2 = 4.0F * h * h;
        const float blur_radius = (a * (std::sqrt(a2 + fh2) - a)) / (4.0F * h);
        const float scaled = blur_radius * std::max(width, height) / 16.0F;
        mip_level =
            scaled > 0.0F ? std::clamp(log2(scaled), 0.0F, static_cast<float>(mipmaps - 1)) : 0.0F;
    }
    // Invalid pixels' mip levels can make a pyramid shape at very rough levels; fading them near
    // the end of the ray hides it.
    out.mip_level = mip_level * power(saturate(1.25F - ray_len), 0.2F);
    return out;
}

// --- The roughness filter, as screen_space_reflection_filter.glsl ---

float filter_weight(const SsrUniforms& u, const Float4& color) {
    const float floor_weight = saturate(static_cast<float>(u.options[1]) * 0.2F);
    return floor_weight + ((1.0F - floor_weight) * color[3]);
}

// Seven samples weighed by the filter's weights and how valid each is, normalized.
template <typename Sample>
Float4 gaussian(const SsrUniforms& u, const Sample& sample) {
    Float4 sum{};
    float total = 0.0F;
    for (std::size_t k = 0; k < kFilterWeights.size(); ++k) {
        const Float4 c = sample(static_cast<int>(k) - kFilterBorder);
        const float w = kFilterWeights.at(k) * filter_weight(u, c);
        for (std::size_t i = 0; i < 4; ++i) {
            sum.at(i) += c.at(i) * w;
        }
        total += w;
    }
    if (!(total > 0.0F)) {
        return {};
    }
    for (float& value : sum) {
        value /= total;
    }
    return sum;
}

// The rows of level m: each texel of the level before averaged where the level's pixel falls,
// blurred along the row, for rows from kFilterBorder above the level to as far below.
Float4 filter_rows(const SsrUniforms& u, const SsrImage& source, int x, int row) {
    const auto width = static_cast<float>(u.sizes[0]);
    const auto height = static_cast<float>(u.sizes[1]);
    const int y = row - kFilterBorder;
    return gaussian(u, [&](int k) {
        return bilinear(source, {(static_cast<float>(x + k) + 0.5F) / width,
                                 (static_cast<float>(y) + 0.5F) / height});
    });
}

Float4 filter_columns(const SsrUniforms& u, const SsrImage& rows, int x, int y) {
    return gaussian(u, [&](int k) { return texel4(rows, x, y + kFilterBorder + k); });
}

// --- Back to the frame's size ---

// The reflections' levels, from a pass's slots.
std::vector<const SsrImage*> color_levels(std::span<const SsrImage> textures,
                                          std::uint32_t levels) {
    std::vector<const SsrImage*> out;
    for (std::uint32_t level = 0; level < levels; ++level) {
        out.push_back(&textures[kSsrColor + level]);
    }
    return out;
}

// Undoes the trace's tone mapping.
Float4 untonemapped(Float4 color) {
    const float luminance = dot3({color[0], color[1], color[2]}, kLuminance);
    for (std::size_t c = 0; c < 3; ++c) {
        color.at(c) /= 1.0F - luminance;
    }
    return color;
}

struct ResolveTextures {
    const SsrImage* normals = nullptr;
    const SsrImage* depth_half = nullptr;
    const SsrImage* normals_half = nullptr;
    const SsrImage* mip_level = nullptr;
    std::vector<const SsrImage*> color;
};

Float4 resolve_pass(const SsrUniforms& u, const SsrInputs& inputs, const ResolveTextures& t, int x,
                    int y) {
    const float depth = depth_at(u, inputs, x, y);
    const Float4 normal_roughness = texel4(*t.normals, x, y);
    const Float3 normal = decode_normal(normal_roughness);
    const float roughness = decode_roughness(normal_roughness[3]);
    const float half_x = (static_cast<float>(x) + 0.5F) * 0.5F;
    const float half_y = (static_cast<float>(y) + 0.5F) * 0.5F;
    const Float2 bilinear_weights{half_x - std::floor(half_x), half_y - std::floor(half_y)};
    const float half_width = static_cast<float>(u.sizes[0]) * 0.5F;
    const float half_height = static_cast<float>(u.sizes[1]) * 0.5F;
    // Integer division truncates toward zero, as GLSL's does.
    const int base_x = (x - 1) / 2;
    const int base_y = (y - 1) / 2;
    Float4 result{};
    float result_weight = 0.0F;
    for (int i = 0; i < 4; ++i) {
        const int px = base_x + (i % 2);
        const int py = base_y + (i / 2);
        const float sample_depth = texel(*t.depth_half, px, py, 0);
        const Float4 sample_nr = texel4(*t.normals_half, px, py);
        const Float3 sample_normal = decode_normal(sample_nr);
        const float sample_roughness = decode_roughness(sample_nr[3]);
        const float mip_level = texel(*t.mip_level, px, py, 0) * kMipScale;
        const Float4 color = untonemapped(trilinear(t.color,
                                                    {(static_cast<float>(px) + 0.5F) / half_width,
                                                     (static_cast<float>(py) + 0.5F) / half_height},
                                                    mip_level));
        const float weight_depth = slash::exp(-std::abs(depth - sample_depth) * 2048.0F);
        const float weight_normal =
            slash::exp(-saturate(1.0F - dot3(normal, sample_normal)) * 32.0F);
        const float weight_roughness = slash::exp(-std::abs(roughness - sample_roughness) * 16.0F);
        float weight = weight_depth * weight_normal * weight_roughness;
        weight *= ((i % 2) == 0 ? bilinear_weights[0] : 1.0F - bilinear_weights[0]) *
                  ((i / 2) == 0 ? bilinear_weights[1] : 1.0F - bilinear_weights[1]);
        for (std::size_t c = 0; c < 4; ++c) {
            result.at(c) += color.at(c) * weight;
        }
        result_weight += weight;
    }
    if (!(result_weight > 0.0F)) {
        return {};
    }
    for (float& value : result) {
        value /= result_weight;
    }
    return result;
}

// The scene shader's read of full-size reflections: at its mip level, untonemapped.
Float4 mip_pass(const SsrUniforms& u, const SsrImage& mip_levels,
                std::span<const SsrImage* const> color, int x, int y) {
    const float mip_level = texel(mip_levels, x, y, 0) * kMipScale;
    return untonemapped(trilinear(color,
                                  {(static_cast<float>(x) + 0.5F) / static_cast<float>(u.sizes[0]),
                                   (static_cast<float>(y) + 0.5F) / static_cast<float>(u.sizes[1])},
                                  mip_level));
}

// copy.glsl's FLAG_SANITIZE_INF_NAN: infinities and NaNs become 100 (alpha 1).
Float4 sanitized(Float4 color) {
    for (std::size_t c = 0; c < 4; ++c) {
        if (std::isinf(color.at(c)) || std::isnan(color.at(c))) {
            color.at(c) = c == 3 ? 1.0F : 100.0F;
        }
    }
    return color;
}

Float4 last_frame_pass(const SsrUniforms& u, const SsrImage& world, int x, int y) {
    if (u.sizes[2] == u.sizes[0] && u.sizes[3] == u.sizes[1]) {
        return sanitized(texel4(world, x, y));
    }
    // copy.glsl's MODE_MIPMAP.
    const int bx = 2 * x;
    const int by = 2 * y;
    const Float4 a = texel4(world, bx, by);
    const Float4 b = texel4(world, bx, by + 1);
    const Float4 c = texel4(world, bx + 1, by);
    const Float4 d = texel4(world, bx + 1, by + 1);
    Float4 out{};
    for (std::size_t i = 0; i < 4; ++i) {
        out.at(i) = (((a.at(i) + b.at(i)) + c.at(i)) + d.at(i)) / 4.0F;
    }
    return sanitized(out);
}

SsrStep make_step(SsrPass pass, std::uint32_t target, std::uint32_t level = 0) {
    SsrStep out{.pass = pass, .level = level, .target = target, .slots = {}};
    out.slots.fill(kSsrUnbound);
    return out;
}

PostSize level_size(PostSize size, std::uint32_t level) {
    return {std::max(1U, size.width >> level), std::max(1U, size.height >> level)};
}

// Godot's store_camera's matrices are this file's Mat4s, in doubles while they are worked out.
using DMat4 = std::array<double, 16>;

DMat4 to_double(const Mat4& m) {
    DMat4 out{};
    for (std::size_t i = 0; i < 16; ++i) {
        out.at(i) = m.at(i);
    }
    return out;
}

DMat4 multiply_double(const DMat4& a, const DMat4& b) {
    DMat4 out{};
    for (std::size_t column = 0; column < 4; ++column) {
        for (std::size_t row = 0; row < 4; ++row) {
            double sum = 0.0;
            for (std::size_t k = 0; k < 4; ++k) {
                sum += a.at((k * 4) + row) * b.at((column * 4) + k);
            }
            out.at((column * 4) + row) = sum;
        }
    }
    return out;
}

Mat4 to_float(const DMat4& m) {
    Mat4 out{};
    for (std::size_t i = 0; i < 16; ++i) {
        out.at(i) = static_cast<float>(m.at(i));
    }
    return out;
}

} // namespace

SsrFormat ssr_format(std::uint32_t texture) {
    if (texture < kSsrColor) {
        return SsrFormat::R32F;
    }
    if (texture < kSsrRows) {
        return SsrFormat::RGBA16F;
    }
    if (texture < kSsrNormalsHalf) {
        return SsrFormat::RGBA32F;
    }
    if (texture == kSsrNormalsHalf) {
        return SsrFormat::RGBA8;
    }
    if (texture == kSsrMipLevel) {
        return SsrFormat::R8;
    }
    return SsrFormat::RGBA16F;
}

std::uint32_t ssr_channels(SsrFormat format) {
    return format == SsrFormat::R32F || format == SsrFormat::R8 ? 1U : 4U;
}

SsrPlan ssr_plan(const SsrSettings& settings, std::uint32_t width, std::uint32_t height) {
    SsrPlan out;
    const bool half = settings.half_size;
    out.size = half ? PostSize{width / 2, height / 2} : PostSize{width, height};
    out.size.width = std::max(1U, out.size.width);
    out.size.height = std::max(1U, out.size.height);
    // ssr_allocate_buffers: levels until either side is 1.
    std::uint32_t cur_width = out.size.width;
    std::uint32_t cur_height = out.size.height;
    out.levels = 1;
    while (cur_width > 1 && cur_height > 1 && out.levels < kSsrMaxLevels) {
        cur_width /= 2;
        cur_height /= 2;
        ++out.levels;
    }
    for (std::uint32_t level = 0; level < out.levels; ++level) {
        const PostSize size = level_size(out.size, level);
        out.sizes.at(kSsrHiz + level) = size;
        out.sizes.at(kSsrColor + level) = size;
        if (level > 0) {
            out.sizes.at(kSsrRows + level) = {size.width, size.height + (2U * kFilterBorder)};
        }
    }
    out.sizes.at(kSsrMipLevel) = out.size;
    out.sizes.at(kSsrFinal) = {width, height};
    if (half) {
        out.sizes.at(kSsrNormalsHalf) = out.size;
        SsrStep depth = make_step(SsrPass::DownsampleDepth, kSsrHiz);
        depth.slots[0] = kSsrDepthBuffer;
        out.steps.push_back(depth);
        SsrStep normals = make_step(SsrPass::DownsampleNormals, kSsrNormalsHalf);
        normals.slots[0] = kSsrDepthBuffer;
        normals.slots[1] = kSsrNormals;
        out.steps.push_back(normals);
    } else {
        SsrStep depth = make_step(SsrPass::CopyDepth, kSsrHiz);
        depth.slots[0] = kSsrDepthBuffer;
        out.steps.push_back(depth);
    }
    for (std::uint32_t level = 1; level < out.levels; ++level) {
        SsrStep hiz = make_step(SsrPass::Hiz, kSsrHiz + level, level);
        hiz.slots[0] = kSsrHiz + level - 1;
        out.steps.push_back(hiz);
    }
    out.steps.push_back(make_step(SsrPass::CopyLevels, kSsrHizMips));
    SsrStep trace = make_step(SsrPass::Trace, kSsrColor);
    trace.slots[1] = half ? kSsrNormalsHalf : kSsrNormals;
    trace.slots[2] = kSsrHizMips;
    trace.slots[3] = kSsrLastFrame;
    out.steps.push_back(trace);
    for (std::uint32_t level = 1; level < out.levels; ++level) {
        SsrStep rows = make_step(SsrPass::FilterRows, kSsrRows + level, level);
        rows.slots[0] = kSsrColor + level - 1;
        out.steps.push_back(rows);
        SsrStep columns = make_step(SsrPass::FilterColumns, kSsrColor + level, level);
        columns.slots[0] = kSsrRows + level;
        out.steps.push_back(columns);
    }
    out.steps.push_back(make_step(SsrPass::CopyLevels, kSsrColorMips));
    if (half) {
        SsrStep resolve = make_step(SsrPass::Resolve, kSsrFinal);
        resolve.slots[0] = kSsrDepthBuffer;
        resolve.slots[1] = kSsrNormals;
        resolve.slots[4] = kSsrColorMips;
        resolve.slots[5] = kSsrMipLevel;
        resolve.slots[6] = kSsrHiz;
        resolve.slots[7] = kSsrNormalsHalf;
        out.steps.push_back(resolve);
    } else {
        SsrStep mip = make_step(SsrPass::Mip, kSsrFinal);
        mip.slots[4] = kSsrColorMips;
        mip.slots[5] = kSsrMipLevel;
        out.steps.push_back(mip);
    }
    return out;
}

SsrStep ssr_last_frame_step() {
    SsrStep out = make_step(SsrPass::LastFrame, kSsrLastFrame);
    out.slots[0] = kSsrWorld;
    return out;
}

SsrCamera ssr_camera(const Scene3D& scene) {
    // Godot's set_depth_correction flips y, so the corrected projection's y runs down the screen
    // as texture coordinates do; its depth is already reversed and from 0 to 1 here.
    SsrCamera out{.projection = scene.projection, .camera = scene.camera};
    for (std::size_t column = 0; column < 4; ++column) {
        out.projection.at((column * 4) + 1) = -out.projection.at((column * 4) + 1);
    }
    return out;
}

SsrUniforms ssr_uniforms(const Ssr3D& ssr, const SsrStep& step, const SsrCamera& camera,
                         const SsrCamera& last, std::uint32_t width, std::uint32_t height) {
    SsrUniforms out;
    const SsrPlan plan = ssr_plan(ssr.settings, width, height);
    std::uint32_t flags = 0;
    if (camera.projection[15] == 1.0F) {
        flags |= kSsrOrthogonal;
    }
    PostSize screen{};
    PostSize source{};
    switch (step.pass) {
    case SsrPass::DownsampleDepth:
    case SsrPass::DownsampleNormals:
    case SsrPass::CopyDepth:
        screen = plan.size;
        source = {width, height};
        break;
    case SsrPass::Hiz:
        screen = level_size(plan.size, step.level);
        source = level_size(plan.size, step.level - 1);
        break;
    case SsrPass::FilterRows:
    case SsrPass::FilterColumns:
        screen = level_size(plan.size, step.level);
        source = step.pass == SsrPass::FilterRows ? level_size(plan.size, step.level - 1)
                                                  : plan.sizes.at(kSsrRows + step.level);
        break;
    case SsrPass::CopyLevels:
    case SsrPass::Trace:
        screen = plan.size;
        source = plan.size;
        break;
    case SsrPass::Resolve:
    case SsrPass::Mip:
        screen = {width, height};
        source = plan.size;
        break;
    case SsrPass::LastFrame:
        screen = plan.size;
        source = {width, height};
        break;
    }
    if (step.pass == SsrPass::DownsampleDepth || step.pass == SsrPass::DownsampleNormals ||
        step.pass == SsrPass::Hiz) {
        if (source.width % 2 != 0) {
            flags |= kSsrOddWidth;
        }
        if (source.height % 2 != 0) {
            flags |= kSsrOddHeight;
        }
    }
    out.options = {static_cast<std::uint32_t>(step.pass), step.level, flags, plan.levels};
    out.sizes = {screen.width, screen.height, source.width, source.height};
    out.frame = {width, height, plan.size.width, plan.size.height};
    out.steps = {std::max(1U, ssr.max_steps), 0, 0, 0};
    out.fade = {std::max(0.0F, ssr.fade_out), std::max(0.0F, ssr.fade_in), ssr.depth_tolerance,
                0.0F};
    out.projection = camera.projection;
    const DMat4 inv_projection = inverse_matrix(camera.projection);
    out.inv_projection = to_float(inv_projection);
    // The frame's clip space to its camera's space, to the world, to the last camera's space, to
    // its clip space.
    const DMat4 to_world = inverse_matrix(camera.camera);
    out.reprojection = to_float(multiply_double(
        to_double(last.projection),
        multiply_double(to_double(last.camera), multiply_double(to_world, inv_projection))));
    return out;
}

void run_ssr_pass(const SsrUniforms& uniforms, const SsrStep& step,
                  std::span<const SsrImage> textures, const SsrInputs& inputs, SsrImage& target) {
    if (step.pass == SsrPass::CopyLevels) {
        return; // the CPU reads the levels as they are
    }
    const SsrFormat format = ssr_format(step.target);
    const std::uint32_t channels = ssr_channels(format);
    target.channels = channels;
    target.values.assign(static_cast<std::size_t>(target.width) * target.height * channels, 0.0F);
    const auto slot = [&](std::size_t i) -> const SsrImage* {
        const std::uint32_t index = step.slots.at(i);
        if (index == kSsrNormals) {
            return inputs.normals;
        }
        if (index == kSsrLastFrame) {
            return inputs.last_frame;
        }
        if (index == kSsrWorld) {
            return inputs.world;
        }
        return index < textures.size() ? &textures[index] : nullptr;
    };
    const std::uint32_t levels = uniforms.options[3];
    TraceTextures trace;
    ResolveTextures resolve;
    if (step.pass == SsrPass::Trace) {
        trace.normals = slot(1);
        trace.last_frame = slot(3);
        for (std::uint32_t level = 0; level < levels; ++level) {
            trace.hiz.push_back(&textures[kSsrHiz + level]);
        }
    }
    if (step.pass == SsrPass::Resolve || step.pass == SsrPass::Mip) {
        resolve.normals = slot(1);
        resolve.mip_level = slot(5);
        resolve.depth_half = slot(6);
        resolve.normals_half = slot(7);
        resolve.color = color_levels(textures, levels);
    }
    std::vector<float> second;
    if (step.pass == SsrPass::Trace) {
        second.assign(static_cast<std::size_t>(target.width) * target.height, 0.0F);
    }
    for (std::uint32_t y = 0; y < target.height; ++y) {
        for (std::uint32_t x = 0; x < target.width; ++x) {
            const auto ix = static_cast<int>(x);
            const auto iy = static_cast<int>(y);
            Float4 out{};
            switch (step.pass) {
            case SsrPass::DownsampleDepth:
                out[0] = downsample(uniforms, inputs, ix, iy).depth;
                break;
            case SsrPass::DownsampleNormals: {
                const Nearest nearest = downsample(uniforms, inputs, ix, iy);
                out = texel4(*inputs.normals, nearest.x, nearest.y);
                break;
            }
            case SsrPass::CopyDepth:
                out[0] = depth_at(uniforms, inputs, ix, iy);
                break;
            case SsrPass::Hiz:
                out[0] = hiz_pass(uniforms, *slot(0), ix, iy);
                break;
            case SsrPass::Trace: {
                const Traced traced = trace_pass(uniforms, trace, ix, iy);
                out = traced.color;
                second[(static_cast<std::size_t>(y) * target.width) + x] =
                    stored(SsrFormat::R8, traced.mip_level / kMipScale);
                break;
            }
            case SsrPass::FilterRows:
                out = filter_rows(uniforms, *slot(0), ix, iy);
                break;
            case SsrPass::FilterColumns:
                out = filter_columns(uniforms, *slot(0), ix, iy);
                break;
            case SsrPass::Resolve:
                out = resolve_pass(uniforms, inputs, resolve, ix, iy);
                break;
            case SsrPass::Mip:
                out = mip_pass(uniforms, *resolve.mip_level, resolve.color, ix, iy);
                break;
            case SsrPass::LastFrame:
                out = last_frame_pass(uniforms, *slot(0), ix, iy);
                break;
            case SsrPass::CopyLevels:
                break;
            }
            const std::size_t at = ((static_cast<std::size_t>(y) * target.width) + x) * channels;
            for (std::uint32_t c = 0; c < channels; ++c) {
                target.values[at + c] = stored(format, out.at(c));
            }
        }
    }
    if (step.pass == SsrPass::Trace) {
        // The trace's second target: the mip levels, which the caller keeps as kSsrMipLevel.
        target.values.insert(target.values.end(), second.begin(), second.end());
    }
}

SsrImage run_ssr(const Ssr3D& ssr, const SsrCamera& camera, const SsrCamera& last,
                 std::uint32_t width, std::uint32_t height, const SsrInputs& inputs) {
    const SsrPlan plan = ssr_plan(ssr.settings, width, height);
    std::vector<SsrImage> textures(kSsrTextures);
    for (std::uint32_t i = 0; i < kSsrTextures; ++i) {
        SsrImage& texture = textures[i];
        texture.width = plan.sizes.at(i).width;
        texture.height = plan.sizes.at(i).height;
        texture.channels = ssr_channels(ssr_format(i));
        texture.values.assign(
            static_cast<std::size_t>(texture.width) * texture.height * texture.channels, 0.0F);
    }
    for (const SsrStep& step : plan.steps) {
        if (step.pass == SsrPass::CopyLevels) {
            continue;
        }
        const SsrUniforms uniforms = ssr_uniforms(ssr, step, camera, last, width, height);
        SsrImage target = std::move(textures[step.target]);
        run_ssr_pass(uniforms, step, textures, inputs, target);
        if (step.pass == SsrPass::Trace) {
            // Split off the mip levels the pass drew after the colors.
            const std::size_t colors = static_cast<std::size_t>(target.width) * target.height * 4;
            SsrImage& mips = textures[kSsrMipLevel];
            mips.values.assign(target.values.begin() + static_cast<std::ptrdiff_t>(colors),
                               target.values.end());
            target.values.resize(colors);
        }
        textures[step.target] = std::move(target);
    }
    return std::move(textures[kSsrFinal]);
}

SsrImage ssr_last_frame(const SsrSettings& settings, std::uint32_t width, std::uint32_t height,
                        std::span<const float> world) {
    const SsrPlan plan = ssr_plan(settings, width, height);
    const SsrImage source{.width = width,
                          .height = height,
                          .channels = 4,
                          .values = std::vector<float>(world.begin(), world.end())};
    SsrImage out{.width = plan.size.width, .height = plan.size.height, .channels = 4, .values = {}};
    Ssr3D ssr;
    ssr.settings = settings;
    const SsrStep step = ssr_last_frame_step();
    const SsrCamera none{};
    const SsrUniforms uniforms = ssr_uniforms(ssr, step, none, none, width, height);
    run_ssr_pass(uniforms, step, {},
                 {.depth = {}, .normals = nullptr, .last_frame = nullptr, .world = &source}, out);
    return out;
}

} // namespace slash::render::detail

namespace slash::render {

SsrSettings ssr_settings_from_json(const json::Value& value) {
    SsrSettings out;
    if (!value.is_object()) {
        return out;
    }
    if (const json::Value* found = value.find("half_size"); found != nullptr && found->is_bool()) {
        out.half_size = found->as_bool();
    }
    return out;
}

} // namespace slash::render
