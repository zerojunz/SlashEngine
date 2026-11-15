// Simulation stages on a 3D particle system, after Unreal's Niagara simulation stages, a thread a
// slot. Stage 0 keeps each particle's place and velocity in snapshot and writes its slot's key:
// the Morton code of its cell of side neighbor_radius (each coordinate's low 10 bits), then its
// index, particles not live and slots after the last particle sorting last.
// particles_sort_3d.hlsl's bitonic steps then sort the keys, so a cell's particles lie together.
// Stage 1 runs each live particle's stages: the nearest max_neighbors live particles within
// neighbor_radius, found with a binary search in the cell round it and its 26 neighbors (8 when
// flat), push it apart, pull it toward their middle and turn it toward their velocity; curl noise
// swirls it, drag slows it and the speed limits hold it, and the change moves it this step too.
// It reads the others from snapshot, so no thread sees another's result. Its CPU twin is
// run_particle_stages_3d in src/particles3d.cpp, which this follows line for line; a change to
// one goes in the other.

#include "particles_3d.hlsl"

// Three buffers written, so Metal's indexes follow on from the uniforms'.
#ifdef SLASH_MSL
#define READ_WRITE_PARTICLES register(u1)
#define READ_WRITE_SNAPSHOT register(u2)
#define READ_WRITE_KEYS register(u3)
#else
#define READ_WRITE_PARTICLES register(u0, space1)
#define READ_WRITE_SNAPSHOT register(u1, space1)
#define READ_WRITE_KEYS register(u2, space1)
#endif

cbuffer Params : UNIFORMS {
    uint amount;
    uint size;
    uint stage;
    uint max_neighbors;
    uint seed;
    uint flat_xy;
    uint pad0;
    uint pad1;
    float neighbor_radius;
    float separation;
    float cohesion;
    float alignment;
    float curl_strength;
    float curl_scale;
    float curl_time;
    float drag;
    float min_speed;
    float max_speed;
    float delta;
    float pad2;
};

RWStructuredBuffer<Particle3D> particles : READ_WRITE_PARTICLES;
RWStructuredBuffer<float4> snapshot : READ_WRITE_SNAPSHOT;
RWStructuredBuffer<uint2> keys : READ_WRITE_KEYS;

static const uint kMostNeighbors = 32u;

uint stage_hash(uint x) {
    x ^= x >> 16u;
    x *= 0x7FEB352Du;
    x ^= x >> 15u;
    x *= 0x846CA68Bu;
    x ^= x >> 16u;
    return x;
}

float stage_lattice(int x, int y, int z, uint noise_seed) {
    uint h = stage_hash(noise_seed ^ 0x9E3779B9u);
    h = stage_hash(h ^ uint(x));
    h = stage_hash(h ^ uint(y));
    h = stage_hash(h ^ uint(z));
    return (float(h >> 8u) * (2.0 / 16777216.0)) - 1.0;
}

float stage_fade(float t) {
    return t * t * t * ((t * ((t * 6.0) - 15.0)) + 10.0);
}

float stage_fade_slope(float t) {
    return 30.0 * t * t * ((t * (t - 2.0)) + 1.0);
}

