// mesh.hlsl without alpha hash, as main's mesh shader was before it: the OpenGL devices draw every
// mesh but alpha-hashed ones with it. Apple's software OpenGL renderer, which macOS CI draws with,
// kept its shader compiler busy for minutes after each draw of the mesh shader with alpha hash
// in it, so only the draws that need it pay for it. Only its GLSL ES is kept.
#define NO_ALPHA_HASH
#include "mesh.hlsl"
