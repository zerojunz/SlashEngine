// Godot's color conversions and tonemappers (drivers/gles3/shaders/tonemap_inc.glsl), which
// mesh.hlsl and post.hlsl share. Keep it in step with tonemap in src/post.cpp, its CPU twin.

// Godot's approximations of the sRGB curves, for colors from 0 to 1.
float3 srgb_to_linear(float3 color) {
    return color * (color * (color * 0.305306011 + 0.682171111) + 0.012522878);
}

float3 linear_to_srgb(float3 color) {
    return max(1.055 * pow(max(color, 0.0), 0.416666667) - 0.055, 0.0);
}

static const uint kTonemapLinear = 0u;
static const uint kTonemapReinhard = 1u;
static const uint kTonemapFilmic = 2u;
static const uint kTonemapAces = 3u;

// Reinhard's extended formula, equation 4 in https://doi.org/cjbgrt.
float3 tonemap_reinhard(float3 color, float white) {
    float white_squared = white * white;
    float3 white_squared_color = white_squared * color;
    return (white_squared_color + color * color) / (white_squared_color + white_squared);
}

float3 tonemap_filmic(float3 color, float white) {
    // An exposure bias of 2, baked into the constants, for brightness like the others'.
    const float A = 0.22 * 2.0 * 2.0;
    const float B = 0.30 * 2.0;
    const float C = 0.10;
    const float D = 0.20;
    const float E = 0.01;
    const float F = 0.30;
    float3 mapped = ((color * (A * color + C * B) + D * E) / (color * (A * color + B) + D * F)) -
                    E / F;
    float mapped_white =
        ((white * (A * white + C * B) + D * E) / (white * (A * white + B) + D * F)) - E / F;
    return mapped / mapped_white;
}

// From https://github.com/TheRealMJP/BakingLab/blob/master/BakingLab/ACES.hlsl (MIT License), as
// Godot adapts it, with an exposure bias of 1.8 in the first matrix.
float3 tonemap_aces(float3 color, float white) {
    const float bias = 1.8;
    const float A = 0.0245786;
    const float B = 0.000090537;
    const float C = 0.983729;
    const float D = 0.432951;
    const float E = 0.238081;
    const float3x3 rgb_to_rrt = float3x3(
        float3(0.59719 * bias, 0.35458 * bias, 0.04823 * bias),
        float3(0.07600 * bias, 0.90834 * bias, 0.01566 * bias),
        float3(0.02840 * bias, 0.13383 * bias, 0.83777 * bias));
    const float3x3 odt_to_rgb = float3x3(float3(1.60475, -0.53108, -0.07367),
                                         float3(-0.10208, 1.10813, -0.00605),
                                         float3(-0.00327, -0.07276, 1.07602));
    color = mul(rgb_to_rrt, color);
    float3 mapped = (color * (color + A) - B) / (color * (C * color + D) + E);
    mapped = mul(odt_to_rgb, mapped);
    white *= bias;
    float mapped_white = (white * (white + A) - B) / (white * (C * white + D) + E);
    return mapped / mapped_white;
}

// A polynomial for EaryChow's AgX sigmoid, for x from 0 to 1.
float3 agx_contrast_approx(float3 x) {
    float3 x2 = x * x;
    float3 x4 = x2 * x2;
    return 0.021 * x + 4.0111 * x2 - 25.682 * x2 * x + 70.359 * x4 - 74.778 * x4 * x +
           27.069 * x4 * x2;
}

// Godot's simplification of EaryChow's AgX, as Blender uses it
// (https://github.com/EaryChow/AgX_LUT_Gen/blob/main/AgXBasesRGB.py).
float3 tonemap_agx(float3 color) {
    // Linear sRGB to linear Rec. 2020 with AgX's inset, and back with its outset; the rows are
    // GLSL's columns, so the color multiplies from the left.
    const float3x3 inset = float3x3(
        float3(0.54490813676363087053, 0.14044005884001287035, 0.088827411851915368603),
        float3(0.37377945959812267119, 0.75410959864013760045, 0.17887712465043811023),
        float3(0.081384976686407536266, 0.10543358536857773485, 0.73224999956948382528));
    const float3x3 outset = float3x3(
        float3(1.9645509602733325934, -0.29932243390911083839, -0.16436833806080403409),
        float3(-0.85585845117807513559, 1.3264510741502356555, -0.23822464068860595117),
        float3(-0.10886710826831608324, -0.027084020983874825605, 1.402665347143271889));
    const float min_ev = -12.4739311883324;
    const float max_ev = 4.02606881166759;
    color = max(color, 2e-10);
    color = mul(color, inset);
    color = clamp(log2(color), min_ev, max_ev);
    color = (color - min_ev) / (max_ev - min_ev);
    color = agx_contrast_approx(color);
    color = pow(color, 2.4);
    return mul(color, outset);
}

// Linear colors in, linear colors out.
float3 apply_tonemapping(float3 color, float white, uint tonemapper) {
    if (tonemapper == kTonemapLinear) {
        return color;
    }
    if (tonemapper == kTonemapReinhard) {
        return tonemap_reinhard(max(color, 0.0), white);
    }
    if (tonemapper == kTonemapFilmic) {
        return tonemap_filmic(max(color, 0.0), white);
    }
    if (tonemapper == kTonemapAces) {
        return tonemap_aces(max(color, 0.0), white);
    }
    return tonemap_agx(color);
}
