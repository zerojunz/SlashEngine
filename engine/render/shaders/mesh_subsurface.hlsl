// The 3D shader's fragment stage for the clustered renderer's subsurface pass, the diffuse buffer:
// mesh_clustered.hlsl, writing each subsurface pixel's diffuse light (its emission, diffuse and
// ambient light, fogged) and its strength (below 0 for skin) for subsurface.hlsl to blur. The
// opaque meshes with a subsurface strength draw it again where they drew, after the opaque pass.
#define CLUSTERED
#define SUBSURFACE
#include "mesh.hlsl"
