// Parts of this file are ported from Godot Engine (MIT); see THIRD_PARTY.md.
// SDFGI's passes over its cascades: one compute shader whose uniforms say which pass runs.
// src/sdfgi.cpp is its CPU twin, and src/sdfgi_gpu.hpp lays out its buffers and lists the passes
// (SdfgiPass). Keep them in step. sdfgi_voxelize.hlsl sums the triangles into cells first and
// sdfgi_gather.hlsl gives each pixel its light after.

// SDL takes a compute shader's uniforms in space 2, what it reads in space 0 (textures first,
// then buffers) and writes in space 1. Metal has one list of buffers, uniforms first.
#ifdef SLASH_MSL
#define UNIFORMS register(b0)
#define ITEMS register(t1)
#define CELLS register(u2)
#define FIELDS register(u3)
#define PROBES register(u4)
#define SCRATCH register(u5)
#define HISTORY register(u6)
#else
#define UNIFORMS register(b0, space2)
#define ITEMS register(t1, space0)
#define CELLS register(u0, space1)
#define FIELDS register(u1, space1)
#define PROBES register(u2, space1)
#define SCRATCH register(u3, space1)
#define HISTORY register(u4, space1)
#endif

cbuffer Params : UNIFORMS {
    // The pass, the cascade, a number it takes (an axis, a parity, or the buffer a clear writes:
    // cells, fields, probes, history, then scratch) and its threads.
    uint4 step;
    uint4 grid;   // the cells a side, the cascades, the light's increment and offset
    uint4 probes; // the history's frames, this frame's, the rays a probe, flags (SdfgiFlags)
    int4 shift;   // how far the cascade moved, in cells or probes; w 1 when its cells scroll
    uint4 range;  // a clear's buffer word, its count and its value; w the lights
    float4 params; // the y scale, the bounce feedback, the probe bias
    float4 ambient;     // the ambient light, w 1 when the sky lights instead
    float4 sky_rows[3]; // the world to the sky's space; the sky's energy in [0].w, its share [1].w
};

// The sky's radiance cube, sRGB, for rays that leave the cascades.
TextureCube<float4> radiance_map : register(t0, space0);
SamplerState radiance_sampler : register(s0, space0);

StructuredBuffer<uint> items : ITEMS;
RWStructuredBuffer<uint> cell_words : CELLS;
RWStructuredBuffer<uint> field_words : FIELDS;
RWStructuredBuffer<uint> probe_words : PROBES;
RWStructuredBuffer<uint> scratch : SCRATCH;
RWStructuredBuffer<uint> history_words : HISTORY;

#include "sdfgi_common.hlsl"

// The passes (SdfgiPass).
static const uint kClear = 0u;
static const uint kResolve = 1u;
static const uint kEdtInit = 2u;
static const uint kEdt = 3u;
static const uint kStore = 4u;
static const uint kOcclusion = 5u;
static const uint kOcclusionStore = 6u;
static const uint kLight = 7u;
static const uint kLightMerge = 8u;
static const uint kLightGather = 9u;
static const uint kIntegrate = 10u;
static const uint kProbeStore = 11u;
static const uint kScrollCopy = 12u;
static const uint kScrollMove = 13u;

// SdfgiFlags.
static const uint kFlagOcclusion = 1u;
static const uint kFlagSkyLight = 2u;

int3 cell_place(uint i) {
    uint n = grid_size();
    return int3(int(i % n), int((i / n) % n), int(i / (n * n)));
}

// --- Cells (voxelize's resolve, its distance field and MODE_STORE) -----------------------------

// solid_cell: a solid cell's two words from its sums.
uint2 solid_cell(uint at) {
    uint count = scratch[at + 6u];
    uint word = kSolid | (scratch[at + 7u] << kFacingShift);
    float3 emission = float3(0.0, 0.0, 0.0);
    [unroll] for (uint c = 0u; c < 3u; ++c) {
        uint a5 = ((62u * scratch[at + c]) + (255u * count)) / (510u * count);
        word |= min(a5, 31u) << (5u * c);
        uint e = (scratch[at + 3u + c] + (count / 2u)) / count;
        emission[c] = float(e) / 256.0;
    }
    return uint2(word, rgbe_encode(emission));
}

// The cascade's new cells: those it still holds moved, the solid ones that came in from their
// sums (8 words a cell in scratch). Written over its field, which is made again after, for the
// device to copy into its cells.
void resolve(uint i) {
    Cascade c = cascade_at(step.y);
    int3 p = cell_place(i);
    uint2 cell = uint2(0u, 0u);
    if (shift.w != 0 && inside_grid(p + shift.xyz)) {
        uint from = c.cells + cell_index(p + shift.xyz) * 2u;
        if ((cell_words[from] & kSolid) != 0u) {
            cell = uint2(cell_words[from], cell_words[from + 1u]);
        }
    } else if (scratch[i * 8u + 6u] > 0u) {
        cell = solid_cell(i * 8u);
    }
    field_words[c.fields + i * 2u] = cell.x;
    field_words[c.fields + i * 2u + 1u] = cell.y;
}

