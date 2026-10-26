// Screen-space ambient occlusion for a frame's 3D world, as Godot 4.6's Forward+ renderer does it
// with Intel's ASSAO (servers/rendering/renderer_rd/shaders/effects/ss_effects_downsample.glsl,
// ssao.glsl, ssao_importance_map.glsl, ssao_blur.glsl and ssao_interleave.glsl): a prepass's depth
// is made linear and split into four slices of every other pixel at up to four levels; each slice's
// occlusion is gathered from its depths and the prepass's normals, blurred along its edges, and the
// slices are put back together at the frame's size for the meshes' color pass. Godot's compute
// shaders run as passes that draw one triangle over their target; they load texels and filter in
// code, so every device draws the same values. Keep it in step with src/ssao.cpp, its CPU twin.

float4 vs_main(uint id : SV_VertexID) : SV_Position {
    float2 corner = float2((float)((id << 1u) & 2u), (float)(id & 2u));
    return float4(corner * 2.0 - 1.0, 0.0, 1.0);
}

// A slice's depth at levels 0 to 3 (the prepass's depth buffer, for the depth pass), the
// normal-roughness buffer, an adaptive gather's base pass, the importance map, the load counter
// (its rows, for the sum pass) and four slices of occlusion. The depths and the load counter are
// floats, which are only loaded, since WebGPU filters no floats; the rest are too, so that every
// device filters them the same way.
Texture2D<float4> depth0 : register(t0, space2);
Texture2D<float4> depth1 : register(t1, space2);
Texture2D<float4> depth2 : register(t2, space2);
Texture2D<float4> depth3 : register(t3, space2);
Texture2D<float4> normals : register(t4, space2);
Texture2D<float4> base_pass : register(t5, space2);
Texture2D<float4> importance_map : register(t6, space2);
Texture2D<float4> counter : register(t7, space2);
Texture2D<float4> slice0 : register(t8, space2);
Texture2D<float4> slice1 : register(t9, space2);
Texture2D<float4> slice2 : register(t10, space2);
Texture2D<float4> slice3 : register(t11, space2);
SamplerState depth0_sampler : register(s0, space2);
SamplerState depth1_sampler : register(s1, space2);
SamplerState depth2_sampler : register(s2, space2);
SamplerState depth3_sampler : register(s3, space2);
SamplerState normals_sampler : register(s4, space2);
SamplerState base_pass_sampler : register(s5, space2);
SamplerState importance_map_sampler : register(s6, space2);
SamplerState counter_sampler : register(s7, space2);
SamplerState slice0_sampler : register(s8, space2);
SamplerState slice1_sampler : register(s9, space2);
SamplerState slice2_sampler : register(s10, space2);
SamplerState slice3_sampler : register(s11, space2);

// The passes, as SsaoPass in src/ssao.hpp.
static const uint kDepth = 0u;
static const uint kGather = 1u;
static const uint kGatherBase = 2u;
static const uint kGatherAdaptive = 3u;
static const uint kImportanceGenerate = 4u;
static const uint kImportanceA = 5u;
static const uint kImportanceB = 6u;
static const uint kImportanceRows = 7u;
static const uint kImportanceSum = 8u;
static const uint kBlurPlain = 9u;
static const uint kBlurSmart = 10u;
static const uint kBlurWide = 11u;
static const uint kInterleavePlain = 12u;
static const uint kInterleaveSmart = 13u;
static const uint kInterleaveHalf = 14u;

cbuffer Ssao : register(b0, space3) {
    // The pass, Godot's shader quality (one less than the setting, at least 0), the slice and the
    // depth level.
    uint4 options;
    uint4 frame;   // the frame's width and height, and the depth's at level 0
    uint4 buffers; // the occlusion slices' width and height, and the importance map's
    // The depth pass's step between source pixels, the gather's size multiplier, the interleave's
    // size modifier, and the levels of depth made.
    uint4 sizes;
    float4 pixel;        // half_screen_pixel_size, and it times 0.75
    float4 ndc;          // NDC_to_view_mul, then NDC_to_view_add
    float4 shadow;       // radius, intensity, shadow_power, shadow_clamp
    float4 fade;         // fade_out_mul, fade_out_add, horizon_angle_threshold,
                         // inv_radius_near_limit
    float4 adaptive;     // neg_inv_radius, load_counter_avg_div, adaptive_sample_limit,
                         // detail_intensity
    float4 pass_offset;  // pass_coord_offset, then pass_uv_offset
    float4 depth_params; // z_near and z_far (or the multiplier and addend), orthogonal, radius_sq
    float4 blur;         // edge_sharpness, then the blur's half_screen_pixel_size
    float4 importance;   // the importance map's half_screen_pixel_size, intensity and power
    float4 rotation_matrices[20];
};

