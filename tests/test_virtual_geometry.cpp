// Virtualized geometry's cluster hierarchies: built once, then cut for a view so detail follows
// distance across one surface, with no cracks between parts drawn at different depths.

#include "gltf.hpp"

#include "slash/asset/assets.hpp"
#include "slash/canvas/canvas.hpp"
#include "slash/core/json.hpp"
#include "slash/core/math.hpp"
#include "slash/scene3d/model.hpp"
#include "slash/scene3d/primitive.hpp"
#include "slash/scene3d/virtual_geometry.hpp"
#include "slash/ui/ui.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/world.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scene3d = slash::scene3d;

namespace {

// A bumpy ground 40 meters square of 128 x 128 quads: plenty to simplify, with a rim that stays
// put (an open edge is locked, as Godot's importer locks it). ripple is how high the bumps between
// corners go.
scene3d::ModelSurface bumpy_ground(float ripple_height = 0.02F) {
    slash::world::PlaneMesh plane;
    plane.size = {40.0F, 40.0F};
    plane.subdivide_width = 127;
    plane.subdivide_depth = 127;
    scene3d::MeshData made = scene3d::make_mesh(scene3d::Primitive{plane}).value();
    scene3d::ModelSurface surface;
    surface.vertices = std::move(made.vertices);
    surface.indices = std::move(made.indices);
    for (slash::render::Vertex3D& vertex : surface.vertices) {
        const float x = vertex.position[0];
        const float z = vertex.position[2];
        // A smooth swell plus ripples, from integer steps so it is the same everywhere.
        const float swell = 1.5F * (1.0F - ((x * x) + (z * z)) / 800.0F);
        const int cx = static_cast<int>(std::lround(x * 3.2F));
        const int cz = static_cast<int>(std::lround(z * 3.2F));
        const float ripple =
            ripple_height * static_cast<float>(((((cx * 7) + (cz * 13)) % 5) + 5) % 5 - 2);
        vertex.position[1] = swell + ripple;
    }
    return surface;
}

using Corner = std::array<float, 3>;

// Each edge of the triangles, by its two corners' places, and how many triangles use it.
std::map<std::pair<Corner, Corner>, int> edges_of(const scene3d::ModelSurface& surface,
                                                  const std::vector<std::uint32_t>& indices) {
    std::map<std::pair<Corner, Corner>, int> edges;
    for (std::size_t t = 0; t + 2 < indices.size(); t += 3) {
        for (std::size_t e = 0; e < 3; ++e) {
            Corner a = surface.vertices.at(indices.at(t + e)).position;
            Corner b = surface.vertices.at(indices.at(t + ((e + 1) % 3))).position;
            if (b < a) {
                std::swap(a, b);
            }
            ++edges[{a, b}];
        }
    }
    return edges;
}

bool on_rim(const Corner& a, const Corner& b) {
    const auto at = [](float v) { return std::abs(std::abs(v) - 20.0F) < 1e-4F; };
    return (at(a[0]) && at(b[0]) && a[0] == b[0]) || (at(a[2]) && at(b[2]) && a[2] == b[2]);
}

// The area the triangles cover seen from above, which a cut must keep: 40 x 40.
double area_from_above(const scene3d::ModelSurface& surface,
                       const std::vector<std::uint32_t>& indices) {
    double area = 0.0;
    for (std::size_t t = 0; t + 2 < indices.size(); t += 3) {
        const Corner& a = surface.vertices.at(indices.at(t)).position;
        const Corner& b = surface.vertices.at(indices.at(t + 1)).position;
        const Corner& c = surface.vertices.at(indices.at(t + 2)).position;
        area +=
            std::abs(((static_cast<double>(b[0]) - a[0]) * (static_cast<double>(c[2]) - a[2])) -
                     ((static_cast<double>(c[0]) - a[0]) * (static_cast<double>(b[2]) - a[2]))) /
            2.0;
    }
    return area;
}

// The same, counting triangles wound the other way (folded over) as taking area away.
double signed_area_from_above(const scene3d::ModelSurface& surface,
                              const std::vector<std::uint32_t>& indices) {
    double area = 0.0;
    for (std::size_t t = 0; t + 2 < indices.size(); t += 3) {
        const Corner& a = surface.vertices.at(indices.at(t)).position;
        const Corner& b = surface.vertices.at(indices.at(t + 1)).position;
        const Corner& c = surface.vertices.at(indices.at(t + 2)).position;
        area += (((static_cast<double>(b[0]) - a[0]) * (static_cast<double>(c[2]) - a[2])) -
                 ((static_cast<double>(c[0]) - a[0]) * (static_cast<double>(b[2]) - a[2]))) /
                2.0;
    }
    return area;
}

scene3d::ClusterView view_from(float distance) {
    return {.camera = {0.0F, 5.0F, distance},
            .projection = 1.0F / std::tan(0.5F * 75.0F * 3.14159265F / 180.0F),
            .height = 720.0F,
            .near = 0.05F,
            .threshold = 1.0F,
            .scale = 1.0F};
}

// A surface as a glTF model.
std::string surface_gltf(const scene3d::ModelSurface& ground) {
    std::vector<float> positions;
    std::vector<float> normals;
    for (const slash::render::Vertex3D& vertex : ground.vertices) {
        positions.insert(positions.end(), vertex.position.begin(), vertex.position.end());
        normals.insert(normals.end(), vertex.normal.begin(), vertex.normal.end());
    }
    std::vector<std::uint16_t> indices;
    indices.reserve(ground.indices.size());
    // glTF's front faces wind counterclockwise, the engine's clockwise: each triangle turned.
    for (std::size_t t = 0; t + 2 < ground.indices.size(); t += 3) {
        for (const std::size_t k : {std::size_t{0}, std::size_t{2}, std::size_t{1}}) {
            indices.push_back(static_cast<std::uint16_t>(ground.indices.at(t + k)));
        }
    }
    slash::test::Gltf out;
    slash::json::Value primitive;
    primitive["attributes"]["POSITION"] = out.floats(positions, "VEC3");
    primitive["attributes"]["NORMAL"] = out.floats(normals, "VEC3");
    primitive["indices"] = out.indices(indices);
    slash::json::Value mesh;
    mesh["primitives"].push_back(std::move(primitive));
    out.document["meshes"].push_back(std::move(mesh));
    out.document["nodes"] = slash::test::gltf_json(R"([{"name": "Ground", "mesh": 0}])");
    out.document["scenes"] = slash::test::gltf_json(R"([{"nodes": [0]}])");
    out.document["scene"] = 0;
    return out.gltf();
}

// The bumpy ground as a glTF model.
std::string ground_gltf() {
    return surface_gltf(bumpy_ground());
}

// A ball 20 meters across, of 128 x 64 quads.
scene3d::ModelSurface big_ball() {
    slash::world::SphereMesh ball;
    ball.radius = 10.0F;
    ball.height = 20.0F;
    ball.radial_segments = 128;
    ball.rings = 64;
    scene3d::MeshData made = scene3d::make_mesh(scene3d::Primitive{ball}).value();
    scene3d::ModelSurface out;
    out.vertices = std::move(made.vertices);
    out.indices = std::move(made.indices);
    return out;
}

} // namespace

