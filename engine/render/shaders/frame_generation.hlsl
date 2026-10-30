// Frame generation's passes, ported from AMD's FidelityFX SDK 1.1.4 (FSR 3.1's
// ffx_frameinterpolation_*.h and ffx_opticalflow_*.h): one compute shader whose uniforms say which
// pass runs. src/frame_generation.cpp is its CPU twin, and src/frame_generation.hpp lays out the
// two buffers it works in and lists the passes (frame_generation_plan). Keep them in step.
//
// Copyright (C) 2024 Advanced Micro Devices, Inc.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software
// and associated documentation files (the "Software"), to deal in the Software without
// restriction, including without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all copies or
// substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
// BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

// SDL takes a compute shader's uniforms in space 2, what it reads in space 0 and writes in space
// 1. Metal has one list of buffers, uniforms first.
#ifdef SLASH_MSL
#define UNIFORMS register(b0)
#define WORDS register(u1)
#define ATOMS register(u2)
#else
#define UNIFORMS register(b0, space2)
#define WORDS register(u0, space1)
#define ATOMS register(u1, space1)
#endif

cbuffer Params : UNIFORMS {
    uint4 step;        // the pass, its level, and two numbers it takes
    int4 sizes;        // the render size, then the display size
    int4 flow_sizes;   // GetOpticalFlowSize2, then the optical flow's vectors at level 0
    float4 view_depth; // deviceToViewDepth
    float4 camera;     // 1 when orthogonal, unused, 1 over the display size
    float4 reprojection[4];
    uint4 sections[8]; // where each part of the two buffers starts (FrameGenerationSection)
};

// The frame (the pack pass) or the prepass's depth (the prepare pass), and its motion vectors.
Texture2D<float4> source_texture : register(t0, space0);
SamplerState source_sampler : register(s0, space0);
Texture2D<float4> motion_texture : register(t1, space0);
SamplerState motion_sampler : register(s1, space0);

// Everything kept and worked out, as uints (floats by their bits), and what the passes write by
// atomic max or add.
RWStructuredBuffer<uint> words : WORDS;
RWStructuredBuffer<uint> atoms : ATOMS;

// The passes (FrameGenerationPass).
static const uint kClearWords = 0u;
static const uint kClearAtoms = 1u;
static const uint kCopyWords = 2u;
static const uint kPrepare = 3u;
static const uint kPack = 4u;
static const uint kLuma = 5u;
static const uint kHistogram = 6u;
static const uint kSceneChange = 7u;
static const uint kSearch = 8u;
static const uint kFilter = 9u;
static const uint kScale = 10u;
static const uint kSetup = 11u;
static const uint kReconstruct = 12u;
static const uint kGamePrimary = 13u;
static const uint kGameSecondary = 14u;
static const uint kGamePyramid = 15u;
static const uint kFlowField = 16u;
static const uint kDisocclusion = 17u;
static const uint kInterpolate = 18u;
static const uint kInpaintPyramid = 19u;
static const uint kInpaint = 20u;

// The sections (FrameGenerationSection).
static const uint kPrevRgba = 0u;
static const uint kCurRgba = 1u;
static const uint kPrevLuma = 2u;
static const uint kCurLuma = 3u;
static const uint kSceneKept = 4u;
static const uint kState = 5u;
static const uint kSearched = 6u;
static const uint kVectors = 7u;
static const uint kPrediction = 8u;
static const uint kGamePyramidWords = 9u;
static const uint kDisocclusionWords = 10u;
static const uint kOutRgba = 11u;
static const uint kMask = 12u;
static const uint kInpaintPyramidWords = 13u;
static const uint kDilatedDepth = 14u;
static const uint kDilatedMotion = 15u;
static const uint kPreviousDepth = 16u;
static const uint kInterpolatedDepth = 17u;
static const uint kGameField = 18u;
static const uint kFlowFieldAtoms = 19u;
static const uint kHistograms = 20u;

// The state's words.
static const uint kSceneHistory = 0u;
static const uint kFrameIndex = 1u;
static const uint kFrames = 2u;
static const uint kSceneChanged = 3u;

static const float kEpsilon = 1e-03;
static const uint kPriorityHighMax = (1u << 10u) - 1u;
static const uint kPriorityLowMax = (1u << 5u) - 1u;
static const uint kPriorityLowOffset = 16u;
static const uint kPriorityHighOffset = 21u;
static const uint kPrimaryBit = 1u << 31u;
static const int kSearchRadius = 8;
static const uint kHistogramBins = 256u;
static const uint kHistogramsPerDim = 3u;
static const uint kHistogramRegions = 9u;
static const uint kOpticalFlowLevels = 7u;
static const int kGameFieldLevels = 11;
static const int kInpaintingLevels = 10;

uint section(uint i) {
    return sections[i / 4u][i % 4u];
}
int2 render_size() {
    return sizes.xy;
}
int2 display_size() {
    return sizes.zw;
}

// HLSL's saturate, max(0, x) and uint(x), with NaN as 0, as the CPU twin has them.
float sat(float x) {
    return x > 0.0 ? min(x, 1.0) : 0.0;
}
float max0(float x) {
    return x > 0.0 ? x : 0.0;
}
uint to_uint(float x) {
    if (!(x > 0.0)) {
        return 0u;
    }
    return x >= 4294967296.0 ? 0xFFFFFFFFu : (uint)x;
}
int to_int(float x) {
    if (x != x) {
        return 0;
    }
    return (int)clamp(x, -2147483648.0, 2147483520.0);
}
// ffxPow for x of 0 or more: 0 to any power is 0. Below 0 it is NaN, which the callers stand in
// for themselves, since Tint keeps no NaN constants.
float power_of(float x, float y) {
    return x > 0.0 ? exp(y * log(x)) : 0.0;
}
// HLSL's round, which AMD's code means as halves to even, as the CPU twin's nearbyint.
float round_even(float x) {
    float r = floor(x + 0.5);
    if (r - x == 0.5 && frac(r * 0.5) != 0.0) {
        r -= 1.0;
    }
    return r;
}
float round_half(float x) {
    return f16tof32(f32tof16(x));
}
uint to_byte(float value) {
    return (uint)floor(sat(value) * 255.0 + 0.5);
}
float from_byte(uint value) {
    return (float)value / 255.0;
}
float load_float(uint at) {
    return asfloat(words[at]);
}
void store_float(uint at, float value) {
    words[at] = asuint(value);
}

bool on_screen(int2 pos, int2 size) {
    return (uint)pos.x < (uint)size.x && (uint)pos.y < (uint)size.y;
}
bool uv_inside(float2 uv) {
    return (uv.x > 0.0 && uv.x < 1.0) && (uv.y > 0.0 && uv.y < 1.0);
}
uint index_of(int2 pos, int2 size) {
    return (uint)pos.y * (uint)size.x + (uint)pos.x;
}