// ssao.glsl's sample_pattern: each sample's offset in the disk, its weight, and log2 of its
// distance from the center.
static const float4 kSamplePattern[32] = {
    float4(0.78488064, 0.56661671, 1.500000, -0.126083),
    float4(0.26022232, -0.29575172, 1.500000, -1.064030),
    float4(0.10459357, 0.08372527, 1.110000, -2.730563),
    float4(-0.68286800, 0.04963045, 1.090000, -0.498827),
    float4(-0.13570161, -0.64190155, 1.250000, -0.532765),
    float4(-0.26193795, -0.08205118, 0.670000, -1.783245),
    float4(-0.61177456, 0.66664219, 0.710000, -0.044234),
    float4(0.43675563, 0.25119025, 0.610000, -1.167283),
    float4(0.07884444, 0.86618668, 0.640000, -0.459002),
    float4(-0.12790935, -0.29869005, 0.600000, -1.729424),
    float4(-0.04031125, 0.02413622, 0.600000, -4.792042),
    float4(0.16201244, -0.52851415, 0.790000, -1.067055),
    float4(-0.70991218, 0.47301072, 0.640000, -0.335236),
    float4(0.03277707, -0.22349690, 0.600000, -1.982384),
    float4(0.68921727, 0.36800742, 0.630000, -0.266718),
    float4(0.29251814, 0.37775412, 0.610000, -1.422520),
    float4(-0.12224089, 0.96582592, 0.600000, -0.426142),
    float4(0.11071457, -0.16131058, 0.600000, -2.165947),
    float4(0.46562141, -0.59747696, 0.600000, -0.189760),
    float4(-0.51548797, 0.11804193, 0.600000, -1.246800),
    float4(0.89141309, -0.42090443, 0.600000, 0.028192),
    float4(-0.32402530, -0.01591529, 0.600000, -1.543018),
    float4(0.60771245, 0.41635221, 0.600000, -0.605411),
    float4(0.02379565, -0.08239821, 0.600000, -3.809046),
    float4(0.48951152, -0.23657045, 0.600000, -1.189011),
    float4(-0.17611565, -0.81696892, 0.600000, -0.513724),
    float4(-0.33930185, -0.20732205, 0.600000, -1.698047),
    float4(-0.91974425, 0.05403209, 0.600000, 0.062246),
    float4(-0.15064627, -0.14949332, 0.600000, -1.896062),
    float4(0.53180975, -0.35210401, 0.600000, -0.758838),
    float4(0.41487166, 0.81442589, 0.600000, -0.505648),
    float4(-0.24106961, -0.32721516, 0.600000, -1.665244),
};

static const int kAdaptiveBaseTaps = 5;
static const int kMaxTaps = 32;
static const float kHaloingReduction = 0.6;
static const float kNormalEdgeThreshold = 0.5;
static const float kMipOffset = -4.3;

// x to the power y for x of 0 or more.
float power(float x, float y) {
    return x > 0.0 ? pow(x, y) : 0.0;
}

// a / b rounded down, for b above 0.
int floor_div(int a, int b) {
    int q = a / b;
    if (a % b != 0 && a < 0) {
        q -= 1;
    }
    return q;
}

// A texel index mirrored at the texture's edges and repeating, as a MIRRORED_REPEAT sampler
// addresses it.
int mirror_index(int i, int size) {
    int period = 2 * size;
    int t = i % period;
    if (t < 0) {
        t += period;
    }
    return t < size ? t : period - 1 - t;
}

float4 load_clamped(Texture2D<float4> image, int2 p, int2 size) {
    return image.Load(int3(clamp(p, int2(0, 0), size - 1), 0));
}