// The cell at t along a line of the distance transform.
uint line_cell(uint row, uint t) {
    uint n = grid_size();
    uint a = row % n;
    uint b = row / n;
    if (step.z == 0u) {
        return (b * n + a) * n + t;
    }
    return step.z == 1u ? (b * n + t) * n + a : (t * n + a) * n + b;
}

// distance_pass over a line, in whole numbers: scratch holds the squared distances in [0, n^3),
// and the line's workspace (v, the crossings and a copy of the line) after.
void distance_line(uint row) {
    uint n = grid_size();
    uint cells = cell_count();
    uint ws = row * n;
    uint v_at = cells + ws;
    uint num_at = cells * 2u + ws;
    uint den_at = cells * 3u + ws;
    uint f_at = cells * 4u + ws;
    [loop] for (uint t = 0u; t < n; ++t) {
        scratch[f_at + t] = scratch[line_cell(row, t)];
    }
    int k = -1;
    [loop] for (int q = 0; q < int(n); ++q) {
        int fq = asint(scratch[f_at + uint(q)]);
        if (fq < kFar) {
            int num = 0;
            int den = 1;
            // No break: Tint's SPIR-V reader can't take loops that leave early with values.
            bool popping = true;
            // Loads are never under a condition of their own: lavapipe reads a buffer's
            // descriptor from a lane no thread runs when none takes such a branch.
            [loop] while (k >= 0 && popping) {
                int p = asint(scratch[v_at + uint(k)]);
                num = (fq + (q * q)) - (asint(scratch[f_at + uint(p)]) + (p * p));
                den = 2 * (q - p);
                int k_num = asint(scratch[num_at + uint(k)]);
                int k_den = asint(scratch[den_at + uint(k)]);
                if (k > 0 && num * k_den <= k_num * den) {
                    --k;
                } else {
                    popping = false;
                }
            }
            ++k;
            scratch[v_at + uint(k)] = asuint(q);
            scratch[num_at + uint(k)] = asuint(num);
            scratch[den_at + uint(k)] = asuint(den);
        }
    }
    if (k < 0) {
        [loop] for (uint t2 = 0u; t2 < n; ++t2) {
            scratch[line_cell(row, t2)] = asuint(kFar);
        }
        return;
    }
    int j = 0;
    [loop] for (int q2 = 0; q2 < int(n); ++q2) {
        bool advancing = j < k;
        [loop] while (advancing) {
            int next = min(j + 1, k);
            int next_num = asint(scratch[num_at + uint(next)]);
            int next_den = asint(scratch[den_at + uint(next)]);
            advancing = j < k && next_num < q2 * next_den;
            j += advancing ? 1 : 0;
            advancing = advancing && j < k;
        }
        int p2 = asint(scratch[v_at + uint(j)]);
        scratch[line_cell(row, uint(q2))] =
            asuint(min(kFar, ((q2 - p2) * (q2 - p2)) + asint(scratch[f_at + uint(p2)])));
    }
}

// sdf_byte: 1 + the square root of a squared distance, rounded, up to 255, found a bit at a time
// with no float and no loop whose length depends on the value, so every device makes the
// same byte.
uint sdf_byte(int d2) {
    uint value = uint(max(d2, 0));
    uint root = 0u;
    [unroll] for (int b = 8; b >= 0; --b) {
        uint tried = root | (1u << uint(b));
        root = tried * tried <= value ? tried : root;
    }
    uint rounded = value > (root * root) + root ? root + 1u : root;
    return min(255u, 1u + rounded);
}

// One of MODE_STORE's 26 neighbors, by its number.
int3 neighbor(uint n) {
    uint m = n < 13u ? n : n + 1u;
    return int3(int(m / 9u) - 1, int((m / 3u) % 3u) - 1, int(m % 3u) - 1);
}

// MODE_STORE: the distance field, the light starting over, and the empty cells next to solid
// ones naming the nearest.
void store(uint i) {
    Cascade c = cascade_at(step.y);
    bool solid = (cell_words[c.cells + i * 2u] & kSolid) != 0u;
    int d2 = asint(scratch[i]);
    field_words[c.fields + i * 2u] = 0u;
    field_words[c.fields + i * 2u + 1u] = solid ? 0u : sdf_byte(d2) << 16u;
    field_words[c.fields + cell_count() * 2u + i] = 0u;
    if (solid || d2 > 3) {
        return;
    }
    int3 p = cell_place(i);
    int best = -1;
    int best_length = 4;
    [loop] for (uint n = 0u; n < 26u; ++n) {
        int3 o = neighbor(n);
        int3 q = p + o;
        if (!inside_grid(q) || (cell_words[c.cells + cell_index(q) * 2u] & kSolid) == 0u) {
            continue;
        }
        int l = dot(o, o);
        if (l < best_length) {
            best_length = l;
            best = int(n);
        }
    }
    if (best >= 0) {
        cell_words[c.cells + i * 2u] = uint(best) + 1u;
    }
}

