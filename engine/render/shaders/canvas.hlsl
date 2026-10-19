// What every canvas shader shares: the vertex layout, the uniform buffers and the 2D lighting
// (light.hpp). The material shader (material.hlsl) includes it, and so does each of a game's own
// shaders, which the shader tools wrap around the game's fragment() and vertex() functions. Keep
// the lighting in step with shade in src/light.cpp and the software device.
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

// Slots 0 to 3 are the draw's texture and the material's; the draw binds its normal and specular
// maps in 4 and 5, and the frame its light atlas and shadow map in 6 and 7, or white.
Texture2D<float4> normal_map : register(t4, space2);
Texture2D<float4> specular_map : register(t5, space2);
Texture2D<float4> light_atlas : register(t6, space2);
Texture2D<float4> shadow_map : register(t7, space2);
SamplerState normal_sampler : register(s4, space2);
SamplerState specular_sampler : register(s5, space2);
SamplerState atlas_sampler : register(s6, space2);
SamplerState shadow_sampler : register(s7, space2);

cbuffer Program : register(b0, space3) {
    // Each: op | destination << 8 | aux << 16, then the operands a, b and c.
    uint4 code[64];
    // The material program's constants; a game's shader reads its params here, and the time in
    // the last one's x.
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
    // x: the canvas layer the draw is in, which a light's layer range must hold.
    int4 layer;
    // Multiplies normal draws: the CanvasModulate of the draw's canvas.
    float4 ambient;
    // Maps the draw's positions to the coordinates lights use: x' = dot(to_lights_x.xyz,
    // (x, y, 1)), y' likewise.
    float4 to_lights_x;
    float4 to_lights_y;
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
    int4 range;           // x, y: the z indexes it lights; z, w: the canvas layers
};

cbuffer Lights : register(b1, space3) {
    uint4 light_count;
    Light lights[64];
};

static const float kTwoPi = 6.28318530717958647;
static const float kPi = 3.14159265358979323;
static const int kShadowWidth = 2048;

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

// Shades a color with the frame's lights.
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
            shading.z > light.range.y || layer.x < light.range.z || layer.x > light.range.w) {
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

// Where a pixel is in the lights' coordinates, and how that and the texture coordinate change
// across the screen, for turning normal maps with the draw. Take it where every pixel of the draw
// does, before any discard: it takes derivatives.
struct CanvasPixel {
    float2 world;
    float2 world_dx;
    float2 world_dy;
    float2 uv_dx;
    float2 uv_dy;
};

CanvasPixel canvas_pixel(VertexOutput input) {
    CanvasPixel pixel;
    float3 at = float3(input.world, 1.0);
    pixel.world = float2(dot(to_lights_x.xyz, at), dot(to_lights_y.xyz, at));
    pixel.world_dx = ddx(pixel.world);
    pixel.world_dy = ddy(pixel.world);
    pixel.uv_dx = ddx(input.uv);
    pixel.uv_dy = ddy(input.uv);
    return pixel;
}

// A pixel's color, shaded by the frame's lights as its light mode says. normal_texel is the
// normal map's texel (rgb, green up) when normal_used, and shine the specular color and
// shininess when shine_used.
float4 shade_canvas_pixel(CanvasPixel pixel, float4 base, float3 normal_texel, bool normal_used,
                          float4 shine, bool shine_used) {
    if (shading.x == 0) {
        return base;
    }
    float3 normal = float3(0.0, 0.0, 1.0);
    if (normal_used) {
        // The texture's u and v axes in the frame.
        float2 u_axis = float2(1.0, 0.0);
        float2 v_axis = float2(0.0, 1.0);
        float det = pixel.uv_dx.x * pixel.uv_dy.y - pixel.uv_dy.x * pixel.uv_dx.y;
        if (abs(det) > 1.0e-12) {
            float2 along_u =
                (pixel.world_dx * pixel.uv_dy.y - pixel.world_dy * pixel.uv_dx.y) / det;
            float2 along_v =
                (pixel.world_dy * pixel.uv_dx.x - pixel.world_dx * pixel.uv_dy.x) / det;
            if (dot(along_u, along_u) > 0.0 && dot(along_v, along_v) > 0.0) {
                u_axis = normalize(along_u);
                v_axis = normalize(along_v);
            }
        }
        float nx = normal_texel.r * 2.0 - 1.0;
        float ny = 1.0 - normal_texel.g * 2.0;
        float nz = sqrt(max(0.0, 1.0 - (nx * nx + ny * ny)));
        normal = normalize(float3(u_axis * nx + v_axis * ny, nz));
    }
    return light_pixel(base, pixel.world, normal, normal_used, shine, shine_used);
}
