// The geometry a bake works from, as Godot 4.6's source geometry parsers: the region entity and
// those under it, each giving what Godot's node of the same kind gives
// (MeshInstance3D::navmesh_parse_source_geometry, StaticBody3D's and StaticBody2D's, CSGShape3D's,
// NavigationObstacle3D's and 2D's, TileMapLayer's), placed in the region's space as Godot's
// root_node_transform places it.

#include "slash/canvas/tilemap.hpp"
#include "slash/navigation/navigation.hpp"
#include "slash/physics3d/physics3d.hpp"
#include "slash/scene3d/csg.hpp"
#include "slash/scene3d/gridmap.hpp"
#include "slash/scene3d/model.hpp"
#include "slash/scene3d/primitive.hpp"
#include "slash/world/tree2d.hpp"
#include "slash/world/tree3d.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace slash::navigation {
namespace {

using world::Affine2D;
using world::Affine3D;

constexpr float kTau = 2.0F * std::numbers::pi_v<float>;
// The points of the circle an obstacle's radius projects, as Godot's: no excessive detail.
constexpr int kObstacleCirclePoints = 12;

// ---- 3D, as Godot's NavigationMeshSourceGeometryData3D.

void add_vertex(SourceGeometry3D& out, Vec3 point) {
    out.vertices.push_back(point.x);
    out.vertices.push_back(point.y);
    out.vertices.push_back(point.z);
}

int vertex_count(const SourceGeometry3D& out) {
    return static_cast<int>(out.vertices.size() / 3);
}

// An indexed surface, as _add_mesh and _add_mesh_array: Godot's clockwise triangles turned to
// Recast's counterclockwise ones.
void add_surface(SourceGeometry3D& out, const std::vector<render::Vertex3D>& vertices,
                 const std::vector<std::uint32_t>& indices, const Affine3D& transform) {
    if (vertices.empty() || indices.empty() || indices.size() % 3 != 0) {
        return;
    }
    const int base = vertex_count(out);
    for (const render::Vertex3D& vertex : vertices) {
        add_vertex(out,
                   transform.apply({vertex.position[0], vertex.position[1], vertex.position[2]}));
    }
    for (std::size_t j = 0; j < indices.size(); j += 3) {
        out.indices.push_back(base + static_cast<int>(indices[j]));
        out.indices.push_back(base + static_cast<int>(indices[j + 2]));
        out.indices.push_back(base + static_cast<int>(indices[j + 1]));
    }
}

// Triangles three points each, as _add_faces.
void add_faces(SourceGeometry3D& out, const std::vector<Vec3>& faces, const Affine3D& transform) {
    if (faces.empty() || faces.size() % 3 != 0) {
        return;
    }
    const int base = vertex_count(out);
    for (const Vec3 point : faces) {
        add_vertex(out, transform.apply(point));
    }
    const int count = static_cast<int>(faces.size() / 3);
    for (int j = 0; j < count; ++j) {
        out.indices.push_back(base + (j * 3));
        out.indices.push_back(base + (j * 3) + 2);
        out.indices.push_back(base + (j * 3) + 1);
    }
}

Affine3D shape_transform(const world::CollisionShape3D& shape) {
    world::Transform3D transform;
    transform.position = shape.position;
    transform.rotation = shape.rotation;
    return world::affine(transform);
}

// A shape as StaticBody3D's parser adds it: primitives as Godot's meshes of them (with the
// meshes' default segments), polygons as their faces. World boundaries add nothing.
void add_shape(SourceGeometry3D& out, const world::CollisionShape3D& shape,
               const Affine3D& transform) {
    std::optional<scene3d::Primitive> primitive;
    if (shape.shape == "box") {
        primitive = world::BoxMesh{.size = shape.size};
    } else if (shape.shape == "capsule") {
        primitive = world::CapsuleMesh{.radius = shape.radius, .height = shape.height};
    } else if (shape.shape == "cylinder") {
        primitive = world::CylinderMesh{
            .top_radius = shape.radius, .bottom_radius = shape.radius, .height = shape.height};
    } else if (shape.shape == "sphere") {
        primitive = world::SphereMesh{.radius = shape.radius, .height = shape.radius * 2.0F};
    } else if (shape.shape == "concave_polygon") {
        add_faces(out, shape.faces, transform);
    } else if (shape.shape == "convex_polygon") {
        add_faces(out, physics3d::convex_hull_triangles(shape.points), transform);
    }
    if (primitive) {
        if (auto mesh = scene3d::make_mesh(*primitive); mesh) {
            add_surface(out, mesh.value().vertices, mesh.value().indices, transform);
        }
    }
}

bool parses_meshes(ParsedGeometry type) {
    return type == ParsedGeometry::MeshInstances || type == ParsedGeometry::Both;
}

bool parses_colliders(ParsedGeometry type) {
    return type == ParsedGeometry::StaticColliders || type == ParsedGeometry::Both;
}

// The body's collision layer when it is a static or animatable body, which Godot's StaticBody
// parsers take (AnimatableBody extends StaticBody); nullopt for any other entity, or one that is
// a rigid or character body first, as physics takes it.
template <typename Static, typename Animatable, typename Rigid, typename Character>
std::optional<std::uint32_t> static_layer(const entt::registry& registry, entt::entity entity) {
    if (registry.any_of<Rigid, Character>(entity)) {
        return std::nullopt;
    }
    if (const auto* body = registry.try_get<Static>(entity); body != nullptr) {
        return body->collision_layer;
    }
    if (const auto* body = registry.try_get<Animatable>(entity); body != nullptr) {
        return body->collision_layer;
    }
    return std::nullopt;
}

// Whether a child's shapes belong to its parent's body: it has no body or area of its own.
bool has_own_object_3d(const entt::registry& registry, entt::entity entity) {
    return registry.any_of<world::StaticBody3D, world::AnimatableBody3D, world::RigidBody3D,
                           world::CharacterBody3D, world::Area3D, world::PhysicalBone3D>(entity);
}

bool has_own_object_2d(const entt::registry& registry, entt::entity entity) {
    return registry.any_of<world::StaticBody2D, world::AnimatableBody2D, world::RigidBody2D,
                           world::CharacterBody2D, world::Area2D>(entity);
}

// NavigationObstacle3D::navmesh_parse_source_geometry: its circle, then its outline, on the
// ground. Only its turn about y counts.
void add_obstacle(SourceGeometry3D& out, const world::NavigationObstacle3D& obstacle,
                  const Affine3D& root, const Affine3D& global) {
    if (!obstacle.affect_navigation_mesh) {
        return;
    }
    const float elevation = global.origin.y + root.origin.y;
    const auto safe = [](Vec3 axis) {
        return std::max(std::sqrt((axis.x * axis.x) + (axis.y * axis.y) + (axis.z * axis.z)),
                        0.001F);
    };
    const Vec3 scale{safe(global.x), safe(global.y), safe(global.z)};
    if (obstacle.radius > 0.0F) {
        const float max_scale = std::max({scale.x, scale.y, scale.z});
        const Affine3D circle = root * Affine3D{.x = {max_scale, 0.0F, 0.0F},
                                                .y = {0.0F, max_scale, 0.0F},
                                                .z = {0.0F, 0.0F, max_scale},
                                                .origin = global.origin};
        SourceGeometry3D::Obstruction obstruction;
        for (int i = 0; i < kObstacleCirclePoints; ++i) {
            const Vec2 cs = cos_sin(static_cast<float>(i) * (kTau / kObstacleCirclePoints));
            obstruction.vertices.push_back(
                circle.apply({cs.x * obstacle.radius, 0.0F, cs.y * obstacle.radius}));
        }
        // Godot lowers it by the radius as written, unscaled.
        obstruction.elevation = elevation - obstacle.radius;
        obstruction.height = max_scale * obstacle.radius;
        obstruction.carve = obstacle.carve_navigation_mesh;
        out.projected_obstructions.push_back(std::move(obstruction));
    }
    if (obstacle.vertices.empty()) {
        return;
    }
    // Basis().scaled(scale).rotated(up, rotation.y), at the obstacle.
    const Vec2 cs = cos_sin(world::euler(global).y);
    const Affine3D node = root * Affine3D{.x = {cs.x * scale.x, 0.0F, -cs.y * scale.x},
                                          .y = {0.0F, scale.y, 0.0F},
                                          .z = {cs.y * scale.z, 0.0F, cs.x * scale.z},
                                          .origin = global.origin};
    SourceGeometry3D::Obstruction obstruction;
    obstruction.vertices.reserve(obstacle.vertices.size());
    for (const Vec3 vertex : obstacle.vertices) {
        Vec3 placed = node.apply(vertex);
        placed.y = 0.0F;
        obstruction.vertices.push_back(placed);
    }
    obstruction.elevation = elevation;
    obstruction.height = scale.y * obstacle.height;
    obstruction.carve = obstacle.carve_navigation_mesh;
    out.projected_obstructions.push_back(std::move(obstruction));
}

void parse_3d(const world::World& world, asset::Assets& assets, const world::Tree3D& tree,
              const Affine3D& root, entt::entity entity, const NavigationMesh& settings,
              SourceGeometry3D& out, std::vector<std::string>* warnings) {
    const entt::registry& registry = world.ecs();
    if (const auto* obstacle = registry.try_get<world::NavigationObstacle3D>(entity);
        obstacle != nullptr) {
        add_obstacle(out, *obstacle, root, tree.global(entity));
    }
    const auto warn = [&](const std::string& message) {
        if (warnings != nullptr) {
            warnings->push_back(std::string(world.id_of(entity)) + ": " + message);
        }
    };
    if (parses_meshes(settings.geometry_parsed_geometry_type)) {
        if (const auto* instance = registry.try_get<world::MeshInstance3D>(entity);
            instance != nullptr) {
            const Affine3D transform = root * tree.global(entity);
            if (!instance->mesh.empty()) {
                auto model = assets.load<scene3d::Model>(instance->mesh);
                if (!model) {
                    warn("its mesh was left out: " + model.error().message);
                } else {
                    const scene3d::Model& loaded = *model.value();
                    for (const scene3d::ModelPart& part : loaded.parts) {
                        for (const scene3d::ModelSurface& surface :
                             loaded.meshes.at(part.mesh).surfaces) {
                            add_surface(out, surface.vertices, surface.indices,
                                        transform * part.transform);
                        }
                    }
                }
            } else if (auto primitive = scene3d::primitive_of(world, entity); !primitive) {
                warn("its mesh was left out: " + primitive.error().message);
            } else if (const auto& found = primitive.value(); found) {
                if (auto mesh = scene3d::make_mesh(*found); !mesh) {
                    warn("its mesh was left out: " + mesh.error().message);
                } else {
                    add_surface(out, mesh.value().vertices, mesh.value().indices, transform);
                }
            }
        }
    }
    // CSGShape3D::navmesh_parse_source_geometry: a root's result, once, with mesh_instances or
    // both, and with static_colliders when it collides on a layer the mask takes.
    if (const std::optional<scene3d::CsgShape> shape = scene3d::csg_shape_of(world, entity);
        shape && scene3d::is_csg_root(world, tree, entity)) {
        const scene3d::CsgSettings csg = scene3d::csg_settings(*shape);
        const bool collides =
            csg.use_collision && (csg.collision_layer & settings.geometry_collision_mask) != 0U;
        if (parses_meshes(settings.geometry_parsed_geometry_type) ||
            (parses_colliders(settings.geometry_parsed_geometry_type) && collides)) {
            const Affine3D transform = root * tree.global(entity);
            for (const scene3d::MeshSurface& surface :
                 scene3d::csg_mesh(world, tree, entity)->surfaces) {
                add_surface(out, surface.vertices, surface.indices, transform);
            }
        }
    }
    // GridMap::navmesh_parse_source_geometry: its cells' meshes with mesh_instances or both,
    // else, with static_colliders, their shapes when it is on a layer the mask takes.
    if (const auto* map = registry.try_get<world::GridMap>(entity); map != nullptr) {
        const Affine3D transform = root * tree.global(entity);
        const std::shared_ptr<const scene3d::GridMesh> mesh = scene3d::grid_mesh(world, entity);
        if (parses_meshes(settings.geometry_parsed_geometry_type)) {
            for (const auto& octant : mesh->octants) {
                for (const scene3d::GridSurface& surface : octant->surfaces) {
                    add_surface(out, surface.mesh.vertices, surface.mesh.indices, transform);
                }
            }
        } else if (parses_colliders(settings.geometry_parsed_geometry_type) &&
                   (map->collision_layer & settings.geometry_collision_mask) != 0U) {
            for (const scene3d::GridShape& shape : *mesh->shapes) {
                add_shape(out, shape.shape, transform * shape.local);
            }
        }
    }
    if (parses_colliders(settings.geometry_parsed_geometry_type)) {
        const auto layer =
            static_layer<world::StaticBody3D, world::AnimatableBody3D, world::RigidBody3D,
                         world::CharacterBody3D>(registry, entity);
        if (layer && (*layer & settings.geometry_collision_mask) != 0U) {
            const Affine3D body = root * tree.global(entity);
            if (const auto* shape = registry.try_get<world::CollisionShape3D>(entity);
                shape != nullptr && !shape->disabled) {
                add_shape(out, *shape, body * shape_transform(*shape));
            }
            if (const auto* transform = registry.try_get<world::Transform3D>(entity);
                transform != nullptr) {
                for (const entt::entity child : transform->children) {
                    const auto* shape = registry.valid(child)
                                            ? registry.try_get<world::CollisionShape3D>(child)
                                            : nullptr;
                    if (shape == nullptr || shape->disabled || tree.parent(child) != entity ||
                        has_own_object_3d(registry, child)) {
                        continue;
                    }
                    add_shape(out, *shape, root * tree.global(child) * shape_transform(*shape));
                }
            }
        }
    }
    if (const auto* transform = registry.try_get<world::Transform3D>(entity);
        transform != nullptr) {
        for (const entt::entity child : transform->children) {
            if (registry.valid(child) && tree.parent(child) == entity) {
                parse_3d(world, assets, tree, root, child, settings, out, warnings);
            }
        }
    }
}

// ---- 2D, as Godot's NavigationMeshSourceGeometryData2D and StaticBody2D's parser.

// The points' convex hull, counterclockwise from the lowest, as Andrew's monotone chain.
std::vector<Vec2> convex_hull(std::vector<Vec2> points) {
    std::sort(points.begin(), points.end(),
              [](Vec2 a, Vec2 b) { return a.x < b.x || (a.x == b.x && a.y < b.y); });
    points.erase(std::unique(points.begin(), points.end()), points.end());
    if (points.size() < 3) {
        return points;
    }
    const auto cross = [](Vec2 o, Vec2 a, Vec2 b) {
        return ((a.x - o.x) * (b.y - o.y)) - ((a.y - o.y) * (b.x - o.x));
    };
    std::vector<Vec2> hull(points.size() * 2);
    std::size_t k = 0;
    for (const Vec2 point : points) {
        while (k >= 2 && cross(hull[k - 2], hull[k - 1], point) <= 0.0F) {
            --k;
        }
        hull[k++] = point;
    }
    const std::size_t lower = k + 1;
    for (std::size_t i = points.size() - 1; i > 0; --i) {
        const Vec2 point = points[i - 1];
        while (k >= lower && cross(hull[k - 2], hull[k - 1], point) <= 0.0F) {
            --k;
        }
        hull[k++] = point;
    }
    hull.resize(k - 1);
    return hull;
}

Affine2D shape_transform(Vec2 position, float rotation) {
    world::Transform2D transform;
    transform.position = position;
    transform.rotation = rotation;
    return world::affine(transform);
}

std::vector<Vec2> placed(const std::vector<Vec2>& points, const Affine2D& transform) {
    std::vector<Vec2> out;
    out.reserve(points.size());
    for (const Vec2 point : points) {
        out.push_back(transform.apply(point));
    }
    return out;
}

// A shape's outline, as StaticBody2D::navmesh_parse_source_geometry makes it: a circle has 12
// sides and a capsule 14 corners. Segments add nothing.
std::vector<Vec2> outline_of(const world::CollisionShape2D& shape) {
    if (shape.shape == "rectangle") {
        const Vec2 half{shape.size.x * 0.5F, shape.size.y * 0.5F};
        return {{-half.x, -half.y},
                {half.x, -half.y},
                {half.x, half.y},
                {-half.x, half.y},
                {-half.x, -half.y}};
    }
    if (shape.shape == "circle") {
        constexpr int kEdges = 12;
        const float step = kTau / static_cast<float>(kEdges);
        std::vector<Vec2> out;
        for (int i = 0; i < kEdges; ++i) {
            const Vec2 turn = cos_sin(static_cast<float>(i) * step);
            out.push_back({turn.x * shape.radius, turn.y * shape.radius});
        }
        return out;
    }
    if (shape.shape == "capsule") {
        const float step = kTau / 12.0F;
        std::vector<Vec2> out;
        for (int i = 0; i < 12; ++i) {
            const Vec2 offset{0.0F, (i > 3 && i <= 9) ? (shape.height * 0.5F) - shape.radius
                                                      : (-shape.height * 0.5F) + shape.radius};
            const Vec2 turn = cos_sin(static_cast<float>(i) * step);
            const Vec2 round{turn.y * shape.radius, -turn.x * shape.radius};
            out.push_back(round + offset);
            if (i == 3 || i == 9) {
                out.push_back(round - offset);
            }
        }
        return out;
    }
    if (shape.shape == "polygon") {
        return convex_hull(shape.points);
    }
    return {};
}

// NavigationObstacle2D::navmesh_parse_source_geometry: its circle, then its outline.
void add_obstacle(SourceGeometry2D& out, const world::NavigationObstacle2D& obstacle,
                  const Affine2D& root, const world::Tree2D& tree, entt::entity entity) {
    if (!obstacle.affect_navigation_mesh) {
        return;
    }
    const Affine2D global = tree.global_affine(entity);
    if (obstacle.radius > 0.0F) {
        const auto safe = [](Vec2 axis) {
            return std::max(std::sqrt((axis.x * axis.x) + (axis.y * axis.y)), 0.001F);
        };
        const float max_scale = std::max(safe(global.x), safe(global.y));
        world::Transform2D parts = tree.global(entity);
        parts.scale = {max_scale, max_scale};
        parts.skew = 0.0F;
        const Affine2D circle = root * world::affine(parts);
        SourceGeometry2D::Obstruction obstruction;
        for (int i = 0; i < kObstacleCirclePoints; ++i) {
            const Vec2 cs = cos_sin(static_cast<float>(i) * (kTau / kObstacleCirclePoints));
            obstruction.vertices.push_back(
                circle.apply({cs.x * obstacle.radius, cs.y * obstacle.radius}));
        }
        obstruction.carve = obstacle.carve_navigation_mesh;
        out.projected_obstructions.push_back(std::move(obstruction));
    }
    if (obstacle.vertices.empty()) {
        return;
    }
    out.projected_obstructions.push_back({.vertices = placed(obstacle.vertices, root * global),
                                          .carve = obstacle.carve_navigation_mesh});
}

// TileMapLayer::navmesh_parse_source_geometry: each cell's navigation outlines, layer by layer,
// as traversable outlines, and with static_colliders or both, its collision polygons in physics
// layers whose collision_layer shares a bit with parsed_collision_mask, as obstructions.
void add_tiles(SourceGeometry2D& out, const world::World& world, asset::Assets& assets,
               const Affine2D& root, const world::Tree2D& tree, entt::entity entity,
               const NavigationPolygon& settings, std::vector<std::string>* warnings) {
    const auto& layer = world.ecs().get<const world::TileMapLayer>(entity);
    if (layer.tile_set.empty()) {
        return;
    }
    const auto warn = [&](const std::string& message) {
        if (warnings != nullptr) {
            const std::string_view id = world.id_of(entity);
            warnings->push_back((id.empty() ? std::string("a tile map layer") : std::string(id)) +
                                ": " + message);
        }
    };
    auto loaded = assets.load<canvas::TileSet>(layer.tile_set);
    if (!loaded) {
        warn(layer.tile_set + ": " + loaded.error().message);
        return;
    }
    const canvas::TileSet& set = *loaded.value();
    const bool colliders = parses_colliders(settings.parsed_geometry_type);
    if (set.navigation_layers.empty() && (set.physics_layers.empty() || !colliders)) {
        return;
    }
    auto cells = canvas::decode_cells(layer, set);
    if (!cells) {
        warn(cells.error().message);
        return;
    }
    const Affine2D tilemap = root * tree.global_affine(entity);
    for (const auto& [cell, tile] : cells.value()) {
        Affine2D placement = tilemap;
        placement.origin = tilemap.apply(canvas::map_to_local(set, cell));
        const std::vector<canvas::TileNavigation> outlines = canvas::tile_navigation(set, tile);
        for (std::size_t i = 0; i < set.navigation_layers.size(); ++i) {
            for (const canvas::TileNavigation& outline : outlines) {
                if (static_cast<std::size_t>(outline.layer) == i && !outline.points.empty()) {
                    out.traversable_outlines.push_back(placed(outline.points, placement));
                }
            }
        }
        if (!colliders) {
            continue;
        }
        const std::vector<canvas::TilePolygon> polygons = canvas::tile_collision(set, tile);
        for (std::size_t i = 0; i < set.physics_layers.size(); ++i) {
            if ((set.physics_layers[i].collision_layer & settings.parsed_collision_mask) == 0U) {
                continue;
            }
            for (const canvas::TilePolygon& polygon : polygons) {
                if (static_cast<std::size_t>(polygon.layer) == i && !polygon.points.empty()) {
                    out.obstruction_outlines.push_back(placed(polygon.points, placement));
                }
            }
        }
    }
}

void parse_2d(const world::World& world, asset::Assets& assets, const world::Tree2D& tree,
              const Affine2D& root, entt::entity entity, const NavigationPolygon& settings,
              SourceGeometry2D& out, std::vector<std::string>* warnings) {
    const entt::registry& registry = world.ecs();
    if (const auto* obstacle = registry.try_get<world::NavigationObstacle2D>(entity);
        obstacle != nullptr) {
        add_obstacle(out, *obstacle, root, tree, entity);
    }
    if (registry.all_of<world::TileMapLayer>(entity)) {
        add_tiles(out, world, assets, root, tree, entity, settings, warnings);
    }
    const auto add = [&](std::vector<Vec2> outline) {
        if (outline.size() >= 3) {
            out.obstruction_outlines.push_back(std::move(outline));
        }
    };
    const auto add_shapes = [&](entt::entity owner, const Affine2D& transform) {
        if (const auto* shape = registry.try_get<world::CollisionShape2D>(owner);
            shape != nullptr && !shape->disabled) {
            add(placed(outline_of(*shape),
                       transform * shape_transform(shape->position, shape->rotation)));
        }
        if (const auto* polygon = registry.try_get<world::CollisionPolygon2D>(owner);
            polygon != nullptr && !polygon->disabled) {
            add(placed(polygon->polygon, transform));
        }
    };
    if (parses_colliders(settings.parsed_geometry_type)) {
        const auto layer =
            static_layer<world::StaticBody2D, world::AnimatableBody2D, world::RigidBody2D,
                         world::CharacterBody2D>(registry, entity);
        if (layer && (*layer & settings.parsed_collision_mask) != 0U) {
            add_shapes(entity, root * tree.global_affine(entity));
            if (const auto* transform = registry.try_get<world::Transform2D>(entity);
                transform != nullptr) {
                for (const entt::entity child : transform->children) {
                    if (registry.valid(child) && tree.parent(child) == entity &&
                        !has_own_object_2d(registry, child)) {
                        add_shapes(child, root * tree.global_affine(child));
                    }
                }
            }
        }
    }
    if (const auto* transform = registry.try_get<world::Transform2D>(entity);
        transform != nullptr) {
        for (const entt::entity child : transform->children) {
            if (registry.valid(child) && tree.parent(child) == entity) {
                parse_2d(world, assets, tree, root, child, settings, out, warnings);
            }
        }
    }
}

} // namespace

SourceGeometry3D parse_source_geometry(const world::World& world, asset::Assets& assets,
                                       entt::entity root, const NavigationMesh& settings,
                                       std::vector<std::string>* warnings) {
    SourceGeometry3D out;
    const world::Tree3D tree(world);
    parse_3d(world, assets, tree, tree.global(root).inverse(), root, settings, out, warnings);
    return out;
}

SourceGeometry2D parse_source_geometry(const world::World& world, asset::Assets& assets,
                                       entt::entity root, const NavigationPolygon& settings,
                                       std::vector<std::string>* warnings) {
    SourceGeometry2D out;
    const world::Tree2D tree(world);
    parse_2d(world, assets, tree, tree.global_affine(root).inverse(), root, settings, out,
             warnings);
    return out;
}

} // namespace slash::navigation