float4 load_mirrored(Texture2D<float4> image, int2 p, int2 size) {
    return image.Load(int3(mirror_index(p.x, size.x), mirror_index(p.y, size.y), 0));
}

// A linearly filtered sample with clamped edges, as Godot's default sampler takes it.
float4 bilinear(Texture2D<float4> image, float2 uv, int2 size) {
    float2 p = uv * (float2)size - 0.5;
    float2 b = floor(p);
    float2 f = p - b;
    int2 i = (int2)b;
    float4 t00 = load_clamped(image, i, size);
    float4 t10 = load_clamped(image, i + int2(1, 0), size);
    float4 t01 = load_clamped(image, i + int2(0, 1), size);
    float4 t11 = load_clamped(image, i + int2(1, 1), size);
    float4 top = t00 + (t10 - t00) * f.x;
    float4 bottom = t01 + (t11 - t01) * f.x;
    return top + (bottom - top) * f.y;
}

// --- The depth, as ss_effects_downsample.glsl ---

// screen_space_to_view_space_depth: a depth buffer value as the distance in front of the camera.
float view_depth(float d) {
    if (depth_params.z != 0.0) {
        float n = d * 2.0 - 1.0;
        return -(n * (depth_params.y - depth_params.x) - (depth_params.y + depth_params.x)) / 2.0;
    }
    return depth_params.x / (depth_params.y - d);
}

// mip_smart_average: four depths averaged, those far behind the nearest weighed less.
float smart_average(float s00, float s01, float s10, float s11, float radius_sq) {
    float closest = min(min(s00, s01), min(s10, s11));
    float falloff_sq = -1.0 / radius_sq;
    float4 depths = float4(s00, s01, s10, s11);
    float4 d = depths - closest;
    float4 weights = saturate(d * d * falloff_sq + 1.0);
    return dot(weights, depths) / dot(weights, float4(1.0, 1.0, 1.0, 1.0));
}

// A slice's linear depth at a level: the level's 2^level x 2^level texels of level 0 averaged in
// pairs of rows and columns, a level at a time, as the downsample's mipmaps are. Level 0's texels
// are depth buffer pixels a step apart, kept inside the buffer.
float depth_pass(int2 p) {
    uint slice = options.z;
    int n = 1 << options.w;
    int stride = (int)sizes.x;
    int2 offset = int2((int)(slice % 2u), (int)(slice / 2u)) * (stride / 2);
    int2 size = (int2)frame.xy;
    float values[64];
    for (int j = 0; j < n; ++j) {
        for (int i = 0; i < n; ++i) {
            int2 at = clamp((p * n + int2(i, j)) * stride + offset, int2(0, 0), size - 1);
            values[j * n + i] = view_depth(depth0.Load(int3(at, 0)).r);
        }
    }
    for (int width = n; width > 1; width /= 2) {
        int half_width = width / 2;
        for (int y = 0; y < half_width; ++y) {
            for (int x = 0; x < half_width; ++x) {
                int at = 2 * y * width + 2 * x;
                values[y * half_width + x] = smart_average(values[at], values[at + width],
                                                           values[at + 1], values[at + width + 1],
                                                           depth_params.w);
            }
        }
    }
    return values[0];
}

// --- The gather, as ssao.glsl ---

int2 level_size(int level) {
    return max((int2)(frame.zw >> (uint)level), int2(1, 1));
}

float load_depth_level(int level, int2 p) {
    int2 size = level_size(level);
    int3 at = int3(mirror_index(p.x, size.x), mirror_index(p.y, size.y), 0);
    if (level == 0) {
        return depth0.Load(at).r;
    }
    if (level == 1) {
        return depth1.Load(at).r;
    }
    if (level == 2) {
        return depth2.Load(at).r;
    }
    return depth3.Load(at).r;
}

float3 ndc_to_view(float2 p, float depth) {
    float x = ndc.x * p.x + ndc.z;
    float y = ndc.y * p.y + ndc.w;
    if (depth_params.z != 0.0) {
        return float3(x, y, depth);
    }
    return float3(x * depth, y * depth, depth);
}

float3 load_normal(int2 p) {
    int2 at = clamp(p, int2(0, 0), (int2)frame.xy - 1);
    float3 n = normals.Load(int3(at, 0)).rgb * 2.0 - 1.0;
    float len = sqrt(dot(n, n));
    return float3(n.x / len, n.y / len, -(n.z / len));
}

