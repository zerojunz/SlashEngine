#include "slash/asset/assets.hpp"
#include "slash/canvas/canvas.hpp"
#include "slash/core/json.hpp"
#include "slash/core/math.hpp"
#include "slash/navigation/navigation.hpp"
#include "slash/physics3d/physics3d.hpp"
#include "slash/render/device.hpp"
#include "slash/scene3d/csg.hpp"
#include "slash/scene3d/scene3d.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/tree3d.hpp"
#include "slash/world/world.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace asset = slash::asset;
namespace canvas = slash::canvas;
namespace json = slash::json;
namespace navigation = slash::navigation;
namespace physics3d = slash::physics3d;
namespace render = slash::render;
namespace scene3d = slash::scene3d;
namespace world = slash::world;
using slash::Vec2;
using slash::Vec3;

namespace {

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE(parsed.ok());
    return std::move(parsed).value();
}

struct MemoryFiles {
    std::map<std::string, std::string, std::less<>> files;

    asset::FileSource source() {
        return {
            .read = [this](std::string_view path) -> slash::Result<std::string> {
                const auto it = files.find(path);
                if (it == files.end()) {
                    return slash::make_error(slash::ErrorCode::NotFound, "no such file");
                }
                return it->second;
            },
            .stamp = [this](std::string_view path) -> std::optional<asset::FileStamp> {
                const auto it = files.find(path);
                if (it == files.end()) {
                    return std::nullopt;
                }
                return asset::FileStamp{.modified = 1, .size = it->second.size()};
            },
        };
    }
};

// A world with the built-in components; installed keeps CSG meshes as sessions and games do.
struct Scene {
    MemoryFiles files;
    asset::Assets assets{files.source()};
    world::ComponentRegistry components;
    world::World world{components};

    explicit Scene(bool installed = true) {
        world::register_builtin_components(components);
        if (installed) {
            canvas::install(world, assets);
        }
    }

    entt::entity create(std::string_view id, std::string_view components_json) {
        const entt::entity entity = world.create(id).value();
        const json::Value values = parse(components_json);
        for (const auto& [name, fields] : values.as_object()) {
            std::string path;
            const auto assigned = components.find(name)->assign(world, entity, fields, path);
            REQUIRE_MESSAGE(assigned.ok(), (assigned.ok() ? "" : assigned.error().message));
        }
        return entity;
    }

    void adopt(entt::entity parent, entt::entity child) {
        world.ecs().get<world::Transform3D>(parent).children.push_back(child);
    }

    std::shared_ptr<const scene3d::CsgMesh> mesh(entt::entity root) {
        const world::Tree3D tree(world);
        return scene3d::csg_mesh(world, tree, root);
    }
};

float dot(Vec3 a, Vec3 b) {
    return (a.x * b.x) + (a.y * b.y) + (a.z * b.z);
}

Vec3 cross(Vec3 a, Vec3 b) {
    return {(a.y * b.z) - (a.z * b.y), (a.z * b.x) - (a.x * b.z), (a.x * b.y) - (a.y * b.x)};
}

Vec3 vec(const std::array<float, 3>& v) {
    return {v[0], v[1], v[2]};
}

// The volume a mesh's collision triangles enclose; positive when they face out with Godot's
// clockwise winding.
double volume(const scene3d::CsgMesh& mesh) {
    double sum = 0.0;
    const std::vector<Vec3>& faces = *mesh.collision;
    for (std::size_t i = 0; i + 2 < faces.size(); i += 3) {
        sum -= static_cast<double>(dot(faces[i], cross(faces[i + 1], faces[i + 2])));
    }
    return sum / 6.0;
}

std::size_t surface_triangles(const scene3d::CsgSurface& surface) {
    return surface.indices.size() / 3;
}

using Rgba = std::array<std::uint8_t, 4>;

Rgba pixel(const render::Image& image, std::uint32_t x, std::uint32_t y) {
    const std::size_t at = ((std::size_t{y} * image.width) + x) * 4;
    return {image.pixels[at], image.pixels[at + 1], image.pixels[at + 2], image.pixels[at + 3]};
}

std::vector<std::string> messages(const std::vector<scene3d::Problem>& problems) {
    std::vector<std::string> out;
    out.reserve(problems.size());
    for (const scene3d::Problem& problem : problems) {
        out.push_back(problem.path + ": " + problem.message);
    }
    return out;
}

} // namespace

