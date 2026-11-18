// What SDFGI's compute shaders share: the cascades' words as src/sdfgi.hpp lays them out, and the
// helpers of src/sdfgi.cpp, its CPU twin, line for line. A shader that includes this declares
// cell_words, field_words, probe_words and items first (read-only or not), and Params.
//
// items holds, as words: kSdfgiItemCascade words a cascade (SdfgiCascadeItem in
// src/sdfgi_gpu.hpp), then the lights from kSdfgiItemLights, kSdfgiItemLight words each, then each
// cascade's probes' ray offsets where its item says.

static const uint kProbeAxis = 17u;
static const uint kProbeCount = 4913u; // kProbeAxis^3
static const uint kShSize = 16u;
static const uint kOctSize = 6u;
static const uint kOctBorder = 8u;
static const uint kProbeImageWidth = 2312u; // kOctBorder * kProbeAxis * kProbeAxis
static const uint kProbeImageHeight = 136u; // kOctBorder * kProbeAxis
static const uint kProbeImageTexels = 314432u;
static const uint kHistoryBits = 10u;
static const uint kSolid = 0x80000000u;
static const uint kFacingShift = 15u;
static const int kFar = 65536;
static const uint kItemCascade = 16u;
static const uint kItemLights = 128u;
static const uint kItemLight = 16u;
static const float kPi = 3.14159265358979;

// A cascade's place, as its item holds it.
struct Cascade {
    float3 offset;  // where its cell 0 starts, in the world with y scaled
    float cell_size;
    int3 position;  // its center, in cells
    float to_cell;  // 1 / cell_size
    uint cells;     // where its cells start in cell_words, then its occlusion after 2 a cell
    uint fields;    // where its field starts in field_words, then its light after 2 a cell
    uint probes;    // where its probes start in probe_words (SdfgiProbeSection)
    uint offsets;   // where its probes' ray offsets start in items
    uint history;   // where its probes' history starts in history_words
};

Cascade cascade_at(uint c) {
    uint at = c * kItemCascade;
    Cascade result;
    result.offset = asfloat(uint3(items[at], items[at + 1u], items[at + 2u]));
    result.cell_size = asfloat(items[at + 3u]);
    result.position = asint(uint3(items[at + 4u], items[at + 5u], items[at + 6u]));
    result.to_cell = asfloat(items[at + 7u]);
    result.cells = items[at + 8u];
    result.fields = items[at + 9u];
    result.probes = items[at + 10u];
    result.offsets = items[at + 11u];
    result.history = items[at + 12u];
    return result;
}

uint grid_size() {
    return grid.x;
}
uint cell_count() {
    return grid.x * grid.x * grid.x;
}
uint cascade_count() {
    return grid.y;
}
uint cell_index(int3 p) {
    return (uint(p.z) * grid.x + uint(p.y)) * grid.x + uint(p.x);
}
bool inside_grid(int3 p) {
    int n = int(grid.x);
    return all(p >= int3(0, 0, 0)) && all(p < int3(n, n, n));
}

// Where the averages and the probe images of a cascade's probes start (the order of
// SdfgiProbeSection); the history is a buffer of its own.
uint probe_average_at(Cascade c) {
    return c.probes;
}
uint probe_irradiance_at(Cascade c) {
    return c.probes + kProbeCount * kShSize * 4u;
}
uint probe_radiance_at(Cascade c) {
    return probe_irradiance_at(c) + kProbeImageTexels;
}

// 2 to a whole power from -126 to 127, exactly (exp2 needn't be exact on every GPU).
float power_of_two(int e) {
    return asfloat(uint(e + 127) << 23u);
}

// sdfgi_rgbe_encode and sdfgi_rgbe_decode.
uint rgbe_encode(float3 color) {
    const float kB = 15.0;
    const float kN = 9.0;
    float3 rgb = clamp(color, 0.0, 65408.0);
    float c_max = max(rgb.r, max(rgb.g, rgb.b));
    float log2_floor = -kB - 1.0;
    if (c_max > 0.0) {
        // frexp's exponent - 1: the float's own exponent.
        int e = int((asuint(c_max) >> 23u) & 0xFFu) - 127;
        if (((asuint(c_max) >> 23u) & 0xFFu) == 0u) {
            e = -127;
        }
        log2_floor = max(-kB - 1.0, float(e));
    }
    float expp = log2_floor + 1.0 + kB;
    float s_max = floor((c_max / power_of_two(int(expp - kB - kN))) + 0.5);
    float exps = expp + 1.0;
    if (0.0 <= s_max && s_max < 512.0) {
        exps = expp;
    }
    float unit = power_of_two(int(exps - kB - kN));
    uint3 q = uint3(floor((rgb / unit) + 0.5)) & 0x1FFu;
    return q.r | (q.g << 9u) | (q.b << 18u) | ((uint(exps) & 0x1Fu) << 27u);
}
float3 rgbe_decode(uint rgbe) {
    float unit = power_of_two(int((rgbe >> 27u) & 0x1Fu) - 15 - 9);
    return float3(float(rgbe & 0x1FFu), float((rgbe >> 9u) & 0x1FFu),
                  float((rgbe >> 18u) & 0x1FFu)) *
           unit;
}

