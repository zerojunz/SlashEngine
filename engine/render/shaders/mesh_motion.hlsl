// The 3D shader for Forward+'s prepass in a frame with TAA, as Godot's
// MODE_RENDER_NORMAL_ROUGHNESS with motion vectors: mesh_normal.hlsl's normals and roughness, and
// in a second target how far each opaque pixel moved since the frame before, which taa.hlsl reads.
#define NORMAL_ROUGHNESS
#define MOTION_VECTORS
#include "mesh.hlsl"