// --- Occlusion (MODE_OCCLUSION, make_sdfgi_occlusion) ------------------------------------------

uint facing_at(Cascade c, int3 p) {
    if (!inside_grid(p)) {
        return 0u;
    }
    return cell_facing(cell_words[c.cells + cell_index(p) * 2u]);
}

uint average_of(uint sum, uint count) {
    return count > 0u ? ((2u * sum) + count) / (2u * count) : 0u;
}

// Where a local cell lies from the probe, without a zero.
int3 proc_pos(int3 l, int occ) {
    int3 p = l - occ;
    return p + int3(p.x >= 0 ? 1 : 0, p.y >= 0 ? 1 : 0, p.z >= 0 ? 1 : 0);
}

// One region of a parity: the flood out from its probe, then the solid cells, then the empty
// cells next to them, in scratch's [0, n^3).
void occlusion_region(uint r) {
    Cascade c = cascade_at(step.y);
    uint index = step.z;
    int occ = int(grid_size() / 16u);
    int span = occ * 2;
    int3 probe_global = c.position / occ;
    int3 offset;
    [unroll] for (uint a = 0u; a < 3u; ++a) {
        offset[a] = int(((index >> a) & 1u) ^ uint(probe_global[a] & 1));
    }
    const int kRegions = 9;
    int3 groups = kRegions - offset;
    int ri = int(r);
    int3 region = int3(ri % groups.x, (ri / groups.x) % groups.y, ri / (groups.x * groups.y));
    int3 base = -occ + (region * span) + (offset * occ);
    [loop] for (int stp = 0; stp <= (occ - 1) * 3; ++stp) {
        [loop] for (int az = 0; az < occ; ++az) {
            [loop] for (int ay = 0; ay < occ; ++ay) {
                int ax = stp - ay - az;
                if (ax < 0 || ax >= occ) {
                    continue;
                }
                int3 proc_abs = int3(ax, ay, az);
                [loop] for (uint i = 0u; i < 8u; ++i) {
                    int3 bit = int3(int(i & 1u), int((i >> 1u) & 1u), int((i >> 2u) & 1u));
                    int3 sign = (bit * 2) - 1;
                    int3 l = occ + (proc_abs * sign) - (1 - bit);
                    if (!inside_grid(base + l)) {
                        continue;
                    }
                    uint o = 0u;
                    if (facing_at(c, base + l) != 0u) {
                        o = 0u;
                    } else if (stp == 0) {
                        o = 255u;
                    } else {
                        int3 read_dir = -sign;
                        int3 major;
                        if (ax < ay) {
                            major = az < ay ? int3(0, 1, 0) : int3(0, 0, 1);
                        } else {
                            major = az < ax ? int3(1, 0, 0) : int3(0, 0, 1);
                        }
                        uint sum = 0u;
                        uint avg = 0u;
                        [unroll] for (uint a2 = 0u; a2 < 3u; ++a2) {
                            int3 read = l;
                            read[a2] += read_dir[a2];
                            if (proc_abs[a2] == 0) {
                                read += major * read_dir;
                            }
                            if (facing_at(c, base + read) == 0u) {
                                if (inside_grid(base + read)) {
                                    sum += scratch[cell_index(base + read)];
                                    ++avg;
                                }
                            } else if (proc_abs[a2] != 0) {
                                ++avg;
                            }
                        }
                        o = average_of(sum, avg);
                    }
                    scratch[cell_index(base + l)] = o;
                }
            }
        }
    }
    // Solid cells take the light of the empty cells toward the probe on the sides they face.
    [loop] for (int z = 0; z < span; ++z) {
        [loop] for (int y = 0; y < span; ++y) {
            [loop] for (int x = 0; x < span; ++x) {
                int3 l = int3(x, y, z);
                if (!inside_grid(base + l)) {
                    continue;
                }
                uint f = facing_at(c, base + l);
                if (f == 0u) {
                    continue;
                }
                int3 p = proc_pos(l, occ);
                int3 read_dir;
                bool3 valid;
                [unroll] for (uint a3 = 0u; a3 < 3u; ++a3) {
                    read_dir[a3] = p[a3] > 0 ? -1 : 1;
                    uint pos_bit = (f >> a3) & 1u;
                    uint neg_bit = (f >> (a3 + 3u)) & 1u;
                    valid[a3] = (read_dir[a3] > 0 ? pos_bit : neg_bit) != 0u;
                }
                uint sum = 0u;
                uint avg = 0u;
                // Sides, then edges, then the corner.
                int3 takes[7] = {int3(1, 0, 0), int3(0, 1, 0), int3(0, 0, 1), int3(0, 1, 1),
                                 int3(1, 0, 1), int3(1, 1, 0), int3(1, 1, 1)};
                [unroll] for (uint t = 0u; t < 7u; ++t) {
                    int3 m = takes[t];
                    bool want = (m.x == 0 || valid.x) && (m.y == 0 || valid.y) &&
                                (m.z == 0 || valid.z);
                    if (!want) {
                        continue;
                    }
                    int3 read = l + (m * read_dir);
                    if (facing_at(c, base + read) == 0u && inside_grid(base + read)) {
                        sum += scratch[cell_index(base + read)];
                        ++avg;
                    }
                }
                scratch[cell_index(base + l)] = average_of(sum, avg);
            }
        }
    }
    // Empty cells next to solid ones see the probe as much as those face it.
    [loop] for (int z2 = 0; z2 < span; ++z2) {
        [loop] for (int y2 = 0; y2 < span; ++y2) {
            [loop] for (int x2 = 0; x2 < span; ++x2) {
                int3 l = int3(x2, y2, z2);
                if (!inside_grid(base + l) || facing_at(c, base + l) != 0u) {
                    continue;
                }
                int3 p = proc_pos(l, occ);
                uint visible = 0u;
                uint total = 0u;
                // One unrolled loop with no continue and no indexing by a counter, so fxc
                // can unroll it inside the loops above.
                [unroll] for (uint k6 = 0u; k6 < 6u; ++k6) {
                    uint which = k6 / 3u;
                    uint a4 = k6 % 3u;
                    int pa = a4 == 0u ? p.x : (a4 == 1u ? p.y : p.z);
                    int dir = pa > 0 ? 1 : -1;
                    bool tested = which == 0u ? abs(pa) < occ : abs(pa) == 1;
                    if (tested) {
                        // Away from the probe: a cell facing back toward it; near its plane,
                        // the other way too.
                        int way = which == 0u ? dir : -dir;
                        uint mask = (which == 0u ? dir > 0 : dir < 0) ? (8u << a4) : (1u << a4);
                        int3 read = l + int3(a4 == 0u ? way : 0, a4 == 1u ? way : 0,
                                             a4 == 2u ? way : 0);
                        uint m = facing_at(c, base + read);
                        if (m != 0u && inside_grid(base + read)) {
                            ++total;
                            if ((m & mask) != 0u) {
                                ++visible;
                            }
                        }
                    }
                }
                if (total > 0u) {
                    uint at = cell_index(base + l);
                    scratch[at] = ((2u * scratch[at] * visible) + total) / (2u * total);
                }
            }
        }
    }
}