// A solid cell's albedo (sdfgi_albedo) and the sides it faces (sdfgi_facing).
float3 cell_albedo(uint word) {
    return float3(float(word & 0x1Fu), float((word >> 5u) & 0x1Fu),
                  float((word >> 10u) & 0x1Fu)) /
           31.0;
}
uint cell_facing(uint word) {
    return (word & kSolid) != 0u ? (word >> kFacingShift) & 0x3Fu : 0u;
}

static const float3 kAnisoDir[6] = {float3(1.0, 0.0, 0.0),  float3(0.0, 1.0, 0.0),
                                    float3(0.0, 0.0, 1.0),  float3(-1.0, 0.0, 0.0),
                                    float3(0.0, -1.0, 0.0), float3(0.0, 0.0, -1.0)};

// A cascade's distance field byte, light and shares at a cell.
float cell_sdf(Cascade c, uint cell) {
    return float((field_words[c.fields + cell * 2u + 1u] >> 16u) & 0xFFu);
}
float3 cell_light(Cascade c, uint cell) {
    return rgbe_decode(field_words[c.fields + cell_count() * 2u + cell]);
}
void cell_aniso(Cascade c, uint cell, out float3 low, out float3 high) {
    uint w0 = field_words[c.fields + cell * 2u];
    uint w1 = field_words[c.fields + cell * 2u + 1u];
    low = float3(float(w0 & 0xFFu), float((w0 >> 8u) & 0xFFu), float((w0 >> 16u) & 0xFFu)) /
          255.0;
    high = float3(float(w0 >> 24u), float(w1 & 0xFFu), float((w1 >> 8u) & 0xFFu)) / 255.0;
}

// sample3's corners and weights: a texture of the cells read linearly at p (in cells), clamped
// to its edges.
struct Taps {
    uint index[8];
    float weight[8];
};
Taps taps_at(float3 p) {
    float most = float(grid_size() - 1u);
    float3 t = clamp(p - 0.5, 0.0, most);
    int3 base = int3(floor(t));
    float3 f = t - float3(base);
    Taps result;
    [unroll] for (uint c = 0u; c < 8u; ++c) {
        int3 d = int3(int(c & 1u), int((c >> 1u) & 1u), int((c >> 2u) & 1u));
        float3 w3 = float3(d.x != 0 ? f.x : 1.0 - f.x, d.y != 0 ? f.y : 1.0 - f.y,
                           d.z != 0 ? f.z : 1.0 - f.z);
        result.weight[c] = w3.x * w3.y * w3.z;
        result.index[c] = cell_index(min(base + d, int(grid_size()) - 1));
    }
    return result;
}
float sdf_at(Cascade c, float3 p) {
    Taps t = taps_at(p);
    float sum = 0.0;
    [unroll] for (uint i = 0u; i < 8u; ++i) {
        if (t.weight[i] != 0.0) {
            sum += cell_sdf(c, t.index[i]) * t.weight[i];
        }
    }
    return sum;
}
float3 light_at(Cascade c, float3 p) {
    Taps t = taps_at(p);
    float3 sum = float3(0.0, 0.0, 0.0);
    [unroll] for (uint i = 0u; i < 8u; ++i) {
        if (t.weight[i] != 0.0) {
            sum += cell_light(c, t.index[i]) * t.weight[i];
        }
    }
    return sum;
}
void aniso_at(Cascade c, float3 p, out float3 low, out float3 high) {
    Taps t = taps_at(p);
    low = float3(0.0, 0.0, 0.0);
    high = float3(0.0, 0.0, 0.0);
    [unroll] for (uint i = 0u; i < 8u; ++i) {
        if (t.weight[i] != 0.0) {
            float3 l;
            float3 h;
            cell_aniso(c, t.index[i], l, h);
            low += l * t.weight[i];
            high += h * t.weight[i];
        }
    }
}

