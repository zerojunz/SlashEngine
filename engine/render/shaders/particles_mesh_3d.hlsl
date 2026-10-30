// Places a draw pass's mesh at each particle of a 3D system, into the vertex buffer of the mesh
// that draws them: a thread a vertex written, each as slash::render::Vertex3D lays it out (60
// bytes), as Godot's particles copy shader and BaseMaterial3D's billboard and particle animation
// code do. Its CPU twin is particle_mesh_vertices_3d in src/particles3d.cpp, which this follows
// line for line; a change to one goes in the other. The source vertices are words in data, as
// src/particle_layout_3d.cpp's pack_particle_vertices_3d lays them out, and keys are
// particles_sort_3d.hlsl's for a pass drawn by view depth.

#include "particles_3d.hlsl"

// Three buffers read, one written, so Metal's indexes follow on from the uniforms'.
#ifdef SLASH_MSL
#define READ_ONLY_DATA register(t1)
#define READ_ONLY_PARTICLES register(t2)
#define READ_ONLY_KEYS register(t3)
#define READ_WRITE_VERTICES register(u4)
#else
#define READ_ONLY_DATA register(t0, space0)
#define READ_ONLY_PARTICLES register(t1, space0)
#define READ_ONLY_KEYS register(t2, space0)
#define READ_WRITE_VERTICES register(u0, space1)
#endif

cbuffer Params : UNIFORMS {
    uint4 counts;  // amount, corners, where the vertices are in words, threads a row
    uint4 look;    // lifetime_split, align, billboard, flags
    float4 frames; // h_frames, v_frames, remainder, 0
    float4 transform[3];
    float4 camera[3];
    float4 sort_direction;
    float4 align_up;
    float4 uv; // scale, offset
};

StructuredBuffer<uint4> data : READ_ONLY_DATA;
StructuredBuffer<Particle3D> particles : READ_ONLY_PARTICLES;
StructuredBuffer<uint2> keys : READ_ONLY_KEYS;
RWByteAddressBuffer vertices : READ_WRITE_VERTICES;

static const uint kVertexWords = 15u;
// ParticleAlign and ParticleBillboard.
static const uint kAlignZBillboard = 1u;
static const uint kAlignYToVelocity = 2u;
static const uint kAlignZBillboardYToVelocity = 3u;
static const uint kBillboardDisabled = 0u;
static const uint kBillboardEnabled = 1u;
static const uint kBillboardFixedY = 2u;
static const uint kBillboardParticles = 3u;
// The flags in look.w (src/particle_layout_3d.hpp).
static const uint kKeepScale = 1u;
static const uint kAnimLoop = 2u;
static const uint kSorted = 4u;
static const uint kLifetime = 8u;
static const uint kReverseLifetime = 16u;

uint source_word(uint at) {
    uint w = counts.z + (at / 4u);
    return data[w][at % 4u];
}

float source_float(uint at) {
    return asfloat(source_word(at));
}

float3 source_float3(uint at) {
    return float3(source_float(at), source_float(at + 1u), source_float(at + 2u));
}

// The rows of a ParticleTransform as an Affine: x, y, z, then the origin.
Affine affine_of(float4 rows[3]) {
    Affine a;
    a.x = rows[0].xyz;
    a.y = float3(rows[0].w, rows[1].xy);
    a.z = float3(rows[1].zw, rows[2].x);
    a.origin = rows[2].yzw;
    return a;
}

Affine times(Affine a, Affine b) {
    Affine out_;
    out_.x = basis(a, b.x);
    out_.y = basis(a, b.y);
    out_.z = basis(a, b.z);
    out_.origin = apply(a, b.origin);
    return out_;
}

// particle_draw_index_3d: the particle a slot shows in Lifetime and ReverseLifetime order.
uint draw_index(uint slot) {
    uint total = counts.x;
    bool reverse = (look.w & kReverseLifetime) != 0u;
    if ((look.w & (kLifetime | kReverseLifetime)) == 0u) {
        return slot;
    }
    uint split = min(look.x, total - 1u);
    uint limit = total - split;
    uint particle = reverse ? total - slot - 1u : slot;
    return particle < limit ? split + particle : particle - limit;
}

void store(uint address, uint value) {
    vertices.Store(address, value);
}

void store_float3(uint address, float3 v) {
    store(address, asuint(v.x));
    store(address + 4u, asuint(v.y));
    store(address + 8u, asuint(v.z));
}

