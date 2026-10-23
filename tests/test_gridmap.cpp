#include "slash/asset/assets.hpp"
#include "slash/canvas/canvas.hpp"
#include "slash/core/json.hpp"
#include "slash/core/math.hpp"
#include "slash/navigation/navigation.hpp"
#include "slash/physics3d/physics3d.hpp"
#include "slash/render/device.hpp"
#include "slash/scene3d/gridmap.hpp"
#include "slash/scene3d/scene3d.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
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
using slash::Vec3;
using slash::Vec3i;

namespace {

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE(parsed.ok());
    return std::move(parsed).value();
}

struct MemoryFiles {
    std::map<std::string, std::string, std::less<>> files;
    std::int64_t modified = 1;

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
                return asset::FileStamp{.modified = modified, .size = it->second.size()};
            },
        };
    }

    world::FileReader reader() {
        return [this](std::string_view path) -> slash::Result<std::string> {
            const auto it = files.find(path);
            if (it == files.end()) {
                return slash::make_error(slash::ErrorCode::NotFound, "no such file");
            }
            return it->second;
        };
    }
};

// Three boxes and a triangle from an OBJ file: a block and a pillar of one material, a thin
// green floor tile whose top is at the cell's center, which walks on a navigation mesh, and a
// ramp of the model's own triangle.
constexpr std::string_view kLibrary = R"({
  "format": 1,
  "items": {
    "block": {
      "primitive": {"BoxMesh": {"size": [2, 2, 2]}},
      "material": {"albedo_color": [1, 0, 0, 1]},
      "shapes": [{"shape": "box", "size": [2, 2, 2]}]
    },
    "floor": {
      "primitive": {"BoxMesh": {"size": [2, 0.2, 2]}},
      "material": {"albedo_color": [0, 1, 0, 1]},
      "mesh_transform": {"position": [0, -0.1, 0]},
      "shapes": [{"shape": "box", "size": [2, 0.2, 2], "position": [0, -0.1, 0]}],
      "navigation_mesh": "floor.navmesh.json",
      "navigation_layers": 3
    },
    "pillar": {
      "primitive": {"BoxMesh": {"size": [1, 2, 1]}},
      "material": {"albedo_color": [1, 0, 0, 1]}
    },
    "ramp": {"mesh": "ramp.obj", "mesh_cast_shadow": "off"}
  }
})";

constexpr std::string_view kFloorNavigation = R"({
  "format": 1,
  "vertices": [[-1, 0, -1], [-1, 0, 1], [1, 0, 1], [1, 0, -1]],
  "polygons": [[0, 1, 2, 3]]
})";

constexpr std::string_view kRamp = "v 0 0 0\nv 1 0 0\nv 0 1 1\nf 1 2 3\n";

// A world with the built-in components and the library's files; installed keeps GridMap meshes
// as sessions and games do.
struct Scene {
    MemoryFiles files;
    asset::Assets assets{files.source()};
    world::ComponentRegistry components;
    world::World world{components};