TEST_CASE("a cluster hierarchy holds the surface's triangles, then coarser and coarser groups") {
    const scene3d::ModelSurface ground = bumpy_ground();
    const scene3d::ClusterHierarchy hierarchy = scene3d::build_cluster_hierarchy(ground);
    CHECK(hierarchy.source_triangles == 128U * 128U * 2U);
    CHECK(hierarchy.levels > 3);

    std::size_t finest = 0;
    for (const scene3d::Cluster& cluster : hierarchy.clusters) {
        CHECK(cluster.indices.size() <= 128U * 3U);
        if (cluster.refined < 0) {
            finest += cluster.indices.size() / 3;
            CHECK(cluster.depth == 0);
        } else {
            // Errors only grow toward the coarse end, so a cut is well defined.
            const auto& refined = hierarchy.groups.at(static_cast<std::size_t>(cluster.refined));
            const auto& own = hierarchy.groups.at(static_cast<std::size_t>(cluster.group));
            CHECK(refined.simplified.error <= own.simplified.error);
            CHECK(refined.depth < cluster.depth);
        }
    }
    CHECK(finest == hierarchy.source_triangles);

    // The coarsest groups are never simplified further.
    const auto coarsest = std::ranges::count_if(hierarchy.groups, [](const auto& group) {
        return group.simplified.error == std::numeric_limits<float>::max();
    });
    CHECK(coarsest > 0);

    // The same surface makes the same hierarchy.
    const scene3d::ClusterHierarchy again = scene3d::build_cluster_hierarchy(ground);
    REQUIRE(again.clusters.size() == hierarchy.clusters.size());
    for (std::size_t i = 0; i < again.clusters.size(); ++i) {
        CHECK(again.clusters[i].indices == hierarchy.clusters[i].indices);
    }
}