TEST_CASE("a CSG box is Godot's: 12 triangles, flat normals, clockwise faces") {
    Scene scene;
    const entt::entity box =
        scene.create("box", R"({"Transform3D": {}, "CSGBox3D": {"size": [2, 1, 4]}})");
    const auto mesh = scene.mesh(box);
    CHECK(mesh->problems.empty());
    CHECK(mesh->triangles == 12);
    CHECK(mesh->min == Vec3{-1.0F, -0.5F, -2.0F});
    CHECK(mesh->max == Vec3{1.0F, 0.5F, 2.0F});
    CHECK(volume(*mesh) == doctest::Approx(8.0));
    REQUIRE(mesh->surfaces.size() == 1);
    const scene3d::CsgSurface& surface = mesh->surfaces[0];
    CHECK_FALSE(surface.material.standard);
    CHECK(surface_triangles(surface) == 12);
    // Every corner's normal points out of its face, and faces wind clockwise seen from outside.
    std::set<std::tuple<float, float, float>> normals;
    for (std::size_t i = 0; i + 2 < surface.indices.size(); i += 3) {
        const Vec3 a = vec(surface.vertices[surface.indices[i]].position);
        const Vec3 b = vec(surface.vertices[surface.indices[i + 1]].position);
        const Vec3 c = vec(surface.vertices[surface.indices[i + 2]].position);
        const Vec3 n = vec(surface.vertices[surface.indices[i]].normal);
        CHECK(dot(cross(b - a, c - a), n) < 0.0F);
        normals.insert({n.x, n.y, n.z});
    }
    CHECK(normals.size() == 6);
    // With tangents, from MikkTSpace.
    for (const auto& vertex : surface.vertices) {
        CHECK(std::abs(dot(vec(vertex.normal),
                           Vec3{vertex.tangent[0], vertex.tangent[1], vertex.tangent[2]})) < 1e-5F);
    }
}

TEST_CASE(
    "CSG children add, cut and intersect in their parent's order, placed by their transforms") {
    Scene scene;
    const entt::entity root = scene.create("root", R"({"Transform3D": {}, "CSGBox3D": {}})");
    const entt::entity cut = scene.create(
        "cut",
        R"({"Transform3D": {"position": [0.5, 0.5, 0.5]}, "CSGBox3D": {"operation": "subtraction"}})");
    scene.adopt(root, cut);
    auto mesh = scene.mesh(root);
    CHECK(mesh->problems.empty());
    CHECK(volume(*mesh) == doctest::Approx(1.0 - 0.125));

    scene.world.ecs().get<world::CSGBox3D>(cut).operation = "intersection";
    mesh = scene.mesh(root);
    CHECK(volume(*mesh) == doctest::Approx(0.125));
    CHECK(mesh->min == Vec3{0.0F, 0.0F, 0.0F});
    CHECK(mesh->max == Vec3{0.5F, 0.5F, 0.5F});

    scene.world.ecs().get<world::CSGBox3D>(cut).operation = "union";
    scene.world.ecs().get<world::Transform3D>(cut).position = {3.0F, 0.0F, 0.0F};
    mesh = scene.mesh(root);
    CHECK(volume(*mesh) == doctest::Approx(2.0));
    CHECK(mesh->max.x == doctest::Approx(3.5F));

    // A hidden child is left out.
    scene.world.ecs().get<world::Transform3D>(cut).visible = false;
    mesh = scene.mesh(root);
    CHECK(volume(*mesh) == doctest::Approx(1.0));
}

TEST_CASE("a CSGCombiner3D whose first child subtracts makes nothing, as Godot's") {
    Scene scene;
    const entt::entity root = scene.create("root", R"({"Transform3D": {}, "CSGCombiner3D": {}})");
    const entt::entity a =
        scene.create("a", R"({"Transform3D": {}, "CSGBox3D": {"operation": "subtraction"}})");
    const entt::entity b = scene.create("b", R"({"Transform3D": {}, "CSGSphere3D": {}})");
    scene.adopt(root, a);
    scene.adopt(root, b);
    auto mesh = scene.mesh(root);
    // Nothing minus the box, then the sphere added.
    CHECK(mesh->problems.empty());
    CHECK(volume(*mesh) > 0.4);
    CHECK(volume(*mesh) < 0.53);
    // With the sphere first, the box cuts it away.
    auto& children = scene.world.ecs().get<world::Transform3D>(root).children;
    std::ranges::reverse(children);
    mesh = scene.mesh(root);
    CHECK(mesh->triangles == 0);
    CHECK(messages(mesh->problems) ==
          std::vector<std::string>{
              "/components/CSGCombiner3D: makes an empty shape: shapes are usually empty because a "
              "mesh isn't closed (manifold), or a subtraction or intersection leaves nothing"});
}