// --- Light (sdfgi_direct_light.glsl's MODE_PROCESS_DYNAMIC, light_cells) ----------------------

bool lit_cell(Cascade c, uint i) {
    return (cell_words[c.cells + i * 2u] & kSolid) != 0u && (i % grid.z) == grid.w;
}

float omni_attenuation(float distance, float inv_range, float decay) {
    float nd = distance * inv_range;
    nd *= nd;
    nd *= nd;
    nd = max(1.0 - nd, 0.0);
    nd *= nd;
    return nd * pow(max(distance, 0.0001), -decay);
}

uint share_of(float v) {
    return uint(floor((clamp(v, 0.0, 1.0) * 255.0) + 0.5));
}

float max3(float3 v) {
    return max(v.x, max(v.y, v.z));
}

void light_cell(uint i) {
    uint ci = step.y;
    Cascade c = cascade_at(ci);
    if (!lit_cell(c, i)) {
        return;
    }
    uint size = grid_size();
    uint word = cell_words[c.cells + i * 2u];
    float3 albedo = cell_albedo(word);
    float3 emission = rgbe_decode(cell_words[c.cells + i * 2u + 1u]);
    uint facing = cell_facing(word);
    int3 cpos = cell_place(i);
    float3 cell_center = float3(cpos) + 0.5;
    float3 position = (cell_center * c.cell_size) + c.offset;
    float3 accum[6];
    [unroll] for (uint k0 = 0u; k0 < 6u; ++k0) {
        accum[k0] = float3(0.0, 0.0, 0.0);
    }
    float feedback_amount = params.y;
    if (feedback_amount > 0.001) {
        float3 feedback = feedback_amount < 1.0
                              ? albedo * feedback_amount
                              : lerp(albedo, float3(1.0, 1.0, 1.0), feedback_amount - 1.0);
        float3 pos = cell_center * (16.0 / float(size));
        int3 base = int3(pos);
        float weights[6] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
        uint irradiance = probe_irradiance_at(c);
        [loop] for (uint j = 0u; j < 8u; ++j) {
            int3 probe = base + int3(int(j & 1u), int((j >> 1u) & 1u), int((j >> 2u) & 1u));
            float3 probe_to_pos = pos - float3(probe);
            float3 probe_dir = normalize(-probe_to_pos);
            float3 trilinear = 1.0 - abs(probe_to_pos);
            [loop] for (uint k = 0u; k < 6u; ++k) {
                if ((facing & (1u << k)) == 0u) {
                    continue;
                }
                float weight = trilinear.x * trilinear.y * trilinear.z *
                               max(0.0, dot(kAnisoDir[k], probe_dir));
                if (weight > 0.0 && (probes.w & kFlagOcclusion) != 0u) {
                    int3 next = clamp(cpos + int3(kAnisoDir[k]), 0, int(size) - 1);
                    weight *= cell_occlusion(c, cell_index(next), probe_parity(c, probe));
                }
                if (weight > 0.0) {
                    accum[k] += probe_texel(irradiance, probe, kAnisoDir[k]) * weight;
                    weights[k] += weight;
                }
            }
        }
        [unroll] for (uint k2 = 0u; k2 < 6u; ++k2) {
            if (weights[k2] > 0.0) {
                accum[k2] = (accum[k2] * (1.0 / weights[k2])) * feedback;
            }
        }
    }
    {
        uint sides = 0u;
        [unroll] for (uint k3 = 0u; k3 < 6u; ++k3) {
            sides += (facing >> k3) & 1u;
        }
        if (sides > 0u) {
            float3 portion = emission * (1.0 / float(sides));
            [unroll] for (uint k4 = 0u; k4 < 6u; ++k4) {
                if ((facing & (1u << k4)) != 0u) {
                    accum[k4] += portion;
                }
            }
        }
    }
    [loop] for (uint li = 0u; li < range.w; ++li) {
        uint at = kItemLights + li * kItemLight;
        uint type = items[at];
        float3 color = asfloat(uint3(items[at + 1u], items[at + 2u], items[at + 3u]));
        float3 way = asfloat(uint3(items[at + 4u], items[at + 5u], items[at + 6u]));
        float3 place = asfloat(uint3(items[at + 7u], items[at + 8u], items[at + 9u]));
        float attenuation = 1.0;
        float3 direction;
        float light_distance = 1e20;
        if (type == 0u) {
            direction = -way;
        } else {
            float3 rel = place - position;
            direction = normalize(rel);
            light_distance = length(rel);
            attenuation = omni_attenuation(light_distance, 1.0 / asfloat(items[at + 11u]),
                                           asfloat(items[at + 10u]));
            if (type == 2u) {
                float cos_spot = asfloat(items[at + 12u]);
                float cos_angle = dot(-direction, way);
                if (cos_angle < cos_spot) {
                    continue;
                }
                float scos = max(cos_angle, cos_spot);
                float rim = max(0.0001, (1.0 - scos) / (1.0 - cos_spot));
                attenuation *= 1.0 - pow(rim, asfloat(items[at + 13u]));
            }
        }
        if (attenuation < 0.001) {
            continue;
        }
        float3 ray_pos = position + (sign(direction) * c.cell_size * 0.48);
        ray_pos += direction * (0.4 * c.cell_size);
        uint hit_cascade;
        float3 hit_at;
        if (march(ci, ray_pos, direction, light_distance, 0.001, hit_cascade, hit_at)) {
            continue;
        }
        float3 l = (albedo * color) * attenuation;
        [unroll] for (uint k5 = 0u; k5 < 6u; ++k5) {
            if ((facing & (1u << k5)) != 0u) {
                accum[k5] += l * max(0.0, dot(kAnisoDir[k5], direction));
            }
        }
    }
    float3 total = float3(0.0, 0.0, 0.0);
    [unroll] for (uint k6 = 0u; k6 < 6u; ++k6) {
        total += accum[k6];
    }
    float luma_total = max3(total);
    uint shares[6];
    [unroll] for (uint k7 = 0u; k7 < 6u; ++k7) {
        shares[k7] = luma_total > 0.0 ? share_of(max3(accum[k7]) / luma_total) : 0u;
    }
    field_words[c.fields + cell_count() * 2u + i] = rgbe_encode(total);
    field_words[c.fields + i * 2u] =
        shares[0] | (shares[1] << 8u) | (shares[2] << 16u) | (shares[3] << 24u);
    scratch[i] = shares[4] | (shares[5] << 8u);
}

