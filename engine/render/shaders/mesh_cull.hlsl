// Culls a frame's mesh draws, as Unity's GPU-driven pipeline does before its indirect draws: a
// thread a draw tests the draw's box against the camera's view and writes its indexed indirect
// arguments, with no instances where every corner of the box is past one edge of the view.
// mesh_cull.cpp's box_in_view and cull_draws are its CPU twin; keep them in step.

#ifdef SLASH_MSL
#define UNIFORMS register(b0)
#define RECORDS register(t1)
#define ARGS register(u2)
#else
#define UNIFORMS register(b0, space2)
#define RECORDS register(t0, space0)
#define ARGS register(u0, space1)
#endif

cbuffer Params : UNIFORMS {
    uint4 counts; // the draws
};

// A draw's box placed in clip space (16 words, column-major), its low and high corners, and its
// indices' first and count; 24 words. A box turned inside out is never culled.
StructuredBuffer<uint> records : RECORDS;
// A draw's indices, instances, first index, vertex offset and first instance; 5 words.
RWStructuredBuffer<uint> args : ARGS;

static const uint kRecordWords = 24u;
static const uint kArgsWords = 5u;
// mesh_cull.hpp's kCullMargin.
static const float kMargin = 1.0 / 64.0;

float word(uint at) {
    return asfloat(records[at]);
}

[numthreads(64, 1, 1)] void cs_main(uint3 id : SV_DispatchThreadID) {
    uint draw = id.x;
    if (draw >= counts.x) {
        return;
    }
    uint at = draw * kRecordWords;
    float3 low = float3(word(at + 16u), word(at + 17u), word(at + 18u));
    float3 high = float3(word(at + 19u), word(at + 20u), word(at + 21u));
    bool shown = low.x > high.x || low.y > high.y || low.z > high.z;
    // Whether every corner is past each edge: left, right, down, up, near (reversed depth puts
    // it at z = w) and far (z = 0).
    bool left = true;
    bool right = true;
    bool down = true;
    bool up = true;
    bool near_edge = true;
    bool far_edge = true;
    [unroll] for (uint corner = 0u; corner < 8u; ++corner) {
        float x = (corner & 1u) != 0u ? high.x : low.x;
        float y = (corner & 2u) != 0u ? high.y : low.y;
        float z = (corner & 4u) != 0u ? high.z : low.z;
        float4 c;
        c.x = word(at + 0u) * x + word(at + 4u) * y + word(at + 8u) * z + word(at + 12u);
        c.y = word(at + 1u) * x + word(at + 5u) * y + word(at + 9u) * z + word(at + 13u);
        c.z = word(at + 2u) * x + word(at + 6u) * y + word(at + 10u) * z + word(at + 14u);
        c.w = word(at + 3u) * x + word(at + 7u) * y + word(at + 11u) * z + word(at + 15u);
        float margin = kMargin * abs(c.w);
        left = left && c.x < -c.w - margin;
        right = right && c.x > c.w + margin;
        down = down && c.y < -c.w - margin;
        up = up && c.y > c.w + margin;
        near_edge = near_edge && c.z > c.w + margin;
        far_edge = far_edge && c.z < -margin;
    }
    shown = shown || !(left || right || down || up || near_edge || far_edge);
    uint out_at = draw * kArgsWords;
    args[out_at] = records[at + 23u];
    args[out_at + 1u] = shown ? 1u : 0u;
    args[out_at + 2u] = records[at + 22u];
    args[out_at + 3u] = 0u;
    args[out_at + 4u] = 0u;
}
