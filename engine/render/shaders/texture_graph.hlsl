// A texture graph's pass (slash/render/texture_graph.hpp): a thread a pixel, the pass's op over
// its inputs' slots into its output slot. run_texture_graph in src/texture_graph.cpp is its CPU
// twin; a change to an op goes in both, and the op numbers and params' layout never change.

#include "fast_noise_lite.hlsl"

#ifdef SLASH_MSL
#define UNIFORMS register(b0)
#define IN0 register(t1)
#define IN1 register(t2)
#define IN2 register(t3)
#define IN3 register(t4)
#define IMAGE register(t5)
#define OUT register(u6)
#else
#define UNIFORMS register(b0, space2)
#define IN0 register(t0, space0)
#define IN1 register(t1, space0)
#define IN2 register(t2, space0)
#define IN3 register(t3, space0)
#define IMAGE register(t4, space0)
#define OUT register(u0, space1)
#endif

cbuffer Params : UNIFORMS {
    // The op, the width and height, 1 when the program tiles.
    uint4 head;
    // A bit per input the pass reads, the image's width and height.
    uint4 more;
    // The pass's 48 params.
    float4 p4[12];
};

StructuredBuffer<float4> in0 : IN0;
StructuredBuffer<float4> in1 : IN1;
StructuredBuffer<float4> in2 : IN2;
StructuredBuffer<float4> in3 : IN3;
StructuredBuffer<float4> image : IMAGE;
RWStructuredBuffer<float4> slot_out : OUT;

static const float kTurn = 6.28318530717958647692;
static const uint kRampAt = 16u;
static const uint kRampPoints = 6u;

float param(uint i) {
    return p4[i >> 2u][i & 3u];
}

int to_int(float v) {
    return v < 0.0 ? (int)(v - 0.5) : (int)(v + 0.5);
}

float clamp01(float v) {
    return clamp(v, 0.0, 1.0);
}

float fract_of(float v) {
    return v - floor(v);
}

float mix1(float a, float b, float t) {
    return a + (b - a) * t;
}

float4 mix4(float4 a, float4 b, float t) {
    return a + (b - a) * t;
}

float smooth(float from, float to, float s) {
    if (from == to) {
        return s < from ? 0.0 : 1.0;
    }
    float t = clamp01((s - from) / (to - from));
    return t * t * (3.0 - 2.0 * t);
}

float luminance(float4 c) {
    return 0.2126 * c.x + 0.7152 * c.y + 0.0722 * c.z;
}