    explicit Scene(bool installed = true) {
        files.files.emplace("lib.meshlib.json", kLibrary);
        files.files.emplace("floor.navmesh.json", kFloorNavigation);
        files.files.emplace("ramp.obj", kRamp);
        world::register_builtin_components(components);
        world.set_file_reader(files.reader());
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

    world::GridMap& map(entt::entity entity) { return world.ecs().get<world::GridMap>(entity); }

    std::shared_ptr<const scene3d::GridMesh> mesh(entt::entity entity) {
        return scene3d::grid_mesh(world, entity);
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

// The corners of the box around a surface's vertices.
std::pair<Vec3, Vec3> bounds(const scene3d::MeshSurface& surface) {
    Vec3 low{1e9F, 1e9F, 1e9F};
    Vec3 high{-1e9F, -1e9F, -1e9F};
    for (const auto& vertex : surface.vertices) {
        const Vec3 p = vec(vertex.position);
        low = {std::min(low.x, p.x), std::min(low.y, p.y), std::min(low.z, p.z)};
        high = {std::max(high.x, p.x), std::max(high.y, p.y), std::max(high.z, p.z)};
    }
    return {low, high};
}

bool near(Vec3 a, Vec3 b) {
    return std::abs(a.x - b.x) < 1e-4F && std::abs(a.y - b.y) < 1e-4F &&
           std::abs(a.z - b.z) < 1e-4F;
}

std::vector<std::string> messages(const std::vector<scene3d::Problem>& problems) {
    std::vector<std::string> out;
    out.reserve(problems.size());
    for (const scene3d::Problem& problem : problems) {
        out.push_back(problem.path + ": " + problem.message);
    }
    return out;
}

std::vector<std::string> messages(const std::vector<json::ValidationError>& errors) {
    std::vector<std::string> out;
    out.reserve(errors.size());
    for (const json::ValidationError& error : errors) {
        out.push_back(error.path + ": " + error.message);
    }
    return out;
}

world::GridMap grid(std::string_view text) {
    world::ComponentRegistry components;
    world::register_builtin_components(components);
    world::World scratch(components);
    world::GridMap map;
    std::string path;
    REQUIRE(world::from_json(scratch, parse(text), map, path).ok());
    return map;
}

using Rgba = std::array<std::uint8_t, 4>;

Rgba pixel(const render::Image& image, std::uint32_t x, std::uint32_t y) {
    const std::size_t at = ((std::size_t{y} * image.width) + x) * 4;
    return {image.pixels[at], image.pixels[at + 1], image.pixels[at + 2], image.pixels[at + 3]};
}

} // namespace

TEST_CASE("a cell's orientation is one of Godot's 24 turns") {
    CHECK(scene3d::orientation_basis(0) == world::Affine3D{});
    // 16, 10 and 22 turn 90, 180 and 270 degrees about y, as Godot's editor does with S.
    const auto turned = [](int orientation, Vec3 v) {
        return scene3d::orientation_basis(orientation).basis(v);
    };
    CHECK(near(turned(16, {1, 0, 0}), {0, 0, -1}));
    CHECK(near(turned(10, {1, 0, 0}), {-1, 0, 0}));
    CHECK(near(turned(22, {1, 0, 0}), {0, 0, 1}));
    CHECK(near(turned(16, {0, 1, 0}), {0, 1, 0}));
    // Every one is a turn, none mirrors, all differ, and each is found again from its basis.
    std::set<std::array<float, 9>> seen;
    for (int i = 0; i < scene3d::kOrientations; ++i) {
        const world::Affine3D basis = scene3d::orientation_basis(i);
        CHECK(dot(basis.x, cross(basis.y, basis.z)) == doctest::Approx(1.0F));
        seen.insert({basis.x.x, basis.x.y, basis.x.z, basis.y.x, basis.y.y, basis.y.z, basis.z.x,
                     basis.z.y, basis.z.z});
        CHECK(scene3d::orientation_index(basis) == i);
    }
    CHECK(seen.size() == 24);
    // A basis near one is rounded to it; none near any, and indices out of range, are no turn.
    world::Affine3D tilted = scene3d::orientation_basis(16);
    tilted.x.y = 0.2F;
    CHECK(scene3d::orientation_index(tilted) == 16);
    CHECK(scene3d::orientation_index(world::Affine3D{
              .x = {0.7F, 0.7F, 0}, .y = {-0.7F, 0.7F, 0}, .z = {0, 0, 1}, .origin = {}}) == 0);
    CHECK(scene3d::orientation_basis(24) == world::Affine3D{});
    CHECK(scene3d::orientation_basis(-1) == world::Affine3D{});
}

TEST_CASE("legend values name an item, then its orientation") {
    CHECK(scene3d::grid_item_text({.item = "wall", .orientation = 0}) == "wall");
    CHECK(scene3d::grid_item_text({.item = "wall", .orientation = 16}) == "wall:16");
    const auto item = scene3d::parse_grid_item("wall:16");
    REQUIRE(item.ok());
    CHECK(item.value() == scene3d::GridItem{.item = "wall", .orientation = 16});
    CHECK(scene3d::parse_grid_item("wall").value() == scene3d::GridItem{.item = "wall"});
    const auto error = [](std::string_view text) {
        return scene3d::parse_grid_item(text).error().message;
    };
    CHECK(error(":3") == R"(":3" names no item: write the item's name, then ":" and an )"
                         "orientation if it is turned");
    CHECK(error("wall:") == R"("wall:": after the ':' comes an orientation, 0 to 23)");
    CHECK(error("wall:x") == R"("wall:x": after the ':' comes an orientation, 0 to 23)");
    CHECK(error("wall:24") == R"("wall:24": 24 is not one of Godot's 24 orientations, 0 to 23)");
}

TEST_CASE("GridMap cells are floors of rows of legend keys, and encode back in canonical form") {
    const world::GridMap map = grid(R"({
        "legend": {"w": "wall", "f": "floor:16"},
        "floors": [{"y": 0, "origin": [-1, 2], "rows": ["ff.w", "", "w"]},
                   {"y": 2, "rows": ["w"]}]})");
    const auto cells = scene3d::decode_grid_cells(map);
    REQUIRE(cells.ok());
    const scene3d::GridItem wall{.item = "wall"};
    const scene3d::GridItem floor{.item = "floor", .orientation = 16};
    CHECK(cells.value() == scene3d::GridCells{{{-1, 0, 2}, floor},
                                              {{0, 0, 2}, floor},
                                              {{2, 0, 2}, wall},
                                              {{-1, 0, 4}, wall},
                                              {{0, 2, 0}, wall}});

    // Encoded afresh: the most used item gets its first letter first.
    world::GridMap fresh;
    scene3d::encode_grid_cells(cells.value(), fresh);
    CHECK(fresh.legend == map.legend);
    CHECK(fresh.floors == map.floors);

    // Keys keep what they stand for, floors go lowest first, and each starts at its cells.
    world::GridMap messy = grid(R"({
        "legend": {"x": "wall", "f": "floor:16", "u": "unused"},
        "floors": [{"y": 2, "origin": [-3, -1], "rows": ["", "...x."]},
                   {"y": 0, "origin": [-2, 2], "rows": [".ff.x", "", ".x", ""]}]})");
    const auto read = scene3d::decode_grid_cells(messy);
    REQUIRE(read.ok());
    CHECK(read.value().size() == 5);
    scene3d::encode_grid_cells(read.value(), messy);
    CHECK(messy.legend == std::map<std::string, std::string>{{"x", "wall"}, {"f", "floor:16"}});
    REQUIRE(messy.floors.size() == 2);
    CHECK(messy.floors[0] ==
          world::GridFloor{.y = 0, .origin = {-1, 2}, .rows = {"ff.x", "", "x"}});
    CHECK(messy.floors[1] == world::GridFloor{.y = 2, .origin = {0, 0}, .rows = {"x"}});
    CHECK(scene3d::decode_grid_cells(messy).value() == read.value());

