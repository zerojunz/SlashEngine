// Screen-space reflections for a frame's 3D world, as Godot 4.6's Forward+ renderer makes them
// (servers/rendering/renderer_rd/shaders/effects/screen_space_reflection_downsample.glsl,
// screen_space_reflection_hiz.glsl, screen_space_reflection.glsl,
// screen_space_reflection_filter.glsl and screen_space_reflection_resolve.glsl): a pyramid of the
// prepass's nearest depths (Hi-Z), a ray from each pixel stepped across it to where it hits the
// frame before, a pyramid of the reflections blurred for rough surfaces, and the reflections at the
// frame's size for the meshes' color pass. Godot's compute shaders run as passes that draw one
// triangle over their target; they load texels and filter in code, so every device draws the same
// values. ssr_trace.hlsl defines SSR_TRACE for the trace, which draws its reflection and mip level
// into two targets. Keep it in step with src/ssr.cpp, its CPU twin.

float4 vs_main(uint id : SV_VertexID) : SV_Position {
    float2 corner = float2((float)((id << 1u) & 2u), (float)(id & 2u));
    return float4(corner * 2.0 - 1.0, 0.0, 1.0);
}

// A pass's source (the prepass's depth buffer, a level of Hi-Z, of the reflections or of the
// filter's rows, or the frame's color), the normal-roughness buffer, Hi-Z's levels with mipmaps,
// the frame before, the reflections' levels with mipmaps, the mip levels, and Hi-Z's first level
// and the normals at half size, for the resolve. All are loaded, never sampled, since WebGPU
// filters no 32-bit floats.
Texture2D<float4> source : register(t0, space2);
Texture2D<float4> normals : register(t1, space2);
Texture2D<float4> hiz : register(t2, space2);
Texture2D<float4> last_frame : register(t3, space2);
Texture2D<float4> color_mips : register(t4, space2);
Texture2D<float4> mip_levels : register(t5, space2);
Texture2D<float4> hiz_half : register(t6, space2);
Texture2D<float4> normals_half : register(t7, space2);
SamplerState source_sampler : register(s0, space2);
SamplerState normals_sampler : register(s1, space2);
SamplerState hiz_sampler : register(s2, space2);
SamplerState last_frame_sampler : register(s3, space2);
SamplerState color_mips_sampler : register(s4, space2);
SamplerState mip_levels_sampler : register(s5, space2);
SamplerState hiz_half_sampler : register(s6, space2);
SamplerState normals_half_sampler : register(s7, space2);

// The passes, as SsrPass in src/ssr.hpp.
static const uint kDownsampleDepth = 0u;
static const uint kDownsampleNormals = 1u;
static const uint kCopyDepth = 2u;
static const uint kHiz = 3u;
static const uint kTrace = 5u;
static const uint kFilterRows = 6u;
static const uint kFilterColumns = 7u;
static const uint kResolve = 8u;
static const uint kMip = 9u;
static const uint kLastFrame = 10u;

static const uint kOddWidth = 1u;
static const uint kOddHeight = 2u;
static const uint kOrthogonal = 4u;

cbuffer Ssr : register(b0, space3) {
    // The pass, the level it draws, flags (odd width, odd height, orthogonal) and the levels of
    // the pyramids.
    uint4 options;
    // The pass's screen size, then its source's.
    uint4 sizes;
    // The frame's width and height, then SSR's.
    uint4 frame;
    // The ray's most steps.
    uint4 steps;
    float4 fade; // distance_fade (fade out), curve_fade_in, depth_tolerance
    float4x4 projection;
    float4x4 inv_projection;
    // The frame's clip space to the frame before's.
    float4x4 reprojection;
};

static const float kPi = 3.14159265358979323846;
static const float3 kLuminance = float3(0.2126, 0.7152, 0.0722);
// screen_space_reflection_filter.glsl's WEIGHTS.
static const float kFilterWeights[7] = {0.07130343198685299, 0.1315141208431224,
                                        0.18987923288883812, 0.21460642856237303,
                                        0.18987923288883812, 0.1315141208431224,
                                        0.07130343198685299};