void light_merge(uint i) {
    Cascade c = cascade_at(step.y);
    if (lit_cell(c, i)) {
        uint at = c.fields + i * 2u + 1u;
        field_words[at] = (field_words[at] & 0xFFFF0000u) | scratch[i];
    }
}

// The empty cells next to solid ones take the light of the one they name.
void light_gather(uint i) {
    Cascade c = cascade_at(step.y);
    uint word = cell_words[c.cells + i * 2u];
    if ((word & kSolid) != 0u || word == 0u) {
        return;
    }
    uint from = cell_index(cell_place(i) + neighbor(word - 1u));
    if (!lit_cell(c, from)) {
        return;
    }
    uint light_at = c.fields + cell_count() * 2u;
    field_words[light_at + i] = field_words[light_at + from];
    field_words[c.fields + i * 2u] = field_words[c.fields + from * 2u];
    uint at = c.fields + i * 2u + 1u;
    field_words[at] = (field_words[at] & 0xFFFF0000u) |
                      (field_words[c.fields + from * 2u + 1u] & 0xFFFFu);
}

// --- Probes (sdfgi_integrate.glsl, integrate) --------------------------------------------------

void sh_basis(float3 d, out float b[16]) {
    float3 d2 = d * d;
    b[0] = 0.282095;
    b[1] = 0.488603 * d.y;
    b[2] = 0.488603 * d.z;
    b[3] = 0.488603 * d.x;
    b[4] = 1.092548 * d.x * d.y;
    b[5] = 1.092548 * d.y * d.z;
    b[6] = 0.315392 * ((3.0 * d2.z) - 1.0);
    b[7] = 1.092548 * d.x * d.z;
    b[8] = 0.546274 * (d2.x - d2.y);
    b[9] = 0.590043 * d.y * ((3.0 * d2.x) - d2.y);
    b[10] = 2.890611 * d.y * d.x * d.z;
    b[11] = 0.646360 * d.y * (-1.0 + (5.0 * d2.z));
    b[12] = 0.373176 * ((5.0 * d2.z * d.z) - (3.0 * d.z));
    b[13] = 0.457045 * d.x * (-1.0 + (5.0 * d2.z));
    b[14] = 1.445305 * (d2.x - d2.y) * d.z;
    b[15] = 0.590043 * d.x * (d2.x - (3.0 * d2.y));
}

