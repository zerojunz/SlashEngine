// shadow.hlsl without alpha hash, which the OpenGL devices draw every shadow with but those of
// alpha-hashed meshes, as they do mesh_plain.hlsl. Only its GLSL ES is kept.
#define NO_ALPHA_HASH
#include "shadow.hlsl"