    // No cells, no legend or floors.
    scene3d::encode_grid_cells({}, messy);
    CHECK(messy.legend.empty());
    CHECK(messy.floors.empty());

    // Past 91 items, keys are two characters.
    scene3d::GridCells many;
    for (int i = 0; i < 100; ++i) {
        many.emplace(Vec3i{i, 0, 0}, scene3d::GridItem{.item = "item" + std::to_string(i)});
    }
    world::GridMap wide;
    scene3d::encode_grid_cells(many, wide);
    CHECK(wide.legend.size() == 100);
    CHECK(wide.legend.begin()->first.size() == 2);
    CHECK(wide.floors.at(0).rows.at(0).size() == 200);
    CHECK(scene3d::decode_grid_cells(wide).value() == many);
}

TEST_CASE("validate_grid_cells finds every error, each with its JSON Pointer") {
    const world::GridMap map = grid(R"({
        "legend": {"a": "wall", "bc": "floor", "d": "door:99", ".": "hole"},
        "floors": [{"y": 0, "rows": ["a"]}]})");
    CHECK(messages(scene3d::validate_grid_cells(map)) ==
          std::vector<std::string>{
              "/legend/.: a key is one to three letters, digits or punctuation marks other than "
              "'.', '\"' and backslash",
              "/legend/bc: keys are all the same length: \"bc\" is 2 characters, \"a\" 1",
              "/legend/d: \"door:99\": 99 is not one of Godot's 24 orientations, 0 to 23"});
    const world::GridMap rows = grid(R"({
        "legend": {"ab": "wall"},
        "floors": [{"y": 0, "rows": ["ab", "abc", "ab..zz"]},
                   {"y": 40000},
                   {"y": 1, "origin": [32767, 0], "rows": ["abab"]},
                   {"y": 0}]})");
    CHECK(messages(scene3d::validate_grid_cells(rows)) ==
          std::vector<std::string>{
              "/floors/0/rows/1: is 3 characters, not a whole number of 2-character keys",
              "/floors/0/rows/2: column 2: \"zz\" is not in the legend",
              "/floors/1/y: cells are from -32768 to 32767 in each direction",
              "/floors/2/rows/0: reaches past 32767, the last cell along x or z",
              "/floors/3/y: another floor is at y 0: put their rows in one floor"});
    CHECK(scene3d::decode_grid_cells(rows).error().message ==
          "/floors/0/rows/1: is 3 characters, not a whole number of 2-character keys");
}

