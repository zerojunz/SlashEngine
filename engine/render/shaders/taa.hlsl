// Temporal antialiasing for a Forward+ frame's 3D world, as Godot 4.6's TAA resolve
// (servers/rendering/renderer_rd/shaders/effects/taa_resolve.glsl, MIT, Copyright (c) 2016-2022
// Panos Karabelas, based on Spartan Engine's TAA): the frame, drawn with its camera jittered,
// blended with the frame before's result, moved by the prepass's motion vectors and clipped to the
// colors around each pixel. Godot's compute shader runs as a pass that draws one triangle over its
// target; it loads texels and filters the history in code, keeping every load inside its texture,
// so every device draws the same values. Keep it in step with src/taa.cpp, its CPU twin.

float4 vs_main(uint id : SV_VertexID) : SV_Position {
    float2 corner = float2((float)((id << 1u) & 2u), (float)(id & 2u));
    return float4(corner * 2.0 - 1.0, 0.0, 1.0);
}

// The frame's color, the prepass's depth, its motion vectors and the frame before's, and the
// history. All are loaded, never sampled.
Texture2D<float4> color_buffer : register(t0, space2);
Texture2D<float4> depth_buffer : register(t1, space2);
Texture2D<float4> velocity_buffer : register(t2, space2);
Texture2D<float4> last_velocity_buffer : register(t3, space2);
Texture2D<float4> history_buffer : register(t4, space2);
SamplerState color_sampler : register(s0, space2);
SamplerState depth_sampler : register(s1, space2);
SamplerState velocity_sampler : register(s2, space2);
SamplerState last_velocity_sampler : register(s3, space2);
SamplerState history_sampler : register(s4, space2);

cbuffer Taa : register(b0, space3) {
    uint4 size;    // the frame's width and height
    float4 params; // the disocclusion threshold in texels, the variance clipping's box size
};

static const float FLT_MIN = 0.00000001;
static const float FLT_MAX = 32767.0;
static const float RPC_9 = 0.11111111111;
static const float RPC_16 = 0.0625;
// Scales the weight of a pixel whose motion changed by more than the threshold.
static const float DISOCCLUSION_SCALE = 0.01;
static const int2 kOffsets3x3[9] = {int2(-1, -1), int2(0, -1), int2(1, -1),
                                    int2(-1, 0),  int2(0, 0),  int2(1, 0),
                                    int2(-1, 1),  int2(0, 1),  int2(1, 1)};
static const float3 lumCoeff = float3(0.299, 0.587, 0.114);

int2 inside(int2 texel) {
    return clamp(texel, int2(0, 0), int2(size.xy) - int2(1, 1));
}

float3 load_color(int2 texel) {
    return color_buffer.Load(int3(inside(texel), 0)).rgb;
}
float load_depth(int2 texel) {
    return depth_buffer.Load(int3(inside(texel), 0)).r;
}
float2 load_velocity(int2 texel) {
    return velocity_buffer.Load(int3(inside(texel), 0)).xy;
}
float3 load_history(int2 texel) {
    return history_buffer.Load(int3(inside(texel), 0)).rgb;
}

float3 reinhard(float3 hdr) {
    return hdr / (hdr + 1.0);
}
float3 reinhard_inverse(float3 sdr) {
    return sdr / (1.0 - sdr);
}

// The history filtered linearly at uv, clamped to its edges, as Godot's linear sampler with
// repeat disabled reads it.
float3 history_linear(float2 uv) {
    float2 position = uv * float2(size.xy) - 0.5;
    float2 base = floor(position);
    float2 f = position - base;
    int2 texel = int2(base);
    float3 top = lerp(load_history(texel), load_history(texel + int2(1, 0)), f.x);
    float3 bottom = lerp(load_history(texel + int2(0, 1)), load_history(texel + int2(1, 1)), f.x);
    return lerp(top, bottom, f.y);
}

// Godot's sample_catmull_rom_9: the history at uv, filtered with a Catmull-Rom spline over 4x4
// texels in nine linear reads.
float3 sample_catmull_rom_9(float2 uv, float2 resolution) {
    float2 sample_pos = uv * resolution;
    float2 texPos1 = floor(sample_pos - 0.5) + 0.5;
    float2 f = sample_pos - texPos1;
    float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    float2 w3 = f * f * (-0.5 + 0.5 * f);
    float2 w12 = w1 + w2;
    float2 offset12 = w2 / (w1 + w2);
    float2 texPos0 = (texPos1 - 1.0) / resolution;
    float2 texPos3 = (texPos1 + 2.0) / resolution;
    float2 texPos12 = (texPos1 + offset12) / resolution;

    float3 result = float3(0.0, 0.0, 0.0);
    result += history_linear(float2(texPos0.x, texPos0.y)) * w0.x * w0.y;
    result += history_linear(float2(texPos12.x, texPos0.y)) * w12.x * w0.y;
    result += history_linear(float2(texPos3.x, texPos0.y)) * w3.x * w0.y;

    result += history_linear(float2(texPos0.x, texPos12.y)) * w0.x * w12.y;
    result += history_linear(float2(texPos12.x, texPos12.y)) * w12.x * w12.y;
    result += history_linear(float2(texPos3.x, texPos12.y)) * w3.x * w12.y;

    result += history_linear(float2(texPos0.x, texPos3.y)) * w0.x * w3.y;
    result += history_linear(float2(texPos12.x, texPos3.y)) * w12.x * w3.y;
    result += history_linear(float2(texPos3.x, texPos3.y)) * w3.x * w3.y;
    return max(result, 0.0);
}

