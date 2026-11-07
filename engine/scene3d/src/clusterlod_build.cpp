// meshoptimizer's cluster LOD builder (demo/clusterlod.h in its source, MIT, by Arseny
// Kapoulkine), compiled once for src/virtual_geometry.cpp. It builds with no fused multiply-adds,
// as the rest of meshoptimizer here, so hierarchies are the same everywhere.

#include <meshoptimizer.h>

#define CLUSTERLOD_IMPLEMENTATION
#include <clusterlod.h>