uint pcg_hash(uint v) {
    uint state = v * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float hash01(uint v) {
    return (float)(pcg_hash(v) >> 8u) * (1.0 / 16777216.0);
}

int width_of() {
    return (int)head.y;
}

int height_of() {
    return (int)head.z;
}

int wrap_x(int x) {
    int w = width_of();
    if (head.w != 0u) {
        x %= w;
        return x < 0 ? x + w : x;
    }
    return clamp(x, 0, w - 1);
}

int wrap_y(int y) {
    int h = height_of();
    if (head.w != 0u) {
        y %= h;
        return y < 0 ? y + h : y;
    }
    return clamp(y, 0, h - 1);
}

// Input k at a pixel, wrapped or clamped; white where the pass leaves it out.
float4 at(uint k, int x, int y) {
    if ((more.x & (1u << k)) == 0u) {
        return float4(1.0, 1.0, 1.0, 1.0);
    }
    uint i = (uint)wrap_y(y) * (uint)width_of() + (uint)wrap_x(x);
    if (k == 0u) {
        return in0[i];
    }
    if (k == 1u) {
        return in1[i];
    }
    if (k == 2u) {
        return in2[i];
    }
    return in3[i];
}

// Input k bilinearly at a UV point.
float4 sample_in(uint k, float u, float v) {
    float fx = u * (float)width_of() - 0.5;
    float fy = v * (float)height_of() - 0.5;
    float x0f = floor(fx);
    float y0f = floor(fy);
    float tx = fx - x0f;
    float ty = fy - y0f;
    int x0 = (int)x0f;
    int y0 = (int)y0f;
    float4 a = at(k, x0, y0);
    float4 b = at(k, x0 + 1, y0);
    float4 c = at(k, x0, y0 + 1);
    float4 d = at(k, x0 + 1, y0 + 1);
    return mix4(mix4(a, b, tx), mix4(c, d, tx), ty);
}

float4 image_texel(int x, int y) {
    int w = (int)more.y;
    int h = (int)more.z;
    if (head.w != 0u) {
        x = ((x % w) + w) % w;
        y = ((y % h) + h) % h;
    } else {
        x = clamp(x, 0, w - 1);
        y = clamp(y, 0, h - 1);
    }
    return image[(uint)y * (uint)w + (uint)x];
}

float4 image_sample(float u, float v) {
    float fx = u * (float)more.y - 0.5;
    float fy = v * (float)more.z - 0.5;
    float x0f = floor(fx);
    float y0f = floor(fy);
    float tx = fx - x0f;
    float ty = fy - y0f;
    int x0 = (int)x0f;
    int y0 = (int)y0f;
    float4 a = image_texel(x0, y0);
    float4 b = image_texel(x0 + 1, y0);
    float4 c = image_texel(x0, y0 + 1);
    float4 d = image_texel(x0 + 1, y0 + 1);
    return mix4(mix4(a, b, tx), mix4(c, d, tx), ty);
}

float4 ramp_point(uint i) {
    uint at_word = kRampAt + 1u + i * 5u;
    return float4(param(at_word + 1u), param(at_word + 2u), param(at_word + 3u),
                  param(at_word + 4u));
}

float ramp_offset(uint i) {
    return param(kRampAt + 1u + i * 5u);
}

// The ramp in the params, as Godot's Gradient with linear interpolation in sRGB.
float4 ramp(float offset) {
    int count = clamp(to_int(param(kRampAt)), 0, (int)kRampPoints);
    if (count == 0) {
        return float4(offset, offset, offset, 1.0);
    }
    if (offset <= ramp_offset(0u)) {
        return ramp_point(0u);
    }
    for (int i = 1; i < count; ++i) {
        float b = ramp_offset((uint)i);
        if (offset < b) {
            float a = ramp_offset((uint)(i - 1));
            float t = b > a ? (offset - a) / (b - a) : 0.0;
            return mix4(ramp_point((uint)(i - 1)), ramp_point((uint)i), t);
        }
    }
    return ramp_point((uint)(count - 1));
}

fnl_state noise_state() {
    fnl_state state = fnlCreateState(to_int(param(1u)));
    state.noise_type = to_int(param(0u));
    state.frequency = param(2u);
    state.fractal_type = to_int(param(3u));
    state.octaves = to_int(param(4u));
    state.lacunarity = param(5u);
    state.gain = param(6u);
    state.weighted_strength = param(7u);
    state.ping_pong_strength = param(8u);
    state.cellular_distance_func = to_int(param(9u));
    state.cellular_return_type = to_int(param(10u));
    state.cellular_jitter_mod = param(11u);
    return state;
}

fnl_state warp_state() {
    fnl_state state = fnlCreateState(to_int(param(1u)));
    state.domain_warp_type = to_int(param(13u));
    state.domain_warp_amp = param(14u);
    state.frequency = param(15u);
    // Godot's _convert_domain_warp_fractal_type_enum: none, progressive, independent.
    int fractal = to_int(param(16u));
    state.fractal_type = fractal == 1   ? FNL_FRACTAL_DOMAIN_WARP_PROGRESSIVE
                         : fractal == 2 ? FNL_FRACTAL_DOMAIN_WARP_INDEPENDENT
                                        : FNL_FRACTAL_NONE;
    state.octaves = to_int(param(17u));
    state.lacunarity = param(18u);
    state.gain = param(19u);
    return state;
}

// Godot's FastNoiseLite::get_noise_2d: the offset added, the domain warped, then the noise.
float noise_at(fnl_state noise, fnl_state warp, float x, float y) {
    x += param(20u);
    y += param(21u);
    if (param(12u) > 0.5) {
        fnlDomainWarp2D(warp, x, y);
    }
    return fnlGetNoise2D(noise, x, y);
}

float skirt_weight(int at_px, int edge, int skirt) {
    return 1.0 - smooth(0.1, 0.9, (float)(at_px - edge) / (float)skirt);
}

// The noise pass's value at a pixel: as Godot's Noise::_generate_seamless_image places it when
// seamless (the CPU twin's seamless_noise), without its 8-bit steps. The places it reads are
// gathered first so the noise is sampled at one call site, which keeps the shader small.
float noise_value(int x, int y) {
    fnl_state noise = noise_state();
    fnl_state warp = warp_state();
    int width = width_of();
    int height = height_of();
    float sx = param(23u) / (float)width;
    float sy = param(24u) / (float)height;
    bool seamless = param(25u) > 0.5;
    float skirt = param(26u);
    int skirt_w = (int)max(1.0, (float)width * skirt);
    int skirt_h = (int)max(1.0, (float)height * skirt);
    int half_w = width / 2;
    int half_h = height / 2;
    int src_w = width + skirt_w;
    int src_h = height + skirt_h;
    bool in_x = seamless && x >= half_w && x < half_w + skirt_w;
    bool in_y = seamless && y >= half_h && y < half_h + skirt_h;
    // Without seams, [0] is the pixel's own place. The source image read as Godot's img_buff, with each mode's modulo: [0] both alternate,
    // [1] x alternate, [2] y alternate, [3] neither.
    float values[4];
    for (uint i = 0u; i < 4u; ++i) {
        bool alt_x = i == 0u || i == 1u;
        bool alt_y = i == 0u || i == 2u;
        bool needed = i == 0u || (i == 1u && in_x) || (i == 2u && in_y) ||
                      (i == 3u && in_x && in_y);
        values[i] = 0.0;
        if (needed) {
            int mx = alt_x ? width : src_w;
            int my = alt_y ? height : src_h;
            int sxp = seamless ? (x + half_w) % mx : x;
            int syp = seamless ? (y + half_h) % my : y;
            values[i] = noise_at(noise, warp, (float)sxp * sx, (float)syp * sy);
        }
    }
    if (in_x && in_y) {
        float wx = skirt_weight(x, half_w, skirt_w);
        float wy = skirt_weight(y, half_h, skirt_h);
        float top = mix1(values[3], values[1], wx);
        float bottom = mix1(values[2], values[0], wx);
        return mix1(top, bottom, wy);
    }
    if (in_x) {
        return mix1(values[0], values[1], skirt_weight(x, half_w, skirt_w));
    }
    if (in_y) {
        return mix1(values[0], values[2], skirt_weight(y, half_h, skirt_h));
    }
    return values[0];
}

// GradientTexture2D::_get_gradient_offset_at.
float gradient_offset(float u, float v) {
    float fx = param(1u);
    float fy = param(2u);
    float tx = param(3u);
    float ty = param(4u);
    if (fx == tx && fy == ty) {
        return 0.0;
    }
    float pos = 0.0;
    int fill = to_int(param(0u));
    if (fill == 1) {
        float dx = u - fx;
        float dy = v - fy;
        float rx = tx - fx;
        float ry = ty - fy;
        pos = sqrt(dx * dx + dy * dy) / sqrt(rx * rx + ry * ry);
    } else if (fill == 2) {
        float dx = abs(u - fx);
        float dy = abs(v - fy);
        float rx = abs(tx - fx);
        float ry = abs(ty - fy);
        pos = max(dx, dy) / max(rx, ry);
    } else {
        float rx = tx - fx;
        float ry = ty - fy;
        pos = ((u - fx) * rx + (v - fy) * ry) / (rx * rx + ry * ry);
    }
    int repeat = to_int(param(5u));
    if (repeat == 1) {
        pos = fract_of(pos);
    } else if (repeat == 2) {
        pos = abs(pos - 2.0 * floor(pos * 0.5 + 0.5));
    } else {
        pos = clamp01(pos);
    }
    return pos;
}

float shape_value(float u, float v) {
    float cells_x = max(1.0, param(1u));
    float cells_y = max(1.0, param(2u));
    float x = fract_of(u * cells_x) - 0.5;
    float y = fract_of(v * cells_y) - 0.5;
    float angle_turn = param(6u) * kTurn;
    float c = cos(angle_turn);
    float s = sin(angle_turn);
    float rx = x * c + y * s;
    float ry = -x * s + y * c;
    x = rx;
    y = ry;
    float size = max(1e-4, param(3u)) * 0.5;
    float soft = max(1e-4, param(5u) * size);
    float d = 0.0;
    int kind = to_int(param(0u));
    if (kind == 1) {
        d = max(abs(x), abs(y)) - size;
    } else if (kind == 2) {
        float sides = max(3.0, floor(param(4u)));
        float angle = atan2(x, -y);
        float sector = kTurn / sides;
        float a = angle - sector * floor(angle / sector + 0.5);
        d = cos(a) * sqrt(x * x + y * y) - size * cos(sector * 0.5);
    } else if (kind == 3) {
        float r = sqrt(x * x + y * y);
        float half_ring = max(1e-4, param(7u)) * size * 0.5;
        d = abs(r - (size - half_ring)) - half_ring;
    } else {
        d = sqrt(x * x + y * y) - size;
    }
    return 1.0 - smooth(-soft, 0.0, d);
}

float4 bricks(float u, float v) {
    float across = max(1.0, param(0u));
    float down = max(1.0, param(1u));
    float by = v * down;
    float row = floor(by);
    float bx = u * across + param(3u) * row;
    float col = floor(bx);
    float fx = bx - col;
    float fy = by - row;
    float aspect = down / across;
    float ex = min(fx, 1.0 - fx) * (1.0 / max(aspect, 1e-4));
    float ey = min(fy, 1.0 - fy);
    float edge = min(ex * aspect, ey);
    float mortar = param(2u) * 0.5;
    float bevel = max(1e-4, param(4u));
    float height = clamp01((edge - mortar) / bevel);
    uint wrapped_col = (uint)((int)col - (int)across * (int)floor(col / across));
    uint row_id = (uint)((int)row - (int)down * (int)floor(row / down));
    uint id = wrapped_col + row_id * 4096u;
    uint seed = (uint)to_int(param(5u));
    return float4(edge > mortar ? height : 0.0, hash01(id ^ pcg_hash(seed)),
                  (float)(id % 1024u) / 1023.0, 1.0);
}

float waves(float u, float v) {
    float angle = param(1u) * kTurn;
    float t = (u * cos(angle) + v * sin(angle)) * param(0u) + param(3u);
    int kind = to_int(param(2u));
    if (kind == 1) {
        return 1.0 - abs(fract_of(t) * 2.0 - 1.0);
    }
    if (kind == 2) {
        return fract_of(t) < 0.5 ? 1.0 : 0.0;
    }
    return 0.5 + 0.5 * sin(fract_of(t) * kTurn);
}

float4 hsv_adjust(float4 c, float hue, float sat, float val) {
    float r = c.x;
    float g = c.y;
    float b = c.z;
    float maxc = max(r, max(g, b));
    float minc = min(r, min(g, b));
    float delta = maxc - minc;
    float h = 0.0;
    if (delta > 0.0) {
        if (maxc == r) {
            h = (g - b) / delta;
        } else if (maxc == g) {
            h = 2.0 + (b - r) / delta;
        } else {
            h = 4.0 + (r - g) / delta;
        }
        h = fract_of(h / 6.0);
    }
    float s = maxc > 0.0 ? delta / maxc : 0.0;
    float value = maxc;
    h = fract_of(h + hue);
    s = clamp01(s * sat);
    value = value * val;
    float h6 = h * 6.0;
    float sector = floor(h6);
    float f = h6 - sector;
    float pv = value * (1.0 - s);
    float qv = value * (1.0 - s * f);
    float tv = value * (1.0 - s * (1.0 - f));
    int which = (int)sector % 6;
    if (which == 0) {
        return float4(value, tv, pv, c.w);
    }
    if (which == 1) {
        return float4(qv, value, pv, c.w);
    }
    if (which == 2) {
        return float4(pv, value, tv, c.w);
    }
    if (which == 3) {
        return float4(pv, qv, value, c.w);
    }
    if (which == 4) {
        return float4(tv, pv, value, c.w);
    }
    return float4(value, pv, qv, c.w);
}

float blend_channel(int mode, float a, float b) {
    if (mode == 1) {
        return a + b;
    }
    if (mode == 2) {
        return a - b;
    }
    if (mode == 3) {
        return a * b;
    }
    if (mode == 4) {
        return 1.0 - (1.0 - a) * (1.0 - b);
    }
    if (mode == 5) {
        return a < 0.5 ? 2.0 * a * b : 1.0 - 2.0 * (1.0 - a) * (1.0 - b);
    }
    if (mode == 6) {
        return min(a, b);
    }
    if (mode == 7) {
        return max(a, b);
    }
    if (mode == 8) {
        return abs(a - b);
    }
    if (mode == 9) {
        return (1.0 - 2.0 * b) * a * a + 2.0 * b * a;
    }
    if (mode == 10) {
        return b > 0.0 ? a / b : 1.0;
    }
    return b;
}

float math_channel(int op, float a, float b) {
    if (op == 1) {
        return a - b;
    }
    if (op == 2) {
        return a * b;
    }
    if (op == 3) {
        return b != 0.0 ? a / b : 0.0;
    }
    if (op == 4) {
        return min(a, b);
    }
    if (op == 5) {
        return max(a, b);
    }
    if (op == 6) {
        return a > 0.0 ? exp(b * log(a)) : 0.0;
    }
    if (op == 7) {
        return abs(a);
    }
    if (op == 8) {
        return fract_of(a);
    }
    if (op == 9) {
        return 1.0 - a;
    }
    return a + b;
}

float channel_of(float4 c, int which) {
    if (which >= 0 && which < 4) {
        return c[which];
    }
    return which == 5 ? 1.0 : 0.0;
}

float4 gray(float g) {
    return float4(g, g, g, 1.0);
}

float4 run_pixel(int x, int y) {
    float u = ((float)x + 0.5) / (float)width_of();
    float v = ((float)y + 0.5) / (float)height_of();
    uint op = head.x;
    if (op == 0u) {
        return float4(param(0u), param(1u), param(2u), param(3u));
    }
    if (op == 1u) {
        return gray(noise_value(x, y) * 0.5 + 0.5);
    }
    if (op == 2u) {
        return ramp(gradient_offset(u, v));
    }
    if (op == 3u) {
        return gray(shape_value(u, v));
    }
    if (op == 4u) {
        return bricks(u, v);
    }
    if (op == 5u) {
        int cx = (int)floor(u * max(1.0, param(0u)));
        int cy = (int)floor(v * max(1.0, param(1u)));
        return gray(((cx + cy) & 1) != 0 ? 1.0 : 0.0);
    }
    if (op == 6u) {
        return gray(waves(u, v));
    }
    if (op == 7u) {
        return image_sample(u, v);
    }
    if (op == 8u) {
        float4 c = at(0u, x, y);
        float range = max(1e-6, param(1u) - param(0u));
        float gamma = max(1e-4, param(2u));
        for (uint k = 0u; k < 3u; ++k) {
            float t = clamp01((c[k] - param(0u)) / range);
            float curved = gamma == 1.0 ? t : (t > 0.0 ? exp(log(t) / gamma) : 0.0);
            c[k] = mix1(param(3u), param(4u), curved);
        }
        return c;
    }
    if (op == 9u) {
        float4 c = at(0u, x, y);
        return float4(1.0 - c.xyz, c.w);
    }
    if (op == 10u) {
        return clamp(at(0u, x, y), 0.0, 1.0);
    }
    if (op == 11u) {
        return ramp(clamp01(luminance(at(0u, x, y))));
    }
    if (op == 12u) {
        return hsv_adjust(at(0u, x, y), param(0u), param(1u), param(2u));
    }
    if (op == 13u) {
        int radius = clamp(to_int(param(0u)), 0, 256);
        bool across = to_int(param(1u)) == 0;
        float4 sum = float4(0.0, 0.0, 0.0, 0.0);
        float total = 0.0;
        for (int d = -radius; d <= radius; ++d) {
            float t = (float)d / (float)(radius + 1);
            float w0 = 1.0 - t * t;
            float w = w0 * w0;
            float4 c = across ? at(0u, x + d, y) : at(0u, x, y + d);
            sum += c * w;
            total += w;
        }
        return sum / total;
    }
    if (op == 14u) {
        float4 c = at(0u, x, y);
        float4 around = (at(0u, x - 1, y) + at(0u, x + 1, y) + at(0u, x, y - 1) +
                         at(0u, x, y + 1)) * 0.25;
        return float4(c.xyz + param(0u) * (c.xyz - around.xyz), c.w);
    }
    if (op == 15u) {
        float du = 0.0;
        float dv = 0.0;
        if (to_int(param(1u)) == 1) {
            float angle = param(2u) * kTurn;
            float g = at(1u, x, y).x;
            du = cos(angle) * g * param(0u);
            dv = sin(angle) * g * param(0u);
        } else {
            float gx = luminance(at(1u, x + 1, y)) - luminance(at(1u, x - 1, y));
            float gy = luminance(at(1u, x, y + 1)) - luminance(at(1u, x, y - 1));
            du = gx * 0.5 * param(0u);
            dv = gy * 0.5 * param(0u);
        }
        return sample_in(0u, u + du, v + dv);
    }
    if (op == 16u) {
        float angle = param(2u) * kTurn;
        float c = cos(angle);
        float s = sin(angle);
        float cu = u - 0.5;
        float cv = v - 0.5;
        float ru = cu * c + cv * s;
        float rv = -cu * s + cv * c;
        float su = ru * param(0u) + 0.5 + param(3u);
        float sv = rv * param(1u) + 0.5 + param(4u);
        if (head.w != 0u) {
            su = fract_of(su);
            sv = fract_of(sv);
        }
        return sample_in(0u, su, sv);
    }
    if (op == 17u) {
        float l00 = luminance(at(0u, x - 1, y - 1));
        float l10 = luminance(at(0u, x, y - 1));
        float l20 = luminance(at(0u, x + 1, y - 1));
        float l01 = luminance(at(0u, x - 1, y));
        float l21 = luminance(at(0u, x + 1, y));
        float l02 = luminance(at(0u, x - 1, y + 1));
        float l12 = luminance(at(0u, x, y + 1));
        float l22 = luminance(at(0u, x + 1, y + 1));
        float gx = (l20 + 2.0 * l21 + l22) - (l00 + 2.0 * l01 + l02);
        float gy = (l02 + 2.0 * l12 + l22) - (l00 + 2.0 * l10 + l20);
        return gray(sqrt(gx * gx + gy * gy) * param(0u));
    }
    if (op == 18u) {
        float4 c = at(0u, x, y);
        float g = luminance(c);
        return float4(g, g, g, c.w);
    }
    if (op == 19u) {
        float4 c = at(0u, x, y);
        float g = smooth(param(0u) - param(1u), param(0u) + param(1u), luminance(c));
        return float4(g, g, g, c.w);
    }
    if (op == 20u) {
        float4 c = at(0u, x, y);
        float steps = max(1.0, floor(param(0u)));
        return float4(floor(clamp(c.xyz, 0.0, 1.0) * steps + 0.5) / steps, c.w);
    }
    if (op == 21u) {
        float4 merged;
        for (uint k = 0u; k < 4u; ++k) {
            merged[k] = (more.x & (1u << k)) == 0u ? param(4u + k)
                                                   : channel_of(at(k, x, y), to_int(param(k)));
        }
        return merged;
    }
    if (op == 22u) {
        float4 c = at(0u, x, y);
        return float4(channel_of(c, to_int(param(0u))), channel_of(c, to_int(param(1u))),
                      channel_of(c, to_int(param(2u))), channel_of(c, to_int(param(3u))));
    }
    if (op == 23u) {
        float dx = (at(0u, x + 1, y).x - at(0u, x - 1, y).x) * 0.5 * param(0u);
        float dy = (at(0u, x, y + 1).x - at(0u, x, y - 1).x) * 0.5 * param(0u);
        float nx = -dx;
        float ny = dy;
        float len = sqrt(nx * nx + ny * ny + 1.0);
        return float4(0.5 + 0.5 * nx / len, 0.5 + 0.5 * ny / len, 0.5 + 0.5 / len, 1.0);
    }
    if (op == 24u) {
        float h = at(0u, x, y).x;
        int radius = clamp(to_int(param(0u)), 1, 64);
        const int2 dirs[8] = {int2(1, 0), int2(-1, 0), int2(0, 1), int2(0, -1), int2(1, 1),
                              int2(-1, -1), int2(1, -1), int2(-1, 1)};
        float occlusion = 0.0;
        for (uint k = 0u; k < 8u; ++k) {
            float most = 0.0;
            for (int step = 1; step <= radius; ++step) {
                float rise = at(0u, x + dirs[k].x * step, y + dirs[k].y * step).x - h;
                most = max(most, rise * (float)radius / (float)step);
            }
            occlusion += clamp01(most);
        }
        return gray(clamp01(1.0 - occlusion * 0.125 * param(1u)));
    }
    if (op == 25u) {
        float lap = at(0u, x + 1, y).x + at(0u, x - 1, y).x + at(0u, x, y + 1).x +
                    at(0u, x, y - 1).x - 4.0 * at(0u, x, y).x;
        return gray(clamp01(0.5 - lap * param(0u)));
    }
    if (op == 26u) {
        float4 a = at(0u, x, y);
        float4 b = at(1u, x, y);
        float mask = at(2u, x, y).x;
        float t = clamp01(param(1u) * mask * b.w);
        int mode = to_int(param(0u));
        float4 blended = a;
        for (uint k = 0u; k < 3u; ++k) {
            blended[k] = mix1(a[k], blend_channel(mode, a[k], b[k]), t);
        }
        return blended;
    }
    if (op == 27u) {
        float4 a = at(0u, x, y);
        float4 b = (more.x & 2u) == 0u ? float4(param(2u), param(3u), param(4u), param(5u))
                                       : at(1u, x, y);
        int which = to_int(param(0u));
        float4 result;
        for (uint k = 0u; k < 4u; ++k) {
            result[k] = math_channel(which, a[k], b[k]);
        }
        return result;
    }
    if (op == 28u) {
        float4 c = at(0u, x, y);
        float range = param(1u) - param(0u);
        for (uint k = 0u; k < 3u; ++k) {
            float t = range != 0.0 ? (c[k] - param(0u)) / range : 0.0;
            if (param(4u) > 0.5) {
                t = clamp01(t);
            }
            c[k] = mix1(param(2u), param(3u), t);
        }
        return c;
    }
    return float4(0.0, 0.0, 0.0, 1.0);
}

[numthreads(8, 8, 1)] void cs_main(uint3 id : SV_DispatchThreadID, uint index : SV_GroupIndex) {
    if (head.x == 1u) {
        fnlLoadTables(index, 64u);
    }
    GroupMemoryBarrierWithGroupSync();
    if (id.x >= head.y || id.y >= head.z) {
        return;
    }
    slot_out[id.y * head.y + id.x] = run_pixel((int)id.x, (int)id.y);
}