TEST_CASE("CSG faces keep their shape's material wherever they end up") {
    Scene scene;
    const entt::entity root = scene.create("root", R"({"Transform3D": {}, "CSGBox3D": {}})");
    const entt::entity hole = scene.create("hole", R"({
        "Transform3D": {"position": [0, 0.5, 0]},
        "CSGCylinder3D": {"operation": "subtraction", "radius": 0.25, "height": 0.5},
        "StandardMaterial3D": {"albedo_color": [1, 0, 0, 1]}
    })");
    scene.adopt(root, hole);
    const auto mesh = scene.mesh(root);
    CHECK(mesh->problems.empty());
    REQUIRE(mesh->surfaces.size() == 2);
    // The box's faces first, with Godot's default material, then the hole's walls in red.
    CHECK_FALSE(mesh->surfaces[0].material.standard);
    REQUIRE(mesh->surfaces[1].material.standard);
    CHECK(mesh->surfaces[1].material.standard->albedo_color == slash::Vec4{1.0F, 0.0F, 0.0F, 1.0F});
    // The hole's walls face into it, toward its axis.
    const scene3d::CsgSurface& walls = mesh->surfaces[1];
    for (const auto& vertex : walls.vertices) {
        const Vec3 p = vec(vertex.position);
        const Vec3 n = vec(vertex.normal);
        if (std::abs(n.y) < 0.5F) {
            CHECK(dot(Vec3{p.x, 0.0F, p.z}, n) < 0.0F);
        }
    }
}

TEST_CASE("smooth CSG shapes share normals at their corners, and flip_faces turns them in") {
    Scene scene;
    const entt::entity ball =
        scene.create("ball", R"({"Transform3D": {}, "CSGSphere3D": {"radius": 2}})");
    auto mesh = scene.mesh(ball);
    CHECK(mesh->problems.empty());
    // Godot's sphere: rings * radial_segments * 2 - radial_segments * 2 triangles.
    CHECK(mesh->triangles == (6 * 12 * 2) - (12 * 2));
    REQUIRE(mesh->surfaces.size() == 1);
    for (const auto& vertex : mesh->surfaces[0].vertices) {
        const Vec3 p = vec(vertex.position);
        const Vec3 n = vec(vertex.normal);
        CHECK(dot(n, n) == doctest::Approx(1.0F));
        CHECK(dot(p, n) > 1.9F);
    }
    scene.world.ecs().get<world::CSGSphere3D>(ball).flip_faces = true;
    mesh = scene.mesh(ball);
    CHECK(volume(*mesh) < 0.0);
    for (const auto& vertex : mesh->surfaces[0].vertices) {
        CHECK(dot(vec(vertex.position), vec(vertex.normal)) < -1.9F);
    }
}

TEST_CASE("CSG cylinders, cones and tori have Godot's triangles") {
    Scene scene;
    const entt::entity cylinder =
        scene.create("cylinder", R"({"Transform3D": {}, "CSGCylinder3D": {}})");
    auto mesh = scene.mesh(cylinder);
    CHECK(mesh->problems.empty());
    CHECK(mesh->triangles == 8 * 4);
    scene.world.ecs().get<world::CSGCylinder3D>(cylinder).cone = true;
    mesh = scene.mesh(cylinder);
    CHECK(mesh->triangles == 8 * 2);
    CHECK(mesh->max.y == doctest::Approx(1.0F));

    const entt::entity torus = scene.create("torus", R"({"Transform3D": {}, "CSGTorus3D": {}})");
    mesh = scene.mesh(torus);
    CHECK(mesh->problems.empty());
    CHECK(mesh->triangles == 8 * 6 * 2);
    CHECK(mesh->max.x == doctest::Approx(1.0F));
    // Ring sides at 60 degree steps reach sin(60) of the tube's radius up.
    CHECK(mesh->max.y == doctest::Approx(0.25F * 0.8660254F));

    scene.world.ecs().get<world::CSGTorus3D>(torus).outer_radius = 0.5F;
    mesh = scene.mesh(torus);
    CHECK(mesh->triangles == 0);
    CHECK(messages(mesh->problems) ==
          std::vector<std::string>{
              "/components/CSGTorus3D/inner_radius: equals outer_radius, so the torus has no "
              "thickness"});
}