float3 vogel_hemisphere(uint index, uint count, float offset) {
    float golden = kPi * (3.0 - sqrt(5.0));
    float r = sqrt(float(index) + 0.5) / sqrt(float(count));
    float theta = (float(index) * golden) + offset;
    float yl = r * kPi * 0.5;
    return float3(sin(yl) * cos(theta), sin(yl) * sin(theta),
                  cos(yl) * ((index & 1u) != 0u ? 1.0 : -1.0));
}

float3 srgb_to_linear(float3 c) {
    return lerp(pow((c + 0.055) * (1.0 / 1.055), 2.4), c * (1.0 / 12.92), float3(c < 0.04045));
}

// The environment's light along a direction: the sky's radiance over the ambient light by its
// share, or the ambient light (ray_light.comp's environment).
float3 environment(float3 direction) {
    if (ambient.w == 0.0) {
        return ambient.rgb;
    }
    float3 turned = float3(dot(sky_rows[0].xyz, direction), dot(sky_rows[1].xyz, direction),
                           dot(sky_rows[2].xyz, direction));
    float3 sky = srgb_to_linear(radiance_map.SampleLevel(radiance_sampler, turned, 0.0).rgb) *
                 sky_rows[0].w;
    return lerp(ambient.rgb, sky, sky_rows[1].w);
}

int3 probe_place(uint p) {
    return int3(int(p % kProbeAxis), int((p / kProbeAxis) % kProbeAxis),
                int(p / (kProbeAxis * kProbeAxis)));
}