// GetBilinearSamplingData.
struct Bilinear {
    int2 base;
    float4 weights;
};
static const int2 kOffsets[4] = {int2(0, 0), int2(1, 0), int2(0, 1), int2(1, 1)};
Bilinear bilinear(float2 uv, int2 size) {
    float px = uv.x * (float)size.x - 0.5;
    float py = uv.y * (float)size.y - 0.5;
    float fx = px - floor(px);
    float fy = py - floor(py);
    Bilinear b;
    b.base = int2(to_int(floor(px)), to_int(floor(py)));
    b.weights = float4((1.0 - fx) * (1.0 - fy), fx * (1.0 - fy), (1.0 - fx) * fy, fx * fy);
    return b;
}

// ffxLinearFromSrgb, then RawRGBToLuminance.
float linear_from_srgb(float value) {
    if (value < 0.04045) {
        return value * (1.0 / 12.92);
    }
    return power_of(value * (1.0 / 1.055) + 0.055 / 1.055, 2.4);
}
float raw_to_luminance(float3 raw) {
    float3 l = float3(linear_from_srgb(raw.x), linear_from_srgb(raw.y), linear_from_srgb(raw.z));
    return (l.x * 0.2126 + l.y * 0.7152) + l.z * 0.0722;
}
float min_divided_by_max(float v0, float v1) {
    float m = max(v0, v1);
    return m != 0.0 ? min(v0, v1) / m : 0.0;
}
float normalized_dot3(float3 v0, float3 v1) {
    float m = max(length(v0), length(v1));
    if (!(m > 0.0)) {
        return 1.0;
    }
    float3 a = v0 / m;
    float3 b = v1 / m;
    return (a.x * b.x + a.y * b.y) + a.z * b.z;
}

uint pack_rgba(float3 color) {
    return to_byte(color.r) | (to_byte(color.g) << 8u) | (to_byte(color.b) << 16u) | (255u << 24u);
}
float3 unpack_rgb(uint packed) {
    return float3(from_byte(packed & 0xFFu), from_byte((packed >> 8u) & 0xFFu),
                  from_byte((packed >> 16u) & 0xFFu));
}
float3 color_of(uint image, int2 pos) {
    return unpack_rgb(words[section(image) + index_of(pos, display_size())]);
}
// A linear sample with clamped addressing, filtered in code: two lerps across, then one down.
float3 sample_frame_linear(uint image, float2 uv) {
    int2 size = display_size();
    float px = uv.x * (float)size.x - 0.5;
    float py = uv.y * (float)size.y - 0.5;
    float fx = px - floor(px);
    float fy = py - floor(py);
    int x0 = to_int(floor(px));
    int y0 = to_int(floor(py));
    int xa = clamp(x0, 0, size.x - 1);
    int xb = clamp(x0 + 1, 0, size.x - 1);
    int ya = clamp(y0, 0, size.y - 1);
    int yb = clamp(y0 + 1, 0, size.y - 1);
    float3 top = lerp(color_of(image, int2(xa, ya)), color_of(image, int2(xb, ya)), fx);
    float3 bottom = lerp(color_of(image, int2(xa, yb)), color_of(image, int2(xb, yb)), fx);
    return lerp(top, bottom, fy);
}

// --- Optical flow ------------------------------------------------------------------------------

int2 luma_size(uint level) {
    return int2(display_size().x >> level, display_size().y >> level);
}
uint luma_offset(uint image, uint level) {
    uint offset = section(image);
    for (uint l = 0u; l < level; ++l) {
        int2 s = luma_size(l);
        offset += (uint)(s.x * s.y);
    }
    return offset;
}
// r_optical_flow_input[pos]: 0 outside; LoadOpticalFlowInput: at least 1.
uint luma_raw(uint base, int2 size, int x, int y) {
    if (x < 0 || y < 0 || x >= size.x || y >= size.y) {
        return 0u;
    }
    return words[base + (uint)y * (uint)size.x + (uint)x];
}
uint luma_load(uint base, int2 size, int x, int y) {
    return max(1u, luma_raw(base, size, x, y));
}
// GetPackedLuma: four pixels from x, a byte each, those past the edges repeating the edge's.
uint luma_packed(uint base, int2 size, int x, int y) {
    int ax = max(0, min(size.x - 4, x));
    int ay = max(0, min(size.y - 1, y));
    uint packed = luma_load(base, size, ax, ay) | (luma_load(base, size, ax + 1, ay) << 8u) |
                  (luma_load(base, size, ax + 2, ay) << 16u) |
                  (luma_load(base, size, ax + 3, ay) << 24u);
    if (x < 0) {
        uint filler = packed & 0xFFu;
        for (int shift = -1; shift >= -3; --shift) {
            if (x <= shift) {
                packed = (packed << 8u) | filler;
            }
        }
    } else if (x > size.x - 4) {
        uint filler = packed & 0xFF000000u;
        for (int shift = 3; shift >= 1; --shift) {
            if (x >= size.x - shift) {
                packed = (packed >> 8u) | filler;
            }
        }
    }
    return packed;
}
uint sad(uint a, uint b) {
    uint sum = 0u;
    for (uint shift = 0u; shift < 32u; shift += 8u) {
        int x = (int)((a >> shift) & 0xFFu);
        int y = (int)((b >> shift) & 0xFFu);
        sum += (uint)abs(x - y);
    }
    return sum;
}

// PrepareLuma (transfer function 0) at level 0, the frame's colors kept beside it.
void pack(int2 pos) {
    if (!on_screen(pos, display_size())) {
        return;
    }
    float4 c = source_texture.Load(int3(pos, 0));
    uint4 bytes = uint4(to_byte(c.r), to_byte(c.g), to_byte(c.b), to_byte(c.a));
    uint at = index_of(pos, display_size());
    words[section(kCurRgba) + at] = bytes.r | (bytes.g << 8u) | (bytes.b << 16u) | (bytes.a << 24u);
    float y = (0.2126 * from_byte(bytes.r) + 0.7152 * from_byte(bytes.g)) + 0.0722 * from_byte(bytes.b);
    words[section(kCurLuma) + at] = min(to_uint(y * 255.0), 255u);
}

// A level of the luma pyramid: the average of the level 0 pixels under each, rounded down.
void luma_level(int2 pos, uint level) {
    int2 size = luma_size(level);
    if (!on_screen(pos, size)) {
        return;
    }
    uint span = 1u << level;
    uint width = (uint)display_size().x;
    uint base = section(kCurLuma);
    uint sum = 0u;
    for (uint dy = 0u; dy < span; ++dy) {
        for (uint dx = 0u; dx < span; ++dx) {
            sum += words[base + ((uint)pos.y * span + dy) * width + (uint)pos.x * span + dx];
        }
    }
    words[luma_offset(kCurLuma, level) + index_of(pos, size)] = sum >> (2u * level);
}

