// mesh.hlsl's vertex stage for runs of draws of one mesh with one material, drawn as instances
// of one indirect draw: each instance reads its draw's Instance from a storage buffer, picked by
// the list mesh_cull.hlsl writes of the run's draws in view. The GPU and WebGPU devices draw the
// camera's passes with it, with mesh.hlsl's fragment stages; only its vertex stage is kept.
#define INSTANCED
#include "mesh.hlsl"
