// Parts of this file are ported from Godot Engine (MIT); see THIRD_PARTY.md.
// SDFGI's voxelization, which rasterizes the region's geometry into its cells: a dispatch a draw, a
// thread for each of its triangles' z slices, summing into each cell the tri overlaps what its
// surface is at the point nearest the cell's center, in fixed point with atomic adds so the sums
// are the same in any order. voxelize in src/sdfgi.cpp is its CPU twin; sdfgi.hlsl's resolve pass
// turns the sums into cells. Keep them in step.

#ifdef SLASH_MSL
#define UNIFORMS register(b0)
#define VERTICES register(t1)
#define INDICES register(t2)
#define SUMS register(u3)
#else
#define UNIFORMS register(b0, space2)
#define VERTICES register(t2, space0)
#define INDICES register(t3, space0)
#define SUMS register(u0, space1)
#endif

cbuffer Params : UNIFORMS {
    float4 model_rows[3];  // the draw's model matrix, its top three rows
    float4 normal_rows[3]; // its normals' matrix, likewise
    float4 albedo;         // MeshSurfaceUniforms' albedo, emission and options
    float4 emission;
    uint4 options;
    float4 uv1;   // the draw's texture coordinates' scale, then offset
    float4 place; // the cascade's cell 0 in the cells' world, then 1 over its cell size
    uint4 work;   // the cells a side, the draw's first index, its triangles, the y scale's bits
    int4 shift;   // how far the cascade moved, in cells; w 1 when it keeps the cells it holds
};

Texture2D<float4> albedo_map : register(t0, space0);
Texture2D<float4> emission_map : register(t1, space0);
SamplerState albedo_sampler : register(s0, space0);
SamplerState emission_sampler : register(s1, space0);

// Vertex3D: position, normal, tangent (4), uv, rgba8, uv2; 15 words.
StructuredBuffer<uint> vertices : VERTICES;
StructuredBuffer<uint> indices : INDICES;
// 8 words a cell: albedo in 255ths (3), emission in 256ths (3), the triangles, the sides faced.
RWStructuredBuffer<uint> sums : SUMS;

static const uint kVertexWords = 15u;
static const float kCellEdge = 1.0 / 1024.0;
// MeshSurfaceUniforms' options[0] flags (mesh_shading.hpp).
static const uint kMeshUnshaded = 1u;
static const uint kMeshVertexColorAlbedo = 2u;
static const uint kMeshAlphaScissor = 4u;
static const uint kMeshEmissionTexture = 8u;
static const uint kMeshHasAlpha = 64u;
static const uint kMeshAlphaHash = 1024u;
static const uint kMeshTerrain = 1u; // options[3]

float3 vertex_float3(uint v, uint word) {
    uint at = v * kVertexWords + word;
    return float3(asfloat(vertices[at]), asfloat(vertices[at + 1u]), asfloat(vertices[at + 2u]));
}

float3 srgb_to_linear(float3 c) {
    return lerp(pow((c + 0.055) * (1.0 / 1.055), 2.4), c * (1.0 / 12.92), float3(c < 0.04045));
}

// triangle_overlaps_cell: whether a tri (relative to a cell's box's center) overlaps the
// box, by the separating axis test.
bool overlaps_cell(float3 v0, float3 v1, float3 v2) {
    const float h = 0.5 - (kCellEdge * 0.5);
    float3 lo = min(v0, min(v1, v2));
    float3 hi = max(v0, max(v1, v2));
    if (any(lo > float3(h, h, h)) || any(hi < float3(-h, -h, -h))) {
        return false;
    }
    float3 e[3] = {v1 - v0, v2 - v1, v0 - v2};
    float3 n = cross(e[0], e[1]);
    float r = h * (abs(n.x) + abs(n.y) + abs(n.z));
    if (abs(dot(n, v0)) > r) {
        return false;
    }
    [unroll] for (uint i = 0u; i < 3u; ++i) {
        [unroll] for (uint axis = 0u; axis < 3u; ++axis) {
            float3 unit = float3(axis == 0u ? 1.0 : 0.0, axis == 1u ? 1.0 : 0.0,
                                 axis == 2u ? 1.0 : 0.0);
            float3 a = cross(unit, e[i]);
            if (dot(a, a) == 0.0) {
                continue;
            }
            float p0 = dot(a, v0);
            float p1 = dot(a, v1);
            float p2 = dot(a, v2);
            float radius = h * (abs(a.x) + abs(a.y) + abs(a.z));
            if (min(p0, min(p1, p2)) > radius || max(p0, max(p1, p2)) < -radius) {
                return false;
            }
        }
    }
    return true;
}