[numthreads(64, 1, 1)]
void cs_main(uint3 id : SV_DispatchThreadID) {
    uint thread = (id.y * counts.w) + id.x;
    uint corners = counts.y;
    if (thread >= counts.x * corners) {
        return;
    }
    uint slot = thread / corners;
    uint c = thread % corners;
    uint index = (look.w & kSorted) != 0u ? keys[slot].y : draw_index(slot);
    Particle3D particle = particles[index];
    uint from = c * kVertexWords;
    uint address = thread * kVertexWords * 4u;
    // Everything but the position, normal, tangent, texture coordinate and color is the source's.
    store(address + 52u, source_word(from + 13u));
    store(address + 56u, source_word(from + 14u));
    if ((particle.flags & kParticleActive) == 0u) {
        // Folded onto one point, as Godot's zero scale.
        store_float3(address, float3(0.0, 0.0, 0.0));
        for (uint w = 3u; w < 13u; ++w) {
            store(address + (w * 4u), source_word(from + w));
        }
        return;
    }
    Affine place;
    place.x = particle.x.xyz;
    place.y = particle.y.xyz;
    place.z = particle.z.xyz;
    place.origin = particle.origin.xyz;
    float3 v = particle.velocity;
    float3 sort = sort_direction.xyz;
    float3 up = align_up.xyz;
    uint align = look.y;
    if (align == kAlignZBillboard) {
        Affine local;
        local.x = unit(cross(up, sort));
        local.y = up;
        local.z = sort;
        local.origin = float3(0.0, 0.0, 0.0);
        place.x = basis(local, place.x);
        place.y = basis(local, place.y);
        place.z = basis(local, place.z);
    } else if (align == kAlignYToVelocity) {
        float s = (length_of(place.x) + length_of(place.y) + length_of(place.z)) / 3.0;
        place.y = unit(length_of(v) > 0.0 ? v : place.y);
        place.x = unit(cross(place.y, place.z));
        place.z = float3(0.0, 0.0, 1.0) * s;
        place.x = place.x * s;
        place.y = place.y * s;
    } else if (align == kAlignZBillboardYToVelocity) {
        float3 screen = v - (sort * dot(sort, v));
        if (length_of(screen) == 0.0) {
            screen = up;
        }
        screen = unit(screen);
        float3 x = unit(cross(screen, sort)) * length_of(place.x);
        float3 y = screen * length_of(place.y);
        place.z = sort * length_of(place.z);
        place.x = x;
        place.y = y;
    }
    place.origin = place.origin + (v * frames.z);
    Affine model = times(affine_of(transform), place);
    // BaseMaterial3D's billboard: the camera's basis in place of the model's.
    Affine placed = model;
    uint billboard = look.z;
    if (billboard != kBillboardDisabled) {
        Affine view = affine_of(camera);
        if (billboard == kBillboardEnabled) {
            placed.x = view.x;
            placed.y = view.y;
            placed.z = view.z;
        } else if (billboard == kBillboardFixedY) {
            placed.x = unit(cross(float3(0.0, 1.0, 0.0), view.z));
            placed.y = float3(0.0, 1.0, 0.0);
            placed.z = unit(cross(view.x, float3(0.0, 1.0, 0.0)));
        } else {
            float3 cx = unit(view.x);
            float3 cy = unit(view.y);
            float cs_x = cos(particle.custom.x);
            float cs_y = sin(particle.custom.x);
            placed.x = (cx * cs_x) - (cy * cs_y);
            placed.y = (cx * cs_y) + (cy * cs_x);
            placed.z = unit(view.z);
        }
        if ((look.w & kKeepScale) != 0u) {
            placed.x = placed.x * length_of(model.x);
            placed.y = placed.y * length_of(model.y);
            placed.z = placed.z * length_of(model.z);
        }
    }
    // Normals turn with the basis, as Godot's MODELVIEW_NORMAL_MATRIX: as it is for a billboard,
    // by its inverse's transpose otherwise.
    Affine turned = inverse(placed);
    float3 n = source_float3(from + 3u);
    float3 normal = billboard != kBillboardDisabled
                        ? unit(basis(placed, n))
                        : unit(float3(dot(turned.x, n), dot(turned.y, n), dot(turned.z, n)));
    float4 tint = clamp(particle.color, 0.0, 1.0);
    // The frame a Particles billboard shows, as Godot's particle animation.
    float h = frames.x;
    float vf = frames.y;
    float frame = 0.0;
    bool animated = billboard == kBillboardParticles;
    if (animated) {
        float count = h * vf;
        frame = floor(particle.custom.z * count);
        if ((look.w & kAnimLoop) == 0u) {
            frame = clamp(frame, 0.0, count - 1.0);
        } else {
            frame -= count * floor(frame / count);
        }
    }
    float column = frame - (h * floor(frame / h));
    float row = floor((frame + 0.5) / h);

    store_float3(address, apply(placed, source_float3(from)));
    store_float3(address + 12u, normal);
    float3 tangent = unit(basis(placed, source_float3(from + 6u)));
    store_float3(address + 24u, tangent);
    store(address + 36u, source_word(from + 9u));
    float u = (source_float(from + 10u) * uv.x) + uv.z;
    float w = (source_float(from + 11u) * uv.y) + uv.w;
    if (animated) {
        u = (u / h) + (column / h);
        w = (w / vf) + (row / vf);
    }
    store(address + 40u, asuint(u));
    store(address + 44u, asuint(w));
    uint rgba = source_word(from + 12u);
    uint packed = 0u;
    for (uint k = 0u; k < 4u; ++k) {
        float channel = float((rgba >> (k * 8u)) & 0xFFu) * tint[k];
        packed |= uint(floor(channel + 0.5)) << (k * 8u);
    }
    store(address + 48u, packed);
}