// GenerateSceneChangeDetectionHistogram: a thread's column of one region, four pixels across.
void histogram(uint3 id) {
    uint width = (uint)display_size().x;
    uint height = (uint)display_size().y;
    uint div_x = width / kHistogramsPerDim;
    uint div_y = height / kHistogramsPerDim;
    uint z = id.z;
    uint start_x = div_x * (z % kHistogramsPerDim);
    uint start_y = div_y * (z / kHistogramsPerDim);
    uint x = start_x + 4u * id.x;
    if (x >= start_x + div_x) {
        return;
    }
    uint base = section(kCurLuma);
    int2 size = display_size();
    for (uint y = start_y + id.y; y < start_y + div_y; y += 128u) {
        for (uint i = 0u; i < 4u; ++i) {
            uint value = luma_load(base, size, (int)(x + i), (int)y);
            InterlockedAdd(atoms[section(kHistograms) + z * kHistogramBins + value], 1u);
        }
    }
}

// The reductions in ComputeSCDHistogramsDivergence: halves added in until one is left.
float tree_sum(float values[256]) {
    for (uint stride = 128u; stride >= 1u; stride /= 2u) {
        for (uint i = 0u; i < stride; ++i) {
            values[i] += values[i + stride];
        }
    }
    return values[0];
}

// ComputeSCDHistogramsDivergence, by one thread: each region's histogram smoothed, shifted a bin
// each way and not, and compared with the frame before's. A reset starts the history again.
void scene_change(bool reset) {
    static const float kKernel[6] = {0.0088122291, 0.027143577, 0.065114059,
                                     0.12164907,   0.17699835,  0.20056541};
    uint state = section(kState);
    uint kept = section(kSceneKept);
    if (reset) {
        words[state + kSceneHistory] = 0u;
        words[state + kFrameIndex] = 0u;
        for (uint i = 0u; i < kHistogramRegions * kHistogramBins; ++i) {
            words[kept + i] = 0u;
        }
    } else {
        words[state + kFrameIndex] += 1u;
    }
    uint sums[3] = {0u, 0u, 0u};
    float filtered[256];
    float forward[256];
    float backward[256];
    // The unshifted histograms go last, kept for the next frame once each region is compared.
    for (uint pass_ = 0u; pass_ < 3u; ++pass_) {
        uint shift = pass_ == 0u ? 0u : (pass_ == 1u ? 2u : 1u);
        for (uint region = 0u; region < kHistogramRegions; ++region) {
            uint first = region * kHistogramBins;
            for (int i = 0; i < 256; ++i) {
                int read = i - 5;
                float value = 0.0;
                for (int k = 0; k < 11; ++k) {
                    int bin = clamp(read + k, 0, 255);
                    value += kKernel[k <= 5 ? k : 10 - k] *
                             (float)atoms[section(kHistograms) + first + (uint)bin];
                }
                value += 1.0;
                if (shift == 0u) {
                    filtered[i == 0 ? 255 : i - 1] = i == 0 ? 1.0 : value;
                } else if (shift == 1u) {
                    filtered[i] = value;
                } else {
                    filtered[i == 255 ? 0 : i + 1] = i == 255 ? 1.0 : value;
                }
            }
            float total = tree_sum(filtered);
            for (uint b = 0u; b < kHistogramBins; ++b) {
                float current = filtered[b] / total;
                float previous = load_float(kept + first + b);
                forward[b] = current * log(current / previous);
                backward[b] = previous * log(previous / current);
            }
            float divergence = abs(tree_sum(forward)) + abs(tree_sum(backward));
            float result = 1.0 - exp(-divergence);
            sums[shift] += to_uint(result / (float)kHistogramRegions * 1000000.0);
            if (shift == 1u) {
                for (uint b = 0u; b < kHistogramBins; ++b) {
                    store_float(kept + first + b, filtered[b] / total);
                }
            }
        }
    }
    float value = (float)min(sums[0], min(sums[1], sums[2])) / 1000000.0;
    uint history = (words[state + kSceneHistory] << 1u) | (value > 0.45 ? 1u : 0u);
    words[state + kSceneHistory] = history;
    words[state + kSceneChanged] =
        (words[state + kFrameIndex] <= 5u || (history & 0xFu) != 0u) ? 1u : 0u;
}

int2 flow_size(uint level) {
    int2 size = int2((display_size().x + 7) / 8, (display_size().y + 7) / 8);
    for (uint i = 0u; i < level; ++i) {
        size = (size + 1) / 2;
    }
    return size;
}
int2 load_vector(uint which, int2 size, int x, int y) {
    if (x < 0 || y < 0 || x >= size.x || y >= size.y) {
        return int2(0, 0);
    }
    uint at = section(which) + ((uint)y * (uint)size.x + (uint)x) * 2u;
    return int2(asint(words[at]), asint(words[at + 1u]));
}
void store_vector(uint which, int2 size, int2 pos, int2 value) {
    uint at = section(which) + index_of(pos, size) * 2u;
    words[at] = asuint(value.x);
    words[at + 1u] = asuint(value.y);
}
bool scene_changed() {
    return words[section(kState) + kSceneChanged] != 0u;
}

// EncodeSearchCoord with the fix for the top left's bias: ties go to the smallest move.
uint encode_search(uint x, uint y) {
    return ((uint)abs((int)y - 8) << 12u) | ((uint)abs((int)x - 8) << 8u) | (y << 4u) | x;
}

// ComputeOpticalFlowAdvanced at a level, a thread a block of 8 x 8.
void search(int2 block_pos, uint level) {
    int2 size = flow_size(level);
    if (!on_screen(block_pos, size)) {
        return;
    }
    if (scene_changed()) {
        store_vector(kSearched, size, block_pos, int2(0, 0));
        return;
    }
    bool predicted = level != kOpticalFlowLevels - 1u;
    bool finest = level == 0u;
    int2 current = predicted ? load_vector(kPrediction, size, block_pos.x, block_pos.y) : int2(0, 0);
    int2 lsize = luma_size(level);
    uint first = luma_offset(kCurLuma, level);
    uint second = luma_offset(kPrevLuma, level);
    int px = block_pos.x * 8;
    int py = block_pos.y * 8;
    int ox = px + current.x - kSearchRadius;
    int oy = py + current.y - kSearchRadius;
    uint window[24 * 6];
    for (int row = 0; row < 24; ++row) {
        for (int word = 0; word < 6; ++word) {
            window[row * 6 + word] = luma_packed(second, lsize, ox + word * 4, oy + row);
        }
    }
    uint block[16];
    uint still = 0u;
    for (int row = 0; row < 8; ++row) {
        for (int half_ = 0; half_ < 2; ++half_) {
            uint packed = luma_packed(first, lsize, px + half_ * 4, py + row);
            if (finest) {
                still += sad(packed, luma_packed(second, lsize, px + half_ * 4, py + row));
            }
            block[row * 2 + half_] = packed;
        }
    }
    uint best = 0xFFFFFFFFu;
    for (uint sy = 0u; sy < 16u; ++sy) {
        for (uint sx = 0u; sx < 16u; ++sx) {
            uint sum = 0u;
            for (uint r = 0u; r < 8u; ++r) {
                for (uint c = 0u; c < 8u; ++c) {
                    uint wx = sx + c;
                    uint w = (window[(sy + r) * 6u + wx / 4u] >> (8u * (wx % 4u))) & 0xFFu;
                    uint b = (block[r * 2u + c / 4u] >> (8u * (c % 4u))) & 0xFFu;
                    sum += (uint)abs((int)w - (int)b);
                }
            }
            best = min(best, (sum << 16u) | encode_search(sx, sy));
        }
    }
    int2 moved = int2(current.x + ((int)(best & 0xFu) - kSearchRadius),
                      current.y + ((int)((best >> 4u) & 0xFu) - kSearchRadius));
    if (finest && still <= (best >> 16u)) {
        moved = int2(0, 0);
    }
    store_vector(kSearched, size, block_pos, moved);
}