// The filter's rows reach this many rows past each edge of the level.
static const int kFilterBorder = 3;
// Mip levels are stored as a fraction of 14 in a byte, as Godot's.
static const float kMipScale = 14.0;

// x to the power y for x of 0 or more.
float power(float x, float y) {
    return x > 0.0 ? pow(x, y) : 0.0;
}

// A texel of tex, its index kept inside size.
float4 texel(Texture2D<float4> tex, int2 p, int2 size) {
    return tex.Load(int3(clamp(p, int2(0, 0), size - 1), 0));
}

// A texel of a level of tex, its index kept inside the level, which is size halved level
// times.
float4 level_texel(Texture2D<float4> tex, int2 p, uint2 size, int level) {
    int2 at = (int2)max(uint2(1u, 1u), size >> (uint)level);
    return tex.Load(int3(clamp(p, int2(0, 0), at - 1), level));
}

// A linearly filtered sample at a texture coordinate, with clamped edges, of a level of tex that
// is size halved level times.
float4 bilinear(Texture2D<float4> tex, float2 uv, uint2 size, int level) {
    float2 at_size = (float2)max(uint2(1u, 1u), size >> (uint)level);
    float2 at = uv * at_size - 0.5;
    float2 b = floor(at);
    float2 f = at - b;
    int2 i = (int2)b;
    float4 t00 = level_texel(tex, i, size, level);
    float4 t10 = level_texel(tex, i + int2(1, 0), size, level);
    float4 t01 = level_texel(tex, i + int2(0, 1), size, level);
    float4 t11 = level_texel(tex, i + int2(1, 1), size, level);
    float4 top = t00 + (t10 - t00) * f.x;
    float4 bottom = t01 + (t11 - t01) * f.x;
    return top + (bottom - top) * f.y;
}

// A sample between mipmaps of tex, levels of them from size, as a sampler with
// LINEAR_WITH_MIPMAPS takes it: each of the two nearest levels filtered linearly, then mixed.
float4 trilinear(Texture2D<float4> tex, float2 uv, float lod, uint2 size, uint levels) {
    float top = (float)(levels - 1u);
    float clamped = clamp(lod, 0.0, top);
    float base = floor(clamped);
    float fraction = clamped - base;
    int first = (int)base;
    int second = min(first + 1, (int)levels - 1);
    float4 a = bilinear(tex, uv, size, first);
    if (fraction == 0.0) {
        return a;
    }
    float4 b = bilinear(tex, uv, size, second);
    return a + (b - a) * fraction;
}

// A pixel's roughness from the normal-roughness buffer's alpha: Godot keeps dynamic objects'
// roughness flipped above half, which these never are.
float decode_roughness(float alpha) {
    float roughness = alpha;
    if (roughness > 0.5) {
        roughness = 1.0 - roughness;
    }
    return roughness / (127.0 / 255.0);
}

float3 decode_normal(float4 value) {
    return normalize(value.xyz * 2.0 - 1.0);
}

// The prepass's depth at a pixel, kept inside the buffer.
float depth_at(int2 p) {
    return texel(source, p, (int2)frame.xy).r;
}

// --- The pyramid, as screen_space_reflection_downsample.glsl and _hiz.glsl ---

// The source pixels a texel of the next level covers, in the order Godot's shaders read them:
// 2x2, then the third column and row of a source of odd width or height. count says how many.
int footprint(out int2 offsets[9]) {
    offsets[0] = int2(0, 0);
    offsets[1] = int2(1, 0);
    offsets[2] = int2(0, 1);
    offsets[3] = int2(1, 1);
    offsets[4] = int2(0, 0);
    offsets[5] = int2(0, 0);
    offsets[6] = int2(0, 0);
    offsets[7] = int2(0, 0);
    offsets[8] = int2(0, 0);
    int count = 4;
    bool odd_width = (options.z & kOddWidth) != 0u;
    bool odd_height = (options.z & kOddHeight) != 0u;
    if (odd_width) {
        offsets[count] = int2(2, 0);
        offsets[count + 1] = int2(2, 1);
        count += 2;
    }
    if (odd_height) {
        offsets[count] = int2(0, 2);
        offsets[count + 1] = int2(1, 2);
        count += 2;
    }
    if (odd_width && odd_height) {
        offsets[count] = int2(2, 2);
        count += 1;
    }
    return count;
}

