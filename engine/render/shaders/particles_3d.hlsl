// What the 3D particle shaders share: the particle, as slash::render::Particle3D lays it out,
// particles.hlsl's flags and bindings, and the vector and transform helpers of
// src/particles3d.cpp.

#include "particles.hlsl"

// One particle (128 bytes): its transform's axes, each scaled, and its origin, their w 0; the
// velocity its forces built up and its flags; its color; Godot's CUSTOM (angle, age, frame,
// lifetime) and spare words.
struct Particle3D {
    float4 x;
    float4 y;
    float4 z;
    float4 origin;
    float3 velocity;
    uint flags;
    float4 color;
    float4 custom;
    float4 userdata;
};

// An affine transform: its axes and its origin.
struct Affine {
    float3 x;
    float3 y;
    float3 z;
    float3 origin;
};

float length_of(float3 v) {
    return sqrt(dot(v, v));
}

// v at length 1, or zero for zero, as the CPU twin's normalize.
float3 unit(float3 v) {
    float l = length_of(v);
    return l > 0.0 ? v / l : float3(0.0, 0.0, 0.0);
}

float3 basis(Affine a, float3 v) {
    return (a.x * v.x) + (a.y * v.y) + (a.z * v.z);
}

float3 apply(Affine a, float3 p) {
    return basis(a, p) + a.origin;
}

Affine inverse(Affine a) {
    float det = dot(a.x, cross(a.y, a.z));
    float inv = det != 0.0 ? 1.0 / det : 0.0;
    float3 r0 = cross(a.y, a.z) * inv;
    float3 r1 = cross(a.z, a.x) * inv;
    float3 r2 = cross(a.x, a.y) * inv;
    Affine out_;
    out_.x = float3(r0.x, r1.x, r2.x);
    out_.y = float3(r0.y, r1.y, r2.y);
    out_.z = float3(r0.z, r1.z, r2.z);
    out_.origin = basis(out_, a.origin) * -1.0;
    return out_;
}