TEST_CASE("cells and points convert as Godot's map_to_local and local_to_map") {
    world::GridMap map = grid(R"({"cell_size": [2, 1, 3]})");
    CHECK(scene3d::map_to_local(map, {1, -1, 2}) == Vec3{3.0F, -0.5F, 7.5F});
    CHECK(scene3d::local_to_map(map, {3.0F, -0.5F, 7.5F}) == Vec3i{1, -1, 2});
    CHECK(scene3d::local_to_map(map, {-0.1F, 0.0F, 2.99F}) == Vec3i{-1, 0, 0});
    map.cell_center_y = false;
    CHECK(scene3d::map_to_local(map, {1, -1, 2}) == Vec3{3.0F, -1.0F, 7.5F});
    // A cell's transform turns its item, scales it by cell_scale and puts it at the cell.
    map.cell_scale = 0.5F;
    const world::Affine3D placed = scene3d::cell_transform(map, {1, -1, 2}, 16);
    CHECK(near(placed.x, {0, 0, -0.5F}));
    CHECK(near(placed.y, {0, 0.5F, 0}));
    CHECK(placed.origin == Vec3{3.0F, -1.0F, 7.5F});
    // Octants of 8 cells each way, the cells below 0 in their own, as Godot's.
    const auto octant = [&](int x) { return scene3d::octant_of(map, {x, 0, 0}).x; };
    CHECK(octant(0) == 0);
    CHECK(octant(7) == 0);
    CHECK(octant(8) == 1);
    CHECK(octant(-1) == -1);
    CHECK(octant(-8) == -1);
    CHECK(octant(-9) == -2);
    map.cell_octant_size = 1;
    CHECK(octant(-3) == -3);
}

TEST_CASE("mesh libraries hold items by name, checked as they load") {
    auto library = scene3d::parse_mesh_library(parse(kLibrary));
    REQUIRE(library.ok());
    const auto& items = library.value().items;
    REQUIRE(items.size() == 4);
    const scene3d::MeshLibraryItem& floor = items.at("floor");
    REQUIRE(floor.primitive.has_value());
    CHECK(std::get<world::BoxMesh>(*floor.primitive).size == Vec3{2.0F, 0.2F, 2.0F});
    CHECK(floor.material->albedo_color.y == 1.0F);
    CHECK(floor.mesh_transform.origin == Vec3{0.0F, -0.1F, 0.0F});
    REQUIRE(floor.shapes.size() == 1);
    CHECK(floor.shapes[0].position == Vec3{0.0F, -0.1F, 0.0F});
    CHECK(floor.navigation_mesh == "floor.navmesh.json");
    CHECK(floor.navigation_layers == 3);
    CHECK(items.at("ramp").mesh == "ramp.obj");
    CHECK(items.at("ramp").mesh_cast_shadow == "off");

    CHECK(messages(scene3d::validate_mesh_library(parse(R"({"items": {}})"))).front() ==
          ": missing required property \"format\"");
    CHECK(messages(scene3d::validate_mesh_library(parse(R"({"format": 1, "items": {
        "a:b": {},
        "rock": {"mesh": "rock.png"},
        "both": {"mesh": "rock.glb", "primitive": {"BoxMesh": {}}},
        "ring": {"primitive": {"TorusMesh": {"inner_radius": 1, "outer_radius": 1}}},
        "plane": {"shapes": [{"shape": "world_boundary"}]},
        "walk": {"navigation_mesh": "walk.json"}}})"))) ==
          std::vector<std::string>{
              "/items/a:b: an item's name has no ':', which comes before an orientation in a "
              "GridMap's legend",
              "/items/both: has both a mesh and a primitive: keep one",
              "/items/plane/shapes/0/shape: a world boundary can't be in a cell: it is infinite",
              "/items/ring/primitive/TorusMesh: a TorusMesh's inner_radius and outer_radius "
              "can't be the same",
              "/items/rock/mesh: is not a .gltf, .glb, .obj, .fbx or .blend file",
              "/items/walk/navigation_mesh: is not a .navmesh.json file"});
    CHECK(scene3d::parse_mesh_library(parse(R"({"format": 1, "items": {"a:b": {}}})"))
              .error()
              .message == "/items/a:b: an item's name has no ':', which comes before an "
                          "orientation in a GridMap's legend");
}