// The downsample's winner: the nearest of the pixels a half-size texel covers (the first of
// equals), its depth in x and where it is in yz.
float3 downsample(int2 p) {
    int2 offsets[9];
    int count = footprint(offsets);
    float3 out_value = float3(0.0, (float)(2 * p.x), (float)(2 * p.y));
    for (int i = 0; i < count; ++i) {
        int2 s = 2 * p + offsets[i];
        float depth = depth_at(s);
        if (i == 0 || out_value.x < depth) {
            out_value = float3(depth, (float)s.x, (float)s.y);
        }
    }
    return out_value;
}

float hiz_pass(int2 p) {
    int2 offsets[9];
    int count = footprint(offsets);
    float depth = 0.0;
    for (int i = 0; i < count; ++i) {
        float value = texel(source, 2 * p + offsets[i], (int2)sizes.zw).r;
        depth = i == 0 ? value : max(depth, value);
    }
    return depth;
}

// --- The trace, as screen_space_reflection.glsl ---

float3 view_position(float3 screen) {
    float4 pos = mul(inv_projection, float4(screen.xy * 2.0 - 1.0, screen.z, 1.0));
    return pos.xyz / pos.w;
}

float3 screen_position(float3 pos) {
    float4 clip = mul(projection, float4(pos, 1.0));
    return float3((clip.xy / clip.w) * 0.5 + 0.5, clip.z / clip.w);
}

float linearize_depth(float depth) {
    float4 pos = mul(inv_projection, float4(0.0, 0.0, depth, 1.0));
    return pos.z / pos.w;
}

float hiz_at(int level, int2 p) {
    return level_texel(hiz, p, sizes.xy, level).r;
}

// compute_geometric_normal: the surface's normal from the depths beside the pixel, each side
// taken from whichever neighbor continues the surface best.
float3 geometric_normal(int2 p, float depth_c, float3 view_c) {
    float4 h = float4(hiz_at(0, p + int2(-1, 0)), hiz_at(0, p + int2(-2, 0)),
                      hiz_at(0, p + int2(1, 0)), hiz_at(0, p + int2(2, 0)));
    float4 v = float4(hiz_at(0, p + int2(0, -1)), hiz_at(0, p + int2(0, -2)),
                      hiz_at(0, p + int2(0, 1)), hiz_at(0, p + int2(0, 2)));
    float2 he = float2(abs((2.0 * h.x - h.y) - depth_c), abs((2.0 * h.z - h.w) - depth_c));
    float2 ve = float2(abs((2.0 * v.x - v.y) - depth_c), abs((2.0 * v.z - v.w) - depth_c));
    int h_sign = he.x < he.y ? -1 : 1;
    int v_sign = ve.x < ve.y ? -1 : 1;
    float2 size = (float2)sizes.xy;
    float3 view_h = view_position(float3(((float)(p.x + h_sign) + 0.5) / size.x,
                                         ((float)p.y + 0.5) / size.y, h_sign < 0 ? h.x : h.z));
    float3 view_v = view_position(float3(((float)p.x + 0.5) / size.x,
                                         ((float)(p.y + v_sign) + 0.5) / size.y,
                                         v_sign < 0 ? v.x : v.z));
    float3 h_der = (view_h - view_c) * (float)h_sign;
    float3 v_der = (view_v - view_c) * (float)v_sign;
    return cross(v_der, h_der);
}

