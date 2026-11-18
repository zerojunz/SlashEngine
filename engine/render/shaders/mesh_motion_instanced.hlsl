// mesh_motion.hlsl's vertex stage for runs of draws drawn as instances, as mesh_instanced.hlsl
// is mesh.hlsl's: the prepass of a frame with motion vectors. Only its vertex stage is kept.
#define INSTANCED
#define NORMAL_ROUGHNESS
#define MOTION_VECTORS
#include "mesh.hlsl"