float3 stage_noise_slope(float3 p, uint noise_seed, bool flat) {
    float fx = floor(p.x);
    float fy = floor(p.y);
    float fz = flat ? 0.0 : floor(p.z);
    int x0 = int(fx);
    int y0 = int(fy);
    int z0 = int(fz);
    float tx = p.x - fx;
    float ty = p.y - fy;
    float tz = flat ? 0.0 : p.z - fz;
    float u = stage_fade(tx);
    float v = stage_fade(ty);
    float w = stage_fade(tz);
    float du = stage_fade_slope(tx);
    float dv = stage_fade_slope(ty);
    float dw = flat ? 0.0 : stage_fade_slope(tz);
    float a = stage_lattice(x0, y0, z0, noise_seed);
    float b = stage_lattice(x0 + 1, y0, z0, noise_seed);
    float c = stage_lattice(x0, y0 + 1, z0, noise_seed);
    float d = stage_lattice(x0 + 1, y0 + 1, z0, noise_seed);
    float e = 0.0;
    float f = 0.0;
    float g = 0.0;
    float h = 0.0;
    if (!flat) {
        e = stage_lattice(x0, y0, z0 + 1, noise_seed);
        f = stage_lattice(x0 + 1, y0, z0 + 1, noise_seed);
        g = stage_lattice(x0, y0 + 1, z0 + 1, noise_seed);
        h = stage_lattice(x0 + 1, y0 + 1, z0 + 1, noise_seed);
    }
    float k1 = b - a;
    float k2 = c - a;
    float k3 = e - a;
    float k4 = a - b - c + d;
    float k5 = a - c - e + g;
    float k6 = a - b - e + f;
    float k7 = -a + b + c - d + e - f - g + h;
    return float3(du * (k1 + (k4 * v) + (k6 * w) + (k7 * v * w)),
                  dv * (k2 + (k5 * w) + (k4 * u) + (k7 * w * u)),
                  dw * (k3 + (k6 * u) + (k5 * v) + (k7 * u * v)));
}

// particle_curl_3d.
float3 stage_curl(float3 p, float time, uint noise_seed, bool flat) {
    float3 at = float3(p.x + (0.37 * time), p.y + (0.71 * time), p.z + (0.53 * time));
    if (flat) {
        float3 n = stage_noise_slope(float3(at.x, at.y, 0.0), noise_seed, true);
        return float3(n.y, -n.x, 0.0);
    }
    float3 a = stage_noise_slope(at, noise_seed, false);
    float3 b = stage_noise_slope(float3(at.x + 31.416, at.y, at.z), noise_seed + 1u, false);
    float3 c = stage_noise_slope(float3(at.x, at.y + 47.853, at.z), noise_seed + 2u, false);
    return float3(c.y - b.z, a.z - c.x, b.x - a.y);
}

// particle_cell_code_3d.
uint spread_bits(int coordinate) {
    uint v = uint(coordinate) & 0x3FFu;
    v = (v | (v << 16u)) & 0x030000FFu;
    v = (v | (v << 8u)) & 0x0300F00Fu;
    v = (v | (v << 4u)) & 0x030C30C3u;
    v = (v | (v << 2u)) & 0x09249249u;
    return v;
}

uint cell_code(int x, int y, int z) {
    return spread_bits(x) | (spread_bits(y) << 1u) | (spread_bits(z) << 2u);
}

int stage_cell(float at, float inverse) {
    return int(floor(clamp(at * inverse, -1.0e9, 1.0e9)));
}

bool stage_nearer(float d, uint i, float e, uint j) {
    return d < e || (d == e && i < j);
}

float3 flattened(float3 v, bool flat) {
    return flat ? float3(v.x, v.y, 0.0) : v;
}