float pixel_obscurance(float3 normal, float3 delta, float falloff_sq) {
    float length_sq = dot(delta, delta);
    float n_dot_d = dot(normal, delta) / sqrt(length_sq);
    float falloff = max(0.0, length_sq * falloff_sq + 1.0);
    return max(0.0, n_dot_d - fade.z) * falloff;
}

// The depth level a sample reads: the nearest mipmap to lod, as a sampler with nearest mipmaps
// picks it, within the levels made.
int sample_level(float lod) {
    if (!(lod > 0.0)) {
        return 0;
    }
    float level = ceil(lod + 0.5) - 1.0;
    return (int)min(level, (float)(sizes.w - 1u));
}

// The texel of a depth level a sample offset from pixel id reads: where its texture coordinate,
// (id + offset + 0.75) * half_screen_pixel_size, falls in the level, in integers.
int sample_texel(int id, int offset, uint multiplier, int size, uint frame_size) {
    int numerator = (4 * (id + offset) + 3) * (int)multiplier * size;
    return floor_div(numerator, 2 * (int)frame_size);
}

struct Gather {
    int quality;
    int2 id;
    float2 screen; // normalized_screen_pos
    float3 center; // pix_center_pos
    float3 normal;
    float falloff_sq;
    float obscurance_sum;
    float weight_sum;
};

void tap_inner(inout Gather g, int2 offset, float mip, float weight_mod) {
    int level = sample_level(mip);
    int2 size = level_size(level);
    int sx = sample_texel(g.id.x, offset.x, sizes.y, size.x, frame.x);
    int sy = sample_texel(g.id.y, offset.y, sizes.y, size.y, frame.y);
    float z = load_depth_level(level, int2(sx, sy));
    float2 uv = (float2)offset * pixel.xy + g.screen;
    float3 hit = ndc_to_view(uv, z);
    float3 delta = hit - g.center;
    float obscurance = pixel_obscurance(g.normal, delta, g.falloff_sq);
    float weight = 1.0;
    if (g.quality >= 1) {
        float reduce = max(0.0, -delta.z);
        reduce = saturate(reduce * adaptive.x + 2.0);
        weight = kHaloingReduction * reduce + (1.0 - kHaloingReduction);
    }
    weight *= weight_mod;
    g.obscurance_sum += obscurance * weight;
    g.weight_sum += weight;
}

void tap(inout Gather g, int index, float4 rotation, float mip_offset, float weight_mod) {
    float4 pattern = kSamplePattern[index];
    // A row vector times the matrix whose columns are (x, y) and (z, w), as GLSL's vec2 * mat2;
    // offsets far off screen are kept within what an int holds.
    float fx = pattern.x * rotation.x + pattern.y * rotation.y;
    float fy = pattern.x * rotation.z + pattern.y * rotation.w;
    int2 offset = int2((int)clamp(round(fx), -32768.0, 32768.0),
                       (int)clamp(round(fy), -32768.0, 32768.0));
    weight_mod *= pattern.z;
    float mip = g.quality < 2 ? 0.0 : pattern.w + mip_offset;
    for (int side = 0; side < 2; ++side) {
        tap_inner(g, side == 0 ? offset : -offset, mip, weight_mod);
    }
}

float4 calculate_edges(float center, float left, float right, float top, float bottom) {
    float4 edges = float4(left, right, top, bottom) - center;
    float4 adjusted = edges + edges.yxwz;
    return saturate(1.3 - min(abs(edges), abs(adjusted)) / (center * 0.040));
}

float pack_edges(float4 edges) {
    float4 rounded = round(saturate(edges) * 3.05);
    return dot(rounded, float4(64.0 / 255.0, 16.0 / 255.0, 4.0 / 255.0, 1.0 / 255.0));
}

float3 detail_delta(float3 center, float3 dir, float dx, float dy, float z) {
    float along = z - center.z;
    return float3(dx + dir.x * along, dy + dir.y * along, dir.z * along);
}