TEST_CASE("a view's cut follows distance and covers the surface once, with no cracks") {
    const scene3d::ModelSurface ground = bumpy_ground();
    const scene3d::ClusterHierarchy hierarchy = scene3d::build_cluster_hierarchy(ground);

    std::size_t last = std::numeric_limits<std::size_t>::max();
    for (const float distance : {0.0F, 30.0F, 120.0F, 600.0F, 5000.0F}) {
        CAPTURE(distance);
        const std::vector<std::uint32_t> cut =
            scene3d::select_clusters(hierarchy, view_from(distance));
        const std::vector<std::uint32_t> indices = scene3d::cluster_indices(hierarchy, cut);
        const std::size_t triangles = indices.size() / 3;
        CHECK(triangles <= last);
        last = triangles;

        // Seen from above, the cut covers the ground once, with no folds.
        CHECK(area_from_above(ground, indices) == doctest::Approx(1600.0).epsilon(1e-6));
        // Every edge inside the rim joins two triangles: parts drawn at different depths meet.
        for (const auto& [edge, uses] : edges_of(ground, indices)) {
            if (!on_rim(edge.first, edge.second)) {
                CHECK(uses == 2);
            }
        }
    }
    // Up close the ground draws with all its triangles; from far away with a few clusters.
    CHECK(scene3d::cluster_indices(hierarchy, scene3d::select_clusters(hierarchy, view_from(0.0F)))
              .size() >= ground.indices.size() / 2);
    CHECK(last < 2000U);

    // A view from above the near corner, allowing two pixels, draws that corner finer than the far
    // one: count the triangles by where their middles are.
    scene3d::ClusterView corner = view_from(0.0F);
    corner.camera = {-20.0F, 3.0F, -20.0F};
    corner.threshold = 2.0F;
    std::size_t near_triangles = 0;
    std::size_t far_triangles = 0;
    const std::vector<std::uint32_t> seen =
        scene3d::cluster_indices(hierarchy, scene3d::select_clusters(hierarchy, corner));
    for (std::size_t t = 0; t + 2 < seen.size(); t += 3) {
        float x = 0.0F;
        float z = 0.0F;
        for (std::size_t k = 0; k < 3; ++k) {
            x += ground.vertices.at(seen.at(t + k)).position[0];
            z += ground.vertices.at(seen.at(t + k)).position[2];
        }
        near_triangles += x < 0.0F && z < 0.0F ? 1 : 0;
        far_triangles += x > 0.0F && z > 0.0F ? 1 : 0;
    }
    CHECK(near_triangles > far_triangles * 2);
}

TEST_CASE("a jagged surface's hierarchy keeps no folds of a triangle over itself") {
    // Bumps as high as the quads are wide, where the simplifier can fold a triangle back over
    // another; those pairs are taken out, so cuts still cover the ground once.
    const scene3d::ModelSurface ground = bumpy_ground(0.15F);
    const scene3d::ClusterHierarchy hierarchy = scene3d::build_cluster_hierarchy(ground);
    std::map<std::array<std::uint32_t, 3>, int> wound;
    for (const scene3d::Cluster& cluster : hierarchy.clusters) {
        for (std::size_t t = 0; t + 2 < cluster.indices.size(); t += 3) {
            std::array<std::uint32_t, 3> corners{cluster.indices[t], cluster.indices[t + 1],
                                                 cluster.indices[t + 2]};
            std::ranges::rotate(corners, std::ranges::min_element(corners));
            const bool back = corners[2] < corners[1];
            if (back) {
                std::swap(corners[1], corners[2]);
            }
            // Clusters of one simplification share a refined group; one of each way is a fold.
            const int way = back ? 2 : 1;
            int& seen = wound[corners];
            CHECK((seen & (3 - way)) == 0);
            seen |= way;
        }
        wound.clear();
    }
    for (const float distance : {0.0F, 30.0F, 120.0F, 600.0F}) {
        CAPTURE(distance);
        const std::vector<std::uint32_t> indices = scene3d::cluster_indices(
            hierarchy, scene3d::select_clusters(hierarchy, view_from(distance)));
        CHECK(signed_area_from_above(ground, indices) == doctest::Approx(1600.0).epsilon(1e-6));
    }
}

