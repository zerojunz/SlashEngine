// Numbers what a sub-emitter takes in a step from the particles the system before started, which
// particles_process.hlsl wrote to the emission buffer: each particle's first emission's place,
// adding at most the sub-emitter's size each and stopping there, then the header the
// sub-emitter's step reads, with the count of the particles its emission cycle took, which its
// first particle keeps. One group of threads, each adding up a run of particles. Its CPU twin is
// take_emissions in src/particles.cpp; a change to one goes in the other.

#include "particles.hlsl"

// No buffer read, two written, so Metal's indexes follow on from the uniforms'.
#ifdef SLASH_MSL
#define READ_WRITE_EMISSIONS register(u1)
#define READ_WRITE_PARTICLES register(u2)
#else
#define READ_WRITE_EMISSIONS register(u0, space1)
#define READ_WRITE_PARTICLES register(u1, space1)
#endif

cbuffer Params : UNIFORMS {
    uint parents; // particles in the system before
    uint size;    // particles in the sub-emitter
    uint tag;     // the sub-emitter's step's emission cycle, plus 1
    uint clear;   // the step starts the sub-emitter over
};

RWStructuredBuffer<uint4> emissions : READ_WRITE_EMISSIONS;
RWStructuredBuffer<Particle> particles : READ_WRITE_PARTICLES;

static const uint kThreads = 256u;
static const uint kEmissionWords = 3u;

groupshared uint sums[kThreads];
groupshared uint total;

[numthreads(256, 1, 1)]
void cs_main(uint3 id : SV_GroupThreadID) {
    uint thread = id.x;
    uint run = (parents + kThreads - 1u) / kThreads;
    uint first = min(thread * run, parents);
    uint end = min(first + run, parents);
    uint sum = 0u;
    for (uint i = first; i < end; ++i) {
        sum = min(sum + min(emissions[1u + (i * kEmissionWords)].x, size), size);
    }
    sums[thread] = sum;
    GroupMemoryBarrierWithGroupSync();
    if (thread == 0u) {
        // Where each run starts.
        uint running = 0u;
        for (uint t = 0u; t < kThreads; ++t) {
            uint added = sums[t];
            sums[t] = running;
            running = min(running + added, size);
        }
        total = running;
    }
    GroupMemoryBarrierWithGroupSync();
    uint offset = sums[thread];
    for (uint j = first; j < end; ++j) {
        uint at = 1u + (j * kEmissionWords);
        uint4 head = emissions[at];
        head.y = offset;
        emissions[at] = head;
        offset = min(offset + min(head.x, size), size);
    }
    if (thread == 0u) {
        uint4 flags = particles[0].flags;
        uint base = (clear != 0u || flags.z != tag) ? 0u : flags.y;
        particles[0].flags = uint4(flags.x, (base + total) % size, tag, flags.w);
        emissions[0] = uint4(base, total, (tag - 1u) % 2u, parents);
    }
}
