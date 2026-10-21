// Turns a particle system into quads in the frame's vertex buffer, a thread a quad, six vertices
// each as slash::render::Vertex2D lays them out (20 bytes): a particle's, or a segment of its
// trail's ribbon. Its CPU twin is particle_vertices in src/particles.cpp; a change to one goes in
// the other.

#include "particles.hlsl"

cbuffer Draw : UNIFORMS {
    float4 transform_a; // ParticleDraw::transform[0..3]
    float4 transform_b; // [4], [5], then the quad's size
    float4 modulate;
    float4 look;  // remainder, hframes, vframes, anim_loop
    uint4 counts; // amount, order, lifetime_split, the first vertex
    uint4 trails; // particles a trail, subdivisions, trail_steps, step_delta's bits
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

uint rgba_of(float4 color) {
    color = color * modulate;
    return to_byte(color.r) | (to_byte(color.g) << 8u) | (to_byte(color.b) << 16u) |
           (to_byte(color.a) << 24u);
}

void store_vertex(uint address, float2 position, float2 uv, uint rgba) {
    vertices.Store4(address,
                    uint4(asuint(position.x), asuint(position.y), asuint(uv.x), asuint(uv.y)));
    vertices.Store(address + 16u, rgba);
}

void store_none(uint address) {
    for (uint i = 0u; i < 6u; ++i) {
        store_vertex(address + (i * kVertexBytes), float2(0.0, 0.0), float2(0.0, 0.0), 0u);
    }
}

float2 transform_point(float2 p) {
    return float2((transform_a.x * p.x) + (transform_a.z * p.y) + transform_b.x,
                  (transform_a.y * p.x) + (transform_a.w * p.y) + transform_b.y);
}

// Bone b of the ribbon of the particle at index: the particle itself, then its copies.
uint bone_index(uint index, uint b) {
    uint sections = trails.x - 1u;
    return b == 0u ? index : counts.x + (index * sections) + b - 1u;
}

// The first and last bone ring r blends, the same when it takes one.
uint2 ring_bones(uint r) {
    uint sections = trails.x - 1u;
    uint b = r / trails.y;
    return uint2(b, (r % trails.y) != 0u ? min(sections, b + 1u) : b);
}

float2 place(uint index, uint b, float side, float half_width) {
    Particle c = particles[bone_index(index, b)];
    float behind = float((b * trails.z) % trails.x) / float(trails.x) * asfloat(trails.w);
    float2 at = c.origin_velocity.xy - (c.origin_velocity.zw * behind) +
                (c.origin_velocity.zw * look.x);
    return at + (c.basis.xy * (side * half_width));
}

void store_ring_vertex(uint address, uint index, uint r, float side, float u, float v0, float v,
                       float half_width) {
    uint2 bones = ring_bones(r);
    float blend = 1.0 - (float(r % trails.y) / float(trails.y));
    float2 spot = place(index, bones.x, side, half_width) * blend;
    float4 color = particles[bone_index(index, bones.x)].color * blend;
    if (bones.x != bones.y) {
        spot = spot + (place(index, bones.y, side, half_width) * (1.0 - blend));
        color = color + (particles[bone_index(index, bones.y)].color * (1.0 - blend));
    }
    uint segments = (trails.x - 1u) * trails.y;
    float tv = v0 + ((float(r) / float(segments)) / v);
    store_vertex(address, transform_point(spot), float2(u, tv), rgba_of(color));
}

[numthreads(64, 1, 1)]
void cs_main(uint3 id : SV_DispatchThreadID) {
    uint amount = counts.x;
    uint segments = trails.x > 1u ? (trails.x - 1u) * trails.y : 1u;
    uint slot = id.x / segments;
    uint segment = id.x % segments;
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
    uint address = (counts.w + (id.x * 6u)) * kVertexBytes;
    if (trails.x <= 1u && (particle.flags.x & kParticleActive) == 0u) {
        store_none(address);
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
    float2 half_size = transform_b.zw * 0.5;
    if (trails.x > 1u) {
        // A segment of Godot's trail ribbon, between rings segment and segment + 1, drawn while
        // the particles it bends with are live: the copies run on after the head has gone.
        uint last = ring_bones(segment + 1u).y;
        for (uint b = ring_bones(segment).x; b <= last; ++b) {
            if ((particles[bone_index(index, b)].flags.x & (kParticleActive | kParticleTrailed)) ==
                0u) {
                store_none(address);
                return;
            }
        }
        store_ring_vertex(address, index, segment, -1.0, u0, v0, v, half_size.x);
        store_ring_vertex(address + kVertexBytes, index, segment, 1.0, u1, v0, v, half_size.x);
        store_ring_vertex(address + (2u * kVertexBytes), index, segment + 1u, -1.0, u0, v0, v,
                          half_size.x);
        store_ring_vertex(address + (3u * kVertexBytes), index, segment, 1.0, u1, v0, v,
                          half_size.x);
        store_ring_vertex(address + (4u * kVertexBytes), index, segment + 1u, 1.0, u1, v0, v,
                          half_size.x);
        store_ring_vertex(address + (5u * kVertexBytes), index, segment + 1u, -1.0, u0, v0, v,
                          half_size.x);
        return;
    }
    uint rgba = rgba_of(particle.color);
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