TEST_CASE("a virtualized MeshInstance3D draws the clusters its distance needs") {
    // A world drawn at 320x320 through a camera at the origin looking down -z, the ground below.
    std::map<std::string, std::string, std::less<>> files{{"ground.gltf", ground_gltf()}};
    slash::asset::Assets assets{slash::asset::FileSource{
        .read = [&](std::string_view path) -> slash::Result<std::string> {
            const auto it = files.find(path);
            if (it == files.end()) {
                return slash::make_error(slash::ErrorCode::NotFound, "no such file");
            }
            return it->second;
        },
        .stamp = [&](std::string_view path) -> std::optional<slash::asset::FileStamp> {
            const auto it = files.find(path);
            if (it == files.end()) {
                return std::nullopt;
            }
            return slash::asset::FileStamp{.modified = 1, .size = it->second.size()};
        }}};
    slash::world::ComponentRegistry components;
    slash::world::World world{components};
    slash::world::register_builtin_components(components);
    slash::canvas::install(world, assets);
    slash::ui::Settings ui;
    ui.viewport = {320.0F, 320.0F};
    slash::ui::set_settings(world, ui);
    const entt::entity camera = world.create("camera").value();
    world.ecs().emplace<slash::world::Transform3D>(camera);
    world.ecs().emplace<slash::world::Camera3D>(camera);
    const entt::entity ground = world.create("ground").value();
    world.ecs().emplace<slash::world::Transform3D>(ground).position = {0.0F, -3.0F, -25.0F};
    world.ecs().emplace<slash::world::MeshInstance3D>(ground).mesh = "ground.gltf";

    const auto drawn = [&] {
        const auto captured = slash::canvas::capture(world, assets);
        REQUIRE(captured.ok());
        CHECK(captured.value().meshes == 1);
        return captured.value().clusters;
    };
    // Not virtualized, it draws as a model: no clusters.
    CHECK(drawn() == 0);
    world.ecs().get<slash::world::MeshInstance3D>(ground).virtualized = true;
    const std::size_t near = drawn();
    CHECK(near > 1);
    // Far away, the same ground is cut into fewer, coarser clusters.
    world.ecs().get<slash::world::Transform3D>(ground).position = {0.0F, -3.0F, -2000.0F};
    const std::size_t far = drawn();
    CHECK(far > 0);
    CHECK(far < near);
    // Brought back, it is cut as before.
    world.ecs().get<slash::world::Transform3D>(ground).position = {0.0F, -3.0F, -25.0F};
    CHECK(drawn() == near);
}

