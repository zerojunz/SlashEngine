// What texture graph passes share that reads nothing of a pass: shaders/texture_graph.hlsl's
// compute passes and live graphs (procedural::texture_graph_shader), which include
// texture_graph_ops.hlsl once a pass. run_texture_graph in src/texture_graph.cpp is the CPU twin.

#ifndef SLASH_TEXTURE_GRAPH_COMMON
#define SLASH_TEXTURE_GRAPH_COMMON

#include "fast_noise_lite.hlsl"

static const float kTgTurn = 6.28318530717958647692;
static const uint kTgRampAt = 16u;
static const uint kTgRampPoints = 6u;

// v wrapped into 0 to n - 1 (n > 0), with % taken only of numbers that aren't negative: GLSL
// leaves it undefined for those, and Mesa's OpenGL gives other numbers there.
int tg_wrap_int(int v, int n) {
    return v >= 0 ? v % n : n - 1 - (-(v + 1)) % n;
}

int tg_to_int(float v) {
    return v < 0.0 ? (int)(v - 0.5) : (int)(v + 0.5);
}

float tg_clamp01(float v) {
    return clamp(v, 0.0, 1.0);
}

float tg_fract(float v) {
    return v - floor(v);
}

float tg_mix1(float a, float b, float t) {
    return a + (b - a) * t;
}

float4 tg_mix4(float4 a, float4 b, float t) {
    return a + (b - a) * t;
}

float tg_smooth(float from, float to, float s) {
    if (from == to) {
        return s < from ? 0.0 : 1.0;
    }
    float t = tg_clamp01((s - from) / (to - from));
    return t * t * (3.0 - 2.0 * t);
}

float tg_luminance(float4 c) {
    return 0.2126 * c.x + 0.7152 * c.y + 0.0722 * c.z;
}

uint tg_pcg_hash(uint v) {
    uint state = v * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float tg_hash01(uint v) {
    return (float)(tg_pcg_hash(v) >> 8u) * (1.0 / 16777216.0);
}

float tg_skirt_weight(int at_px, int edge, int skirt) {
    return 1.0 - tg_smooth(0.1, 0.9, (float)(at_px - edge) / (float)skirt);
}

float4 tg_hsv_adjust(float4 c, float hue, float sat, float val) {
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
        h = tg_fract(h / 6.0);
    }
    float s = maxc > 0.0 ? delta / maxc : 0.0;
    float value = maxc;
    h = tg_fract(h + hue);
    s = tg_clamp01(s * sat);
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

float tg_blend_channel(int mode, float a, float b) {
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

float tg_math_channel(int op, float a, float b) {
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
        return tg_fract(a);
    }
    if (op == 9) {
        return 1.0 - a;
    }
    return a + b;
}

float tg_channel_of(float4 c, int which) {
    if (which >= 0 && which < 4) {
        return c[which];
    }
    return which == 5 ? 1.0 : 0.0;
}

float4 tg_gray(float g) {
    return float4(g, g, g, 1.0);
}

#endif