TEST_CASE("a GridMap's octants merge their cells' meshes by material, kept until cells change") {
    Scene scene;
    const entt::entity entity = scene.create("grid", R"({"Transform3D": {},
        "GridMap": {"mesh_library": "lib.meshlib.json",
                    "legend": {"b": "block", "p": "pillar", "f": "floor", "r": "ramp"},
                    "floors": [{"y": 0, "rows": ["bbp......b", "f", "r"]}]}})");
    const auto mesh = scene.mesh(entity);
    CHECK(messages(mesh->problems).empty());
    CHECK(mesh->cells == 6);
    REQUIRE(mesh->octants.size() == 2);
    const scene3d::GridOctant& first = *mesh->octants[0];
    CHECK(first.key == Vec3i{0, 0, 0});
    // Red blocks and the pillar share a surface; the floor and the ramp have their own.
    REQUIRE(first.surfaces.size() == 3);
    const scene3d::GridSurface& red = first.surfaces[0];
    CHECK(red.mesh.material.standard->albedo_color.x == 1.0F);
    CHECK(red.mesh.material.entity == entity);
    CHECK(red.mesh.indices.size() / 3 == 36);
    const auto [low, high] = bounds(red.mesh);
    CHECK(near(low, {0, 0, 0}));
    CHECK(near(high, {5.5F, 2, 2}));
    // The floor tile's top is at its cell's center.
    const auto [floor_low, floor_high] = bounds(first.surfaces[1].mesh);
    CHECK(near(floor_low, {0, 0.8F, 2}));
    CHECK(near(floor_high, {2, 1, 4}));
    // The ramp's triangle is the model's, in Godot's default material, casting no shadow.
    const scene3d::GridSurface& ramp = first.surfaces[2];
    CHECK_FALSE(ramp.mesh.material.standard);
    CHECK(ramp.cast_shadow == "off");
    CHECK(ramp.mesh.indices.size() == 3);
    CHECK(first.triangles == 36 + 12 + 1);
    CHECK(mesh->octants[1]->key == Vec3i{1, 0, 0});
    CHECK(mesh->triangles == 36 + 12 + 1 + 12);
    // Shapes in every cell that has them, and the floor's navigation region.
    REQUIRE(mesh->shapes->size() == 4);
    CHECK(mesh->shapes->at(3).cell == Vec3i{0, 0, 1});
    CHECK(mesh->shapes->at(3).local.origin == Vec3{1.0F, 0.9F, 3.0F});
    REQUIRE(mesh->regions.size() == 1);
    CHECK(mesh->regions[0].cell == Vec3i{0, 0, 1});
    CHECK(mesh->regions[0].navigation_layers == 3);
    CHECK(mesh->regions[0].local.origin == Vec3{1.0F, 1.0F, 3.0F});

    // Kept until a cell changes, then only its octant is made again.
    CHECK(scene.mesh(entity) == mesh);
    scene.map(entity).floors[0].rows[0] = "bbp......p";
    const auto changed = scene.mesh(entity);
    CHECK(changed != mesh);
    CHECK(changed->octants[0] == mesh->octants[0]);
    CHECK(changed->octants[1] != mesh->octants[1]);
    CHECK(changed->octants[1]->version > mesh->octants[1]->version);
    // A new cell size moves every cell.
    scene.map(entity).cell_size = {1.0F, 1.0F, 1.0F};
    CHECK(scene.mesh(entity)->octants[0] != changed->octants[0]);
    const auto memory = scene.world.memory();
    const auto part = std::ranges::find(memory, std::string("gridmap"), &world::MemoryPart::name);
    REQUIRE(part != memory.end());
    CHECK(part->counts.at("gridmaps") == 1);
    CHECK(part->counts.at("octants") == 2);
    CHECK(part->counts.at("cells") == 6);
    CHECK(part->bytes > 0);

    // A world without install_gridmap makes them on each call, reading its files.
    Scene bare(false);
    const entt::entity loose = bare.create("grid", R"({"Transform3D": {},
        "GridMap": {"mesh_library": "lib.meshlib.json", "legend": {"b": "block"},
                    "floors": [{"y": 0, "rows": ["b"]}]}})");
    CHECK(bare.mesh(loose) != bare.mesh(loose));
    CHECK(bare.mesh(loose)->triangles == 12);
}

