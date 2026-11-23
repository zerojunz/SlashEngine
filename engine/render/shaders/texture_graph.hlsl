// A texture graph's pass (slash/render/texture_graph.hpp): a thread a pixel, the pass's op over
// its inputs' slots into its output slot. run_texture_graph in src/texture_graph.cpp is its CPU
// twin; a change to an op goes in both, and the op numbers and params' layout never change.

#include "texture_graph_common.hlsl"

#ifdef SLASH_MSL
#define UNIFORMS register(b0)
#define IN0 register(t1)
#define IN1 register(t2)
#define IN2 register(t3)
#define IN3 register(t4)
#define IMAGE register(t5)
#define OUT register(u6)
#else
#define UNIFORMS register(b0, space2)
#define IN0 register(t0, space0)
#define IN1 register(t1, space0)
#define IN2 register(t2, space0)
#define IN3 register(t3, space0)
#define IMAGE register(t4, space0)
#define OUT register(u0, space1)
#endif

cbuffer Params : UNIFORMS {
    // The op, the width and height, 1 when the program tiles.
    uint4 head;
    // A bit per input the pass reads, the image's width and height.
    uint4 more;
    // The pass's 48 params.
    float4 p4[12];
};

StructuredBuffer<float4> in0 : IN0;
StructuredBuffer<float4> in1 : IN1;
StructuredBuffer<float4> in2 : IN2;
StructuredBuffer<float4> in3 : IN3;
StructuredBuffer<float4> image : IMAGE;
RWStructuredBuffer<float4> slot_out : OUT;

#define TG(name) name
#define TG_OP -1
#define TG_OP_VALUE head.x
#define TG_PARAM(tg_i) p4[(tg_i) >> 2u][(tg_i) & 3u]
#define TG_WIDTH ((int)head.y)
#define TG_HEIGHT ((int)head.z)
#define TG_TILING (head.w != 0u)
#define TG_HAS_INPUT(tg_k) ((more.x & (1u << (tg_k))) != 0u)
#define TG_INPUT(tg_k, tg_x, tg_y) read_input(tg_k, (uint)(tg_y) * head.y + (uint)(tg_x))
#define TG_IMAGE_WIDTH ((int)more.y)
#define TG_IMAGE_HEIGHT ((int)more.z)
#define TG_IMAGE(tg_x, tg_y) image[(uint)(tg_y) * more.y + (uint)(tg_x)]

float4 read_input(uint k, uint i) {
    if (k == 0u) {
        return in0[i];
    }
    if (k == 1u) {
        return in1[i];
    }
    if (k == 2u) {
        return in2[i];
    }
    return in3[i];
}

#include "texture_graph_ops.hlsl"

[numthreads(8, 8, 1)] void cs_main(uint3 id : SV_DispatchThreadID, uint index : SV_GroupIndex) {
    if (head.x == 1u) {
        fnlLoadTables(index, 64u);
    }
    GroupMemoryBarrierWithGroupSync();
    if (id.x >= head.y || id.y >= head.z) {
        return;
    }
    slot_out[id.y * head.y + id.x] = run_pixel((int)id.x, (int)id.y);
}
