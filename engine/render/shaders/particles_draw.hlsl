// Turns a particle system into quads in the frame's vertex buffer, a thread a particle, six
// vertices each as slash::render::Vertex2D lays them out (20 bytes). Its CPU twin is
// particle_vertices in src/particles.cpp; a change to one goes in the other.

#include "particles.hlsl"

cbuffer Draw : UNIFORMS {
    float4 transform_a; // ParticleDraw::transform[0..3]
    float4 transform_b; // [4], [5], then the quad's size
    float4 modulate;
    float4 look;  // remainder, hframes, vframes, anim_loop
    uint4 counts; // amount, order, lifetime_split, the first vertex
};

StructuredBuffer<Particle> particles : READ_ONLY;
RWByteAddressBuffer vertices : READ_WRITE;

static const uint kVertexBytes = 20u;

float mod(float x, float y) {
    return x - (y * floor(x / y));
}

uint to_byte(float channel) {
    return uint(floor((clamp(channel, 0.0, 1.0) * 255.0) + 0.5));
}

void store_vertex(uint address, float2 position, float2 uv, uint rgba) {
    vertices.Store4(address,
                    uint4(asuint(position.x), asuint(position.y), asuint(uv.x), asuint(uv.y)));
    vertices.Store(address + 16u, rgba);
}

float2 transform_point(float2 p) {
    return float2((transform_a.x * p.x) + (transform_a.z * p.y) + transform_b.x,
                  (transform_a.y * p.x) + (transform_a.w * p.y) + transform_b.y);
}

[numthreads(64, 1, 1)]
void cs_main(uint3 id : SV_DispatchThreadID) {
    uint amount = counts.x;
    uint slot = id.x;
    if (slot >= amount) {
        return;
    }
    uint index = slot;
    if (counts.y != 0u) {
        uint limit = amount - min(counts.z, amount);
        if (counts.y == 2u) {
            index = amount - index - 1u;
        }
        index = index < limit ? counts.z + index : index - limit;
    }
    Particle particle = particles[index];
    uint address = (counts.w + (slot * 6u)) * kVertexBytes;
    if ((particle.flags.x & kParticleActive) == 0u) {
        for (uint i = 0u; i < 6u; ++i) {
            store_vertex(address + (i * kVertexBytes), float2(0.0, 0.0), float2(0.0, 0.0), 0u);
        }
        return;
    }
    float h = max(look.y, 1.0);
    float v = max(look.z, 1.0);
    float total = h * v;
    float frame = floor(particle.custom.z * total);
    frame = look.w != 0.0 ? mod(frame, total) : clamp(frame, 0.0, total - 1.0);
    float u0 = mod(frame, h) / h;
    float v0 = floor((frame + 0.5) / h) / v;
    float u1 = u0 + (1.0 / h);
    float v1 = v0 + (1.0 / v);
    float4 color = particle.color * modulate;
    uint rgba = to_byte(color.r) | (to_byte(color.g) << 8u) | (to_byte(color.b) << 16u) |
                (to_byte(color.a) << 24u);
    float2 half_size = transform_b.zw * 0.5;
    float2 at = particle.origin_velocity.xy + (particle.origin_velocity.zw * look.x);
    float2 x_axis = particle.basis.xy;
    float2 y_axis = particle.basis.zw;
    float2 top_left = transform_point(at + (x_axis * (-1.0 * half_size.x)) +
                                      (y_axis * (-1.0 * half_size.y)));
    float2 top_right = transform_point(at + (x_axis * (1.0 * half_size.x)) +
                                       (y_axis * (-1.0 * half_size.y)));
    float2 bottom_left = transform_point(at + (x_axis * (-1.0 * half_size.x)) +
                                         (y_axis * (1.0 * half_size.y)));
    float2 bottom_right = transform_point(at + (x_axis * (1.0 * half_size.x)) +
                                          (y_axis * (1.0 * half_size.y)));
    store_vertex(address, top_left, float2(u0, v0), rgba);
    store_vertex(address + kVertexBytes, top_right, float2(u1, v0), rgba);
    store_vertex(address + (2u * kVertexBytes), bottom_left, float2(u0, v1), rgba);
    store_vertex(address + (3u * kVertexBytes), top_right, float2(u1, v0), rgba);
    store_vertex(address + (4u * kVertexBytes), bottom_right, float2(u1, v1), rgba);
    store_vertex(address + (5u * kVertexBytes), bottom_left, float2(u0, v1), rgba);
}