TEST_CASE("orientation and cell_scale turn and scale items; mirrored ones keep facing out") {
    Scene scene;
    scene.files.files.emplace("long.meshlib.json", R"({"format": 1, "items": {
        "beam": {"primitive": {"BoxMesh": {"size": [2, 0.5, 0.5]}}},
        "mirror": {"primitive": {"BoxMesh": {}}, "mesh_transform": {"scale": [-1, 1, 1]}}}})");
    const entt::entity entity = scene.create("grid", R"({"Transform3D": {},
        "GridMap": {"mesh_library": "long.meshlib.json", "cell_scale": 0.5,
                    "legend": {"b": "beam:16"}, "floors": [{"y": 0, "rows": ["b"]}]}})");
    // Turned to run along z, at half size.
    const auto [low, high] = bounds(scene.mesh(entity)->octants[0]->surfaces[0].mesh);
    CHECK(near(low, {0.875F, 0.875F, 0.5F}));
    CHECK(near(high, {1.125F, 1.125F, 1.5F}));

    scene.map(entity).legend = {{"b", "mirror"}};
    const scene3d::MeshSurface& surface = scene.mesh(entity)->octants[0]->surfaces[0].mesh;
    REQUIRE(surface.indices.size() == 36);
    for (std::size_t i = 0; i + 2 < surface.indices.size(); i += 3) {
        const Vec3 a = vec(surface.vertices[surface.indices[i]].position);
        const Vec3 b = vec(surface.vertices[surface.indices[i + 1]].position);
        const Vec3 c = vec(surface.vertices[surface.indices[i + 2]].position);
        const Vec3 n = vec(surface.vertices[surface.indices[i]].normal);
        CHECK(dot(cross(b - a, c - a), n) < 0.0F); // clockwise from outside, as Godot's
        CHECK(dot(n, a - Vec3{1, 1, 1}) > 0.0F);   // normals point out of the box
    }
}

TEST_CASE("check_gridmaps reports cells that can't be made") {
    Scene scene;
    scene.files.files.emplace("broken.meshlib.json",
                              R"({"format": 1, "items": {"rock": {"mesh": "rock.obj"}}})");
    scene.create("small", R"({"Transform3D": {}, "GridMap": {"mesh_library": "lib.meshlib.json",
        "cell_size": [0, 1, 1]}})");
    scene.create("missing", R"({"Transform3D": {}, "GridMap": {
        "mesh_library": "missing.meshlib.json"}})");
    scene.create("none", R"({"Transform3D": {}, "GridMap": {"legend": {"b": "block"},
        "floors": [{"y": 0, "rows": ["bz"]}]}})");
    scene.create("typo", R"({"Transform3D": {}, "GridMap": {"mesh_library": "lib.meshlib.json",
        "legend": {"b": "blok"}, "floors": [{"y": 0, "rows": ["b"]}]}})");
    scene.create("rock", R"({"Transform3D": {}, "GridMap": {
        "mesh_library": "broken.meshlib.json", "legend": {"r": "rock"},
        "floors": [{"y": 0, "rows": ["r"]}]}})");
    scene.create("body", R"({"Transform3D": {}, "StaticBody3D": {}, "GridMap": {}})");
    CHECK(messages(scene3d::check_gridmaps(scene.world)) ==
          std::vector<std::string>{
              "/components/GridMap/cell_size: a cell is at least 0.001 meters along each axis",
              "/components/GridMap/mesh_library: asset \"missing.meshlib.json\" could not be "
              "read: no such file",
              "/components/GridMap/floors/0/rows/0: column 1: \"z\" is not in the legend",
              "/components/GridMap/mesh_library: names no mesh library, so its cells hold "
              "nothing: name a .meshlib.json file",
              "/components/GridMap/legend/b: \"blok\" is not an item of \"lib.meshlib.json\"; "
              "did you mean \"block\"?",
              "/components/GridMap/mesh_library: item \"rock\"'s mesh: asset \"rock.obj\" could "
              "not be read: no such file",
              "/components/GridMap: is on a physics body, so its cells don't collide: put the "
              "GridMap on an entity of its own"});
}