TEST_CASE("a cluster's cone faces its triangles' way and culls only clusters seen from behind") {
    slash::world::SphereMesh ball;
    ball.radius = 2.0F;
    ball.height = 4.0F;
    ball.radial_segments = 128;
    ball.rings = 64;
    scene3d::MeshData made = scene3d::make_mesh(scene3d::Primitive{ball}).value();
    scene3d::ModelSurface sphere;
    sphere.vertices = std::move(made.vertices);
    sphere.indices = std::move(made.indices);
    const scene3d::ClusterHierarchy hierarchy = scene3d::build_cluster_hierarchy(sphere);
    REQUIRE(hierarchy.clusters.size() > 8);

    using V = std::array<double, 3>;
    const auto position = [&](std::uint32_t index) {
        const auto& p = sphere.vertices.at(index).position;
        return V{p[0], p[1], p[2]};
    };
    const auto sub = [](const V& a, const V& b) {
        return V{a[0] - b[0], a[1] - b[1], a[2] - b[2]};
    };
    const auto dot = [](const V& a, const V& b) {
        return (a[0] * b[0]) + (a[1] * b[1]) + (a[2] * b[2]);
    };
    const auto cross = [](const V& a, const V& b) {
        return V{(a[1] * b[2]) - (a[2] * b[1]), (a[2] * b[0]) - (a[0] * b[2]),
                 (a[0] * b[1]) - (a[1] * b[0])};
    };
    // A triangle's front, by the side its corners' normals take (so whichever way the engine
    // winds them).
    const auto front_of = [&](const std::vector<std::uint32_t>& indices, std::size_t t) {
        const V a = position(indices.at(t));
        const V n = cross(sub(position(indices.at(t + 1)), a), sub(position(indices.at(t + 2)), a));
        V normals{};
        for (std::size_t k = 0; k < 3; ++k) {
            const auto& from = sphere.vertices.at(indices.at(t + k)).normal;
            normals = {normals[0] + from[0], normals[1] + from[1], normals[2] + from[2]};
        }
        return dot(n, normals) >= 0.0 ? n : V{-n[0], -n[1], -n[2]};
    };

    // Each cone's axis leans the way its triangles face: out of the ball.
    std::size_t coned = 0;
    for (const scene3d::Cluster& cluster : hierarchy.clusters) {
        if (cluster.depth != 0 || cluster.cone.cutoff >= 1.0F) {
            continue;
        }
        ++coned;
        V facing{};
        for (std::size_t t = 0; t + 2 < cluster.indices.size(); t += 3) {
            const V front = front_of(cluster.indices, t);
            facing = {facing[0] + front[0], facing[1] + front[1], facing[2] + front[2]};
        }
        const V axis{cluster.cone.axis.x, cluster.cone.axis.y, cluster.cone.axis.z};
        CHECK(dot(axis, facing) > 0.0);
    }
    CHECK(coned > 4);

    // Seen from 10 meters along +z, the clusters culled as seen from behind are on the far side,
    // and every triangle of them faces away (or, at the poles, has no area).
    scene3d::ClusterView view = view_from(10.0F);
    view.camera = {0.0F, 0.0F, 10.0F};
    const scene3d::ClusterCulling back{.planes = {}, .back_faces = true};
    std::size_t culled = 0;
    for (const scene3d::Cluster& cluster : hierarchy.clusters) {
        if (!scene3d::cluster_culled(cluster, view, back)) {
            continue;
        }
        ++culled;
        CHECK(cluster.bounds.center.z < 0.5F);
        for (std::size_t t = 0; t + 2 < cluster.indices.size(); t += 3) {
            const V to_camera = sub(V{0.0, 0.0, 10.0}, position(cluster.indices.at(t)));
            CHECK(dot(front_of(cluster.indices, t), to_camera) <= 0.0);
        }
    }
    CHECK(culled > hierarchy.clusters.size() / 4);
    // Turned off, nothing is culled that way.
    CHECK(std::ranges::none_of(hierarchy.clusters, [&](const scene3d::Cluster& cluster) {
        return scene3d::cluster_culled(cluster, view, {});
    }));

    // A plane culls just the clusters whose spheres lie wholly beyond it: here x > 0 is out.
    const scene3d::ClusterCulling left{.planes = {{-1.0F, 0.0F, 0.0F, 0.0F}}, .back_faces = false};
    std::size_t beyond = 0;
    for (const scene3d::Cluster& cluster : hierarchy.clusters) {
        const bool out = cluster.bounds.center.x > cluster.bounds.radius;
        CHECK(scene3d::cluster_culled(cluster, view, left) == out);
        beyond += out ? 1 : 0;
    }
    CHECK(beyond > 0);
    // Scaled up in the world, the same spheres reach further, and fewer lie beyond it.
    view.scale = 1000.0F;
    CHECK(std::ranges::none_of(hierarchy.clusters, [&](const scene3d::Cluster& cluster) {
        return scene3d::cluster_culled(cluster, view, left);
    }));
}

