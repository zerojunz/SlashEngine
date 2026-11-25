// Parts of this file are ported from Godot Engine (MIT); see THIRD_PARTY.md.
// Skinning on the GPU, which moves a skinned surface's vertices: a thread a vertex, its rest vertex
// moved by the blend shapes at their weights, then by its joints' transforms summed by its weights,
// into the mesh's vertex buffer. scene3d's blend_vertices and skin_vertices are its CPU twins
// (src/blend_shapes.cpp, src/skeleton.cpp); keep them in step.

#ifdef SLASH_MSL
#define UNIFORMS register(b0)
#define REST register(t1)
#define WEIGHTS register(t2)
#define SHAPES register(t3)
#define POSES register(t4)
#define MOVED register(u5)
#else
#define UNIFORMS register(b0, space2)
#define REST register(t0, space0)
#define WEIGHTS register(t1, space0)
#define SHAPES register(t2, space0)
#define POSES register(t3, space0)
#define MOVED register(u0, space1)
#endif

cbuffer Params : UNIFORMS {
    // The vertices, the joints, the blend shapes, 1 when the shapes at their weights turn
    // normals or tangents.
    uint4 counts;
    // Where this mesh's joints and shape weights start in poses, 1 when the skin has weights.
    uint4 at;
};

// Vertex3D: position, normal, tangent (4), uv, rgba8, uv2; 15 words.
StructuredBuffer<uint> rest : REST;
// A vertex's eight joints, two to a word, then their eight weights; 12 words.
StructuredBuffer<uint> weights : WEIGHTS;
// Each blend shape's offsets, a vertex's position, normal and tangent; 9 words a vertex.
StructuredBuffer<uint> shapes : SHAPES;
// Each mesh's joints (x, y, z, origin; 12 words) and blend shape weights this frame.
StructuredBuffer<uint> poses : POSES;
RWStructuredBuffer<uint> moved : MOVED;

static const uint kVertexWords = 15u;
static const uint kWeightWords = 12u;
static const uint kShapeWords = 9u;
static const uint kJointWords = 12u;

float3 load3(StructuredBuffer<uint> from, uint word) {
    return float3(asfloat(from[word]), asfloat(from[word + 1u]), asfloat(from[word + 2u]));
}

void store3(uint word, float3 value) {
    moved[word] = asuint(value.x);
    moved[word + 1u] = asuint(value.y);
    moved[word + 2u] = asuint(value.z);
}

// As blend_shapes.cpp's unit and skeleton.cpp's normalized: a vector of no length stays.
float3 unit(float3 v) {
    float length = sqrt(dot(v, v));
    return length == 0.0 ? v : v / length;
}

[numthreads(64, 1, 1)] void cs_main(uint3 id : SV_DispatchThreadID) {
    uint i = id.x + id.y * 65536u;
    uint count = counts.x;
    if (i >= count) {
        return;
    }
    uint base = i * kVertexWords;
    float3 position = load3(rest, base);
    float3 normal = load3(rest, base + 3u);
    float3 tangent = load3(rest, base + 6u);
    // Blend shapes first, then the skin.
    if (counts.z > 0u) {
        float3 normals = float3(0.0, 0.0, 0.0);
        float3 tangents = float3(0.0, 0.0, 0.0);
        for (uint k = 0u; k < counts.z; ++k) {
            float w = asfloat(poses[at.y + k]);
            if (w == 0.0) {
                continue;
            }
            uint shape = (k * count + i) * kShapeWords;
            position += load3(shapes, shape) * w;
            normals += load3(shapes, shape + 3u) * w;
            tangents += load3(shapes, shape + 6u) * w;
        }
        if (counts.w != 0u) {
            normal = unit(normal + normals);
            tangent = unit(tangent + tangents);
        }
    }
    if (at.z != 0u && counts.y > 0u) {
        uint weighs = i * kWeightWords;
        float3 x = float3(0.0, 0.0, 0.0);
        float3 y = float3(0.0, 0.0, 0.0);
        float3 z = float3(0.0, 0.0, 0.0);
        float3 origin = float3(0.0, 0.0, 0.0);
        float total = 0.0;
        for (uint k = 0u; k < 8u; ++k) {
            float weight = asfloat(weights[weighs + 4u + k]);
            uint joint = (weights[weighs + k / 2u] >> ((k % 2u) * 16u)) & 0xFFFFu;
            if (weight == 0.0 || joint >= counts.y) {
                continue;
            }
            uint j = at.x + joint * kJointWords;
            x += load3(poses, j) * weight;
            y += load3(poses, j + 3u) * weight;
            z += load3(poses, j + 6u) * weight;
            origin += load3(poses, j + 9u) * weight;
            total += weight;
        }
        if (total > 0.0) {
            position = x * position.x + y * position.y + z * position.z + origin;
            normal = unit(x * normal.x + y * normal.y + z * normal.z);
            tangent = unit(x * tangent.x + y * tangent.y + z * tangent.z);
        }
    }
    store3(base, position);
    store3(base + 3u, normal);
    store3(base + 6u, tangent);
    for (uint word = 9u; word < kVertexWords; ++word) {
        moved[base + word] = rest[base + word];
    }
}