TEST_CASE("CSGPolygon3D pushes a polygon along -z, or spins it around y") {
    Scene scene;
    const entt::entity prism = scene.create("prism", R"({
        "Transform3D": {},
        "CSGPolygon3D": {"polygon": [[0, 0], [2, 0], [0, 1]], "depth": 3}
    })");
    auto mesh = scene.mesh(prism);
    CHECK(mesh->problems.empty());
    CHECK(volume(*mesh) == doctest::Approx(3.0));
    CHECK(mesh->min == Vec3{0.0F, 0.0F, -3.0F});
    CHECK(mesh->max == Vec3{2.0F, 1.0F, 0.0F});

    // An L, which isn't convex.
    auto& polygon = scene.world.ecs().get<world::CSGPolygon3D>(prism);
    polygon.polygon = {{0, 0}, {2, 0}, {2, 1}, {1, 1}, {1, 2}, {0, 2}};
    polygon.depth = 1.0F;
    mesh = scene.mesh(prism);
    CHECK(mesh->problems.empty());
    CHECK(volume(*mesh) == doctest::Approx(3.0));

    // A square a unit from the axis, spun all the way round: a ring.
    polygon.polygon = {{1, 0}, {2, 0}, {2, 1}, {1, 1}};
    polygon.mode = "spin";
    polygon.spin_sides = 16;
    mesh = scene.mesh(prism);
    CHECK(mesh->problems.empty());
    CHECK(mesh->triangles == 16 * 4 * 2);
    CHECK(mesh->max.x == doctest::Approx(2.0F));
    CHECK(mesh->min.x == doctest::Approx(-2.0F));
    // Half way round has ends.
    polygon.spin_degrees = 180.0F;
    mesh = scene.mesh(prism);
    CHECK(mesh->problems.empty());
    CHECK(mesh->triangles == (16 * 4 * 2) + 4);

    // Edges that cross can't be split into triangles.
    polygon.polygon = {{0, 0}, {1, 1}, {1, 0}, {0, 1}};
    polygon.mode = "depth";
    mesh = scene.mesh(prism);
    CHECK(mesh->triangles == 0);
    REQUIRE_FALSE(mesh->problems.empty());
    CHECK(mesh->problems[0].path == "/components/CSGPolygon3D/polygon");
    polygon.polygon = {{0, 0}, {1, 1}};
    mesh = scene.mesh(prism);
    CHECK(messages(mesh->problems)[0] ==
          "/components/CSGPolygon3D/polygon: has 2 points; a polygon needs at least 3");
}

TEST_CASE("triangulate follows Godot's ear clipping") {
    CHECK(scene3d::triangulate({{0, 0}, {1, 0}, {1, 1}, {0, 1}}) ==
          std::vector<int>{3, 0, 1, 1, 2, 3});
    CHECK(scene3d::triangulate({{0, 0}, {0, 1}, {1, 1}, {1, 0}}) ==
          std::vector<int>{0, 3, 2, 2, 1, 0});
    CHECK(scene3d::triangulate({{0, 0}, {1, 0}}).empty());
    // Three points in a line still close the last triangle, flat.
    CHECK(scene3d::triangulate({{0, 0}, {1, 0}, {2, 0}, {1, 1}}).size() == 6);
}

