#pragma once

// Virtualized geometry, a research feature (docs/reference/virtualized-geometry.md): a surface cut
// into clusters of up to 128 triangles, grouped and simplified again and again into a hierarchy
// (a directed acyclic graph of cluster groups, as Brian Karis describes Nanite in "Nanite: A Deep
// Dive", SIGGRAPH 2021), built with meshoptimizer's cluster LOD builder (its demo/clusterlod.h).
// A view then draws, from each part of the surface, the coarsest clusters whose error it sees as
// under a pixel, so detail follows distance across one surface rather than switching whole LODs.

#include "slash/core/math.hpp"
#include "slash/scene3d/model.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace slash::scene3d {

// A sphere around some triangles, in the surface's space, and how far (at most) they stray from
// the surface's full triangles there.
struct ClusterBounds {
    Vec3f center;
    float radius = 0.0F;
    float error = 0.0F;
};

// Up to ClusterSettings::max_triangles triangles over the surface's vertices.
struct Cluster {
    std::vector<std::uint32_t> indices;
    // The cluster's own sphere, for culling.
    ClusterBounds bounds;
    // The group whose clusters were simplified into this one (its error says when this one is
    // fine enough), or -1 for a cluster of the surface's own triangles.
    int refined = -1;
    // The group this cluster is in (its simplified error says when this one is too fine).
    int group = -1;
    // How many simplifications made it: 0 for the surface's own triangles.
    int depth = 0;
};

// Clusters simplified together. simplified is the sphere and error of the clusters made from
// them; its error is the largest float for a group never simplified (the coarsest).
struct ClusterGroup {
    ClusterBounds simplified;
    int depth = 0;
    std::vector<std::uint32_t> clusters;
};

struct ClusterHierarchy {
    std::vector<Cluster> clusters;
    std::vector<ClusterGroup> groups;
    std::size_t source_triangles = 0;
    int levels = 0; // the deepest cluster's depth plus 1
};

struct ClusterSettings {
    std::size_t max_triangles = 128;
    std::size_t max_vertices = 128;
};

// Builds the hierarchy of a surface's triangles. Positions decide it; normals weigh the
// simplification so shading changes little. The same surface gives the same hierarchy everywhere.
[[nodiscard]] ClusterHierarchy build_cluster_hierarchy(const ModelSurface& surface,
                                                       const ClusterSettings& settings = {});

// Where a surface is seen from: the camera's place in the surface's space, the projection's
// cot(fov_y / 2), how many pixels tall the view is, the near plane's distance and how many pixels
// of error to allow. scale is the surface's largest scale in the world, which errors grow by.
struct ClusterView {
    Vec3f camera;
    float projection = 1.0F;
    float height = 1080.0F;
    float near = 0.05F;
    float threshold = 1.0F;
    float scale = 1.0F;
};

// How many pixels an error over a sphere covers from the view: the error over the distance to the
// sphere's nearest point (at least the near plane), as the projection makes it, in pixels.
[[nodiscard]] float projected_error(const ClusterBounds& bounds, const ClusterView& view);

// The clusters to draw: each cluster whose group's simplified error the view sees as over the
// threshold (so the coarser clusters made from it would show), and whose refined group's is at or
// under it (so it is coarse enough), or that is of the surface's own triangles. Errors only grow
// down the hierarchy, so the clusters chosen cover the surface once, with no cracks where
// neighbouring parts are drawn at different depths. In the hierarchy's order.
[[nodiscard]] std::vector<std::uint32_t> select_clusters(const ClusterHierarchy& hierarchy,
                                                         const ClusterView& view);

// The triangles of some clusters as one index list over the surface's vertices.
[[nodiscard]] std::vector<std::uint32_t>
cluster_indices(const ClusterHierarchy& hierarchy, const std::vector<std::uint32_t>& clusters);

} // namespace slash::scene3d