// FilterOpticalFlow: each vector replaced by the one of its 3 x 3 neighbors closest to the rest.
void filter_vectors(int2 pos, uint level) {
    int2 size = flow_size(level);
    if (!on_screen(pos, size)) {
        return;
    }
    int2 near_[9];
    uint n = 0u;
    for (int xx = -1; xx < 2; ++xx) {
        for (int yy = -1; yy < 2; ++yy) {
            near_[n] = load_vector(kSearched, size, pos.x + xx, pos.y + yy);
            n += 1u;
        }
    }
    uint best = 0xFFFFFFFFu;
    for (uint i = 0u; i < 9u; ++i) {
        uint sum = 0u;
        for (uint j = 0u; j < 9u; ++j) {
            uint dx = (uint)(near_[i].x - near_[j].x);
            uint dy = (uint)(near_[i].y - near_[j].y);
            sum = dx * dx + (dy * dy + sum);
        }
        best = min((sum << 4u) | i, best);
    }
    store_vector(kVectors, size, pos, near_[best & 0xFu]);
}

// ScaleOpticalFlowAdvanced from level to the next finer: of the four coarser vectors nearest a
// block, the one that matches its pixels best, doubled.
void scale_vectors(int2 pos, uint level) {
    int2 size = flow_size(level);
    int2 next = flow_size(level - 1u);
    if (!on_screen(pos, next)) {
        return;
    }
    if (scene_changed()) {
        store_vector(kPrediction, next, pos, int2(0, 0));
        return;
    }
    int2 lsize = luma_size(level);
    uint first = luma_offset(kCurLuma, level);
    uint second = luma_offset(kPrevLuma, level);
    uint best_sad = 0xFFFFFFFFu;
    int2 best = int2(0, 0);
    for (int z = 0; z < 4; ++z) {
        int x_offset = (z % 2) - 1 + (pos.x % 2);
        int y_offset = (z / 2) - 1 + (pos.y % 2);
        int2 nearest = load_vector(kVectors, size, pos.x / 2 + x_offset, pos.y / 2 + y_offset);
        uint sum = 0u;
        for (int n = 0; n < 4; ++n) {
            int lx = pos.x * 4;
            int ly = pos.y * 4 + n;
            sum += sad(luma_packed(first, lsize, lx, ly),
                       luma_packed(second, lsize, lx + nearest.x, ly + nearest.y));
        }
        if (sum < best_sad) {
            best_sad = sum;
            best = nearest * 2;
        }
    }
    store_vector(kPrediction, next, pos, best);
}

// --- Frame interpolation -----------------------------------------------------------------------

float view_space_depth(float device_depth) {
    if (camera.x != 0.0) {
        return (device_depth - view_depth.x) / view_depth.y;
    }
    return view_depth.y / (device_depth - view_depth.x);
}
// GetViewSpacePosition.
float3 view_space_position(int2 pos, int2 size, float device_depth) {
    float z = view_space_depth(device_depth);
    float2 ndc = float2(((float)pos.x / (float)size.x) * 2.0 + -1.0,
                        ((float)pos.y / (float)size.y) * -2.0 + 1.0);
    float s = camera.x != 0.0 ? 1.0 : z;
    return float3((view_depth.z * ndc.x) * s, (view_depth.w * ndc.y) * s, z);
}

struct Entry {
    float2 motion;
    float high;
    float low;
    bool valid;
    bool primary;
    bool pos_outside;
    bool neg_outside;
};

uint2 pack_entry(bool primary, uint high, uint low, float2 motion) {
    uint priority = (primary ? kPrimaryBit : 0u) | ((high & kPriorityHighMax) << kPriorityHighOffset) |
                    ((low & kPriorityLowMax) << kPriorityLowOffset);
    return uint2(priority | f32tof16(motion.x), priority | f32tof16(motion.y));
}
Entry unpack_entry(uint2 packed) {
    Entry e;
    e.high = (float)((packed.x >> kPriorityHighOffset) & kPriorityHighMax) / (float)kPriorityHighMax;
    e.low = (float)((packed.x >> kPriorityLowOffset) & kPriorityLowMax) / (float)kPriorityLowMax;
    e.primary = (packed.x & kPrimaryBit) != 0u;
    e.valid = e.high > 0.0;
    if (e.valid && !e.primary) {
        e.high = 1.0 - e.high;
    }
    e.motion = float2(f16tof32(packed.x & 0xFFFFu), f16tof32(packed.y & 0xFFFFu));
    e.pos_outside = false;
    e.neg_outside = false;
    return e;
}
uint2 load_field(uint which, int2 pos, int2 size) {
    if (!on_screen(pos, size)) {
        return uint2(0u, 0u);
    }
    uint at = section(which) + index_of(pos, size) * 2u;
    return uint2(atoms[at], atoms[at + 1u]);
}
// UpdateGameMotionVectorFieldEx: the larger of what the two held.
uint update_field(uint which, int2 pos, int2 size, uint2 packed) {
    uint at = section(which) + index_of(pos, size) * 2u;
    uint x;
    uint y;
    InterlockedMax(atoms[at], packed.x, x);
    InterlockedMax(atoms[at + 1u], packed.y, y);
    return max(x, y);
}

float dilated_depth(int2 pos) {
    return on_screen(pos, render_size())
               ? load_float(section(kDilatedDepth) + index_of(pos, render_size()))
               : 0.0;
}
float2 dilated_motion(int2 pos) {
    if (!on_screen(pos, render_size())) {
        return float2(0.0, 0.0);
    }
    uint at = section(kDilatedMotion) + index_of(pos, render_size()) * 2u;
    return float2(load_float(at), load_float(at + 1u));
}