TEST_CASE("CSGMesh3D takes the entity's primitive mesh, but not a flat one") {
    Scene scene;
    const entt::entity shape = scene.create(
        "shape", R"({"Transform3D": {}, "CSGMesh3D": {}, "BoxMesh": {"size": [1, 2, 1]}})");
    auto mesh = scene.mesh(shape);
    CHECK(mesh->problems.empty());
    CHECK(volume(*mesh) == doctest::Approx(2.0));
    scene.world.ecs().remove<world::BoxMesh>(shape);
    scene.world.ecs().emplace<world::PlaneMesh>(shape);
    mesh = scene.mesh(shape);
    REQUIRE(mesh->problems.size() == 1);
    CHECK(messages(mesh->problems)[0] ==
          "/components/CSGMesh3D/mesh: its PlaneMesh is flat, so it has no inside: use a closed "
          "mesh such as a BoxMesh");
    scene.world.ecs().remove<world::PlaneMesh>(shape);
    mesh = scene.mesh(shape);
    CHECK(mesh->problems[0].path == "/components/CSGMesh3D/mesh");
}

TEST_CASE("CSG meshes are kept until a shape under the root changes") {
    Scene scene;
    const entt::entity root = scene.create("root", R"({"Transform3D": {}, "CSGBox3D": {}})");
    const entt::entity child =
        scene.create("child", R"({"Transform3D": {"position": [0.5, 0, 0]}, "CSGSphere3D": {}})");
    scene.adopt(root, child);
    const auto first = scene.mesh(root);
    CHECK(scene.mesh(root) == first);
    // Moving the root doesn't change its mesh; moving a child does.
    scene.world.ecs().get<world::Transform3D>(root).position = {5.0F, 0.0F, 0.0F};
    CHECK(scene.mesh(root) == first);
    scene.world.ecs().get<world::Transform3D>(child).position = {0.25F, 0.0F, 0.0F};
    const auto second = scene.mesh(root);
    CHECK(second != first);
    CHECK(second->version > first->version);
    const auto memory = scene.world.memory();
    const auto part = std::ranges::find(memory, std::string("csg"), &world::MemoryPart::name);
    REQUIRE(part != memory.end());
    CHECK(part->counts.at("meshes") == 1);
    CHECK(part->bytes > 0);

    // A world without install_csg makes the mesh on each call.
    Scene bare(false);
    const entt::entity box = bare.create("box", R"({"Transform3D": {}, "CSGBox3D": {}})");
    CHECK(bare.mesh(box) != bare.mesh(box));
    CHECK(bare.mesh(box)->triangles == 12);
}

TEST_CASE(
    "CSG roots are the shapes whose parents aren't, and only roots with use_collision collide") {
    Scene scene;
    const entt::entity node = scene.create("node", R"({"Transform3D": {}})");
    const entt::entity root = scene.create(
        "root",
        R"({"Transform3D": {}, "CSGBox3D": {"use_collision": true, "collision_layer": 4}})");
    const entt::entity child =
        scene.create("child", R"({"Transform3D": {}, "CSGSphere3D": {"use_collision": true}})");
    const entt::entity loose = scene.create("loose", R"({"CSGTorus3D": {}})");
    scene.adopt(node, root);
    scene.adopt(root, child);
    const world::Tree3D tree(scene.world);
    CHECK(scene3d::csg_roots(scene.world, tree) == std::vector<entt::entity>{root, loose});
    CHECK(scene3d::is_csg_root(scene.world, tree, root));
    CHECK_FALSE(scene3d::is_csg_root(scene.world, tree, child));
    const auto bodies = scene3d::csg_bodies(scene.world, tree);
    REQUIRE(bodies.size() == 1);
    CHECK(bodies[0].entity == root);
    CHECK(bodies[0].layer == 4);
    CHECK(bodies[0].faces->size() == scene.mesh(root)->triangles * 3);
}

TEST_CASE("check_csg reports shapes that can't be made") {
    Scene scene;
    scene.create("two", R"({"Transform3D": {}, "CSGBox3D": {}, "CSGSphere3D": {}})");
    scene.create("drawn", R"({"Transform3D": {}, "CSGBox3D": {}, "MeshInstance3D": {}})");
    scene.create("model", R"({"Transform3D": {}, "CSGMesh3D": {"mesh": "missing.glb"}})");
    CHECK(messages(scene3d::check_csg(scene.world)) ==
          std::vector<std::string>{
              "/components: has more than one CSG shape (CSGBox3D and CSGSphere3D): keep one",
              "/components/MeshInstance3D: is on a CSG shape, which draws itself: remove the "
              "MeshInstance3D",
              "/components/CSGMesh3D/mesh: asset \"missing.glb\" could not be read: no such file"});
}