// generate_SSAO_shadows_internal and main: the pixel's occlusion and packed edges, or for the
// adaptive base pass its occlusion and weight.
float2 gather_pass(int2 p, uint pass_kind) {
    bool base = pass_kind == kGatherBase;
    bool adaptive_pass = pass_kind == kGatherAdaptive;
    int quality = (int)options.y;
    uint slice = options.z;
    int2 size0 = level_size(0);
    float pix_z = load_mirrored(depth0, p, size0).r;
    float left_z = load_mirrored(depth0, p + int2(-1, 0), size0).r;
    float top_z = load_mirrored(depth0, p + int2(0, -1), size0).r;
    float right_z = load_mirrored(depth0, p + int2(1, 0), size0).r;
    float bottom_z = load_mirrored(depth0, p + int2(0, 1), size0).r;

    Gather g;
    g.quality = quality;
    g.id = p;
    g.screen = (float2)p * pixel.xy + pixel.zw;
    g.center = ndc_to_view(g.screen, pix_z);
    int2 full = p * 2 * (int)sizes.y + int2((int)(slice % 2u), (int)(slice / 2u));
    g.normal = load_normal(full);
    float3 beside = ndc_to_view(g.screen + pixel.xy, g.center.z);
    float2 pixel_size = beside.xy - g.center.xy;
    g.obscurance_sum = 0.0;
    g.weight_sum = 0.0;

    // calculate_radius_parameters
    float center_length = sqrt(dot(g.center, g.center));
    float too_close = saturate(center_length * fade.w) * 0.8 + 0.2;
    float radius = shadow.x * too_close;
    float lookup_radius = 0.85 * radius / pixel_size.x;
    g.falloff_sq = -1.0 / (radius * radius);

    if (!base && quality >= 1) {
        float border = min(min(g.screen.x, 1.0 - g.screen.x), min(g.screen.y, 1.0 - g.screen.y));
        border = saturate(10.0 * border + 0.6);
        lookup_radius *= border;
    }
    uint random_index = (uint)(p.y * 2 + p.x) % 5u;
    float4 rotation = rotation_matrices[slice * 5u + random_index] * lookup_radius;

    float4 edges = float4(1.0, 1.0, 1.0, 1.0);
    g.center *= 0.99;
    if (!base && quality >= 1) {
        edges = calculate_edges(pix_z, left_z, right_z, top_z, bottom_z);
    }
    if (!base && quality >= 1 && quality != 4) {
        float3 dir = float3(g.center.x / g.center.z, g.center.y / g.center.z, 1.0);
        float detail_falloff = 4.0 * g.falloff_sq;
        float4 extra = float4(
            pixel_obscurance(g.normal, detail_delta(g.center, dir, -pixel_size.x, 0.0, left_z),
                             detail_falloff),
            pixel_obscurance(g.normal, detail_delta(g.center, dir, pixel_size.x, 0.0, right_z),
                             detail_falloff),
            pixel_obscurance(g.normal, detail_delta(g.center, dir, 0.0, -pixel_size.y, top_z),
                             detail_falloff),
            pixel_obscurance(g.normal, detail_delta(g.center, dir, 0.0, pixel_size.y, bottom_z),
                             detail_falloff));
        g.obscurance_sum += adaptive.w * dot(extra, edges);
    }
    if (!base && quality >= 2) {
        for (int i = 0; i < 4; ++i) {
            int2 around = int2(i == 0 ? -2 : (i == 1 ? 2 : 0), i == 2 ? -2 : (i == 3 ? 2 : 0));
            edges[i] *=
                saturate(dot(g.normal, load_normal(full + around)) + kNormalEdgeThreshold);
        }
    }
    float mip_offset = quality < 2 ? 0.0 : log2(lookup_radius) + kMipOffset;

    // The samples: the quality's, or the adaptive pass's after the base pass's five, as many more
    // as the importance map asks for, each fading in over three.
    int first = 0;
    int last = quality == 0 ? 3 : (quality == 1 ? 5 : 12);
    if (base) {
        last = kAdaptiveBaseTaps;
    }
    float extra_samples = 0.0;
    if (adaptive_pass) {
        float importance_value = bilinear(importance_map, g.screen + pass_offset.zw,
                                          (int2)buffers.zw).r;
        g.obscurance_sum *= (float)kAdaptiveBaseTaps / (float)kMaxTaps +
                            importance_value * (float)(kMaxTaps - kAdaptiveBaseTaps) /
                                (float)kMaxTaps;
        float2 base_value = base_pass.Load(int3(p, 0)).rg;
        g.weight_sum += base_value.y * (float)(kAdaptiveBaseTaps * 4);
        g.obscurance_sum += base_value.x * g.weight_sum;
        float average_importance = counter.Load(int3(0, 0, 0)).r * adaptive.y;
        float limiter = saturate(adaptive.z / average_importance);
        importance_value *= limiter;
        extra_samples = (float)(kMaxTaps - kAdaptiveBaseTaps) * importance_value;
        extra_samples += 0.5;
        int count = (int)extra_samples;
        first = kAdaptiveBaseTaps;
        last = min(kMaxTaps, count + kAdaptiveBaseTaps);
    }
    for (int i = first; i < last; ++i) {
        float weight_mod = 1.0;
        if (adaptive_pass) {
            extra_samples -= 1.0;
            weight_mod = saturate(extra_samples * (1.0 / 3.0));
        }
        tap(g, i, rotation, mip_offset, weight_mod);
    }

    if (base) {
        return float2(g.obscurance_sum / g.weight_sum,
                      g.weight_sum / ((float)kAdaptiveBaseTaps * 4.0));
    }
    float obscurance = g.obscurance_sum / g.weight_sum;
    float fade_out = saturate(g.center.z * fade.x + fade.y);
    if (quality >= 1) {
        float edge_fade = saturate((1.0 - edges.x - edges.y) * 0.35) +
                          saturate((1.0 - edges.z - edges.w) * 0.35);
        fade_out *= saturate(1.0 - edge_fade);
    }
    obscurance = shadow.y * obscurance;
    obscurance = min(obscurance, shadow.w);
    obscurance *= fade_out;
    float occlusion = 1.0 - obscurance;
    occlusion = power(saturate(occlusion), shadow.z);
    if (quality == 0) {
        edges = float4(1.0, 1.0, 1.0, 1.0);
    }
    return float2(occlusion, pack_edges(edges));
}

