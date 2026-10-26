// Builds V-HACD 4.1 (BSD-3-Clause, https://github.com/kmammou/v-hacd), the convex decomposition
// Godot's importer makes collision hulls with, so that its hulls are the same on every platform:
// it runs on one thread, cmake/patches/vhacd-deterministic.cmake gives it an order of its own
// wherever a standard library would choose one, and its one pow call goes through the engine's
// exp and ln rather than libm's, whose last bits differ between platforms.
#include "vhacd.hpp"

#include "slash/core/math.hpp"

namespace {

double vhacd_pow(double x, double y) {
    return static_cast<double>(
        slash::exp(static_cast<float>(y) * slash::ln(static_cast<float>(x))));
}

} // namespace

#define ENABLE_VHACD_IMPLEMENTATION 1 // NOLINT(cppcoreguidelines-macro-usage)
#define VHACD_DISABLE_THREADING 1     // NOLINT(cppcoreguidelines-macro-usage)
#define VHACD_POW vhacd_pow           // NOLINT(cppcoreguidelines-macro-usage)
#include <VHACD.h>

namespace slash::scene3d::detail {

namespace {

std::vector<VHACD::Vertex> vertices_of(std::span<const float> points) {
    std::vector<VHACD::Vertex> out;
    out.reserve(points.size() / 3);
    for (std::size_t i = 0; i + 2 < points.size(); i += 3) {
        out.emplace_back(points[i], points[i + 1], points[i + 2]);
    }
    return out;
}

VhacdHull hull_of(const std::vector<VHACD::Vertex>& points,
                  const std::vector<VHACD::Triangle>& triangles) {
    VhacdHull out;
    out.points.reserve(points.size() * 3);
    for (const VHACD::Vertex& point : points) {
        out.points.insert(out.points.end(), {point.mX, point.mY, point.mZ});
    }
    out.triangles.reserve(triangles.size() * 3);
    for (const VHACD::Triangle& triangle : triangles) {
        out.triangles.insert(out.triangles.end(), {triangle.mI0, triangle.mI1, triangle.mI2});
    }
    return out;
}

} // namespace

VhacdHull vhacd_hull(std::span<const float> points, std::uint32_t max_points) {
    const std::vector<VHACD::Vertex> vertices = vertices_of(points);
    if (vertices.size() < 4) {
        return {};
    }
    VHACD::QuickHull hull;
    if (hull.ComputeConvexHull(vertices, max_points == 0 ? std::uint32_t(vertices.size())
                                                         : max_points) == 0) {
        return {};
    }
    return hull_of(hull.GetVertices(), hull.GetIndices());
}

std::vector<VhacdHull> vhacd_decompose(std::span<const float> points,
                                       std::span<const std::uint32_t> triangles,
                                       const VhacdSettings& settings) {
    if (points.size() < 9 || triangles.size() < 3) {
        return {};
    }
    VHACD::IVHACD::Parameters parameters;
    parameters.m_maxConvexHulls = settings.max_hulls;
    parameters.m_resolution = settings.resolution;
    parameters.m_maxNumVerticesPerCH = settings.max_points;
    parameters.m_minimumVolumePercentErrorAllowed = settings.volume_error_percent;
    parameters.m_maxRecursionDepth = settings.max_depth;
    parameters.m_shrinkWrap = settings.shrink_wrap;
    parameters.m_asyncACD = false;
    VHACD::IVHACD* vhacd = VHACD::CreateVHACD();
    std::vector<VhacdHull> hulls;
    if (vhacd->Compute(points.data(), static_cast<std::uint32_t>(points.size() / 3),
                       triangles.data(), static_cast<std::uint32_t>(triangles.size() / 3),
                       parameters)) {
        for (std::uint32_t i = 0; i < vhacd->GetNConvexHulls(); ++i) {
            VHACD::IVHACD::ConvexHull hull;
            if (vhacd->GetConvexHull(i, hull)) {
                hulls.push_back(hull_of(hull.m_points, hull.m_triangles));
            }
        }
    }
    vhacd->Release();
    return hulls;
}

} // namespace slash::scene3d::detail
