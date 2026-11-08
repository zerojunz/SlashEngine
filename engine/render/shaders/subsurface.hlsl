// Subsurface scattering for a Forward+ frame's opaque meshes, as Godot 4.6's
// (servers/rendering/renderer_rd/shaders/effects/subsurface_scattering.glsl): the subsurface
// pass's diffuse light (mesh_subsurface.hlsl) blurred across, then down, by each pixel's strength,
// over a width that narrows with its depth, through Jimenez's kernels (skin's own profile for a
// strength below 0), stopping at pixels with none. Godot's compute shader runs as a pass that
// draws one triangle over its target; it loads texels and filters them in code, keeping every
// load inside its texture, so every device draws the same values. The pass down gives the change
// to the pixel's diffuse light, which is added to the frame, as Godot adds the specular light
// back. Keep it in step with src/subsurface.cpp, its CPU twin.

float4 vs_main(uint id : SV_VertexID) : SV_Position {
    float2 corner = float2((float)((id << 1u) & 2u), (float)(id & 2u));
    return float4(corner * 2.0 - 1.0, 0.0, 1.0);
}

// The pass's source (the subsurface pass's diffuse light, or the blur across), the frame's depth,
// and for the pass down the subsurface pass's diffuse light, whose change it gives.
Texture2D<float4> source_buffer : register(t0, space2);
Texture2D<float4> depth_buffer : register(t1, space2);
Texture2D<float4> diffuse_buffer : register(t2, space2);
SamplerState source_sampler : register(s0, space2);
SamplerState depth_sampler : register(s1, space2);
SamplerState diffuse_sampler : register(s2, space2);

cbuffer Subsurface : register(b0, space3) {
    // The frame's width and height, 1 for the pass down, 1 for an orthogonal camera.
    uint4 size;
    // Godot's unit_size, the scale, the depth scale, the kernel's samples (6, 9 or 13).
    float4 params;
    // The projection's depth rows, from which a depth is made linear: its m10, m11, m14 and m15.
    float4 projection;
};

// Each sample's weight and offset, from the center out, for 11, 17 and 25 samples across.
static const float2 kKernel11[6] = {float2(0.560479, 0.0),  float2(0.0771802, 0.08),
                                    float2(0.0821904, 0.32), float2(0.03639, 0.72),
                                    float2(0.0192831, 1.28), float2(0.00471691, 2.0)};
static const float3 kSkin11[6] = {
    float3(0.560479, 0.669086, 0.784728),     float3(0.0771802, 0.113491, 0.0793803),
    float3(0.0821904, 0.0358608, 0.0209261), float3(0.03639, 0.0130999, 0.00643685),
    float3(0.0192831, 0.00282018, 0.00084214), float3(0.00471691, 0.000184771, 5.07565e-005)};
static const float2 kKernel17[9] = {
    float2(0.536343, 0.0),     float2(0.0324462, 0.03125), float2(0.0582416, 0.125),
    float2(0.0571056, 0.28125), float2(0.0347317, 0.5),     float2(0.0216301, 0.78125),
    float2(0.0144609, 1.125),  float2(0.0100386, 1.53125), float2(0.00317394, 2.0)};
static const float3 kSkin17[9] = {
    float3(0.536343, 0.624624, 0.748867),      float3(0.0324462, 0.0656718, 0.0532821),
    float3(0.0582416, 0.0659959, 0.0411329),   float3(0.0571056, 0.0287432, 0.0172844),
    float3(0.0347317, 0.0151085, 0.00871983),  float3(0.0216301, 0.00794618, 0.00376991),
    float3(0.0144609, 0.00317269, 0.00106399), float3(0.0100386, 0.000914679, 0.000275702),
    float3(0.00317394, 0.000134823, 3.77269e-005)};
static const float2 kKernel25[13] = {
    float2(0.530605, 0.0),      float2(0.0211412, 0.0208333), float2(0.0402784, 0.0833333),
    float2(0.0493588, 0.1875),  float2(0.0410172, 0.333333),  float2(0.0263642, 0.520833),
    float2(0.017924, 0.75),     float2(0.0128496, 1.02083),   float2(0.0094389, 1.33333),
    float2(0.00700976, 1.6875), float2(0.00500364, 2.08333),  float2(0.00333804, 2.52083),
    float2(0.000973794, 3.0)};
