// The 3D shader for the clustered renderer's prepass in a frame with TAA: mesh_normal.hlsl's
// normals and roughness, and in a second target how far each opaque pixel moved since the frame
// before, which taa.hlsl reads.
#define NORMAL_ROUGHNESS
#define MOTION_VECTORS
#include "mesh.hlsl"
