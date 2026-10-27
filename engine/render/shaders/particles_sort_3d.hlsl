// Sorts a 3D particle system's slots by view depth, as Godot's particles sort buffer, a thread a
// slot of the system's amount rounded up to a power of two. Stage 0 makes each slot's key: its
// particle's depth along sort_direction as bits that order as the floats do, then its index, and
// keys after the last particle that sort last. The other stages are a bitonic sort's steps over
// the keys, comparing slots j apart in runs of k, so the keys end in the order the CPU twin's
// stable sort gives (particle_mesh_vertices_3d in src/particles3d.cpp): farthest first, ties by
// index. The steps are src/particle_layout_3d.cpp's bitonic_steps.

#include "particles_3d.hlsl"

cbuffer Params : UNIFORMS {
    uint amount;
    uint size;
    uint j;
    uint k;
    uint stage;
    uint pad0;
    uint pad1;
    uint pad2;
    float4 sort_direction;
};

StructuredBuffer<Particle3D> particles : READ_ONLY;
RWStructuredBuffer<uint2> keys : READ_WRITE;

// A float's bits, turned so that unsigned order is the floats' order; -0 sorts as 0.
uint order_bits(float value) {
    uint bits = value == 0.0 ? 0u : asuint(value);
    return (bits & 0x80000000u) != 0u ? ~bits : (bits | 0x80000000u);
}

bool after(uint2 a, uint2 b) {
    return a.x != b.x ? a.x > b.x : a.y > b.y;
}

[numthreads(64, 1, 1)]
void cs_main(uint3 id : SV_DispatchThreadID) {
    uint slot = id.x;
    if (slot >= size) {
        return;
    }
    if (stage == 0u) {
        if (slot < amount) {
            float depth = dot(sort_direction.xyz, particles[slot].origin.xyz);
            keys[slot] = uint2(order_bits(depth), slot);
        } else {
            keys[slot] = uint2(0xFFFFFFFFu, 0xFFFFFFFFu);
        }
        return;
    }
    uint other = slot ^ j;
    if (other <= slot) {
        return;
    }
    uint2 a = keys[slot];
    uint2 b = keys[other];
    bool ascending = (slot & k) == 0u;
    if (ascending ? after(a, b) : after(b, a)) {
        keys[slot] = b;
        keys[other] = a;
    }
}
