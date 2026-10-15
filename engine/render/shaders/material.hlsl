// The material shader: runs a material program (engine/render/include/slash/render/material.hpp)
// for each pixel, then shades it with the frame's 2D lights (light.hpp). Canvas materials compile
// from node graphs to a few instructions, so they need no shader compiler at run time. Keep it in
// step with run_program in src/material.cpp and shade in src/light.cpp.
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
    // The vertex position, which lights are placed in.
    float2 world : TEXCOORD2;
    float4 position : SV_Position;
};

VertexOutput vs_main(VertexInput input) {
    VertexOutput output;
    output.position = mul(view_projection, float4(input.position, 0.0, 1.0));
    output.uv = input.uv;
    output.color = input.color;
    output.world = input.position;
    return output;
}

// Slot 0 is the draw's texture; the material binds 1 to 3, the draw its normal and specular
// maps, and the frame its light atlas and shadow map, or white.
Texture2D<float4> texture0 : register(t0, space2);
Texture2D<float4> texture1 : register(t1, space2);
Texture2D<float4> texture2 : register(t2, space2);
Texture2D<float4> texture3 : register(t3, space2);
Texture2D<float4> normal_map : register(t4, space2);
Texture2D<float4> specular_map : register(t5, space2);
Texture2D<float4> light_atlas : register(t6, space2);
Texture2D<float4> shadow_map : register(t7, space2);
SamplerState sampler0 : register(s0, space2);
SamplerState sampler1 : register(s1, space2);
SamplerState sampler2 : register(s2, space2);
SamplerState sampler3 : register(s3, space2);
SamplerState normal_sampler : register(s4, space2);
SamplerState specular_sampler : register(s5, space2);
SamplerState atlas_sampler : register(s6, space2);
SamplerState shadow_sampler : register(s7, space2);

cbuffer Program : register(b0, space3) {
    // Each: op | destination << 8 | aux << 16, then the operands a, b and c.
    uint4 code[64];
    float4 constants[32];
    // One over texture0's size, in pixels.
    float4 texel;
    // x: how many instructions to run; none draws the texture times the vertex color.
    uint4 counts;
    // x: the light mode (0 unshaded, 1 normal, 2 light only); y: the light mask; z: the z index;
    // w: 1 with a normal map, plus 2 with a specular map.
    int4 shading;
    // The specular color (rgb) and shininess (a).
    float4 specular;
    // x: the distance field's pixel range in texels, or 0 to draw texture0 as colors; y: its
    // outline in texels.
    float4 field;
};

struct Light {
    float4 color;         // rgb times energy, and alpha
    float4 position;      // xy: position, or direction; z: height; w: 0 point, 1 directional
    float4 texture_u;     // the light texture's u at (x, y) is dot(xyz, (x, y, 1)), v likewise
    float4 texture_v;
    float4 atlas;         // where the light texture is in the atlas: x, y, width, height
    float4 shadow;        // x: shadow row (-1 none); y: far; z: filter step; w: reach
    float4 shadow_origin; // x, y: where a directional row starts across and in depth; z: span
    float4 shadow_color;
    uint4 flags;          // x: blend (0 mix, 1 add, 2 sub); y: filter taps each side; z: cull mask
    int4 range;           // x, y: the z indexes it lights
};

cbuffer Lights : register(b1, space3) {
    float4 ambient;
    uint4 light_count;
    Light lights[64];
};

static const float kTwoPi = 6.28318530717958647;
static const float kPi = 3.14159265358979323;
static const int kShadowWidth = 2048;

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

// The depth a shadow map texel holds, or 2 for none.
float shadow_depth(int column, int row) {
    float4 texel = shadow_map.Load(int3(column, row, 0));
    if (texel.a < 0.5) {
        return 2.0;
    }
    float3 bytes = round(texel.rgb * 255.0);
    return (bytes.r * 65536.0 + bytes.g * 256.0 + bytes.b) / 16777216.0;
}

// From 0 to 1: how much of the filter's samples an occluder hides the position from.
float shadow_amount(Light light, float2 position) {
    float column;
    float depth;
    bool wrap = light.position.w < 0.5;
    if (wrap) {
        float2 d = position - light.position.xy;
        column = (atan2(d.y, d.x) / kTwoPi + 0.5) * float(kShadowWidth);
        depth = sqrt(d.x * d.x + d.y * d.y) / light.shadow.y;
    } else {
        float across = position.x * -light.position.y + position.y * light.position.x;
        float along = position.x * light.position.x + position.y * light.position.y;
        column = (across - light.shadow_origin.x) / light.shadow_origin.z * float(kShadowWidth);
        depth = (along - light.shadow_origin.y) / light.shadow.y;
    }
    int taps = int(light.flags.y);
    int row = int(light.shadow.x);
    float bias = 0.5 / light.shadow.y;
    float hidden = 0.0;
    [loop] for (int k = -taps; k <= taps; ++k) {
        int at = int(floor(column + float(k) * light.shadow.z));
        if (wrap) {
            at = ((at % kShadowWidth) + kShadowWidth) % kShadowWidth;
        } else if (at < 0 || at >= kShadowWidth) {
            continue;
        }
        float stored = shadow_depth(at, row);
        if (stored <= 1.0 && depth > stored + bias && (wrap || depth - stored < light.shadow.w)) {
            hidden += 1.0;
        }
    }
    return hidden / float(2 * taps + 1);
}