TEST_CASE("a virtualized MeshInstance3D leaves out the clusters the camera can't see") {
    std::map<std::string, std::string, std::less<>> files{{"ground.gltf", ground_gltf()},
                                                          {"ball.gltf", surface_gltf(big_ball())}};
    slash::asset::Assets assets{slash::asset::FileSource{
        .read = [&](std::string_view path) -> slash::Result<std::string> {
            const auto it = files.find(path);
            if (it == files.end()) {
                return slash::make_error(slash::ErrorCode::NotFound, "no such file");
            }
            return it->second;
        },
        .stamp = [&](std::string_view path) -> std::optional<slash::asset::FileStamp> {
            const auto it = files.find(path);
            if (it == files.end()) {
                return std::nullopt;
            }
            return slash::asset::FileStamp{.modified = 1, .size = it->second.size()};
        }}};
    slash::world::ComponentRegistry components;
    slash::world::World world{components};
    slash::world::register_builtin_components(components);
    slash::canvas::install(world, assets);
    slash::ui::Settings ui;
    ui.viewport = {160.0F, 120.0F};
    slash::ui::set_settings(world, ui);
    const entt::entity camera = world.create("camera").value();
    world.ecs().emplace<slash::world::Transform3D>(camera);
    world.ecs().emplace<slash::world::Camera3D>(camera);
    // A low sun casting shadows, so clusters out of view still shade what is in it.
    const entt::entity sun = world.create("sun").value();
    world.ecs().emplace<slash::world::Transform3D>(sun).rotation = {-0.3F, 1.2F, 0.0F};
    world.ecs().emplace<slash::world::DirectionalLight3D>(sun).shadow_enabled = true;
    const entt::entity ground = world.create("ground").value();
    world.ecs().emplace<slash::world::Transform3D>(ground).position = {0.0F, -3.0F, -25.0F};
    auto& instance = world.ecs().emplace<slash::world::MeshInstance3D>(ground);
    instance.mesh = "ground.gltf";
    instance.virtualized = true;

    const auto capture = [&](bool cull) {
        slash::canvas::CaptureOptions options;
        options.cull_clusters = cull;
        auto captured = slash::canvas::capture(world, assets, options);
        REQUIRE(captured.ok());
        return std::move(captured).value();
    };
    const auto same_pixels = [&] {
        const slash::canvas::Capture whole = capture(false);
        CHECK(whole.culled == 0);
        const slash::canvas::Capture culled = capture(true);
        CHECK(culled.image.pixels == whole.image.pixels);
        CHECK(culled.clusters + culled.culled == whole.clusters);
        CHECK(culled.meshes == whole.meshes);
        return culled;
    };
    // The ground reaches past both sides of the view near the camera: those clusters are culled,
    // and the frame (shadows included) is the same.
    const slash::canvas::Capture seen = same_pixels();
    CHECK(seen.culled > 0);
    CHECK(seen.clusters > 0);

    // A ball seen from outside: the clusters on its far side face away.
    world.ecs().get<slash::world::MeshInstance3D>(ground).mesh = "ball.gltf";
    world.ecs().get<slash::world::Transform3D>(ground).position = {0.0F, 0.0F, -16.0F};
    const slash::canvas::Capture outside = same_pixels();
    CHECK(outside.culled > 0);
    // From its middle, the camera sees only its backs, which its material culls: every cluster is
    // left out of the camera's draw, which still casts.
    world.ecs().get<slash::world::Transform3D>(ground).position = {0.0F, 0.0F, 0.0F};
    const slash::canvas::Capture backs = same_pixels();
    CHECK(backs.clusters == 0);
    CHECK(backs.culled > 0);
    CHECK(backs.meshes == 1);
    // Casting no shadow, it isn't drawn at all.
    world.ecs().get<slash::world::MeshInstance3D>(ground).cast_shadow = "off";
    CHECK(capture(true).meshes == 0);
    world.ecs().get<slash::world::MeshInstance3D>(ground).cast_shadow = "on";
    world.ecs().get<slash::world::MeshInstance3D>(ground).mesh = "ground.gltf";

    // Behind an occluder, with occlusion culling on, the far clusters are culled too.
    world.ecs().get<slash::world::Transform3D>(ground).position = {0.0F, -3.0F, -25.0F};
    const entt::entity wall = world.create("wall").value();
    world.ecs().emplace<slash::world::Transform3D>(wall).position = {0.0F, 0.0F, -12.0F};
    auto& occluder = world.ecs().emplace<slash::world::OccluderInstance3D>(wall);
    occluder.shape = "quad";
    occluder.size = {80.0F, 30.0F, 1.0F};
    // Occluders draw nothing: a wall where it is hides what it hides.
    world.ecs().emplace<slash::world::MeshInstance3D>(wall);
    world.ecs().emplace<slash::world::BoxMesh>(wall).size = {80.0F, 30.0F, 0.2F};
    slash::canvas::Settings settings = slash::canvas::settings(world);
    settings.occlusion_culling = {.enabled = true, .rays_per_thread = 512, .jitter = false};
    slash::canvas::set_settings(world, settings);
    const slash::canvas::Capture occluded = same_pixels();
    CHECK(occluded.culled > seen.culled);
}