// The occlusion of a parity at a cell (sdfgi_occlusion).
float cell_occlusion(Cascade c, uint cell, uint parity) {
    return float((cell_words[c.cells + cell_count() * 2u + cell] >> (parity * 4u)) & 0xFu) / 15.0;
}
float occlusion_at(Cascade c, float3 p, uint parity) {
    Taps t = taps_at(p);
    float sum = 0.0;
    [unroll] for (uint i = 0u; i < 8u; ++i) {
        if (t.weight[i] != 0.0) {
            sum += cell_occlusion(c, t.index[i], parity) * t.weight[i];
        }
    }
    return sum;
}

// probe_parity: which of the occlusion's eight parities a probe is.
uint probe_parity(Cascade c, int3 probe) {
    int occ = int(grid_size() / 16u);
    int3 world = (c.position / occ) + probe;
    return uint(world.x & 1) | (uint(world.y & 1) << 1u) | (uint(world.z & 1) << 2u);
}

// Godot's octahedron_encode (to 0 to 1) and its inverse.
float2 octahedron_encode(float3 n) {
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    float2 xy = n.xy;
    if (n.z < 0.0) {
        xy = float2((1.0 - abs(n.y)) * (n.x >= 0.0 ? 1.0 : -1.0),
                    (1.0 - abs(n.x)) * (n.y >= 0.0 ? 1.0 : -1.0));
    }
    return (xy * 0.5) + 0.5;
}
float3 octahedron_decode(float2 f) {
    f = (f * 2.0) - 1.0;
    float3 n = float3(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
    float t = clamp(-n.z, 0.0, 1.0);
    n.x += n.x >= 0.0 ? -t : t;
    n.y += n.y >= 0.0 ? -t : t;
    return normalize(n);
}

// A probe image (rgbe texels at base in probe_words, x fastest) read linearly at (x, y) in texels.
float3 probe_image_at(uint base, float x, float y) {
    float tx = clamp(x - 0.5, 0.0, float(kProbeImageWidth - 1u));
    float ty = clamp(y - 0.5, 0.0, float(kProbeImageHeight - 1u));
    uint x0 = uint(floor(tx));
    uint y0 = uint(floor(ty));
    uint x1 = min(x0 + 1u, kProbeImageWidth - 1u);
    uint y1 = min(y0 + 1u, kProbeImageHeight - 1u);
    float fx = tx - float(x0);
    float fy = ty - float(y0);
    float3 a = rgbe_decode(probe_words[base + y0 * kProbeImageWidth + x0]);
    float3 b = rgbe_decode(probe_words[base + y0 * kProbeImageWidth + x1]);
    float3 c = rgbe_decode(probe_words[base + y1 * kProbeImageWidth + x0]);
    float3 d = rgbe_decode(probe_words[base + y1 * kProbeImageWidth + x1]);
    return lerp(lerp(a, b, fx), lerp(c, d, fx), fy);
}
// probe_texel: a probe's image along a direction.
float3 probe_texel(uint base, int3 probe, float3 n) {
    float2 oct = octahedron_encode(n);
    float x = float((probe.x + probe.z * int(kProbeAxis)) * int(kOctBorder) + 1) +
              oct.x * float(kOctSize);
    float y = float(probe.y * int(kOctBorder) + 1) + oct.y * float(kOctSize);
    return probe_image_at(base, x, y);
}

// march: a ray through the cascades' distance fields from cascade first on. Returns whether it
// met something; then hit_cascade and hit_at (in its cells) say where.
bool march(uint first, float3 ray_pos, float3 ray_dir, float limit, float least,
           out uint hit_cascade, out float3 hit_at) {
    hit_cascade = 0u;
    hit_at = float3(0.0, 0.0, 0.0);
    float size = float(grid_size());
    float3 inv = 1.0 / ray_dir;
    float light_distance = limit;
    [loop] for (uint j = first; j < cascade_count(); ++j) {
        Cascade c = cascade_at(j);
        float3 pos = (ray_pos - c.offset) * c.to_cell;
        float local_distance = light_distance * c.to_cell;
        if (any(pos < 0.0) || any(pos >= size)) {
            continue;
        }
        float max_advance = 3.402823466e+38;
        [unroll] for (uint a = 0u; a < 3u; ++a) {
            float t0 = -pos[a] * inv[a];
            float t1 = (size - pos[a]) * inv[a];
            max_advance = min(max_advance, max(t0, t1));
        }
        max_advance = min(local_distance, max_advance);
        float advance = 0.0;
        [loop] while (advance < max_advance) {
            float3 p = pos + ray_dir * advance;
            float distance = sdf_at(c, p) - 1.0;
            if (distance < least) {
                hit_cascade = j;
                hit_at = p;
                return true;
            }
            advance += distance;
        }
        if (advance >= local_distance) {
            return false;
        }
        pos += ray_dir * max_advance;
        ray_pos = pos / c.to_cell + c.offset;
        light_distance -= max_advance / c.to_cell;
    }
    return false;
}