TEST_CASE("a GridMap's cells' shapes are a static body that bodies rest on and rays hit") {
    Scene scene;
    physics3d::install(scene.world);
    // A floor of 3 by 3 tiles whose tops are at y 1, with a block on the middle one.
    const entt::entity entity = scene.create("grid", R"({"Transform3D": {"position": [-3, 0, -3]},
        "GridMap": {"mesh_library": "lib.meshlib.json", "collision_layer": 2, "friction": 0.5,
                    "legend": {"f": "floor", "b": "block"},
                    "floors": [{"y": 0, "rows": ["fff", "fff", "fff"]},
                               {"y": 1, "origin": [1, 1], "rows": ["b"]}]}})");
    // A body is only stopped by what its mask names, as in Godot.
    scene.create("crate", R"({"Transform3D": {"position": [-2, 3, -2]},
        "RigidBody3D": {"collision_mask": 2}, "CollisionShape3D": {}})");
    scene.create("ghost", R"({"Transform3D": {"position": [2, 3, -2]}, "RigidBody3D": {},
        "CollisionShape3D": {}})");
    for (int i = 0; i < 240; ++i) {
        scene.world.step(1.0 / 60.0);
    }
    const auto y = [&](std::string_view id) {
        return scene.world.ecs().get<world::Transform3D>(scene.world.find(id)).position.y;
    };
    CHECK(y("crate") == doctest::Approx(1.5F).epsilon(0.02));
    CHECK(y("ghost") < -10.0F);

    // Rays name the cell they hit, even where a shape reaches out of its cell.
    const auto down = [&](float x, float z) {
        return physics3d::raycast(scene.world, {x, 9.0F, z}, {x, -9.0F, z});
    };
    const auto tile = down(2.0F, 2.5F);
    REQUIRE(tile.has_value());
    CHECK(tile->entity == entity);
    CHECK(tile->point.y == doctest::Approx(1.0F).epsilon(0.001));
    CHECK(tile->cell == Vec3i{2, 0, 2});
    const auto block = down(0.0F, 0.0F);
    REQUIRE(block.has_value());
    CHECK(block->point.y == doctest::Approx(4.0F).epsilon(0.001));
    CHECK(block->cell == Vec3i{1, 1, 1});
    // Other bodies have no cell.
    const auto crate = physics3d::raycast(scene.world, {-2.0F, 9.0F, -2.0F}, {-2.0F, -9.0F, -2.0F});
    REQUIRE(crate.has_value());
    CHECK(crate->entity == scene.world.find("crate"));
    CHECK_FALSE(crate->cell.has_value());

    // A GridMap with one shape is a body too.
    scene.map(entity).floors = {world::GridFloor{.y = 0, .origin = {2, 2}, .rows = {"f"}}};
    scene.world.step(1.0 / 60.0);
    const auto alone = down(2.0F, 2.0F);
    REQUIRE(alone.has_value());
    CHECK(alone->cell == Vec3i{2, 0, 2});
    CHECK_FALSE(down(0.0F, 0.0F).has_value());

    // Without cells it is no body at all.
    scene.map(entity).floors.clear();
    scene.world.step(1.0 / 60.0);
    CHECK_FALSE(down(2.0F, 2.0F).has_value());
}

TEST_CASE("with bake_navigation, cells whose items have navigation meshes are regions") {
    Scene scene;
    navigation::install(scene.world, scene.assets);
    const entt::entity entity = scene.create("grid", R"({"Transform3D": {},
        "GridMap": {"mesh_library": "lib.meshlib.json", "legend": {"f": "floor"},
                    "floors": [{"y": 0, "rows": ["ffff", "...f"]}]}})");
    navigation::PathOptions options;
    options.navigation_layers = 1;
    CHECK(
        navigation::query_path(scene.world, Vec3{1, 1, 1}, Vec3{7, 1, 3}, options).points.empty());
    scene.map(entity).bake_navigation = true;
    const auto path = navigation::query_path(scene.world, Vec3{1, 1, 1}, Vec3{7, 1, 3}, options);
    REQUIRE(path.points.size() >= 2);
    CHECK(near(path.points.front(), {1, 1, 1}));
    CHECK(near(path.points.back(), {7, 1, 3}));
    CHECK(path.owners.front() == entity);
    const navigation::MapInfo info = navigation::map_info(scene.world, navigation::Map::ThreeD);
    CHECK(info.regions == 5);
    CHECK(info.polygons == 5);
    CHECK(info.errors.empty());
    // Only on the item's layers.
    options.navigation_layers = 4;
    CHECK(
        navigation::query_path(scene.world, Vec3{1, 1, 1}, Vec3{7, 1, 3}, options).points.empty());
}