// Godot's 2D normal lighting: Lambert, plus Blinn-Phong with a specular map.
float3 light_normal(float3 light_vec, float3 normal, float3 base, float3 light_color,
                    float4 shine, bool shine_used) {
    float n_dot_l = max(0.0, dot(normal, light_vec));
    float3 lit = light_color * base * n_dot_l;
    if (!shine_used) {
        return lit;
    }
    float3 view = float3(0.0, 0.0, 1.0);
    float3 half_vec = normalize(view + light_vec);
    float n_dot_v = max(dot(normal, view), 0.0);
    float n_dot_h = max(dot(normal, half_vec), 0.0);
    float shininess = exp2(15.0 * shine.a + 1.0) * 0.25;
    float blinn = pow(n_dot_h, shininess);
    blinn *= (shininess + 8.0) * (1.0 / (8.0 * kPi));
    float s = blinn / max(4.0 * n_dot_v * n_dot_l, 0.75);
    return lit + shine.rgb * light_color * s;
}

// Shades the color the program made with the frame's lights.
float4 light_pixel(float4 base, float2 position, float3 normal, bool normal_used, float4 shine,
                   bool shine_used) {
    float4 color = base;
    if (shading.x == 1) {
        color *= ambient;
    }
    float light_only_alpha = 0.0;
    uint count = min(light_count.x, 64u);
    [loop] for (uint i = 0u; i < count; ++i) {
        Light light = lights[i];
        if ((light.flags.z & uint(shading.y)) == 0u || shading.z < light.range.x ||
            shading.z > light.range.y) {
            continue;
        }
        float4 light_color = light.color;
        float3 light_vec;
        if (light.position.w < 0.5) {
            float3 at = float3(position, 1.0);
            float2 luv = float2(dot(light.texture_u.xyz, at), dot(light.texture_v.xyz, at));
            if (luv.x < 0.0 || luv.y < 0.0 || luv.x >= 1.0 || luv.y >= 1.0) {
                continue;
            }
            light_color *=
                light_atlas.SampleLevel(atlas_sampler, light.atlas.xy + luv * light.atlas.zw, 0.0);
            light_vec = normalize(float3(light.position.xy - position, light.position.z));
        } else {
            float h = light.position.z;
            light_vec = normalize(float3(-light.position.xy * (1.0 - h), h));
        }
        if (normal_used) {
            light_color.rgb =
                light_normal(light_vec, normal, base.rgb, light_color.rgb, shine, shine_used);
        } else {
            light_color.rgb *= base.rgb;
        }
        if (light.shadow.x >= 0.0) {
            float hidden = shadow_amount(light, position);
            float4 shadow_color = light.shadow_color;
            shadow_color.a *= light_color.a;
            light_color += (shadow_color - light_color) * hidden;
        }
        light_only_alpha += light_color.a;
        if (light.flags.x == 2u) {
            color.rgb -= light_color.rgb * light_color.a;
        } else if (light.flags.x == 0u) {
            color.rgb += (light_color.rgb - color.rgb) * light_color.a;
        } else {
            color.rgb += light_color.rgb * light_color.a;
        }
    }
    if (shading.x == 2) {
        color.a *= light_only_alpha;
    }
    return color;
}

float4 fs_main(VertexOutput input) : SV_Target0 {
    // The texture's u and v axes in the frame, from how both change across the screen, for
    // turning normal maps with the draw.
    float2 world_dx = ddx(input.world);
    float2 world_dy = ddy(input.world);
    float2 uv_dx = ddx(input.uv);
    float2 uv_dy = ddy(input.uv);
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
    if (counts.x == 0u) {
        float4 sampled = texture0.SampleLevel(sampler0, input.uv, 0.0);
        if (field.x > 0.0) {
            registers[0] = float4(input.color.rgb,
                                  input.color.a * field_coverage(sampled, uv_dx, uv_dy));
        } else {
            registers[0] = sampled * input.color;
        }
    }
    if (shading.x == 0) {
        return registers[0];
    }
    bool normal_used = (shading.w & 1) != 0;
    bool shine_used = (shading.w & 2) != 0;
    float3 normal = float3(0.0, 0.0, 1.0);
    if (normal_used) {
        float2 u_axis = float2(1.0, 0.0);
        float2 v_axis = float2(0.0, 1.0);
        float det = uv_dx.x * uv_dy.y - uv_dy.x * uv_dx.y;
        if (abs(det) > 1.0e-12) {
            float2 along_u = (world_dx * uv_dy.y - world_dy * uv_dx.y) / det;
            float2 along_v = (world_dy * uv_dx.x - world_dx * uv_dy.x) / det;
            if (dot(along_u, along_u) > 0.0 && dot(along_v, along_v) > 0.0) {
                u_axis = normalize(along_u);
                v_axis = normalize(along_v);
            }
        }
        float4 texel = normal_map.SampleLevel(normal_sampler, input.uv, 0.0);
        float nx = texel.r * 2.0 - 1.0;
        float ny = 1.0 - texel.g * 2.0;
        float nz = sqrt(max(0.0, 1.0 - (nx * nx + ny * ny)));
        normal = normalize(float3(u_axis * nx + v_axis * ny, nz));
    }
    float4 shine = float4(0.0, 0.0, 0.0, 0.0);
    if (shine_used) {
        shine = specular_map.SampleLevel(specular_sampler, input.uv, 0.0) * specular;
    }
    return light_pixel(registers[0], input.world, normal, normal_used, shine, shine_used);
}