// Where the ray crosses into the next cell of a level count cells across and high.
float cell_edge(float3 screen_pos, float3 screen_ray_dir, float2 cell_step, float2 step_up,
                float2 cell_index, float2 count) {
    float2 next = (cell_index + step_up) / count + cell_step * 0.000001;
    return min((next.x - screen_pos.x) / screen_ray_dir.x,
               (next.y - screen_pos.y) / screen_ray_dir.y);
}

// The reflection (premultiplied by how much it counts, tone mapped) and its mip level.
void trace_pass(int2 p, out float4 color, out float mip_level_out) {
    color = float4(0.0, 0.0, 0.0, 0.0);
    mip_level_out = 0.0;
    float width = (float)sizes.x;
    float height = (float)sizes.y;
    int mipmaps = (int)options.w;
    float3 screen_pos =
        float3(((float)p.x + 0.5) / width, ((float)p.y + 0.5) / height, hiz_at(0, p));
    if (screen_pos.z == 0.0) {
        return;
    }
    float3 pos = view_position(screen_pos);
    float4 normal_roughness = texel(normals, p, (int2)sizes.xy);
    float3 normal = decode_normal(normal_roughness);
    float roughness = decode_roughness(normal_roughness.w);
    // Rough materials take no reflection, which saves the trace.
    if (roughness >= 0.7) {
        return;
    }
    float3 geom_normal = normalize(geometric_normal(p, screen_pos.z, pos));
    // A small bias towards the geometry's normal keeps the ray off its own surface.
    pos += geom_normal * (1.0 - power(saturate(dot(normal, geom_normal)), 8.0));
    screen_pos = screen_position(pos);
    bool orthogonal = (options.z & kOrthogonal) != 0u;
    float3 view_dir = orthogonal ? float3(0.0, 0.0, -1.0) : normalize(pos);
    float3 ray_dir = normalize(reflect(view_dir, normal));
    // A ray going straight into its own surface bounces off it.
    if (dot(ray_dir, geom_normal) < 0.0) {
        ray_dir = normalize(reflect(ray_dir, geom_normal));
    }
    float3 end_pos = pos + ray_dir;
    // Clipped to the near plane, with a small bias so it doesn't go to infinity.
    if (end_pos.z > 0.0) {
        end_pos -= ray_dir * ((end_pos.z + 0.00001) / ray_dir.z);
    }
    float3 screen_end_pos = screen_position(end_pos);
    // Z normalized to -1 or +1, for parametric T tracing.
    float3 screen_ray_dir = screen_end_pos - screen_pos;
    screen_ray_dir *= 1.0 / abs(screen_ray_dir.z);
    bool facing_camera = screen_ray_dir.z >= 0.0;
    // Where the ray leaves the screen.
    float2 t0 = (float2(0.0, 0.0) - screen_pos.xy) / screen_ray_dir.xy;
    float2 t1 = (float2(1.0, 1.0) - screen_pos.xy) / screen_ray_dir.xy;
    float t_max = min(max(t0.x, t1.x), max(t0.y, t1.y));
    float2 cell_step = float2(screen_ray_dir.x < 0.0 ? -1.0 : 1.0,
                              screen_ray_dir.y < 0.0 ? -1.0 : 1.0);
    float2 step_up = clamp(cell_step, 0.0, 1.0);
    int cur_level = 0;
    int cur_iteration = (int)steps.x;
    // The start moved to the next cell, so the ray doesn't hit itself at once.
    float t = cell_edge(screen_pos, screen_ray_dir, cell_step, step_up,
                        floor(screen_pos.xy * float2(width, height)), float2(width, height));
    while (cur_level >= 0 && cur_iteration > 0 && t < t_max) {
        float3 cur_screen_pos = screen_pos + screen_ray_dir * t;
        float2 cell_count = (float2)max(uint2(1u, 1u), sizes.xy >> (uint)cur_level);
        float2 cell_index = floor(cur_screen_pos.xy * cell_count);
        float cell_depth = hiz_at(cur_level, (int2)cell_index);
        // Z is -1 or 1, so this needs no division.
        float depth_t = (cell_depth - screen_pos.z) * screen_ray_dir.z;
        float edge_t =
            cell_edge(screen_pos, screen_ray_dir, cell_step, step_up, cell_index, cell_count);
        bool hit = facing_camera ? (t <= depth_t) : (depth_t <= edge_t);
        int mip_offset = hit ? -1 : 1;
        if (cur_level == 0) {
            float z0 = linearize_depth(cell_depth);
            float z1 = linearize_depth(cur_screen_pos.z);
            if ((z0 - z1) > fade.z) {
                hit = false;
                // The level stays, so it doesn't go down and up again and again.
                mip_offset = 0;
            }
        }
        if (hit) {
            if (!facing_camera) {
                t = max(t, depth_t);
            }
        } else {
            t = edge_t;
        }
        cur_level = min(cur_level + mip_offset, mipmaps - 1);
        --cur_iteration;
    }
    float3 cur_screen_pos = screen_pos + screen_ray_dir * t;
    float4 reprojected = mul(reprojection, float4(cur_screen_pos.xy * 2.0 - 1.0,
                                                  cur_screen_pos.z, 1.0));
    float2 reprojected_uv = (reprojected.xy / reprojected.w) * 0.5 + 0.5;
    // How much the sample counts, rather than a hard yes or no, so invalid samples still give
    // mip levels for smoother changes of roughness.
    float validity = 1.0;
    int2 cur_p = int2((int)(cur_screen_pos.x * width), (int)(cur_screen_pos.y * height));
    float hit_depth = hiz_at(0, cur_p);
    if (t >= t_max || hit_depth == 0.0) {
        validity = 0.0;
    }
    if (abs(screen_ray_dir.x * t) < 2.0 / width && abs(screen_ray_dir.y * t) < 2.0 / height) {
        float3 hit_normal = texel(normals, cur_p, (int2)sizes.xy).xyz * 2.0 - 1.0;
        if (dot(ray_dir, hit_normal) >= 0.0) {
            validity = 0.0;
        }
    }
    float3 cur_pos = view_position(cur_screen_pos);
    float3 hit_pos = view_position(float3(cur_screen_pos.xy, hit_depth));
    float delta = length(cur_pos - hit_pos);
    float confidence = 1.0 - smoothstep(0.0, fade.z, delta);
    validity *= saturate(confidence * confidence);
    // Fades out toward an inner margin of the screen.
    float2 reprojected_pixel = reprojected_uv * float2(width, height);
    float margin = (width + height) * 0.05;
    float2 margin_grad = float2(
        reprojected_pixel.x < width * 0.5 ? reprojected_pixel.x : width - reprojected_pixel.x,
        reprojected_pixel.y < height * 0.5 ? reprojected_pixel.y : height - reprojected_pixel.y);
    float margin_blend = smoothstep(0.0, margin * margin, margin_grad.x * margin_grad.y);
    float ray_len = sqrt(screen_ray_dir.x * t * screen_ray_dir.x * t +
                         screen_ray_dir.y * t * screen_ray_dir.y * t);
    float grad = ray_len;
    float fade_in = fade.y == 0.0 ? 1.0 : power(saturate(grad), fade.y);
    float fade_out = fade.x == 0.0 ? 1.0 : power(saturate(1.0 - grad), fade.x);
    float faded = fade_in * fade_out;
    // So precision doesn't fade a reflection that shouldn't, which strong specular light would
    // leak through.
    if (faded > 0.999) {
        faded = 1.0;
    }
    validity *= faded * margin_blend;
    if (validity > 0.0) {
        float4 last = bilinear(last_frame, reprojected_uv, frame.zw, 0);
        color = float4(last.rgb * validity, validity);
        // Tone mapped, so rough reflections filter more smoothly across samples of different
        // brightness.
        color.rgb /= 1.0 + dot(color.rgb, kLuminance);
    }
    float mip_level = 0.0;
    if (roughness > 0.001) {
        float cone_angle = min(roughness, 0.999) * kPi * 0.5;
        float cone_len = ray_len;
        // The opposite side of the cone's isosceles triangle.
        float op_len = 2.0 * tan(cone_angle) * cone_len;
        // The sphere that fits inside the cone and ends at its end, which keeps blur from
        // bleeding past the reflection and stretches rough reflections a little.
        float a = op_len;
        float h = cone_len;
        float a2 = a * a;
        float fh2 = 4.0 * h * h;
        float blur_radius = (a * (sqrt(a2 + fh2) - a)) / (4.0 * h);
        float scaled = blur_radius * max(width, height) / 16.0;
        mip_level = scaled > 0.0 ? clamp(log2(scaled), 0.0, (float)(mipmaps - 1)) : 0.0;
    }
    // Invalid pixels' mip levels can make a pyramid shape at very rough levels; fading them near
    // the end of the ray hides it.
    mip_level_out = mip_level * power(saturate(1.25 - ray_len), 0.2);
}

