// The 3D shader's fragment stage as Godot's Forward+ renderer lights it: mesh.hlsl, with each
// pixel's omni and spot lights picked from the clusters of the screen it falls in (built on the
// CPU by src/clusters.cpp), so a frame lights its meshes with many more of them. It reads storage
// buffers, which OpenGL ES 3.0 lacks, so the OpenGL devices draw only with mesh.hlsl.
#define CLUSTERED
#include "mesh.hlsl"