// Godot's clip_aabb, from Playdead's "Temporal Reprojection Anti-Aliasing".
float3 clip_aabb(float3 aabb_min, float3 aabb_max, float3 p, float3 q) {
    float3 r = q - p;
    float3 rmax = aabb_max - p;
    float3 rmin = aabb_min - p;
    if (r.x > rmax.x + FLT_MIN) {
        r *= (rmax.x / r.x);
    }
    if (r.y > rmax.y + FLT_MIN) {
        r *= (rmax.y / r.y);
    }
    if (r.z > rmax.z + FLT_MIN) {
        r *= (rmax.z / r.z);
    }
    if (r.x < rmin.x - FLT_MIN) {
        r *= (rmin.x / r.x);
    }
    if (r.y < rmin.y - FLT_MIN) {
        r *= (rmin.y / r.y);
    }
    if (r.z < rmin.z - FLT_MIN) {
        r *= (rmin.z / r.z);
    }
    return p + r;
}

float luminance(float3 color) {
    return max(dot(color, lumCoeff), 0.0001);
}

float4 fs_main(float4 position : SV_Position) : SV_Target0 {
    float2 resolution = float2(size.xy);
    int2 pos_screen = int2(position.xy);
    float2 uv = (float2(pos_screen) + 0.5) / resolution;
    float2 velocity = load_velocity(pos_screen);
    float2 uv_reprojected = uv + velocity;
    float3 color_input = load_color(pos_screen);
    float3 color_history = sample_catmull_rom_9(uv_reprojected, resolution);

    // get_closest_pixel_velocity_3x3: the motion of the neighbor of least depth. With reversed
    // depth, as Godot 4.6 has it, that is the farthest.
    float min_depth = 1.0;
    int2 min_pos = pos_screen;
    [unroll] for (uint i = 0u; i < 9u; ++i) {
        float depth = load_depth(pos_screen + kOffsets3x3[i]);
        if (depth < min_depth) {
            min_depth = depth;
            min_pos = pos_screen + kOffsets3x3[i];
        }
    }
    float2 velocity_closest = load_velocity(min_pos);

    // clip_history_3x3: variance clipping to the neighborhood, its box shrinking with motion.
    float3 color_avg = float3(0.0, 0.0, 0.0);
    float3 color_avg2 = float3(0.0, 0.0, 0.0);
    [unroll] for (uint j = 0u; j < 9u; ++j) {
        float3 s = load_color(pos_screen + kOffsets3x3[j]);
        color_avg += s;
        color_avg2 += s * s;
    }
    color_avg *= RPC_9;
    color_avg2 *= RPC_9;
    // smoothstep(0.02, 0.0, length), written out, as SPIR-V leaves it undefined.
    float t = saturate((length(velocity_closest) - 0.02) / (0.0 - 0.02));
    float box_size = lerp(0.0, params.y, t * t * (3.0 - 2.0 * t));
    float3 dev = sqrt(abs(color_avg2 - (color_avg * color_avg))) * box_size;
    float3 color_min = color_avg - dev;
    float3 color_max = color_avg + dev;
    color_history = clip_aabb(color_min, color_max, clamp(color_avg, color_min, color_max),
                              color_history);
    color_history = clamp(color_history, FLT_MIN, FLT_MAX);

    float blend_factor = RPC_16;
    {
        float factor_screen = any(uv_reprojected < 0.0) || any(uv_reprojected > 1.0) ? 1.0 : 0.0;
        // get_factor_disocclusion: more of the frame where the motion changed.
        float2 velocity_previous =
            last_velocity_buffer.Load(int3(inside(int2(uv_reprojected * resolution)), 0)).xy;
        float2 velocity_texels = velocity * resolution;
        float2 prev_velocity_texels = velocity_previous * resolution;
        float disocclusion = length(prev_velocity_texels - velocity_texels) - params.x;
        float factor_disocclusion = clamp(disocclusion * DISOCCLUSION_SCALE, 0.0, 1.0);
        blend_factor = clamp(blend_factor + factor_screen + factor_disocclusion, 0.0, 1.0);
    }

    color_history = reinhard(color_history);
    color_input = reinhard(color_input);
    // Less of the frame where it flickers against the history.
    float lum_color = luminance(color_input);
    float lum_history = luminance(color_history);
    float diff = abs(lum_color - lum_history) / max(lum_color, max(lum_history, 1.001));
    diff = 1.0 - diff;
    diff = diff * diff;
    blend_factor = lerp(0.0, blend_factor, diff);
    float3 color_resolved = reinhard_inverse(lerp(color_history, color_input, blend_factor));
    return float4(color_resolved, 1.0);
}