// --- The roughness filter, as screen_space_reflection_filter.glsl ---

float filter_weight(float4 color) {
    float floor_weight = saturate((float)options.y * 0.2);
    return floor_weight + (1.0 - floor_weight) * color.a;
}

// The rows of level m: each texel of the level before averaged where the level's pixel falls,
// blurred along the row, for rows from kFilterBorder above the level to as far below.
float4 filter_rows(int2 p) {
    float2 size = (float2)sizes.xy;
    int y = p.y - kFilterBorder;
    float4 sum = float4(0.0, 0.0, 0.0, 0.0);
    float total = 0.0;
    for (int k = 0; k < 7; ++k) {
        float2 uv = float2(((float)(p.x + k - kFilterBorder) + 0.5) / size.x,
                           ((float)y + 0.5) / size.y);
        float4 c = bilinear(source, uv, sizes.zw, 0);
        float w = kFilterWeights[k] * filter_weight(c);
        sum += c * w;
        total += w;
    }
    if (!(total > 0.0)) {
        return float4(0.0, 0.0, 0.0, 0.0);
    }
    return sum / total;
}

float4 filter_columns(int2 p) {
    float4 sum = float4(0.0, 0.0, 0.0, 0.0);
    float total = 0.0;
    for (int k = 0; k < 7; ++k) {
        float4 c = texel(source, int2(p.x, p.y + k), (int2)sizes.zw);
        float w = kFilterWeights[k] * filter_weight(c);
        sum += c * w;
        total += w;
    }
    if (!(total > 0.0)) {
        return float4(0.0, 0.0, 0.0, 0.0);
    }
    return sum / total;
}