TEST_CASE("navigation bakes take a GridMap's meshes, and its shapes when it collides") {
    Scene scene;
    const entt::entity level =
        scene.create("level", R"({"Transform3D": {}, "NavigationRegion3D": {}})");
    const entt::entity entity = scene.create("grid", R"({"Transform3D": {},
        "GridMap": {"mesh_library": "lib.meshlib.json", "collision_layer": 2,
                    "legend": {"f": "floor", "p": "pillar"}, "floors": [{"y": 0, "rows": ["fp"]}]}})");
    scene.world.ecs().get<world::Transform3D>(level).children.push_back(entity);
    const auto parsed = [&](std::string_view type, std::uint32_t mask) {
        auto read = navigation::parse_mesh(parse(
            R"({"format": 1, "geometry_parsed_geometry_type": ")" + std::string(type) + "\"}"));
        REQUIRE(read.ok());
        navigation::NavigationMesh settings = read.value();
        settings.geometry_collision_mask = mask;
        return navigation::parse_source_geometry(scene.world, scene.assets, level, settings)
                   .indices.size() /
               3;
    };
    // Both tiles' meshes; only the floor has a shape, a box of 12 triangles.
    CHECK(parsed("mesh_instances", 1) == 24);
    CHECK(parsed("both", 1) == 24);
    CHECK(parsed("static_colliders", 1) == 0);
    CHECK(parsed("static_colliders", 2) == 12);
}

TEST_CASE("GridMaps draw their octants, the same pixels as a mesh in the cell") {
    const auto draw = [](bool gridmap) {
        Scene scene;
        scene.files.files.emplace("cube.meshlib.json", R"({"format": 1, "items": {"cube": {
            "primitive": {"BoxMesh": {}}, "material": {"albedo_color": [1, 0.5, 0.2, 1]}}}})");
        scene.create("camera",
                     R"({"Transform3D": {"position": [1.5, 1.7, 3.5], "rotation": [-0.35, 0.3, 0]},
                                   "Camera3D": {}})");
        scene.create("sun", R"({"Transform3D": {"rotation": [-0.8, 0.6, 0]},
                                "DirectionalLight3D": {}})");
        if (gridmap) {
            scene.create("grid", R"({"Transform3D": {}, "GridMap": {
                "mesh_library": "cube.meshlib.json", "cell_size": [1, 1, 1],
                "legend": {"c": "cube"}, "floors": [{"y": 0, "rows": ["c"]}]}})");
        } else {
            scene.create("cube", R"({"Transform3D": {"position": [0.5, 0.5, 0.5]},
                "MeshInstance3D": {}, "BoxMesh": {},
                "StandardMaterial3D": {"albedo_color": [1, 0.5, 0.2, 1]}})");
        }
        auto captured = canvas::capture(scene.world, scene.assets);
        REQUIRE(captured.ok());
        CHECK(captured.value().meshes == 1);
        return std::move(captured.value().image);
    };
    const render::Image box = draw(false);
    const render::Image grid = draw(true);
    REQUIRE(box.pixels.size() == grid.pixels.size());
    int lit = 0;
    for (std::uint32_t y = 0; y < box.height; ++y) {
        for (std::uint32_t x = 0; x < box.width; ++x) {
            const Rgba want = pixel(box, x, y);
            const Rgba got = pixel(grid, x, y);
            for (std::size_t c = 0; c < 4; ++c) {
                CHECK(std::abs(int{got[c]} - int{want[c]}) <= 1);
            }
            lit += want[0] > 0 ? 1 : 0;
        }
    }
    CHECK(lit > 100);

    // Meshes stay on the device until their octant changes; a hidden GridMap draws nothing.
    Scene scene;
    scene.create("camera", R"({"Transform3D": {"position": [0, 0, 9]}, "Camera3D": {}})");
    const entt::entity entity = scene.create("grid", R"({"Transform3D": {},
        "GridMap": {"mesh_library": "lib.meshlib.json", "legend": {"b": "block", "f": "floor"},
                    "floors": [{"y": 0, "rows": ["bf......b"]}]}})");
    const auto device = render::create_null_device();
    scene3d::Renderer renderer(*device, nullptr);
    REQUIRE(renderer.prepare(scene.world, 64.0F, 64.0F, 1.0, {}).has_value());
    CHECK(renderer.meshes() == 3);
    CHECK(renderer.meshes_kept() == 3);
    scene.map(entity).floors[0].rows[0] = "bf......f";
    (void)renderer.prepare(scene.world, 64.0F, 64.0F, 1.0, {});
    CHECK(renderer.meshes() == 3);
    CHECK(renderer.meshes_kept() == 3);
    scene.world.ecs().get<world::Transform3D>(entity).visible = false;
    (void)renderer.prepare(scene.world, 64.0F, 64.0F, 1.0, {});
    CHECK(renderer.meshes() == 0);
}
