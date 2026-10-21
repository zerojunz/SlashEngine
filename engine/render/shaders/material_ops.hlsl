// The material program's operations (engine/render/include/slash/render/material.hpp), which the
// material shader runs for each pixel and particles_process.hlsl for particle graphs. Keep it in
// step with run_instructions in src/material.cpp.

float4 splat(float value) {
    return float4(value, value, value, value);
}

// 1 in the first count components, 0 in the rest.
float4 first_components(uint count) {
    return float4(count > 0u ? 1.0 : 0.0, count > 1u ? 1.0 : 0.0, count > 2u ? 1.0 : 0.0,
                  count > 3u ? 1.0 : 0.0);
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

// An angle moved into -pi to pi, where every GPU's sin, cos and tan are close to the CPU twin's:
// Metal's fast ones, which SDL compiles with, drift far from it.
float4 wrap_angle(float4 a) {
    return a - 6.28318530718 * floor(a * 0.159154943092 + splat(0.5));
}

// One instruction's result from its operands, for every op but Sample (37), which reads textures
// and is the caller's: a stays as it is for that one and for unknown ops.
float4 material_op(uint op, uint aux, float4 a, float4 b, float4 c) {
    float4 m = first_components(aux);
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
    case 18u: o = sin(wrap_angle(a)); break;
    case 19u: o = cos(wrap_angle(a)); break;
    case 20u: o = tan(wrap_angle(a)); break;
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
    case 38u: o = rgb_to_hsv(a); break;
    case 39u: o = hsv_to_rgb(a); break;
    default: break;
    }
    return o;
}