[numthreads(64, 1, 1)]
void cs_main(uint3 id : SV_DispatchThreadID) {
    uint slot = id.x;
    bool flat = flat_xy != 0u;
    float radius = neighbor_radius;
    float inverse = radius > 0.0 ? 1.0 / radius : 0.0;
    if (stage == 0u) {
        if (slot >= size) {
            return;
        }
        if (slot >= amount) {
            keys[slot] = uint2(0xFFFFFFFFu, 0xFFFFFFFFu);
            return;
        }
        Particle3D particle = particles[slot];
        float3 place = flattened(particle.origin.xyz, flat);
        snapshot[slot * 2u] = float4(place, 0.0);
        snapshot[(slot * 2u) + 1u] = float4(particle.velocity, 0.0);
        uint code = 0xFFFFFFFFu;
        if ((particle.flags & kParticleActive) != 0u) {
            code = cell_code(stage_cell(place.x, inverse), stage_cell(place.y, inverse),
                             flat ? 0 : stage_cell(place.z, inverse));
        }
        keys[slot] = uint2(code, slot);
        return;
    }
    if (slot >= amount) {
        return;
    }
    Particle3D particle = particles[slot];
    if ((particle.flags & kParticleActive) == 0u) {
        return;
    }
    float dt = delta;
    float3 p = snapshot[slot * 2u].xyz;
    float3 before = snapshot[(slot * 2u) + 1u].xyz;
    float3 v = before;
    uint count = 0u;
    if (radius > 0.0) {
        float near_d[kMostNeighbors];
        uint near_i[kMostNeighbors];
        uint limit = clamp(max_neighbors, 1u, kMostNeighbors);
        int cx = stage_cell(p.x, inverse);
        int cy = stage_cell(p.y, inverse);
        int cz = flat ? 0 : stage_cell(p.z, inverse);
        int z_reach = flat ? 0 : 1;
        for (int dz = -z_reach; dz <= z_reach; ++dz) {
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    uint code = cell_code(cx + dx, cy + dy, cz + dz);
                    uint low = 0u;
                    uint high = amount;
                    while (low < high) {
                        uint middle = low + ((high - low) / 2u);
                        if (keys[middle].x < code) {
                            low = middle + 1u;
                        } else {
                            high = middle;
                        }
                    }
                    for (uint k = low; k < amount; ++k) {
                        uint2 key = keys[k];
                        if (key.x != code) {
                            break;
                        }
                        uint other = key.y;
                        if (other == slot) {
                            continue;
                        }
                        float d = length_of(p - snapshot[other * 2u].xyz);
                        if (!(d < radius)) {
                            continue;
                        }
                        uint at = count;
                        if (count < limit) {
                            count += 1u;
                        } else if (stage_nearer(d, other, near_d[limit - 1u],
                                                near_i[limit - 1u])) {
                            at = limit - 1u;
                        } else {
                            continue;
                        }
                        while (at > 0u && stage_nearer(d, other, near_d[at - 1u], near_i[at - 1u])) {
                            near_d[at] = near_d[at - 1u];
                            near_i[at] = near_i[at - 1u];
                            at -= 1u;
                        }
                        near_d[at] = d;
                        near_i[at] = other;
                    }
                }
            }
        }
        if (count > 0u) {
            float3 push = float3(0.0, 0.0, 0.0);
            float3 center = float3(0.0, 0.0, 0.0);
            float3 heading = float3(0.0, 0.0, 0.0);
            for (uint n = 0u; n < count; ++n) {
                uint other = near_i[n];
                float d = near_d[n];
                float3 q = snapshot[other * 2u].xyz;
                if (d > 1e-6) {
                    push = push + ((p - q) * ((1.0 - (d / radius)) / d));
                }
                center = center + q;
                heading = heading + snapshot[(other * 2u) + 1u].xyz;
            }
            float share = 1.0 / float(count);
            center = center * share;
            heading = heading * share;
            float3 dv = push * (separation * radius);
            dv = dv + ((center - p) * cohesion);
            dv = dv + ((heading - before) * (min(alignment * dt, 1.0) * (1.0 / dt)));
            v = v + flattened(dv * dt, flat);
        }
    }
    if (curl_strength != 0.0) {
        float scale = max(curl_scale, 0.0001);
        float3 swirl = stage_curl(p * (1.0 / scale), curl_time, seed, flat);
        v = v + (swirl * (curl_strength * dt));
    }
    if (drag > 0.0) {
        v = v * max(0.0, 1.0 - (drag * dt));
    }
    float moving = length_of(v);
    if (max_speed > 0.0 && moving > max_speed) {
        v = v * (max_speed / moving);
    } else if (min_speed > 0.0 && moving > 1e-6 && moving < min_speed) {
        v = v * (min_speed / moving);
    }
    v = flattened(v, flat);
    particle.origin = float4(particle.origin.xyz + ((v - before) * dt), 0.0);
    particle.velocity = v;
    particle.userdata.x = float(count);
    particles[slot] = particle;
}
