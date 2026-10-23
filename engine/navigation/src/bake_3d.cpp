// Baking 3D navigation meshes on Recast, as Godot 4.6's NavMeshGenerator3D
// (modules/navigation/3d/nav_mesh_generator_3d.cpp, generator_bake_from_source_geometry_data):
// the same Recast steps with the same settings, and the detail mesh's triangles kept as the
// polygons, as Godot keeps them.

#include "slash/navigation/navigation.hpp"

#include <Recast.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace slash::navigation {
namespace {

// Godot's CMP_EPSILON, Math::is_equal_approx and Math::is_zero_approx.
constexpr float kEpsilon = 0.00001F;

bool equal_approx(float a, float b) {
    if (a == b) {
        return true;
    }
    float tolerance = kEpsilon * std::abs(a);
    if (tolerance < kEpsilon) {
        tolerance = kEpsilon;
    }
    return std::abs(a - b) < tolerance;
}

bool zero_approx(float value) {
    return std::abs(value) < kEpsilon;
}

// rcMarkWalkableTriangles, with the slope's cosine from slash::cos_sin rather than libm's, so
// a bake marks the same triangles on every platform.
void mark_walkable_triangles(float slope_degrees, const float* verts, const int* tris, int ntris,
                             unsigned char* areas) {
    const float threshold = cos_sin(slope_degrees / 180.0F * RC_PI).x;
    for (int i = 0; i < ntris; ++i) {
        const int* tri = &tris[static_cast<std::ptrdiff_t>(i) * 3];
        float e0[3];
        float e1[3];
        float normal[3];
        rcVsub(e0, &verts[static_cast<std::ptrdiff_t>(tri[1]) * 3],
               &verts[static_cast<std::ptrdiff_t>(tri[0]) * 3]);
        rcVsub(e1, &verts[static_cast<std::ptrdiff_t>(tri[2]) * 3],
               &verts[static_cast<std::ptrdiff_t>(tri[0]) * 3]);
        rcVcross(normal, e0, e1);
        rcVnormalize(normal);
        if (normal[1] > threshold) {
            areas[i] = RC_WALKABLE_AREA;
        }
    }
}

// Past this many cells Godot refuses to bake, as its crash prevention check does.
constexpr std::int64_t kMaxCells = 30'000'000;

// Recast's objects, freed however the bake ends.
struct RecastObjects {
    rcHeightfield* heightfield = nullptr;
    rcCompactHeightfield* compact = nullptr;
    rcContourSet* contours = nullptr;
    rcPolyMesh* poly_mesh = nullptr;
    rcPolyMeshDetail* detail_mesh = nullptr;

    RecastObjects() = default;
    RecastObjects(const RecastObjects&) = delete;
    RecastObjects& operator=(const RecastObjects&) = delete;
    RecastObjects(RecastObjects&&) = delete;
    RecastObjects& operator=(RecastObjects&&) = delete;
    ~RecastObjects() {
        rcFreeHeightField(heightfield);
        rcFreeCompactHeightfield(compact);
        rcFreeContourSet(contours);
        rcFreePolyMesh(poly_mesh);
        rcFreePolyMeshDetail(detail_mesh);
    }
};

Error failed(std::string_view step) {
    return Error{ErrorCode::InvalidArgument, "Recast could not " + std::string(step)};
}

} // namespace