TEST_CASE("a CSG root with use_collision is a static body that bodies rest on and rays hit") {
    Scene scene;
    physics3d::install(scene.world);
    // A floor whose top is at y 0, with a pit 0.75 deep cut into it.
    const entt::entity floor = scene.create("floor", R"({"Transform3D": {"position": [0, -0.5, 0]},
        "CSGBox3D": {"size": [20, 1, 20], "use_collision": true}})");
    const entt::entity pit = scene.create("pit", R"({"Transform3D": {"position": [0, 0.25, 0]},
        "CSGBox3D": {"size": [2, 1, 2], "operation": "subtraction"}})");
    scene.adopt(floor, pit);
    scene.create("crate", R"({"Transform3D": {"position": [0, 3, 0]}, "RigidBody3D": {},
        "CollisionShape3D": {}})");
    scene.create("box", R"({"Transform3D": {"position": [5, 3, 0]}, "RigidBody3D": {},
        "CollisionShape3D": {}})");
    for (int i = 0; i < 300; ++i) {
        scene.world.step(1.0 / 60.0);
    }
    const auto y = [&](std::string_view id) {
        return scene.world.ecs().get<world::Transform3D>(scene.world.find(id)).position.y;
    };
    CHECK(y("crate") == doctest::Approx(-0.25F).epsilon(0.02));
    CHECK(y("box") == doctest::Approx(0.5F).epsilon(0.02));

    const auto down = [&](float x) {
        return physics3d::raycast(scene.world, {x, 5.0F, 3.0F}, {x, -5.0F, 3.0F});
    };
    const auto ground = down(8.0F);
    REQUIRE(ground.has_value());
    CHECK(ground->entity == floor);
    CHECK(ground->point.y == doctest::Approx(0.0F).epsilon(0.001));
    CHECK(ground->normal.y == doctest::Approx(1.0F));

    // Moving the pit moves the hole in the body.
    scene.world.ecs().get<world::Transform3D>(pit).position = {8.0F, 0.25F, 3.0F};
    scene.world.step(1.0 / 60.0);
    const auto in_pit = down(8.0F);
    REQUIRE(in_pit.has_value());
    CHECK(in_pit->point.y == doctest::Approx(-0.75F).epsilon(0.001));

    // Without use_collision it is no body at all.
    scene.world.ecs().get<world::CSGBox3D>(floor).use_collision = false;
    scene.world.step(1.0 / 60.0);
    CHECK_FALSE(down(8.0F).has_value());
}

TEST_CASE("navigation bakes take a CSG root's mesh, and its collision only when it collides") {
    Scene scene;
    navigation::install(scene.world, scene.assets);
    const entt::entity floor = scene.create("floor", R"({"Transform3D": {},
        "CSGBox3D": {"size": [10, 1, 10], "collision_layer": 2}})");
    const entt::entity pit = scene.create("pit", R"({"Transform3D": {"position": [0, 0.5, 0]},
        "CSGCylinder3D": {"operation": "subtraction"}})");
    scene.adopt(floor, pit);
    const entt::entity level =
        scene.create("level", R"({"Transform3D": {}, "NavigationRegion3D": {}})");
    scene.adopt(level, floor);
    const std::size_t made = scene.mesh(floor)->triangles;
    CHECK(made > 12);
    const auto parsed = [&](std::string_view type, std::uint32_t mask) {
        navigation::NavigationMesh settings;
        auto read = navigation::parse_mesh(parse(
            R"({"format": 1, "geometry_parsed_geometry_type": ")" + std::string(type) + "\"}"));
        REQUIRE(read.ok());
        settings = read.value();
        settings.geometry_collision_mask = mask;
        return navigation::parse_source_geometry(scene.world, scene.assets, level, settings)
                   .indices.size() /
               3;
    };
    // Once for mesh_instances and both, whatever its collision; the pit under it adds nothing.
    CHECK(parsed("mesh_instances", 1) == made);
    CHECK(parsed("both", 1) == made);
    CHECK(parsed("static_colliders", 2) == 0);
    scene.world.ecs().get<world::CSGBox3D>(floor).use_collision = true;
    CHECK(parsed("static_colliders", 1) == 0);
    CHECK(parsed("static_colliders", 2) == made);

    // A bake walks round the pit, whose radius is 0.5.
    navigation::NavigationMesh settings;
    auto baked = navigation::bake(
        settings, navigation::parse_source_geometry(scene.world, scene.assets, level, settings));
    REQUIRE(baked.ok());
    CHECK_FALSE(baked.value().polygons.empty());
    float nearest = 100.0F;
    for (const Vec3 vertex : baked.value().vertices) {
        nearest = std::min(nearest, std::sqrt((vertex.x * vertex.x) + (vertex.z * vertex.z)));
    }
    CHECK(nearest > 0.5F);
    CHECK(nearest < 1.5F);
}

