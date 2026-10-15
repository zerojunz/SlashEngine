// The material shader: runs a material program (engine/render/include/slash/render/material.hpp)
// for each pixel. Canvas materials compile from node graphs to a few instructions, so they need
// no shader compiler at run time. Keep it in step with run_program in src/material.cpp.
// Registers follow SDL's GPU conventions: vertex uniforms in space1, fragment textures and
// samplers in space2 and fragment uniforms in space3.

cbuffer View : register(b0, space1) {
    // Maps positions to clip space; column-major, so it multiplies column vectors.
    float4x4 view_projection;
};

struct VertexInput {
    float2 position : TEXCOORD0;
    float2 uv : TEXCOORD1;
    float4 color : TEXCOORD2;
};

struct VertexOutput {
    float2 uv : TEXCOORD0;
    float4 color : TEXCOORD1;
    float4 position : SV_Position;
};

VertexOutput vs_main(VertexInput input) {
    VertexOutput output;
    output.position = mul(view_projection, float4(input.position, 0.0, 1.0));
    output.uv = input.uv;
    output.color = input.color;
    return output;
}

// Slot 0 is the draw's texture; the material binds the others, or white.
Texture2D<float4> texture0 : register(t0, space2);
Texture2D<float4> texture1 : register(t1, space2);
Texture2D<float4> texture2 : register(t2, space2);
Texture2D<float4> texture3 : register(t3, space2);
SamplerState sampler0 : register(s0, space2);
SamplerState sampler1 : register(s1, space2);
SamplerState sampler2 : register(s2, space2);
SamplerState sampler3 : register(s3, space2);

cbuffer Program : register(b0, space3) {
    // Each: op | destination << 8 | aux << 16, then the operands a, b and c.
    uint4 code[64];
    float4 constants[32];
    // One over texture0's size, in pixels.
    float4 texel;
    // x: how many instructions to run.
    uint4 counts;
};

static float4 registers[32];

// An operand: a register (0 to 31) or constant (32 to 63) in bits 0 to 7, then two bits for each
// component, picking x, y, z or w.
float4 fetch(uint operand) {
    uint index = operand & 0xFFu;
    float4 value;
    if (index < 32u) {
        value = registers[index];
    } else {
        value = constants[(index - 32u) & 31u];
    }
    uint pick = operand >> 8u;
    return float4(value[pick & 3u], value[(pick >> 2u) & 3u], value[(pick >> 4u) & 3u],
                  value[(pick >> 6u) & 3u]);
}

// 1 in the first count components, 0 in the rest.
float4 first(uint count) {
    return float4(count > 0u ? 1.0 : 0.0, count > 1u ? 1.0 : 0.0, count > 2u ? 1.0 : 0.0,
                  count > 3u ? 1.0 : 0.0);
}

float4 splat(float value) {
    return float4(value, value, value, value);
}

// There are no mipmaps, so level 0 is what Sample would read, and an explicit level is allowed in
// the loop's flow control. The slots are read in order, so SPIRV-Cross numbers Metal's textures
// and samplers 0 to 3, as SDL binds them.
float4 sample_slot(uint slot, float2 uv) {
    if (slot == 0u) {
        return texture0.SampleLevel(sampler0, uv, 0.0);
    }
    if (slot == 1u) {
        return texture1.SampleLevel(sampler1, uv, 0.0);
    }
    if (slot == 2u) {
        return texture2.SampleLevel(sampler2, uv, 0.0);
    }
    return texture3.SampleLevel(sampler3, uv, 0.0);
}

float4 rgb_to_hsv(float4 c) {
    float4 k = float4(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0);
    float4 p = c.g < c.b ? float4(c.b, c.g, k.w, k.z) : float4(c.g, c.b, k.x, k.y);
    float4 q = c.r < p.x ? float4(p.x, p.y, p.w, c.r) : float4(c.r, p.y, p.z, p.x);
    float d = q.x - min(q.w, q.y);
    float e = 1.0e-10;
    return float4(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x, 0.0);
}

