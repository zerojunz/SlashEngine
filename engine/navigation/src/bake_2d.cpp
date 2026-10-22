// Baking 2D navigation polygons, as Godot 4.6's NavMeshGenerator2D
// (modules/navigation/2d/nav_mesh_generator_2d.cpp, generator_bake_from_source_geometry_data):
// Clipper2 merges the traversable outlines, takes the obstructions out and shrinks the rest by
// agent_radius, then polypartition splits it into convex polygons or triangles.

#include "slash/navigation/navigation.hpp"

#include <algorithm>
#include <clipper2/clipper.h>
#include <cstddef>
#include <list>
#include <map>
#include <polypartition.h>
#include <utility>
#include <vector>

namespace slash::navigation {
namespace {

using Clipper2Lib::PathD;
using Clipper2Lib::PathsD;

PathD path_of(const std::vector<Vec2>& outline) {
    PathD path;
    path.reserve(outline.size());
    for (const Vec2 point : outline) {
        path.emplace_back(point.x, point.y);
    }
    return path;
}

// A polygon tree's outlines and holes as polypartition takes them: outlines counterclockwise,
// holes clockwise, as Godot's generator_recursive_process_polytree_items.
void add_polytree_items(std::list<TPPLPoly>& polygons, const Clipper2Lib::PolyPathD& item) {
    TPPLPoly polygon;
    polygon.Init(static_cast<long>(item.Polygon().size()));
    int j = 0;
    for (const Clipper2Lib::PointD& point : item.Polygon()) {
        // Through float, as Godot's points are Vector2.
        polygon[j].x = static_cast<float>(point.x);
        polygon[j].y = static_cast<float>(point.y);
        ++j;
    }
    if (item.IsHole()) {
        polygon.SetOrientation(TPPL_ORIENTATION_CW);
        polygon.SetHole(true);
    } else {
        polygon.SetOrientation(TPPL_ORIENTATION_CCW);
    }
    polygons.push_back(polygon);
    for (std::size_t i = 0; i < item.Count(); ++i) {
        add_polytree_items(polygons, *item.Child(i));
    }
}

Clipper2Lib::RectD rect_of(Vec2 position, Vec2 size, Vec2 offset, double border) {
    return {position.x + offset.x + border, position.y + offset.y + border,
            position.x + size.x + offset.x - border, position.y + size.y + offset.y - border};
}

} // namespace

Result<NavigationPolygon> bake(const NavigationPolygon& settings,
                               const SourceGeometry2D& geometry) {
    NavigationPolygon out = settings;
    out.vertices.clear();
    out.polygons.clear();
    if (settings.outlines.empty() && geometry.traversable_outlines.empty()) {
        return out;
    }

    PathsD traversable;
    traversable.reserve(settings.outlines.size() + geometry.traversable_outlines.size());
    for (const std::vector<Vec2>& outline : settings.outlines) {
        traversable.push_back(path_of(outline));
    }
    for (const std::vector<Vec2>& outline : geometry.traversable_outlines) {
        traversable.push_back(path_of(outline));
    }
    PathsD obstructions;
    obstructions.reserve(geometry.obstruction_outlines.size());
    for (const std::vector<Vec2>& outline : geometry.obstruction_outlines) {
        obstructions.push_back(path_of(outline));
    }

    const bool has_rect = settings.baking_rect_size.x > 0.0F && settings.baking_rect_size.y > 0.0F;
    if (has_rect) {
        // Godot clips to the rect with its corners cut to whole pixels.
        const Vec2 offset = settings.baking_rect_offset;
        const Vec2 position = settings.baking_rect_position;
        const Vec2 size = settings.baking_rect_size;
        const Clipper2Lib::RectD rect(static_cast<int>(position.x + offset.x),
                                      static_cast<int>(position.y + offset.y),
                                      static_cast<int>(position.x + size.x + offset.x),
                                      static_cast<int>(position.y + size.y + offset.y));
        traversable = Clipper2Lib::RectClip(rect, traversable);
        obstructions = Clipper2Lib::RectClip(rect, obstructions);
    }

    // Merge the traversable outlines, then the obstructions, which have no holes since they are
    // solid, and take one from the other.
    const PathsD no_clips;
    using Clipper2Lib::FillRule;
    traversable = Clipper2Lib::Union(traversable, no_clips, FillRule::NonZero);
    obstructions = Clipper2Lib::Union(obstructions, no_clips, FillRule::NonZero);
    PathsD solution = Clipper2Lib::Difference(traversable, obstructions, FillRule::NonZero);
    if (settings.agent_radius > 0.0F) {
        solution =
            Clipper2Lib::InflatePaths(solution, -settings.agent_radius,
                                      Clipper2Lib::JoinType::Miter, Clipper2Lib::EndType::Polygon);
    }
    if (has_rect && settings.border_size > 0.0F) {
        solution =
            Clipper2Lib::RectClip(rect_of(settings.baking_rect_position, settings.baking_rect_size,
                                          settings.baking_rect_offset, settings.border_size),
                                  solution);
    }
    if (solution.empty()) {
        return out;
    }

    Clipper2Lib::PolyTreeD tree;
    Clipper2Lib::ClipperD clipper;
    clipper.AddSubject(solution);
    clipper.Execute(Clipper2Lib::ClipType::Union, FillRule::NonZero, tree);
    std::list<TPPLPoly> in_polygons;
    for (std::size_t i = 0; i < tree.Count(); ++i) {
        add_polytree_items(in_polygons, *tree[i]);
    }

    TPPLPartition partition;
    std::list<TPPLPoly> out_polygons;
    if (settings.sample_partition_type == Partition2D::ConvexPartition) {
        if (partition.ConvexPartition_HM(&in_polygons, &out_polygons) == 0) {
            return Error{ErrorCode::InvalidArgument,
                         "the outlines could not be split into convex polygons"};
        }
    } else if (partition.Triangulate_EC(&in_polygons, &out_polygons) == 0) {
        return Error{ErrorCode::InvalidArgument, "the outlines could not be split into triangles"};
    }

    // Each point once, in the order they first come.
    std::map<std::pair<float, float>, int> points;
    for (TPPLPoly& polygon : out_polygons) {
        std::vector<int>& indices = out.polygons.emplace_back();
        for (long i = 0; i < polygon.GetNumPoints(); ++i) {
            const TPPLPoint& point = polygon.GetPoint(i);
            const Vec2 vertex{static_cast<float>(point.x), static_cast<float>(point.y)};
            const auto [it, added] = points.emplace(std::pair{vertex.x, vertex.y},
                                                    static_cast<int>(out.vertices.size()));
            if (added) {
                out.vertices.push_back(vertex);
            }
            indices.push_back(it->second);
        }
    }
    return out;
}

} // namespace slash::navigation