// The prepare pass (ReconstructAndDilate): each pixel's nearest depth among its neighbors and the
// motion vector there, and the depth pushed back to where it was in the frame before.
// A still frame (step.z) has no prepass: nothing near, moved as its camera turned.
float input_depth(int2 pos) {
    if (step.z != 0u || !on_screen(pos, render_size())) {
        return 0.0;
    }
    return source_texture.Load(int3(pos, 0)).r;
}
float2 derive_motion_vector(float2 uv) {
    float x = uv.x * 2.0 - 1.0;
    float y = 1.0 - uv.y * 2.0;
    float4 previous = ((reprojection[0] * x + reprojection[1] * y) + reprojection[2] * 0.0) +
                      reprojection[3];
    float px = previous.x / previous.w;
    float py = previous.y / previous.w;
    return float2((0.5 + px * 0.5) - uv.x, (0.5 - py * 0.5) - uv.y);
}
float2 input_motion(int2 pos) {
    if (!on_screen(pos, render_size())) {
        return float2(0.0, 0.0);
    }
    if (input_depth(pos) == 0.0) {
        float2 uv = ((float2)pos + 0.5) / (float2)render_size();
        return derive_motion_vector(uv);
    }
    return motion_texture.Load(int3(pos, 0)).rg;
}
void prepare(int2 pos) {
    static const int2 kNear[9] = {int2(0, 0),  int2(1, 0),  int2(0, 1),
                                  int2(0, -1), int2(-1, 0), int2(-1, 1),
                                  int2(1, 1),  int2(-1, -1), int2(1, -1)};
    int2 size = render_size();
    int2 nearest_at = pos;
    float nearest = input_depth(pos);
    for (uint i = 1u; i < 9u; ++i) {
        int2 at = pos + kNear[i];
        if (on_screen(at, size) && input_depth(at) > nearest) {
            nearest_at = at;
            nearest = input_depth(at);
        }
    }
    float2 motion = input_motion(nearest_at);
    if (on_screen(pos, size)) {
        uint at = index_of(pos, size);
        store_float(section(kDilatedDepth) + at, nearest);
        store_float(section(kDilatedMotion) + at * 2u, round_half(motion.x));
        store_float(section(kDilatedMotion) + at * 2u + 1u, round_half(motion.y));
    }
    motion = motion * (length(motion * (float2)display_size()) > 0.1 ? 1.0 : 0.0);
    float2 uv = ((float2)pos + 0.5) / (float2)size;
    Bilinear b = bilinear(uv + motion, size);
    for (uint i = 0u; i < 4u; ++i) {
        int2 at = b.base + kOffsets[i];
        if (b.weights[i] > kEpsilon && on_screen(at, size)) {
            InterlockedMax(atoms[section(kPreviousDepth) + index_of(at, size)], asuint(nearest));
        }
    }
}

// ReconstructPrevDepth of reconstructPreviousDepth: each pixel's depth pushed halfway back along
// its motion, the nearest kept.
void reconstruct(int2 pos) {
    int2 size = render_size();
    float2 motion = dilated_motion(pos) * 0.5;
    float depth = dilated_depth(pos);
    float2 uv = ((float2)pos + 0.5) / (float2)size;
    Bilinear b = bilinear(uv + motion, size);
    for (uint i = 0u; i < 4u; ++i) {
        int2 at = b.base + kOffsets[i];
        if (b.weights[i] > kEpsilon && on_screen(at, size)) {
            InterlockedMax(atoms[section(kInterpolatedDepth) + index_of(at, size)], asuint(depth));
        }
    }
}

// getPriorityFactorFromViewSpaceDepth.
uint depth_priority(float view_depth_) {
    if (!(view_depth_ >= 0.0)) {
        return 1u; // NaN's priority
    }
    float scaled = power_of(view_depth_, 0.33);
    uint priority = to_uint((1.0 - scaled * (1.0 / (1.0 + scaled))) * (float)kPriorityHighMax);
    return max(1u, priority);
}

// computeGameFieldMvs: the primary vectors in one pass, then the secondary ones.
void game_field(int2 pos, bool secondary) {
    int2 size = render_size();
    float2 fsize = (float2)size;
    float2 uv = ((float2)pos + 0.5) / fsize;
    float depth = dilated_depth(pos);
    float2 motion = dilated_motion(pos);
    float2 half_motion = motion * 0.5;
    float2 location = uv + half_motion;
    uint high = depth_priority(view_space_depth(depth));
    if (!secondary) {
        float3 previous = sample_frame_linear(kPrevRgba, uv + motion);
        float3 current = sample_frame_linear(kCurRgba, uv);
        float previous_luma = 0.001 + raw_to_luminance(previous);
        float current_luma = 0.001 + raw_to_luminance(current);
        uint low = to_uint(round_even(min_divided_by_max(previous_luma, current_luma) *
                                 (float)kPriorityLowMax)) *
                   (uv_inside(uv + motion) ? 1u : 0u);
        uint2 packed = pack_entry(true, high, low, half_motion);
        Bilinear b = bilinear(location, size);
        for (uint i = 0u; i < 4u; ++i) {
            int2 at = b.base + kOffsets[i];
            if (on_screen(at, size)) {
                update_field(kGameField, at, size, packed);
            }
        }
        return;
    }
    if (!(length(half_motion * fsize) > kEpsilon)) {
        return;
    }
    float step_ = length(float2(1.0 / fsize.x, 1.0 / fsize.y));
    float2 direction = motion / length(motion);
    float stop = min(length(half_motion), length(float2(0.5, 0.5)));
    uint secondary_high = max(1u, kPriorityHighMax - high);
    bool write = true;
    uint primary_hits = 0u;
    for (float s = step_; s <= stop && write; s += step_) {
        float2 at_uv = location - direction * s;
        Bilinear b = bilinear(at_uv, size);
        float2 to_center = normalize(float2(0.5, 0.5) - at_uv);
        uint low = to_uint(max0(dot(to_center, direction)) * (float)kPriorityLowMax);
        uint2 packed = pack_entry(false, secondary_high, low, half_motion);
        int2 at = b.base;
        write = write && on_screen(at, size);
        if (write) {
            uint before = update_field(kGameField, at, size, packed);
            primary_hits += (before & kPrimaryBit) != 0u ? 1u : 0u;
            write = write && primary_hits <= 3u;
        }
    }
}

