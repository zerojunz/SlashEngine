// The material shader: runs a material program (engine/render/include/slash/render/material.hpp)
// for each pixel, then shades it with the frame's 2D lights (canvas.hlsl). Canvas materials
// compile from node graphs to a few instructions, so they need no shader compiler at run time.
// Keep it in step with run_program in src/material.cpp.

#include "canvas.hlsl"
#include "material_ops.hlsl"

// Slot 0 is the draw's texture; the material binds 1 to 3.
Texture2D<float4> texture0 : register(t0, space2);
Texture2D<float4> texture1 : register(t1, space2);
Texture2D<float4> texture2 : register(t2, space2);
Texture2D<float4> texture3 : register(t3, space2);
SamplerState sampler0 : register(s0, space2);
SamplerState sampler1 : register(s1, space2);
SamplerState sampler2 : register(s2, space2);
SamplerState sampler3 : register(s3, space2);

VertexOutput vs_main(VertexInput input) {
    VertexOutput output;
    output.position = mul(view_projection, float4(input.position, 0.0, 1.0));
    output.uv = input.uv;
    output.color = input.color;
    output.world = input.position;
    return output;
}

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

// There are no mipmaps, so level 0 is what Sample would read, and an explicit level is allowed in
// the loop's flow control.
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

// Godot's MSDF text: coverage from the median distance of the field's three channels, sharp at
// any scale. uv_dx and uv_dy are how the texture coordinate changes across one pixel. Keep it in
// step with distance_field_coverage in src/device.cpp.
float field_coverage(float4 t, float2 uv_dx, float2 uv_dy) {
    float2 dest_size = 1.0 / max(abs(uv_dx) + abs(uv_dy), float2(1.0e-8, 1.0e-8));
    float px_size = max(0.5 * dot(field.x * texel.xy, dest_size), 1.0);
    float d = max(min(t.r, t.g), min(max(t.r, t.g), t.b));
    if (field.y > 0.0) {
        float cr = clamp(field.y, 0.0, field.x / 2.0 - 1.0) / field.x;
        d = min(d, t.a);
        return clamp((d - 0.5 + cr) * px_size, 0.0, 1.0);
    }
    return clamp((d - 0.5) * px_size + 0.5, 0.0, 1.0);
}

float4 fs_main(VertexOutput input) : SV_Target0 {
    CanvasPixel pixel = canvas_pixel(input);
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
        float4 o;
        if (op == 37u) {
            o = sample_slot(aux, a.xy);
        } else {
            o = material_op(op, aux, a, b, c);
        }
        registers[(instruction.x >> 8u) & 31u] = o;
    }
    if (counts.x == 0u) {
        float4 sampled = texture0.SampleLevel(sampler0, input.uv, 0.0);
        if (field.x > 0.0) {
            float coverage = field_coverage(sampled, pixel.uv_dx, pixel.uv_dy);
            registers[0] = float4(input.color.rgb, input.color.a * coverage);
        } else {
            registers[0] = sampled * input.color;
        }
    }
    bool normal_used = (shading.w & 1) != 0;
    bool shine_used = (shading.w & 2) != 0;
    float3 normal_texel = float3(0.5, 0.5, 1.0);
    if (normal_used) {
        normal_texel = normal_map.SampleLevel(normal_sampler, input.uv, 0.0).rgb;
    }
    float4 shine = float4(0.0, 0.0, 0.0, 0.0);
    if (shine_used) {
        shine = specular_map.SampleLevel(specular_sampler, input.uv, 0.0) * specular;
    }
    return shade_canvas_pixel(pixel, registers[0], normal_texel, normal_used, shine, shine_used);
}