Result<NavigationMesh> bake(const NavigationMesh& settings, const SourceGeometry3D& geometry,
                            std::vector<std::string>* warnings) {
    const auto warn = [&](std::string message) {
        if (warnings != nullptr) {
            warnings->push_back(std::move(message));
        }
    };
    NavigationMesh mesh = settings;
    mesh.vertices.clear();
    mesh.polygons.clear();
    if (geometry.vertices.size() < 3 || geometry.indices.size() < 3) {
        return mesh;
    }

    const float* verts = geometry.vertices.data();
    const int nverts = static_cast<int>(geometry.vertices.size() / 3);
    const int* tris = geometry.indices.data();
    const int ntris = static_cast<int>(geometry.indices.size() / 3);

    std::array<float, 3> bmin{};
    std::array<float, 3> bmax{};
    rcCalcBounds(verts, nverts, bmin.data(), bmax.data());

    rcConfig cfg{};
    cfg.cs = settings.cell_size;
    cfg.ch = settings.cell_height;
    if (settings.border_size > 0.0F) {
        cfg.borderSize = static_cast<int>(std::ceil(settings.border_size / cfg.cs));
    }
    cfg.walkableSlopeAngle = settings.agent_max_slope;
    cfg.walkableHeight = static_cast<int>(std::ceil(settings.agent_height / cfg.ch));
    cfg.walkableClimb = static_cast<int>(std::floor(settings.agent_max_climb / cfg.ch));
    cfg.walkableRadius = static_cast<int>(std::ceil(settings.agent_radius / cfg.cs));
    cfg.maxEdgeLen = static_cast<int>(settings.edge_max_length / settings.cell_size);
    cfg.maxSimplificationError = settings.edge_max_error;
    cfg.minRegionArea = static_cast<int>(settings.region_min_size * settings.region_min_size);
    cfg.mergeRegionArea = static_cast<int>(settings.region_merge_size * settings.region_merge_size);
    cfg.maxVertsPerPoly = static_cast<int>(settings.vertices_per_polygon);
    cfg.detailSampleDist = std::max(settings.cell_size * settings.detail_sample_distance, 0.1F);
    cfg.detailSampleMaxError = settings.cell_height * settings.detail_sample_max_error;

    // Godot's warnings for settings that don't fit the cells.
    if (settings.border_size > 0.0F &&
        !zero_approx(std::fmod(settings.border_size, settings.cell_size))) {
        warn("border_size is rounded up to whole cells of cell_size");
    }
    if (!equal_approx(static_cast<float>(cfg.walkableHeight) * cfg.ch, settings.agent_height)) {
        warn("agent_height is rounded up to whole cells of cell_height");
    }
    if (!equal_approx(static_cast<float>(cfg.walkableClimb) * cfg.ch, settings.agent_max_climb)) {
        warn("agent_max_climb is rounded down to whole cells of cell_height");
    }
    if (!equal_approx(static_cast<float>(cfg.walkableRadius) * cfg.cs, settings.agent_radius)) {
        warn("agent_radius is rounded up to whole cells of cell_size");
    }
    if (!equal_approx(static_cast<float>(cfg.maxEdgeLen) * cfg.cs, settings.edge_max_length)) {
        warn("edge_max_length is rounded to whole cells of cell_size");
    }
    if (!equal_approx(static_cast<float>(cfg.minRegionArea),
                      settings.region_min_size * settings.region_min_size)) {
        warn("region_min_size squared is rounded down to a whole number");
    }
    if (!equal_approx(static_cast<float>(cfg.mergeRegionArea),
                      settings.region_merge_size * settings.region_merge_size)) {
        warn("region_merge_size squared is rounded down to a whole number");
    }
    if (!equal_approx(static_cast<float>(cfg.maxVertsPerPoly), settings.vertices_per_polygon)) {
        warn("vertices_per_polygon is rounded down to a whole number");
    }
    if (settings.cell_size * settings.detail_sample_distance < 0.1F) {
        warn("detail_sample_distance times cell_size is raised to 0.1");
    }

    cfg.bmin[0] = bmin[0];
    cfg.bmin[1] = bmin[1];
    cfg.bmin[2] = bmin[2];
    cfg.bmax[0] = bmax[0];
    cfg.bmax[1] = bmax[1];
    cfg.bmax[2] = bmax[2];
    const Vec3 box = settings.filter_baking_aabb_size;
    if (box.x > 0.0F && box.y > 0.0F && box.z > 0.0F) {
        const Vec3 start =
            settings.filter_baking_aabb_position + settings.filter_baking_aabb_offset;
        cfg.bmin[0] = start.x;
        cfg.bmin[1] = start.y;
        cfg.bmin[2] = start.z;
        cfg.bmax[0] = cfg.bmin[0] + box.x;
        cfg.bmax[1] = cfg.bmin[1] + box.y;
        cfg.bmax[2] = cfg.bmin[2] + box.z;
    }

    rcCalcGridSize(cfg.bmin, cfg.bmax, cfg.cs, &cfg.width, &cfg.height);
    const std::int64_t cells = static_cast<std::int64_t>(cfg.width) * cfg.height;
    if (cells > kMaxCells) {
        return Error{ErrorCode::InvalidArgument,
                     "the geometry is " + std::to_string(cfg.width) + " by " +
                         std::to_string(cfg.height) +
                         " cells, too many to bake; raise cell_size or bake a smaller "
                         "filter_baking_aabb"};
    }

    rcContext ctx(false);
    RecastObjects recast;
    recast.heightfield = rcAllocHeightfield();
    if (recast.heightfield == nullptr ||
        !rcCreateHeightfield(&ctx, *recast.heightfield, cfg.width, cfg.height, cfg.bmin, cfg.bmax,
                             cfg.cs, cfg.ch)) {
        return failed("make the heightfield");
    }
    {
        std::vector<unsigned char> areas(static_cast<std::size_t>(ntris), 0);
        mark_walkable_triangles(cfg.walkableSlopeAngle, verts, tris, ntris, areas.data());
        if (!rcRasterizeTriangles(&ctx, verts, nverts, tris, areas.data(), ntris,
                                  *recast.heightfield, cfg.walkableClimb)) {
            return failed("rasterize the triangles");
        }
    }
    if (settings.filter_low_hanging_obstacles) {
        rcFilterLowHangingWalkableObstacles(&ctx, cfg.walkableClimb, *recast.heightfield);
    }
    if (settings.filter_ledge_spans) {
        rcFilterLedgeSpans(&ctx, cfg.walkableHeight, cfg.walkableClimb, *recast.heightfield);
    }
    if (settings.filter_walkable_low_height_spans) {
        rcFilterWalkableLowHeightSpans(&ctx, cfg.walkableHeight, *recast.heightfield);
    }

    recast.compact = rcAllocCompactHeightfield();
    if (recast.compact == nullptr ||
        !rcBuildCompactHeightfield(&ctx, cfg.walkableHeight, cfg.walkableClimb, *recast.heightfield,
                                   *recast.compact)) {
        return failed("compact the heightfield");
    }
    rcFreeHeightField(recast.heightfield);
    recast.heightfield = nullptr;

    if (!rcErodeWalkableArea(&ctx, cfg.walkableRadius, *recast.compact)) {
        return failed("erode the walkable area by agent_radius");
    }

    bool partitioned = false;
    switch (settings.sample_partition_type) {
    case Partition3D::Watershed:
        partitioned = rcBuildDistanceField(&ctx, *recast.compact) &&
                      rcBuildRegions(&ctx, *recast.compact, cfg.borderSize, cfg.minRegionArea,
                                     cfg.mergeRegionArea);
        break;
    case Partition3D::Monotone:
        partitioned = rcBuildRegionsMonotone(&ctx, *recast.compact, cfg.borderSize,
                                             cfg.minRegionArea, cfg.mergeRegionArea);
        break;
    case Partition3D::Layers:
        partitioned = rcBuildLayerRegions(&ctx, *recast.compact, cfg.borderSize, cfg.minRegionArea);
        break;
    }
    if (!partitioned) {
        return failed("partition the walkable area");
    }

    recast.contours = rcAllocContourSet();
    if (recast.contours == nullptr ||
        !rcBuildContours(&ctx, *recast.compact, cfg.maxSimplificationError, cfg.maxEdgeLen,
                         *recast.contours)) {
        return failed("trace the contours");
    }
    recast.poly_mesh = rcAllocPolyMesh();
    if (recast.poly_mesh == nullptr ||
        !rcBuildPolyMesh(&ctx, *recast.contours, cfg.maxVertsPerPoly, *recast.poly_mesh)) {
        return failed("build the polygons");
    }
    recast.detail_mesh = rcAllocPolyMeshDetail();
    if (recast.detail_mesh == nullptr ||
        !rcBuildPolyMeshDetail(&ctx, *recast.poly_mesh, *recast.compact, cfg.detailSampleDist,
                               cfg.detailSampleMaxError, *recast.detail_mesh)) {
        return failed("build the detail mesh");
    }

    // The detail mesh's vertices, each once, in the order they first come, and its triangles as
    // the polygons, turned the other way round, as Godot converts them.
    const rcPolyMeshDetail& detail = *recast.detail_mesh;
    std::map<std::array<float, 3>, int> seen;
    std::vector<int> native(static_cast<std::size_t>(detail.nverts));
    for (int i = 0; i < detail.nverts; ++i) {
        const float* v = &detail.verts[static_cast<std::ptrdiff_t>(i) * 3];
        const std::array<float, 3> key{v[0], v[1], v[2]};
        const auto [it, added] = seen.emplace(key, static_cast<int>(mesh.vertices.size()));
        if (added) {
            mesh.vertices.push_back({v[0], v[1], v[2]});
        }
        native[static_cast<std::size_t>(i)] = it->second;
    }
    for (int i = 0; i < detail.nmeshes; ++i) {
        const unsigned int* m = &detail.meshes[static_cast<std::ptrdiff_t>(i) * 4];
        const unsigned int base_vert = m[0];
        const unsigned int base_tri = m[2];
        const unsigned int count = m[3];
        const unsigned char* t = &detail.tris[static_cast<std::ptrdiff_t>(base_tri) * 4];
        for (unsigned int j = 0; j < count; ++j) {
            const auto corner = [&](unsigned int k) { return native[base_vert + t[(j * 4) + k]]; };
            mesh.polygons.push_back({corner(0), corner(2), corner(1)});
        }
    }
    return mesh;
}

} // namespace slash::navigation
