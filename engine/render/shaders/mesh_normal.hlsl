// The 3D shader's fragment stage for Forward+'s normal-roughness prepass, as Godot's
// MODE_RENDER_NORMAL_ROUGHNESS: mesh.hlsl, writing each opaque pixel's normal in the camera's space
// and its roughness instead of its color, for the screen-space effects (ssao.hlsl) to read.
#define NORMAL_ROUGHNESS
#include "mesh.hlsl"