int2 game_level_size(uint level) {
    return int2(render_size().x >> (level + 1u), render_size().y >> (level + 1u));
}
int2 inpaint_level_size(uint level) {
    return int2(display_size().x >> (level + 1u), display_size().y >> (level + 1u));
}
uint pyramid_offset(uint which, uint level, bool game) {
    uint offset = section(which);
    for (uint l = 0u; l < level; ++l) {
        int2 s = game ? game_level_size(l) : inpaint_level_size(l);
        offset += (uint)(s.x * s.y) * 4u;
    }
    return offset;
}
float4 load_pyramid(uint which, uint level, int2 pos, bool game) {
    int2 size = game ? game_level_size(level) : inpaint_level_size(level);
    uint at = pyramid_offset(which, level, game) + index_of(pos, size) * 4u;
    return float4(load_float(at), load_float(at + 1u), load_float(at + 2u), load_float(at + 3u));
}

// A level of the game's vector field's inpainting pyramid, its first from the field.
void game_pyramid(int2 pos, uint level) {
    int2 size = game_level_size(level);
    if (!on_screen(pos, size)) {
        return;
    }
    float4 sum = float4(0.0, 0.0, 0.0, 0.0);
    float weights = 0.0;
    for (uint i = 0u; i < 4u; ++i) {
        int2 at = pos * 2 + kOffsets[i];
        float4 s;
        if (level == 0u) {
            Entry e = unpack_entry(load_field(kGameField, at, render_size()));
            s = float4(e.motion, e.high, e.low);
        } else {
            s = load_pyramid(kGamePyramidWords, level - 1u, at, true);
        }
        float weight = s.z > 0.0 ? 1.0 : 0.0;
        sum += s * weight;
        weights += weight;
    }
    sum /= weights > kEpsilon ? weights : 1.0;
    uint at = pyramid_offset(kGamePyramidWords, level, true) + index_of(pos, size) * 4u;
    for (uint c = 0u; c < 4u; ++c) {
        store_float(at + c, round_half(sum[c]));
    }
}

// ComputeMvInpaintingLevel.
float4 game_pyramid_level(float2 uv, uint level, int2 size) {
    Bilinear b = bilinear(uv, size);
    float sum = 0.0;
    float4 color = float4(0.0, 0.0, 0.0, 0.0);
    for (uint i = 0u; i < 4u; ++i) {
        int2 at = b.base + kOffsets[i];
        if (on_screen(at, size)) {
            float4 s = load_pyramid(kGamePyramidWords, level, at, true);
            float weight = (b.weights[i] * (s.z > 0.0 ? 1.0 : 0.0)) * s.z;
            sum += weight;
            color += s * weight;
        }
    }
    return color / (sum > 0.0 ? sum : 1.0);
}

// LoadInpaintedGameFieldMv.
Entry game_vector(float2 uv) {
    int2 pos = int2(to_int(uv.x * (float)render_size().x), to_int(uv.y * (float)render_size().y));
    Entry e = unpack_entry(load_field(kGameField, pos, render_size()));
    if (!e.valid) {
        int2 size = render_size();
        float4 filled = float4(0.0, 0.0, 0.0, 0.0);
        for (int level = 0; level < kGameFieldLevels && filled.w == 0.0; ++level) {
            size = size / 2;
            int2 s = game_level_size((uint)level);
            if (s.x >= 1 && s.y >= 1) {
                filled = game_pyramid_level(uv, (uint)level, size);
            }
        }
        e.motion = filled.xy;
        e.high = filled.z;
        e.low = filled.w;
    }
    e.neg_outside = !uv_inside(uv - e.motion);
    e.pos_outside = !uv_inside(uv + e.motion);
    return e;
}

float2 flow_vector(int2 pos) {
    if (!on_screen(pos, flow_sizes.zw)) {
        return float2(0.0, 0.0);
    }
    int2 v = load_vector(kVectors, flow_sizes.zw, pos.x, pos.y);
    return (float2)v * camera.zw;
}

// computeOpticalFlowVectorField and computeOpticalFlowFieldMvs.
void flow_field(int2 pos) {
    int2 flow = flow_sizes.xy;
    float2 rect = (float2)display_size();
    float2 average = float2(0.0, 0.0);
    float weights = 0.0;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            float2 v = flow_vector(pos + int2(dx, dy));
            float l = length(v * rect);
            float weight = max0(512.0 - l) * (l > 1.0 ? 1.0 : 0.0);
            average += v * weight;
            weights += weight;
        }
    }
    average = average / weights;
    float2 vector_ = float2(0.0, 0.0);
    weights = 0.0;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            float2 v = flow_vector(pos + int2(dx, dy));
            float l = length(v * rect);
            float length_factor = max0(512.0 - l) * (l > 1.0 ? 1.0 : 0.0);
            float weight = max0(power_of(dot(average, v), 1.25)) * length_factor;
            vector_ += v * weight;
            weights += weight;
        }
    }
    if (weights > kEpsilon) {
        vector_ = vector_ / weights;
    }
    float2 uv = ((float2)pos + 0.5) / (float2)flow;
    float2 half_motion = vector_ * 0.5;
    float3 previous = sample_frame_linear(kPrevRgba, uv + vector_);
    float3 current = sample_frame_linear(kCurRgba, uv);
    float previous_luma = 0.001 + raw_to_luminance(previous);
    float current_luma = 0.001 + raw_to_luminance(current);
    float velocity = length(vector_ * rect);
    uint high = (velocity > 1.0 ? 1u : 0u) *
                to_uint(sat(velocity / length(rect * 0.05)) * (float)kPriorityHighMax);
    if (high > 0u) {
        uint low = to_uint(round_even(min_divided_by_max(previous_luma, current_luma) *
                                 (float)kPriorityLowMax)) *
                   (uv_inside(uv + vector_) ? 1u : 0u);
        uint2 packed = pack_entry(true, high, low, half_motion);
        Bilinear b = bilinear(uv + half_motion, flow);
        for (uint i = 0u; i < 4u; ++i) {
            int2 at = b.base + kOffsets[i];
            if (on_screen(at, flow)) {
                update_field(kFlowFieldAtoms, at, flow, packed);
            }
        }
    }
}

// SampleOpticalFlowMotionVectorField.
Entry flow_entry(float2 uv) {
    int2 flow = flow_sizes.xy;
    Bilinear b = bilinear(uv, flow);
    Entry e;
    e.motion = float2(0.0, 0.0);
    e.high = 0.0;
    e.low = 0.0;
    e.valid = false;
    e.primary = false;
    float weights = 0.0;
    for (uint i = 0u; i < 4u; ++i) {
        int2 at = b.base + kOffsets[i];
        if (on_screen(at, flow)) {
            float weight = b.weights[i];
            Entry s = unpack_entry(load_field(kFlowFieldAtoms, at, flow));
            e.motion += s.motion * weight;
            e.high += s.high * weight;
            e.low += s.low * weight;
            weights += weight;
        }
    }
    if (weights > 0.0) {
        e.motion /= weights;
        e.high /= weights;
        e.low /= weights;
    }
    e.neg_outside = !uv_inside(uv - e.motion);
    e.pos_outside = !uv_inside(uv + e.motion);
    return e;
}

