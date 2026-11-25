// The 3D shader's fragment stage for the clustered renderer's normal-roughness prepass: mesh.hlsl,
// writing each opaque pixel's normal in the camera's space and its roughness instead of its color,
// for the screen-space effects (ssao.hlsl) to read.
#define NORMAL_ROUGHNESS
#include "mesh.hlsl"
