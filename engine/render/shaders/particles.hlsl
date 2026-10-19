// What particles_process.hlsl and particles_draw.hlsl share: the particle, as
// slash::render::Particle lays it out, and where SDL binds each kind of resource.

// One particle (96 bytes): basis is its x axis [x, y] then its y axis [z, w]; origin_velocity
// its origin [x, y] and velocity [z, w]; custom Godot's CUSTOM and userdata its USERDATA1.
struct Particle {
    float4 basis;
    float4 origin_velocity;
    float4 color;
    float4 custom;
    float4 userdata;
    uint4 flags; // .x; the rest is padding
};

static const uint kParticleActive = 1u;
static const uint kParticleStarted = 2u;
static const uint kParticleCycleShift = 16u;

// SDL takes a compute shader's uniforms in space 2, what it reads in space 0 and writes in space
// 1. Metal has one list of buffers, uniforms first, so its SPIR-V is compiled with SLASH_MSL and
// the registers in that order.
#ifdef SLASH_MSL
#define UNIFORMS register(b0)
#define READ_ONLY register(t1)
#define READ_WRITE register(u2)
#else
#define UNIFORMS register(b0, space2)
#define READ_ONLY register(t0, space0)
#define READ_WRITE register(u0, space1)
#endif