// ComputeDepthClip.
float depth_clip(int which, float2 uv, float depth) {
    int2 size = render_size();
    float current_view = view_space_depth(depth);
    Bilinear b = bilinear(uv, size);
    float covered = 0.0;
    float weights = 0.0;
    for (uint i = 0u; i < 4u; ++i) {
        int2 at = b.base + kOffsets[i];
        if (!on_screen(at, size)) {
            continue;
        }
        float weight = b.weights[i];
        if (!(weight > kEpsilon)) {
            continue;
        }
        float previous_depth = which == 0
                                   ? asfloat(atoms[section(kPreviousDepth) + index_of(at, size)])
                                   : load_float(section(kDilatedDepth) + index_of(at, size));
        float previous_view = view_space_depth(previous_depth);
        float difference = current_view - previous_view;
        if (difference > 0.0) {
            float plane = min(previous_depth, depth);
            float3 center = view_space_position(
                int2(to_int((float)size.x * 0.5), to_int((float)size.y * 0.5)), size, plane);
            float3 corner = view_space_position(int2(0, 0), size, plane);
            float half_width = length((float2)size);
            float threshold = min(current_view, previous_view);
            float fov = length(corner) / length(center);
            float required = ((1.37e-05 * fov) * half_width) * threshold;
            covered += ((required / difference) >= 1.0 ? 1.0 : 0.0) * weight;
            weights += weight;
        }
    }
    return weights > 0.0 ? sat(1.0 - covered / weights) : 0.0;
}

// computeDisocclusionMask.
void disocclusion(int2 pos) {
    int2 size = render_size();
    if (!on_screen(pos, size)) {
        return;
    }
    float2 fsize = (float2)size;
    float depth = asfloat(atoms[section(kInterpolatedDepth) + index_of(pos, size)]);
    float2 uv = ((float2)pos + 0.5) / fsize;
    Entry game = game_vector(uv);
    float to_previous = 1.0 - depth_clip(0, uv + game.motion, depth);
    float to_current = 1.0 - depth_clip(1, uv - game.motion, depth);
    float2 mask = float2(to_previous >= kEpsilon ? 1.0 : 0.0, to_current >= kEpsilon ? 1.0 : 0.0);
    float2 source = game.motion * 2.0;
    float2 before = (uv + source) * fsize;
    float2 after = (uv - source) * fsize;
    mask.x = sat(mask.x + (on_screen(int2(to_int(before.x), to_int(before.y)), size) ? 0.0 : 1.0));
    mask.y = sat(mask.y + (on_screen(int2(to_int(after.x), to_int(after.y)), size) ? 0.0 : 1.0));
    uint at = section(kDisocclusionWords) + index_of(pos, size) * 2u;
    store_float(at, mask.x);
    store_float(at + 1u, mask.y);
}

// SampleTextureBilinear.
float4 sample_frame(uint image, float2 uv) {
    int2 display = display_size();
    Bilinear b = bilinear(uv, display);
    float3 color = float3(0.0, 0.0, 0.0);
    float weights = 0.0;
    for (uint i = 0u; i < 4u; ++i) {
        int2 at = b.base + kOffsets[i];
        if (at.x >= 0 && at.x < display.x && at.y >= 0 && at.y < display.y) {
            float weight = b.weights[i];
            color += color_of(image, at) * weight;
            weights += weight;
        }
    }
    color = weights != 0.0 ? color / weights : float3(0.0, 0.0, 0.0);
    return float4(color, weights);
}

float2 disocclusion_at(float2 uv) {
    int2 size = render_size();
    float px = uv.x * (float)size.x - 0.5;
    float py = uv.y * (float)size.y - 0.5;
    float fx = px - floor(px);
    float fy = py - floor(py);
    int x0 = to_int(floor(px));
    int y0 = to_int(floor(py));
    int xa = clamp(x0, 0, size.x - 1);
    int xb = clamp(x0 + 1, 0, size.x - 1);
    int ya = clamp(y0, 0, size.y - 1);
    int yb = clamp(y0 + 1, 0, size.y - 1);
    uint base = section(kDisocclusionWords);
    uint ia = (uint)ya * (uint)size.x;
    uint ib = (uint)yb * (uint)size.x;
    float2 a = float2(load_float(base + (ia + (uint)xa) * 2u), load_float(base + (ia + (uint)xa) * 2u + 1u));
    float2 b = float2(load_float(base + (ia + (uint)xb) * 2u), load_float(base + (ia + (uint)xb) * 2u + 1u));
    float2 c = float2(load_float(base + (ib + (uint)xa) * 2u), load_float(base + (ib + (uint)xa) * 2u + 1u));
    float2 d = float2(load_float(base + (ib + (uint)xb) * 2u), load_float(base + (ib + (uint)xb) * 2u + 1u));
    float2 top = lerp(a, b, fx);
    float2 bottom = lerp(c, d, fx);
    return lerp(top, bottom, fy);
}

// computeInterpolatedColor.
void interpolated_color(int2 pos, out float3 color, out float in_painting) {
    int2 display = display_size();
    float2 uv = ((float2)pos + 0.5) / (float2)display;
    in_painting = 0.0;
    Entry game = game_vector(uv);
    Entry flow = flow_entry(uv);
    float2 mask = disocclusion_at(uv);
    float2 disoccluded_mask = float2(sat(mask.x) == 1.0 ? 1.0 : 0.0, sat(mask.y) == 1.0 ? 1.0 : 0.0);
    float4 previous_game = sample_frame(kPrevRgba, uv + game.motion);
    float4 current_game = sample_frame(kCurRgba, uv - game.motion);
    float4 previous_flow = sample_frame(kPrevRgba, uv + flow.motion);
    float4 current_flow = sample_frame(kCurRgba, uv - flow.motion);

    disoccluded_mask.x *= game.pos_outside ? 0.0 : 1.0;
    disoccluded_mask.y *= game.neg_outside ? 0.0 : 1.0;
    in_painting = sat(max(in_painting, length(disoccluded_mask) <= kEpsilon ? 1.0 : 0.0));
    float t = 0.5;
    t += 0.5 * (1.0 - disoccluded_mask.x);
    t -= 0.5 * (1.0 - disoccluded_mask.y);
    color = lerp(previous_game.rgb, current_game.rgb, sat(t));
    float disoccluded = sat(1.0 - min(disoccluded_mask.x, disoccluded_mask.y));
    if (previous_game.w == 0.0) {
        color = current_game.rgb;
    } else if (current_game.w == 0.0) {
        color = previous_game.rgb;
    }
    if (previous_game.w == 0.0 && current_game.w == 0.0) {
        in_painting = 1.0;
    }

    float flow_t = 0.5;
    if (previous_flow.w > 0.0 && current_flow.w > 0.0) {
        flow_t = 0.5;
    } else if (previous_flow.w > 0.0) {
        flow_t = 0.0;
    } else {
        flow_t = 1.0;
    }
    float3 flow_color = lerp(previous_flow.rgb, current_flow.rgb, flow_t);
    float flow_similarity = normalized_dot3(previous_flow.rgb, current_flow.rgb);
    float game_similarity = normalized_dot3(previous_game.rgb, current_game.rgb);
    game_similarity = lerp(max(kEpsilon, game_similarity), 1.0, sat(disoccluded));
    float bias = sat(game_similarity / max(kEpsilon, flow_similarity));
    bias = lerp(bias, 1.0, words[section(kState) + kFrames] < 10u ? 1.0 : 0.0);
    color = lerp(flow_color, color, sat(bias));
}