// --- The importance map, as ssao_importance_map.glsl ---

void importance_texels(Texture2D<float4> image, int2 p, inout float low, inout float high) {
    int2 size = (int2)buffers.xy;
    for (int j = 0; j < 2; ++j) {
        for (int i = 0; i < 2; ++i) {
            float v = load_clamped(image, 2 * p + int2(i, j), size).r;
            v = importance.z * v;
            v = 1.0 - v;
            v = power(saturate(v), importance.w);
            high = max(high, v);
            low = min(low, v);
        }
    }
}

float importance_generate(int2 p) {
    float low = 1.0;
    float high = 0.0;
    importance_texels(slice0, p, low, high);
    importance_texels(slice1, p, low, high);
    importance_texels(slice2, p, low, high);
    importance_texels(slice3, p, low, high);
    return power(saturate((high - low) * 2.0), 0.8);
}

float importance_blur(int2 p, bool second) {
    float hx = importance.x;
    float hy = importance.y;
    float2 uv = ((float2)p + 0.5) * importance.xy * 2.0;
    int2 size = (int2)buffers.zw;
    // The first pass's offsets turn around the pixel one way, the second's the other.
    float2 first_offsets[4] = {float2(-hx * 3.0, -hy), float2(hx, -hy * 3.0),
                               float2(hx * 3.0, hy), float2(-hx, hy * 3.0)};
    float2 second_offsets[4] = {float2(-hx, -hy * 3.0), float2(hx * 3.0, -hy),
                                float2(hx, hy * 3.0), float2(-hx * 3.0, hy)};
    float4 values;
    for (int i = 0; i < 4; ++i) {
        values[i] = bilinear(importance_map, uv + (second ? second_offsets[i] : first_offsets[i]),
                             size).r;
    }
    return dot(values, float4(0.25, 0.25, 0.25, 0.25));
}

// The load counter's row: the importance map's bytes at every third texel of every third row,
// summed, as PROCESS_MAPB adds them up.
float importance_row(int y) {
    float sum = 0.0;
    for (int x = 0; x < (int)buffers.z; x += 3) {
        sum += floor(saturate(importance_map.Load(int3(x, y * 3, 0)).r) * 255.0 + 0.5);
    }
    return sum;
}