// nearest_on_triangle: the point of a triangle nearest p, as the second and third corners'
// shares.
float2 nearest_on_triangle(float3 p, float3 a, float3 b, float3 c) {
    float3 ab = b - a;
    float3 ac = c - a;
    float3 ap = p - a;
    float d1 = dot(ab, ap);
    float d2 = dot(ac, ap);
    if (d1 <= 0.0 && d2 <= 0.0) {
        return float2(0.0, 0.0);
    }
    float3 bp = p - b;
    float d3 = dot(ab, bp);
    float d4 = dot(ac, bp);
    if (d3 >= 0.0 && d4 <= d3) {
        return float2(1.0, 0.0);
    }
    float vc = (d1 * d4) - (d3 * d2);
    if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0) {
        return float2(d1 / (d1 - d3), 0.0);
    }
    float3 cp = p - c;
    float d5 = dot(ab, cp);
    float d6 = dot(ac, cp);
    if (d6 >= 0.0 && d5 <= d6) {
        return float2(0.0, 1.0);
    }
    float vb = (d5 * d2) - (d1 * d6);
    if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0) {
        return float2(0.0, d2 / (d2 - d6));
    }
    float va = (d3 * d6) - (d5 * d4);
    if (va <= 0.0 && (d4 - d3) >= 0.0 && (d5 - d6) >= 0.0) {
        float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return float2(1.0 - w, w);
    }
    float denom = 1.0 / (va + vb + vc);
    return float2(vb * denom, vc * denom);
}

uint fixed_albedo(float v) {
    return uint(floor((clamp(v, 0.0, 1.0) * 255.0) + 0.5));
}
uint fixed_emission(float v) {
    return uint(floor((clamp(v, 0.0, 255.99) * 256.0) + 0.5));
}