void integrate(uint p) {
    uint ci = step.y;
    Cascade c = cascade_at(ci);
    uint size = grid_size();
    float y_mult = params.x;
    uint history_size = probes.x;
    uint history_index = probes.y;
    uint ray_count = probes.z;
    uint ray_total = history_size * ray_count;
    float probe_cell_size = float(size / 16u) * c.cell_size;
    int3 probe = probe_place(p);
    float3 probe_pos = c.offset + float3(probe) * probe_cell_size;
    float offset = asfloat(items[c.offsets + p]);
    float3 sh[16];
    [unroll] for (uint s0 = 0u; s0 < 16u; ++s0) {
        sh[s0] = float3(0.0, 0.0, 0.0);
    }
    [loop] for (uint i = 0u; i < ray_count; ++i) {
        float3 ray_dir = vogel_hemisphere(history_index + (i * history_size), ray_total, offset);
        ray_dir.y *= y_mult;
        ray_dir = normalize(ray_dir);
        float3 ray_pos = probe_pos + ray_dir * ((1.0 / max3(abs(ray_dir))) * params.z / c.to_cell);
        uint hit_cascade;
        float3 hit_at;
        float3 light = float3(0.0, 0.0, 0.0);
        if (march(ci, ray_pos, ray_dir, 3.402823466e+38, 0.05, hit_cascade, hit_at)) {
            Cascade hit = cascade_at(hit_cascade);
            float eps = 0.001 * float(size);
            float3 n;
            [unroll] for (uint axis = 0u; axis < 3u; ++axis) {
                float3 a = hit_at;
                float3 b = hit_at;
                a[axis] += eps;
                b[axis] -= eps;
                n[axis] = sdf_at(hit, a) - sdf_at(hit, b);
            }
            n = normalize(n);
            float3 hit_light = light_at(hit, hit_at);
            float3 low;
            float3 high;
            aniso_at(hit, hit_at, low, high);
            float portion = 0.0;
            [unroll] for (uint k = 0u; k < 3u; ++k) {
                portion += max(0.0, n[k] * low[k]);
                portion += max(0.0, -n[k] * high[k]);
            }
            light = hit_light * portion;
        } else if ((probes.w & kFlagSkyLight) != 0u) {
            light = environment(ray_dir);
        }
        float basis[16];
        sh_basis(ray_dir, basis);
        [unroll] for (uint s = 0u; s < 16u; ++s) {
            sh[s] += light * basis[s];
        }
    }
    uint average = probe_average_at(c);
    uint history = c.history;
    uint sh_count = kProbeCount * kShSize;
    [unroll] for (uint s2 = 0u; s2 < 16u; ++s2) {
        uint at = (p * kShSize) + s2;
        // Whole vectors: Windows' compiler reads both sides of a select, so sh[s2][3] fails.
        float4 v = float4(sh[s2], 1.0) * 4.0 / float(ray_count);
        float4 scaled = v * float(1u << kHistoryBits);
        int4 value = clamp(int4(scaled), int4(-32768, -32768, -32768, -32768),
                           int4(32767, 32767, 32767, 32767));
        uint h = history + ((history_index * sh_count) + at) * 2u;
        uint w0 = history_words[h];
        uint w1 = history_words[h + 1u];
        int4 prev = int4(int(w0 << 16u) >> 16, int(w0) >> 16, int(w1 << 16u) >> 16, int(w1) >> 16);
        [unroll] for (uint ch2 = 0u; ch2 < 4u; ++ch2) {
            uint a = average + at * 4u + ch2;
            probe_words[a] = asuint(asint(probe_words[a]) + (value[ch2] - prev[ch2]));
        }
        history_words[h] = (uint(value.x) & 0xFFFFu) | (uint(value.y) << 16u);
        history_words[h + 1u] = (uint(value.z) & 0xFFFFu) | (uint(value.w) << 16u);
    }
}

void put_texels(uint irradiance_at, uint radiance_at, int x, int y, uint irr, uint rad) {
    uint at = uint(y) * kProbeImageWidth + uint(x);
    probe_words[irradiance_at + at] = irr;
    probe_words[radiance_at + at] = rad;
}

// MODE_STORE: a probe's octahedral irradiance and radiance, with borders.
void probe_store(uint p) {
    Cascade c = cascade_at(step.y);
    int3 probe = probe_place(p);
    int ox = ((probe.x + (probe.z * int(kProbeAxis))) * int(kOctBorder)) + 1;
    int oy = (probe.y * int(kOctBorder)) + 1;
    const float l_mult[16] = {1.0,  2.0 / 3.0, 2.0 / 3.0, 2.0 / 3.0, 0.25, 0.25, 0.25, 0.25,
                              0.25, 0.0,       0.0,       0.0,       0.0,  0.0,  0.0,  0.0};
    uint average = probe_average_at(c);
    uint irradiance_at = probe_irradiance_at(c);
    uint radiance_at = probe_radiance_at(c);
    const int kO = int(kOctSize);
    float k = 1.0 / float(probes.x) / float(1u << kHistoryBits);
    [loop] for (int ly = 0; ly < kO; ++ly) {
        [loop] for (int lx = 0; lx < kO; ++lx) {
            float3 normal = octahedron_decode(float2(float(lx) / float(kO), float(ly) / float(kO)));
            float basis[16];
            sh_basis(normal, basis);
            float3 irradiance = float3(0.0, 0.0, 0.0);
            float3 radiance = float3(0.0, 0.0, 0.0);
            [unroll] for (uint s = 0u; s < 16u; ++s) {
                uint a = average + ((p * kShSize) + s) * 4u;
                float3 m = float3(float(asint(probe_words[a])) * k,
                                  float(asint(probe_words[a + 1u])) * k,
                                  float(asint(probe_words[a + 2u])) * k) *
                           (basis[s] * 4.0);
                irradiance += m * l_mult[s];
                radiance += m;
            }
            uint irr = rgbe_encode(irradiance);
            uint rad = rgbe_encode(radiance);
            put_texels(irradiance_at, radiance_at, ox + lx, oy + ly, irr, rad);
            int2 to[3] = {int2(0, 0), int2(0, 0), int2(0, 0)};
            uint count = 0u;
            if (lx == 0 && ly == 0) {
                to[0] = int2(kO - 1, -1);
                to[1] = int2(-1, kO - 1);
                to[2] = int2(kO, kO);
                count = 3u;
            } else if (lx == kO - 1 && ly == 0) {
                to[0] = int2(0, -1);
                to[1] = int2(kO, kO - 1);
                to[2] = int2(-1, kO);
                count = 3u;
            } else if (lx == 0 && ly == kO - 1) {
                to[0] = int2(-1, 0);
                to[1] = int2(kO - 1, kO);
                to[2] = int2(kO, -1);
                count = 3u;
            } else if (lx == kO - 1 && ly == kO - 1) {
                to[0] = int2(0, kO);
                to[1] = int2(kO, 0);
                to[2] = int2(-1, -1);
                count = 3u;
            } else if (ly == 0) {
                to[0] = int2(kO - lx - 1, ly - 1);
                count = 1u;
            } else if (lx == 0) {
                to[0] = int2(lx - 1, kO - ly - 1);
                count = 1u;
            } else if (ly == kO - 1) {
                to[0] = int2(kO - lx - 1, ly + 1);
                count = 1u;
            } else if (lx == kO - 1) {
                to[0] = int2(lx + 1, kO - ly - 1);
                count = 1u;
            }
            [loop] for (uint t = 0u; t < count; ++t) {
                put_texels(irradiance_at, radiance_at, ox + to[t].x, oy + to[t].y, irr, rad);
            }
        }
    }
}