float importance_sum() {
    float sum = 0.0;
    int rows = ((int)buffers.w + 2) / 3;
    for (int row = 0; row < rows; ++row) {
        sum += counter.Load(int3(0, row, 0)).r;
    }
    return sum;
}

// --- The blur, as ssao_blur.glsl ---

float4 unpack_edges(float packed, float sharpness) {
    uint bits = (uint)(packed * 255.5);
    float4 edges = float4((float)((bits >> 6u) & 3u), (float)((bits >> 4u) & 3u),
                          (float)((bits >> 2u) & 3u), (float)(bits & 3u)) /
                   3.0;
    return saturate(edges + sharpness);
}

float2 blur_pass(int2 p, uint pass_kind) {
    float sharpness = blur.x;
    int2 size = (int2)buffers.xy;
    if (pass_kind == kBlurPlain) {
        float hx = blur.y * 0.5;
        float hy = blur.z * 0.5;
        float2 uv = ((float2)p + 0.5) * blur.yz;
        float2 center = bilinear(slice0, uv, size).rg;
        float2 offsets[4] = {float2(-hx * 3.0, -hy), float2(hx, -hy * 3.0), float2(-hx, hy * 3.0),
                             float2(hx * 3.0, hy)};
        float4 values;
        for (int i = 0; i < 4; ++i) {
            values[i] = bilinear(slice0, uv + offsets[i], size).r;
        }
        return float2(dot(values, float4(0.2, 0.2, 0.2, 0.2)) + center.x * 0.2, center.y);
    }
    bool wide = pass_kind == kBlurWide;
    int reach = wide ? 2 : 1;
    float packed = load_mirrored(slice0, p, size).g;
    float4 edges = unpack_edges(packed, sharpness);
    float center = load_mirrored(slice0, p, size).r;
    float left = load_mirrored(slice0, p + int2(-reach, 0), size).r;
    float top = load_mirrored(slice0, p + int2(0, -reach), size).r;
    float right = load_mirrored(slice0, p + int2(reach, 0), size).r;
    float bottom = load_mirrored(slice0, p + int2(0, reach), size).r;
    if (wide) {
        edges.x *= unpack_edges(load_mirrored(slice0, p + int2(-2, 0), size).g, sharpness).y;
        edges.z *= unpack_edges(load_mirrored(slice0, p + int2(0, -2), size).g, sharpness).w;
        edges.y *= unpack_edges(load_mirrored(slice0, p + int2(2, 0), size).g, sharpness).x;
        edges.w *= unpack_edges(load_mirrored(slice0, p + int2(0, 2), size).g, sharpness).z;
    }
    float sum_weight = wide ? 0.8 : 0.5;
    float sum = center * sum_weight;
    sum += edges.x * left;
    sum_weight += edges.x;
    sum += edges.y * right;
    sum_weight += edges.y;
    sum += edges.z * top;
    sum_weight += edges.z;
    sum += edges.w * bottom;
    sum_weight += edges.w;
    return float2(sum / sum_weight, packed);
}

// --- Putting the slices back together, as ssao_interleave.glsl ---

float4 slice_load(int index, int2 p) {
    if (index == 0) {
        return slice0.Load(int3(p, 0));
    }
    if (index == 1) {
        return slice1.Load(int3(p, 0));
    }
    if (index == 2) {
        return slice2.Load(int3(p, 0));
    }
    return slice3.Load(int3(p, 0));
}

// bilinear for a slice of occlusion picked by index.
float slice_bilinear(int index, float2 uv) {
    int2 size = (int2)buffers.xy;
    float2 at = uv * (float2)size - 0.5;
    float2 b = floor(at);
    float2 f = at - b;
    int2 i = (int2)b;
    float t00 = slice_load(index, clamp(i, int2(0, 0), size - 1)).r;
    float t10 = slice_load(index, clamp(i + int2(1, 0), int2(0, 0), size - 1)).r;
    float t01 = slice_load(index, clamp(i + int2(0, 1), int2(0, 0), size - 1)).r;
    float t11 = slice_load(index, clamp(i + int2(1, 1), int2(0, 0), size - 1)).r;
    float top = t00 + (t10 - t00) * f.x;
    float bottom = t01 + (t11 - t01) * f.x;
    return top + (bottom - top) * f.y;
}