float4 hsv_to_rgb(float4 c) {
    float3 p = abs(frac(float3(c.x, c.x, c.x) + float3(1.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 -
                   float3(3.0, 3.0, 3.0));
    float3 rgb = float3(1.0, 1.0, 1.0) + (saturate(p - float3(1.0, 1.0, 1.0)) -
                                          float3(1.0, 1.0, 1.0)) * c.y;
    return float4(rgb * c.z, 0.0);
}

float4 fs_main(VertexOutput input) : SV_Target0 {
    registers[0] = float4(input.uv, 0.0, 0.0);
    registers[1] = input.color;
    registers[2] = float4(input.position.xy, 0.0, 0.0);
    registers[3] = texel;
    uint count = min(counts.x, 64u);
    [loop] for (uint i = 0u; i < count; ++i) {
        uint4 instruction = code[i];
        uint op = instruction.x & 0xFFu;
        uint aux = (instruction.x >> 16u) & 0xFFu;
        float4 a = fetch(instruction.y);
        float4 b = fetch(instruction.z);
        float4 c = fetch(instruction.w);
        float4 m = first(aux);
        float4 o = a;
        switch (op) {
        case 1u: o = a + b; break;
        case 2u: o = a - b; break;
        case 3u: o = a * b; break;
        case 4u: o = a / b; break;
        case 5u: o = a - b * floor(a / b); break;
        case 6u: o = pow(max(a, splat(0.0)), b); break;
        case 7u: o = min(a, b); break;
        case 8u: o = max(a, b); break;
        case 9u: o = float4(b.x >= a.x ? 1.0 : 0.0, b.y >= a.y ? 1.0 : 0.0, b.z >= a.z ? 1.0 : 0.0,
                            b.w >= a.w ? 1.0 : 0.0); break;
        case 10u: o = atan2(a, b); break;
        case 11u: o = abs(a); break;
        case 12u: o = float4(sign(a)); break;
        case 13u: o = floor(a); break;
        case 14u: o = ceil(a); break;
        case 15u: o = floor(a + splat(0.5)); break;
        case 16u: o = a - floor(a); break;
        case 17u: o = sqrt(a); break;
        case 18u: o = sin(a); break;
        case 19u: o = cos(a); break;
        case 20u: o = tan(a); break;
        case 21u: o = asin(a); break;
        case 22u: o = acos(a); break;
        case 23u: o = atan(a); break;
        case 24u: o = exp(a); break;
        case 25u: o = log(a); break;
        case 26u: o = saturate(a); break;
        case 27u: o = splat(1.0) - a; break;
        case 28u: o = -a; break;
        case 29u: o = a + (b - a) * c; break;
        case 30u: o = min(max(a, b), c); break;
        case 31u: {
            float4 t = saturate((c - a) / (b - a));
            o = t * t * (splat(3.0) - 2.0 * t);
            break;
        }
        case 32u: o = splat(dot(a * m, b * m)); break;
        case 33u: o = splat(sqrt(dot(a * m, a * m))); break;
        case 34u: o = splat(sqrt(dot((a - b) * m, (a - b) * m))); break;
        case 35u: {
            float size = sqrt(dot(a * m, a * m));
            o = size > 0.0 ? a * m / size : splat(0.0);
            break;
        }
        case 36u: o = float4((aux & 1u) != 0u ? b.x : a.x, (aux & 2u) != 0u ? b.y : a.y,
                             (aux & 4u) != 0u ? b.z : a.z, (aux & 8u) != 0u ? b.w : a.w); break;
        case 37u: o = sample_slot(aux, a.xy); break;
        case 38u: o = rgb_to_hsv(a); break;
        case 39u: o = hsv_to_rgb(a); break;
        default: break;
        }
        registers[(instruction.x >> 8u) & 31u] = o;
    }
    return registers[0];
}