// --- Back to the frame's size ---

// Undoes the trace's tone mapping.
float4 untonemapped(float4 color) {
    color.rgb /= 1.0 - dot(color.rgb, kLuminance);
    return color;
}

float4 resolve_pass(int2 p) {
    float depth = depth_at(p);
    float4 normal_roughness = texel(normals, p, (int2)frame.xy);
    float3 normal = decode_normal(normal_roughness);
    float roughness = decode_roughness(normal_roughness.w);
    float2 half_coord = ((float2)p + 0.5) * 0.5;
    float2 bilinear_weights = half_coord - floor(half_coord);
    float2 half_size = (float2)sizes.xy * 0.5;
    // Integer division truncates toward zero, as GLSL's does.
    int2 base = (p - 1) / 2;
    float4 result = float4(0.0, 0.0, 0.0, 0.0);
    float result_weight = 0.0;
    for (int i = 0; i < 4; ++i) {
        int2 sp = base + int2(i % 2, i / 2);
        float sample_depth = texel(hiz_half, sp, (int2)frame.zw).r;
        float4 sample_nr = texel(normals_half, sp, (int2)frame.zw);
        float3 sample_normal = decode_normal(sample_nr);
        float sample_roughness = decode_roughness(sample_nr.w);
        float mip_level = texel(mip_levels, sp, (int2)frame.zw).r * kMipScale;
        float4 color = untonemapped(
            trilinear(color_mips, ((float2)sp + 0.5) / half_size, mip_level, frame.zw, options.w));
        float weight_depth = exp(-abs(depth - sample_depth) * 2048.0);
        float weight_normal = exp(-saturate(1.0 - dot(normal, sample_normal)) * 32.0);
        float weight_roughness = exp(-abs(roughness - sample_roughness) * 16.0);
        float weight = weight_depth * weight_normal * weight_roughness;
        weight *= ((i % 2) == 0 ? bilinear_weights.x : 1.0 - bilinear_weights.x) *
                  ((i / 2) == 0 ? bilinear_weights.y : 1.0 - bilinear_weights.y);
        result += color * weight;
        result_weight += weight;
    }
    if (!(result_weight > 0.0)) {
        return float4(0.0, 0.0, 0.0, 0.0);
    }
    return result / result_weight;
}

