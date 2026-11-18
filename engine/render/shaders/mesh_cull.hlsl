// Culls a frame's mesh draws, as Unity's GPU-driven pipeline does before its indirect draws, and
// draws runs of them (one mesh with one material, in the camera's order) as instances of one
// indexed indirect draw, as Godot's Forward+ draws repeated elements. Its first pass (counts.w 0)
// tests a draw a thread: whether any corner of its box is in the camera's view. Its second
// (counts.w 1) lists a run a thread the run's draws in view, in order, for mesh_instanced.hlsl
// to read, and writes the run's arguments: its indices, as many instances as draws in view.
// mesh_cull.cpp's box_in_view and cull_runs are its CPU twin; keep them in step.

#ifdef SLASH_MSL
#define UNIFORMS register(b0)
#define RECORDS register(t1)
#define OUT register(u2)
#else
#define UNIFORMS register(b0, space2)
#define RECORDS register(t0, space0)
#define OUT register(u0, space1)
#endif

cbuffer Params : UNIFORMS {
    uint4 counts; // the draws, the runs, the run's draws in all, and the pass
};

// Each draw's record: its box placed in clip space (16 words, column-major), its low and high
// corners, and its indices' first and count; 24 words. A box turned inside out is never culled.
// Then each run's first place in the order and its draws, 2 words, then the order: a draw's
// index at each place.
StructuredBuffer<uint> records : RECORDS;
// Each run's indices, instances, first index, vertex offset and first instance (5 words), then
// a word a draw that is 1 where it is in view, then the run's draws in view from each run's first
// place.
RWStructuredBuffer<uint> outputs : OUT;

static const uint kRecordWords = 24u;
static const uint kArgsWords = 5u;
// mesh_cull.hpp's kCullMargin.
static const float kMargin = 1.0 / 64.0;

float word(uint at) {
    return asfloat(records[at]);
}

// Whether a draw's box may show: false only where every corner is past one edge of the view.
bool in_view(uint draw) {
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
    return shown || !(left || right || down || up || near_edge || far_edge);
}

[numthreads(64, 1, 1)] void cs_main(uint3 id : SV_DispatchThreadID) {
    uint draws = counts.x;
    uint runs = counts.y;
    uint flags = runs * kArgsWords;
    if (counts.w == 0u) {
        if (id.x < draws) {
            outputs[flags + id.x] = in_view(id.x) ? 1u : 0u;
        }
        return;
    }
    uint run = id.x;
    if (run >= runs) {
        return;
    }
    uint runs_at = draws * kRecordWords;
    uint order_at = runs_at + runs * 2u;
    uint list = flags + draws;
    uint first = records[runs_at + run * 2u];
    uint count = records[runs_at + run * 2u + 1u];
    uint shown = 0u;
    // No break or early return in the loop: Tint's SPIR-V reader can't take them.
    for (uint i = 0u; i < count; ++i) {
        uint draw = records[order_at + first + i];
        uint flag = outputs[flags + draw];
        if (flag != 0u) {
            outputs[list + first + shown] = draw;
        }
        shown += flag;
    }
    uint lead = records[order_at + first] * kRecordWords;
    uint out_at = run * kArgsWords;
    outputs[out_at] = records[lead + 23u];
    outputs[out_at + 1u] = shown;
    outputs[out_at + 2u] = records[lead + 22u];
    outputs[out_at + 3u] = 0u;
    outputs[out_at + 4u] = 0u;
}
