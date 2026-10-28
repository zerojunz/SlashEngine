#pragma once

#include <cstdint>
#include <span>
#include <vector>

// V-HACD's convex hulls and decompositions, built in src/vhacd_build.cpp, over plain arrays so
// that the build needs no engine headers but the engine's math.
namespace slash::scene3d::detail {

// A convex hull: its corners, x, y and z each, and its faces over them, wound counter-clockwise
// seen from outside, as V-HACD makes them.
struct VhacdHull {
    std::vector<double> points;
    std::vector<std::uint32_t> triangles;
};

// The convex hull of points (x, y and z each), with at most max_points corners (0 for any
// number), as V-HACD's QuickHull makes it; no faces for points that span no volume.
[[nodiscard]] VhacdHull vhacd_hull(std::span<const float> points, std::uint32_t max_points);

// How V-HACD 4 decomposes a mesh: its IVHACD::Parameters, run on one thread.
struct VhacdSettings {
    std::uint32_t max_hulls = 16;
    std::uint32_t resolution = 10000;
    std::uint32_t max_points = 32;
    double volume_error_percent = 1.0;
    std::uint32_t max_depth = 10;
    bool shrink_wrap = true;
};

// A mesh (points as for vhacd_hull, triangles three corners each) decomposed into convex hulls,
// as V-HACD 4 does; none for a mesh it can't.
[[nodiscard]] std::vector<VhacdHull> vhacd_decompose(std::span<const float> points,
                                                     std::span<const std::uint32_t> triangles,
                                                     const VhacdSettings& settings);

} // namespace slash::scene3d::detail