void interpolate(int2 pos) {
    int2 display = display_size();
    if (!on_screen(pos, display)) {
        return;
    }
    float3 color;
    float in_painting = 0.0;
    if (words[section(kState) + kFrames] == 0u) {
        color = color_of(kCurRgba, pos);
    } else {
        interpolated_color(pos, color, in_painting);
    }
    uint at = index_of(pos, display);
    words[section(kOutRgba) + at] = pack_rgba(color);
    words[section(kMask) + at] = to_byte(in_painting);
}

// A level of the inpainting pyramid, its first from the interpolated frame.
void inpaint_pyramid(int2 pos, uint level) {
    int2 size = inpaint_level_size(level);
    if (!on_screen(pos, size)) {
        return;
    }
    float4 v[4];
    for (uint i = 0u; i < 4u; ++i) {
        int2 at = pos * 2 + kOffsets[i];
        if (level == 0u) {
            uint j = index_of(at, display_size());
            v[i] = float4(unpack_rgb(words[section(kOutRgba) + j]),
                          sat(1.0 - from_byte(words[section(kMask) + j])));
        } else {
            v[i] = load_pyramid(kInpaintPyramidWords, level - 1u, at, false);
        }
    }
    float sum = ((v[0].w + v[1].w) + v[2].w) + v[3].w;
    float4 mixed = float4(0.0, 0.0, 0.0, 0.0);
    if (sum != 0.0) {
        for (uint c = 0u; c < 4u; ++c) {
            mixed[c] = ((((v[0][c] * v[0].w) + (v[1][c] * v[1].w)) + (v[2][c] * v[2].w)) +
                        (v[3][c] * v[3].w)) /
                       sum;
        }
    }
    uint at = pyramid_offset(kInpaintPyramidWords, level, false) + index_of(pos, size) * 4u;
    for (uint c = 0u; c < 4u; ++c) {
        store_float(at + c, round_half(mixed[c]));
    }
}

// ComputeInpainting.
void inpaint(int2 pos) {
    int2 display = display_size();
    if (!on_screen(pos, display)) {
        return;
    }
    uint at = index_of(pos, display);
    float weight = from_byte(words[section(kMask) + at]);
    if (!(weight > kEpsilon)) {
        return;
    }
    float2 uv = ((float2)pos + 0.5) / (float2)display;
    float4 color = float4(0.0, 0.0, 0.0, 0.0);
    int2 size = display;
    for (int level = 0; level < kInpaintingLevels; ++level) {
        size = int2(size.x / 2, size.y / 2);
        int2 s = inpaint_level_size((uint)level);
        if (s.x < 1 || s.y < 1) {
            continue;
        }
        Bilinear b = bilinear(uv, size);
        float4 mip = float4(0.0, 0.0, 0.0, 0.0);
        for (uint i = 0u; i < 4u; ++i) {
            int2 tap = b.base + kOffsets[i];
            if (on_screen(tap, size)) {
                float4 t = load_pyramid(kInpaintPyramidWords, (uint)level, tap, false);
                float w = b.weights[i] * (t.w > 0.0 ? 1.0 : 0.0);
                mip.rgb += t.rgb * w;
                mip.w += w;
            }
        }
        if (mip.w > 0.0) {
            float base = 1.0 - (float)level / 10.0;
            float level_weight = ((base * base) * base) * mip.w;
            color.rgb += (mip.rgb / mip.w) * level_weight;
            color.w += level_weight;
        }
    }
    float3 filled = lerp(unpack_rgb(words[section(kOutRgba) + at]), color.rgb / color.w, weight);
    words[section(kOutRgba) + at] = pack_rgba(filled);
}

// setupFrameinterpolationResources: frames since the last reset or scene change.
void setup(bool reset) {
    uint state = section(kState);
    if (reset || (words[state + kSceneHistory] & 0xFu) != 0u) {
        words[state + kFrames] = 0u;
    } else {
        words[state + kFrames] += 1u;
    }
}

// A thread's place in a run of count words: 64 a group, 1024 groups a row.
uint linear_index(uint3 group, uint local) {
    return (group.y * 1024u + group.x) * 64u + local;
}

[numthreads(8, 8, 1)]
void cs_main(uint3 id : SV_DispatchThreadID, uint3 group : SV_GroupID, uint local : SV_GroupIndex) {
    int2 pos = int2(id.xy);
    switch (step.x) {
    case kClearWords: {
        uint i = linear_index(group, local);
        if (i < step.w) {
            words[step.z + i] = 0u;
        }
        break;
    }
    case kClearAtoms: {
        uint i = linear_index(group, local);
        if (i < step.w) {
            atoms[step.z + i] = 0u;
        }
        break;
    }
    case kCopyWords: {
        uint i = linear_index(group, local);
        if (i < step.y) {
            words[step.w + i] = words[step.z + i];
        }
        break;
    }
    case kPrepare:
        prepare(pos);
        break;
    case kPack:
        pack(pos);
        break;
    case kLuma:
        luma_level(pos, step.y);
        break;
    case kHistogram:
        histogram(id);
        break;
    case kSceneChange:
        if (id.x == 0u && id.y == 0u) {
            scene_change(step.z != 0u);
        }
        break;
    case kSearch:
        search(pos, step.y);
        break;
    case kFilter:
        filter_vectors(pos, step.y);
        break;
    case kScale:
        scale_vectors(pos, step.y);
        break;
    case kSetup:
        if (id.x == 0u && id.y == 0u) {
            setup(step.z != 0u);
        }
        break;
    case kReconstruct:
        reconstruct(pos);
        break;
    case kGamePrimary:
        game_field(pos, false);
        break;
    case kGameSecondary:
        game_field(pos, true);
        break;
    case kGamePyramid:
        game_pyramid(pos, step.y);
        break;
    case kFlowField:
        flow_field(pos);
        break;
    case kDisocclusion:
        disocclusion(pos);
        break;
    case kInterpolate:
        interpolate(pos);
        break;
    case kInpaintPyramid:
        inpaint_pyramid(pos, step.y);
        break;
    case kInpaint:
        inpaint(pos);
        break;
    default:
        break;
    }
}