static const float3 kSkin25[13] = {
    float3(0.530605, 0.613514, 0.739601),       float3(0.0211412, 0.0459286, 0.0378196),
    float3(0.0402784, 0.0657244, 0.04631),      float3(0.0493588, 0.0367726, 0.0219485),
    float3(0.0410172, 0.0199899, 0.0118481),    float3(0.0263642, 0.0119715, 0.00684598),
    float3(0.017924, 0.00711691, 0.00347194),   float3(0.0128496, 0.00356329, 0.00132016),
    float3(0.0094389, 0.00139119, 0.000416598), float3(0.00700976, 0.00049366, 0.000151938),
    float3(0.00500364, 0.00020094, 5.28848e-005), float3(0.00333804, 7.85443e-005, 1.2945e-005),
    float3(0.000973794, 1.11862e-005, 9.43437e-007)};

// Sample i's offset, and its weight for skin or not.
float kernel_offset(uint samples, uint i) {
    return samples == 13u ? kKernel25[i].y : (samples == 9u ? kKernel17[i].y : kKernel11[i].y);
}
float3 kernel_weight(uint samples, uint i, bool skin) {
    if (skin) {
        return samples == 13u ? kSkin25[i] : (samples == 9u ? kSkin17[i] : kSkin11[i]);
    }
    return (float3)(samples == 13u ? kKernel25[i].x
                                   : (samples == 9u ? kKernel17[i].x : kKernel11[i].x));
}

float4 load_source(int2 texel) {
    return source_buffer.Load(int3(clamp(texel, int2(0, 0), int2(size.xy) - int2(1, 1)), 0));
}

// The source filtered linearly at uv, clamped to its edges, as Godot's linear sampler with repeat
// disabled reads it.
float4 source_linear(float2 uv) {
    float2 position = uv * float2(size.xy) - 0.5;
    float2 base = floor(position);
    float2 f = position - base;
    int2 texel = int2(base);
    float4 top = lerp(load_source(texel), load_source(texel + int2(1, 0)), f.x);
    float4 bottom = lerp(load_source(texel + int2(0, 1)), load_source(texel + int2(1, 1)), f.x);
    return lerp(top, bottom, f.y);
}

// Godot's do_filter: the samples one way from the pixel, until one has no strength.
void filter(inout float3 color, inout float3 divisor, float2 uv, float2 step, bool skin) {
    uint samples = (uint)params.w;
    [loop] for (uint i = 1u; i < samples; ++i) {
        float4 sampled = source_linear(uv + kernel_offset(samples, i) * step);
        if (abs(sampled.a) < 0.001) {
            break;
        }
        float3 w = kernel_weight(samples, i, skin);
        color += sampled.rgb * w;
        divisor += w;
    }
}

float4 fs_main(float4 position : SV_Position) : SV_Target0 {
    int2 pixel = int2(position.xy);
    float2 uv = (float2(pixel) + 0.5) / float2(size.xy);
    float4 base = load_source(pixel);
    float strength = abs(base.a);
    if (strength > 0.0) {
        float2 dir = size.z != 0u ? float2(0.0, 1.0) : float2(1.0, 0.0);
        // The pixel's linear depth, from the reversed depth buffer.
        float depth = depth_buffer.Load(int3(pixel, 0)).r;
        float view_z = (depth * projection.w - projection.z) / (projection.x - depth * projection.y);
        float depth_scale = size.w != 0u ? params.x : params.x / -view_z;
        float scale = lerp(params.y, depth_scale, params.z);
        float2 step = scale * dir * strength / 3.0;
        bool skin = base.a < 0.0;
        float3 divisor = kernel_weight((uint)params.w, 0u, skin);
        float3 color = base.rgb * divisor;
        filter(color, divisor, uv, step, skin);
        filter(color, divisor, uv, -step, skin);
        base.rgb = color / divisor;
    }
    if (size.z == 0u) {
        return base;
    }
    // Down: the change to the pixel's diffuse light, added to the frame.
    float3 diffuse = diffuse_buffer.Load(int3(pixel, 0)).rgb;
    return float4(strength > 0.0 ? base.rgb - diffuse : (float3)0.0, 0.0);
}