// The scene shader's read of full-size reflections: at its mip level, untonemapped.
float4 mip_pass(int2 p) {
    float mip_level = texel(mip_levels, p, (int2)frame.zw).r * kMipScale;
    return untonemapped(
        trilinear(color_mips, ((float2)p + 0.5) / (float2)sizes.xy, mip_level, frame.zw,
                  options.w));
}

// copy.glsl's FLAG_SANITIZE_INF_NAN: infinities and NaNs become 100 (alpha 1). Tested by their
// bits, which fast math can't take away.
float4 sanitized(float4 color) {
    float4 out_value = color;
    for (int c = 0; c < 4; ++c) {
        if ((asuint(color[c]) & 0x7F800000u) == 0x7F800000u) {
            out_value[c] = c == 3 ? 1.0 : 100.0;
        }
    }
    return out_value;
}

float4 last_frame_pass(int2 p) {
    int2 size = (int2)sizes.zw;
    if (sizes.z == sizes.x && sizes.w == sizes.y) {
        return sanitized(texel(source, p, size));
    }
    // copy.glsl's MODE_MIPMAP.
    int2 b = 2 * p;
    float4 a = texel(source, b, size);
    float4 c = texel(source, b + int2(0, 1), size);
    float4 d = texel(source, b + int2(1, 0), size);
    float4 e = texel(source, b + int2(1, 1), size);
    return sanitized((((a + c) + d) + e) / 4.0);
}

#ifdef SSR_TRACE
struct TraceOutput {
    float4 color : SV_Target0;
    float4 mip_level : SV_Target1;
};

TraceOutput fs_main(float4 position : SV_Position) {
    TraceOutput output;
    float mip_level = 0.0;
    trace_pass((int2)position.xy, output.color, mip_level);
    output.mip_level = float4(mip_level / kMipScale, 0.0, 0.0, 1.0);
    return output;
}
#else
float4 fs_main(float4 position : SV_Position) : SV_Target0 {
    int2 p = (int2)position.xy;
    uint pass_kind = options.x;
    if (pass_kind == kDownsampleDepth) {
        return float4(downsample(p).x, 0.0, 0.0, 1.0);
    }
    if (pass_kind == kDownsampleNormals) {
        float3 nearest = downsample(p);
        return texel(normals, int2((int)nearest.y, (int)nearest.z), (int2)frame.xy);
    }
    if (pass_kind == kCopyDepth) {
        return float4(depth_at(p), 0.0, 0.0, 1.0);
    }
    if (pass_kind == kHiz) {
        return float4(hiz_pass(p), 0.0, 0.0, 1.0);
    }
    if (pass_kind == kFilterRows) {
        return filter_rows(p);
    }
    if (pass_kind == kFilterColumns) {
        return filter_columns(p);
    }
    if (pass_kind == kResolve) {
        return resolve_pass(p);
    }
    if (pass_kind == kMip) {
        return mip_pass(p);
    }
    if (pass_kind == kLastFrame) {
        return last_frame_pass(p);
    }
    return float4(0.0, 0.0, 0.0, 0.0);
}
#endif