// MODE_SCROLL: the averages, then the history, copied into scratch, then moved back by whole
// probes, those that come in cleared.
uint scroll_words() {
    return kProbeCount * kShSize * 4u + probes.x * kProbeCount * kShSize * 2u;
}
uint scroll_word(Cascade c, uint w) {
    uint averages = kProbeCount * kShSize * 4u;
    return w < averages ? probe_words[probe_average_at(c) + w]
                        : history_words[c.history + (w - averages)];
}

void scroll_move(uint p) {
    Cascade c = cascade_at(step.y);
    int3 to = probe_place(p);
    int3 from = to + shift.xyz;
    bool kept = all(from >= int3(0, 0, 0)) && all(from < int3(17, 17, 17));
    uint src = kept ? uint(((from.z * 17) + from.y) * 17 + from.x) : 0u;
    uint averages = kProbeCount * kShSize * 4u;
    uint sh_count = kProbeCount * kShSize;
    [loop] for (uint i = 0u; i < kShSize * 4u; ++i) {
        uint value = kept ? scratch[src * kShSize * 4u + i] : 0u;
        probe_words[probe_average_at(c) + p * kShSize * 4u + i] = value;
    }
    [loop] for (uint h = 0u; h < probes.x; ++h) {
        [loop] for (uint j = 0u; j < kShSize * 2u; ++j) {
            uint value = kept ? scratch[averages + (h * sh_count + src * kShSize) * 2u + j] : 0u;
            history_words[c.history + (h * sh_count + p * kShSize) * 2u + j] = value;
        }
    }
}

[numthreads(64, 1, 1)]
void cs_main(uint3 id : SV_DispatchThreadID) {
    uint i = id.x + (id.y * 65536u);
    if (i >= step.w) {
        return;
    }
    uint which = step.x;
    if (which == kClear) {
        uint at = range.x + i;
        if (step.z == 0u) {
            cell_words[at] = range.z;
        } else if (step.z == 1u) {
            field_words[at] = range.z;
        } else if (step.z == 2u) {
            probe_words[at] = range.z;
        } else if (step.z == 3u) {
            history_words[at] = range.z;
        } else {
            scratch[at] = range.z;
        }
    } else if (which == kResolve) {
        resolve(i);
    } else if (which == kEdtInit) {
        Cascade c = cascade_at(step.y);
        scratch[i] = (cell_words[c.cells + i * 2u] & kSolid) != 0u ? 0u : asuint(kFar);
    } else if (which == kEdt) {
        distance_line(i);
    } else if (which == kStore) {
        store(i);
    } else if (which == kOcclusion) {
        occlusion_region(i);
    } else if (which == kOcclusionStore) {
        Cascade c = cascade_at(step.y);
        uint at = c.cells + cell_count() * 2u + i;
        cell_words[at] = cell_words[at] | ((scratch[i] / 17u) << (step.z * 4u));
    } else if (which == kLight) {
        light_cell(i);
    } else if (which == kLightMerge) {
        light_merge(i);
    } else if (which == kLightGather) {
        light_gather(i);
    } else if (which == kIntegrate) {
        integrate(i);
    } else if (which == kProbeStore) {
        probe_store(i);
    } else if (which == kScrollCopy) {
        scratch[i] = scroll_word(cascade_at(step.y), i);
    } else if (which == kScrollMove) {
        scroll_move(i);
    }
}