[numthreads(64, 1, 1)]
void cs_main(uint3 id : SV_DispatchThreadID) {
    uint size = work.x;
    uint index = id.x + id.y * 65536u;
    uint tri = index / size;
    int z = int(index % size);
    if (tri >= work.z) {
        return;
    }
    uint corner[3] = {indices[work.y + tri * 3u], indices[work.y + tri * 3u + 1u],
                      indices[work.y + tri * 3u + 2u]};
    float y_mult = asfloat(work.w);
    float3 p[3];
    float3 normal = float3(0.0, 0.0, 0.0);
    [unroll] for (uint i = 0u; i < 3u; ++i) {
        float4 local = float4(vertex_float3(corner[i], 0u), 1.0);
        float3 world =
            float3(dot(model_rows[0], local), dot(model_rows[1], local), dot(model_rows[2], local));
        world.y *= y_mult;
        p[i] = (world - place.xyz) * place.w;
        float3 n = vertex_float3(corner[i], 3u);
        normal += float3(dot(normal_rows[0].xyz, n), dot(normal_rows[1].xyz, n),
                         dot(normal_rows[2].xyz, n));
    }
    float lo_z = min(p[0].z, min(p[1].z, p[2].z));
    float hi_z = max(p[0].z, max(p[1].z, p[2].z));
    if (z < max(0, int(floor(lo_z - 0.5))) || z > min(int(size) - 1, int(floor(hi_z + 0.5)))) {
        return;
    }
    uint facing = 0u;
    [unroll] for (uint k = 0u; k < 3u; ++k) {
        facing |= normal[k] > 0.0 ? 1u << k : 0u;
        facing |= normal[k] < 0.0 ? 1u << (k + 3u) : 0u;
    }
    float3 lo = min(p[0], min(p[1], p[2]));
    float3 hi = max(p[0], max(p[1], p[2]));
    int x0 = max(0, int(floor(lo.x - 0.5)));
    int x1 = min(int(size) - 1, int(floor(hi.x + 0.5)));
    int y0 = max(0, int(floor(lo.y - 0.5)));
    int y1 = min(int(size) - 1, int(floor(hi.y + 0.5)));
    // The corners' texture coordinates and colors, for the surface at the nearest point.
    float2 uv[3];
    float4 color[3];
    [unroll] for (uint j = 0u; j < 3u; ++j) {
        uint at = corner[j] * kVertexWords;
        uv[j] = float2(asfloat(vertices[at + 10u]), asfloat(vertices[at + 11u])) * uv1.xy + uv1.zw;
        uint rgba = vertices[at + 12u];
        color[j] = float4(float(rgba & 255u), float((rgba >> 8u) & 255u),
                          float((rgba >> 16u) & 255u), float(rgba >> 24u)) *
                   (1.0 / 255.0);
    }
    uint flags = options.x;
    [loop] for (int y = y0; y <= y1; ++y) {
        [loop] for (int x = x0; x <= x1; ++x) {
            int3 cell = int3(x, y, z);
            int3 from = cell + shift.xyz;
            if (shift.w != 0 && all(from >= int3(0, 0, 0)) && all(from < int3(int(size), int(size), int(size)))) {
                continue;
            }
            float3 middle = float3(cell) + float3(0.5, 0.5, 0.5);
            float3 box = middle - float3(kCellEdge, kCellEdge, kCellEdge) * 0.5;
            if (!overlaps_cell(p[0] - box, p[1] - box, p[2] - box)) {
                continue;
            }
            float2 s = nearest_on_triangle(middle, p[0], p[1], p[2]);
            float w = 1.0 - s.x - s.y;
            float2 at_uv = uv[0] * w + uv[1] * s.x + uv[2] * s.y;
            float4 tint = color[0] * w + color[1] * s.x + color[2] * s.y;
            // surface_pixel's albedo and alpha scissor, and path_surface's emission.
            float4 c = albedo * albedo_map.SampleLevel(albedo_sampler, at_uv, 0.0);
            if ((flags & kMeshVertexColorAlbedo) != 0u) {
                c *= tint;
            }
            float alpha = (flags & kMeshHasAlpha) != 0u ? c.a : 1.0;
            if ((flags & kMeshAlphaScissor) != 0u) {
                float threshold = (flags & kMeshAlphaHash) != 0u ? 0.0 : asfloat(options.y);
                if (alpha < threshold) {
                    continue;
                }
            }
            float3 base = (options.w & kMeshTerrain) != 0u ? srgb_to_linear(albedo.rgb)
                                                            : srgb_to_linear(c.rgb);
            float3 emitted = emission.rgb;
            if ((flags & kMeshEmissionTexture) != 0u) {
                emitted += emission_map.SampleLevel(emission_sampler, at_uv, 0.0).rgb;
            }
            emitted = srgb_to_linear(emitted) * emission.w;
            bool unshaded = (flags & kMeshUnshaded) != 0u;
            float3 cell_albedo = unshaded ? float3(0.0, 0.0, 0.0) : base;
            float3 cell_emission = unshaded ? base : emitted;
            uint sum = ((uint(z) * size + uint(y)) * size + uint(x)) * 8u;
            uint ignored;
            [unroll] for (uint ch = 0u; ch < 3u; ++ch) {
                InterlockedAdd(sums[sum + ch], fixed_albedo(cell_albedo[ch]), ignored);
                InterlockedAdd(sums[sum + 3u + ch], fixed_emission(cell_emission[ch]), ignored);
            }
            InterlockedAdd(sums[sum + 6u], 1u, ignored);
            InterlockedOr(sums[sum + 7u], facing, ignored);
        }
    }
}