float interleave_pass(int2 p, uint pass_kind) {
    float2 px = float2(1.0 / (float)frame.x, 1.0 / (float)frame.y);
    float2 uv = ((float2)p + 0.5) * px;
    if (pass_kind != kInterleaveSmart) {
        // Very low's first and last slices, or all four, averaged.
        bool half_pass = pass_kind == kInterleaveHalf;
        float sum = 0.0;
        for (int index = 0; index < 4; ++index) {
            if (!half_pass || index == 0 || index == 3) {
                sum += slice_bilinear(index, uv);
            }
        }
        return sum * (half_pass ? 0.5 : 0.25);
    }
    int mx = p.x % 2;
    int my = p.y % 2;
    int center_index = mx + my * 2;
    int horizontal_index = (1 - mx) + my * 2;
    int vertical_index = mx + (1 - my) * 2;
    int diagonal_index = (1 - mx) + (1 - my) * 2;
    int modifier = (int)sizes.z;
    float2 center = slice_load(center_index, p / modifier).rg;
    float4 edges = unpack_edges(center.y, blur.x);
    float fmx = (float)mx;
    float fmy = (float)my;
    float fmxe = edges.y - edges.x;
    float fmye = edges.w - edges.z;
    // The other three slices, beside the pixel across, along and diagonally, nudged along the
    // edges.
    int indices[3] = {horizontal_index, vertical_index, diagonal_index};
    float2 offsets[3] = {float2(fmx + fmxe - 0.5, 0.5 - fmy), float2(0.5 - fmx, fmy - 0.5 + fmye),
                         float2(fmx - 0.5 + fmxe, fmy - 0.5 + fmye)};
    float4 values = float4(center.x, 0.0, 0.0, 0.0);
    for (int k = 0; k < 3; ++k) {
        values[k + 1] = slice_bilinear(indices[k], ((float2)p + 0.5 + offsets[k]) * px);
    }
    float4 weights = float4(1.0, (edges.x + edges.y) * 0.5, (edges.z + edges.w) * 0.5, 0.0);
    weights.w = (weights.y + weights.z) * 0.5;
    float weight_sum = dot(weights, float4(1.0, 1.0, 1.0, 1.0));
    return dot(values, weights) / weight_sum;
}

// Direct3D 12 makes a shader for each group of passes, from SSAO_PASS_FIRST to SSAO_PASS_LAST,
// as Godot's SSAO is a shader per pass (downsample, gather, importance map, blur, interleave):
// WARP loses the device running one shader with every pass in it. The other backends take one
// shader for all.
#ifndef SSAO_PASS_FIRST
#define SSAO_PASS_FIRST 0
#define SSAO_PASS_LAST 14
#endif

float4 fs_main(float4 position : SV_Position) : SV_Target0 {
    int2 p = (int2)position.xy;
    uint pass_kind = options.x;
    float2 result = float2(0.0, 0.0);
#if SSAO_PASS_FIRST <= 0
    if (pass_kind == kDepth) {
        result.x = depth_pass(p);
    }
#endif
#if SSAO_PASS_FIRST <= 3 && SSAO_PASS_LAST >= 1
    if (pass_kind >= kGather && pass_kind <= kGatherAdaptive) {
        result = gather_pass(p, pass_kind);
    }
#endif
#if SSAO_PASS_FIRST <= 8 && SSAO_PASS_LAST >= 4
    if (pass_kind == kImportanceGenerate) {
        result.x = importance_generate(p);
    } else if (pass_kind == kImportanceA || pass_kind == kImportanceB) {
        result.x = importance_blur(p, pass_kind == kImportanceB);
    } else if (pass_kind == kImportanceRows) {
        result.x = importance_row(p.y);
    } else if (pass_kind == kImportanceSum) {
        result.x = importance_sum();
    }
#endif
#if SSAO_PASS_FIRST <= 11 && SSAO_PASS_LAST >= 9
    if (pass_kind >= kBlurPlain && pass_kind <= kBlurWide) {
        result = blur_pass(p, pass_kind);
    }
#endif
#if SSAO_PASS_LAST >= 12
    if (pass_kind >= kInterleavePlain) {
        result.x = interleave_pass(p, pass_kind);
    }
#endif
    return float4(result, 0.0, 1.0);
}
