// Virtualized geometry's cluster hierarchies (slash/scene3d/virtual_geometry.hpp), built with
// meshoptimizer's cluster LOD builder and cut for a view as its demo/nanite.cpp cuts them.

#include "slash/scene3d/virtual_geometry.hpp"

#include <algorithm>
#include <array>
#include <clusterlod.h>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <meshoptimizer.h>
#include <utility>
#include <vector>

namespace slash::scene3d {

namespace {

// How much normals weigh in the simplification, against positions (in the surface's units, which
// the simplifier scales to the mesh's size), as in meshoptimizer's demo.
constexpr float kNormalWeight = 0.5F;

ClusterBounds bounds_of(const clodBounds& bounds) {
    return {.center = {bounds.center[0], bounds.center[1], bounds.center[2]},
            .radius = bounds.radius,
            .error = bounds.error};
}

// Which corners lie on the surface's open edges (edges of one triangle, joining corners by
// place), locked so the surface's rim stays where it is at every depth, as Godot's importer locks
// the border when it makes LODs.
std::vector<unsigned char> open_edge_locks(const ModelSurface& surface,
                                           const std::vector<float>& positions) {
    std::vector<unsigned int> remap(surface.vertices.size());
    meshopt_generatePositionRemap(remap.data(), positions.data(), surface.vertices.size(),
                                  sizeof(float) * 3);
    std::map<std::pair<unsigned int, unsigned int>, int> edges;
    for (std::size_t t = 0; t + 2 < surface.indices.size(); t += 3) {
        for (std::size_t e = 0; e < 3; ++e) {
            unsigned int a = remap[surface.indices[t + e]];
            unsigned int b = remap[surface.indices[t + ((e + 1) % 3)]];
            if (b < a) {
                std::swap(a, b);
            }
            ++edges[{a, b}];
        }
    }
    std::vector<unsigned char> on_rim(surface.vertices.size(), 0);
    for (const auto& [edge, uses] : edges) {
        if (uses == 1) {
            on_rim[edge.first] = 1;
            on_rim[edge.second] = 1;
        }
    }
    std::vector<unsigned char> locks(surface.vertices.size(), 0);
    for (std::size_t i = 0; i < locks.size(); ++i) {
        locks[i] = on_rim[remap[i]] != 0 ? meshopt_SimplifyVertex_Lock : 0;
    }
    return locks;
}

// Takes out the pairs of triangles over the same three corners wound opposite ways that the
// simplifier can leave on a jagged surface: a fold with no area, whose edges each such pair uses
// once each way. Pairs are looked for among the clusters one simplification made (those of one
// refined group), as they are drawn together.
void drop_folds(ClusterHierarchy& hierarchy) {
    std::map<int, std::vector<std::uint32_t>> made_by;
    for (std::size_t c = 0; c < hierarchy.clusters.size(); ++c) {
        if (hierarchy.clusters[c].refined >= 0) {
            made_by[hierarchy.clusters[c].refined].push_back(static_cast<std::uint32_t>(c));
        }
    }
    for (const auto& [refined, clusters] : made_by) {
        // Each triangle by its corners in order, with which way it winds.
        std::map<std::array<std::uint32_t, 3>, std::vector<std::pair<std::uint32_t, std::size_t>>>
            wound[2];
        for (const std::uint32_t c : clusters) {
            const std::vector<std::uint32_t>& indices = hierarchy.clusters[c].indices;
            for (std::size_t t = 0; t + 2 < indices.size(); t += 3) {
                std::array<std::uint32_t, 3> corners{indices[t], indices[t + 1], indices[t + 2]};
                // Rotate the smallest corner first; the winding is then whether the rest go up.
                std::ranges::rotate(corners, std::ranges::min_element(corners));
                const std::size_t way = corners[1] < corners[2] ? 0 : 1;
                if (way == 1) {
                    std::swap(corners[1], corners[2]);
                }
                wound[way][corners].emplace_back(c, t);
            }
        }
        std::map<std::uint32_t, std::vector<std::size_t>> dropped;
        for (auto& [corners, forward] : wound[0]) {
            const auto back = wound[1].find(corners);
            if (back == wound[1].end()) {
                continue;
            }
            const std::size_t pairs = std::min(forward.size(), back->second.size());
            for (std::size_t i = 0; i < pairs; ++i) {
                dropped[forward[i].first].push_back(forward[i].second);
                dropped[back->second[i].first].push_back(back->second[i].second);
            }
        }
        for (auto& [c, triangles] : dropped) {
            std::ranges::sort(triangles);
            std::vector<std::uint32_t>& indices = hierarchy.clusters[c].indices;
            std::vector<std::uint32_t> kept;
            kept.reserve(indices.size());
            std::size_t next = 0;
            for (std::size_t t = 0; t + 2 < indices.size(); t += 3) {
                if (next < triangles.size() && triangles[next] == t) {
                    ++next;
                    continue;
                }
                kept.insert(kept.end(), indices.begin() + static_cast<std::ptrdiff_t>(t),
                            indices.begin() + static_cast<std::ptrdiff_t>(t + 3));
            }
            indices = std::move(kept);
        }
    }
}

} // namespace

ClusterHierarchy build_cluster_hierarchy(const ModelSurface& surface,
                                         const ClusterSettings& settings) {
    ClusterHierarchy hierarchy;
    hierarchy.source_triangles = surface.indices.size() / 3;
    if (surface.indices.size() < 3 || surface.vertices.empty()) {
        return hierarchy;
    }
    const std::size_t max_triangles = std::clamp<std::size_t>(settings.max_triangles, 4, 256);
    clodConfig config = clodDefaultConfig(max_triangles);
    config.max_vertices = std::clamp<std::size_t>(settings.max_vertices, 3, 256);
    // Only the simplifier's regular mode, which keeps the surface's topology: the permissive mode
    // and the sloppy fallback may cross hard edges and leave holes, which a cut can't hide.
    config.simplify_permissive = false;
    config.simplify_fallback_sloppy = false;

    std::vector<float> positions;
    std::vector<float> normals;
    positions.reserve(surface.vertices.size() * 3);
    normals.reserve(surface.vertices.size() * 3);
    for (const render::Vertex3D& vertex : surface.vertices) {
        positions.insert(positions.end(), vertex.position.begin(), vertex.position.end());
        normals.insert(normals.end(), vertex.normal.begin(), vertex.normal.end());
    }
    const std::array<float, 3> weights{kNormalWeight, kNormalWeight, kNormalWeight};
    const std::vector<unsigned char> locks = open_edge_locks(surface, positions);
    clodMesh mesh{};
    mesh.indices = surface.indices.data();
    mesh.index_count = surface.indices.size();
    mesh.vertex_count = surface.vertices.size();
    mesh.vertex_positions = positions.data();
    mesh.vertex_positions_stride = sizeof(float) * 3;
    mesh.vertex_attributes = normals.data();
    mesh.vertex_attributes_stride = sizeof(float) * 3;
    mesh.attribute_weights = weights.data();
    mesh.attribute_count = weights.size();
    mesh.vertex_lock = locks.data();

    clodBuild(
        config, mesh, [&](clodGroup group, const clodCluster* clusters, std::size_t count) -> int {
            const int id = static_cast<int>(hierarchy.groups.size());
            ClusterGroup made{
                .simplified = bounds_of(group.simplified), .depth = group.depth, .clusters = {}};
            for (std::size_t i = 0; i < count; ++i) {
                const clodCluster& from = clusters[i]; // NOLINT(*-pointer-arithmetic)
                made.clusters.push_back(static_cast<std::uint32_t>(hierarchy.clusters.size()));
                hierarchy.clusters.push_back(Cluster{
                    .indices = {from.indices, from.indices + from.index_count}, // NOLINT
                    .bounds = bounds_of(from.bounds),
                    .refined = from.refined,
                    .group = id,
                    .depth = group.depth,
                });
                hierarchy.levels = std::max(hierarchy.levels, group.depth + 1);
            }
            hierarchy.groups.push_back(std::move(made));
            return id;
        });
    drop_folds(hierarchy);
    return hierarchy;
}

float projected_error(const ClusterBounds& bounds, const ClusterView& view) {
    if (bounds.error == std::numeric_limits<float>::max()) {
        return bounds.error;
    }
    const Vec3f to{bounds.center.x - view.camera.x, bounds.center.y - view.camera.y,
                   bounds.center.z - view.camera.z};
    const float distance =
        (std::sqrt((to.x * to.x) + (to.y * to.y) + (to.z * to.z)) - bounds.radius) * view.scale;
    return bounds.error * view.scale / std::max(distance, view.near) * (view.projection * 0.5F) *
           view.height;
}

std::vector<std::uint32_t> select_clusters(const ClusterHierarchy& hierarchy,
                                           const ClusterView& view) {
    std::vector<float> group_errors;
    group_errors.reserve(hierarchy.groups.size());
    for (const ClusterGroup& group : hierarchy.groups) {
        group_errors.push_back(projected_error(group.simplified, view));
    }
    std::vector<std::uint32_t> chosen;
    for (std::size_t i = 0; i < hierarchy.clusters.size(); ++i) {
        const Cluster& cluster = hierarchy.clusters[i];
        const bool coarse_enough =
            cluster.refined < 0 ||
            group_errors.at(static_cast<std::size_t>(cluster.refined)) <= view.threshold;
        const bool too_fine_to_drop =
            group_errors.at(static_cast<std::size_t>(cluster.group)) > view.threshold;
        if (coarse_enough && too_fine_to_drop) {
            chosen.push_back(static_cast<std::uint32_t>(i));
        }
    }
    return chosen;
}

std::vector<std::uint32_t> cluster_indices(const ClusterHierarchy& hierarchy,
                                           const std::vector<std::uint32_t>& clusters) {
    std::vector<std::uint32_t> indices;
    for (const std::uint32_t c : clusters) {
        const std::vector<std::uint32_t>& own = hierarchy.clusters.at(c).indices;
        indices.insert(indices.end(), own.begin(), own.end());
    }
    return indices;
}

} // namespace slash::scene3d