TEST_CASE("CSG roots draw as Godot's root shapes, the same pixels as a primitive mesh") {
    const auto draw = [](bool csg) {
        Scene scene;
        scene.create("camera",
                     R"({"Transform3D": {"position": [1, 1.2, 3], "rotation": [-0.35, 0.3, 0]},
                                   "Camera3D": {}})");
        scene.create("sun", R"({"Transform3D": {"rotation": [-0.8, 0.6, 0]},
                                "DirectionalLight3D": {}})");
        if (csg) {
            scene.create("cube", R"({"Transform3D": {}, "CSGBox3D": {},
                                     "StandardMaterial3D": {"albedo_color": [1, 0.5, 0.2, 1]}})");
        } else {
            scene.create("cube", R"({"Transform3D": {}, "MeshInstance3D": {}, "BoxMesh": {},
                                     "StandardMaterial3D": {"albedo_color": [1, 0.5, 0.2, 1]}})");
        }
        auto captured = canvas::capture(scene.world, scene.assets);
        REQUIRE(captured.ok());
        CHECK(captured.value().meshes == 1);
        return std::move(captured.value().image);
    };
    const render::Image box = draw(false);
    const render::Image csg = draw(true);
    REQUIRE(box.pixels.size() == csg.pixels.size());
    int lit = 0;
    for (std::uint32_t y = 0; y < box.height; ++y) {
        for (std::uint32_t x = 0; x < box.width; ++x) {
            const Rgba want = pixel(box, x, y);
            const Rgba got = pixel(csg, x, y);
            for (std::size_t c = 0; c < 4; ++c) {
                CHECK(std::abs(int{got[c]} - int{want[c]}) <= 1);
            }
            lit += want[0] > 0 ? 1 : 0;
        }
    }
    CHECK(lit > 100);

    // A child shape draws nothing of its own; a hidden root draws nothing.
    Scene scene;
    scene.create("camera", R"({"Transform3D": {"position": [0, 0, 3]}, "Camera3D": {}})");
    const entt::entity root = scene.create("root", R"({"Transform3D": {}, "CSGBox3D": {},
        "StandardMaterial3D": {"shading_mode": "unshaded", "albedo_color": [1, 0, 0, 1]}})");
    const entt::entity door = scene.create("door", R"({"Transform3D": {}, "CSGBox3D": {
        "size": [0.5, 0.5, 2], "operation": "subtraction"}})");
    scene.adopt(root, door);
    const auto device = render::create_null_device();
    scene3d::Renderer renderer(*device, nullptr);
    // Two surfaces: the root's material and the door's faces, in Godot's default material.
    REQUIRE(renderer.prepare(scene.world, 64.0F, 64.0F, 1.0, {}).has_value());
    CHECK(renderer.meshes() == 2);
    CHECK(renderer.meshes_kept() == 2);
    scene.world.ecs().get<world::Transform3D>(root).visible = false;
    (void)renderer.prepare(scene.world, 64.0F, 64.0F, 1.0, {});
    CHECK(renderer.meshes() == 0);
    CHECK(renderer.meshes_kept() == 0);
    scene.world.ecs().get<world::Transform3D>(root).visible = true;
    auto captured = canvas::capture(scene.world, scene.assets);
    REQUIRE(captured.ok());
    const render::Image& image = captured.value().image;
    // The cube's front reaches 0.26 of the way from the middle to the top, and the door 0.13.
    const std::uint32_t middle = image.width / 2;
    const std::uint32_t above = (image.height / 2) - (image.height / 10);
    CHECK(pixel(image, middle, image.height / 2)[0] < 128);     // through the door
    CHECK(pixel(image, middle, above) == Rgba{255, 0, 0, 255}); // the wall above it
}
