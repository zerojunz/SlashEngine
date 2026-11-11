// Damage that doesn't break: dents, holes and carves in 3D meshes and 2D outlines, craters in
// terrain, tile maps losing cells, and cloth tearing in 2D and 3D.

#include "slash/asset/assets.hpp"
#include "slash/canvas/canvas.hpp"
#include "slash/core/json.hpp"
#include "slash/core/math.hpp"
#include "slash/physics2d/physics2d.hpp"
#include "slash/physics3d/physics3d.hpp"
#include "slash/scene3d/fracture.hpp"
#include "slash/scene3d/soft_body.hpp"
#include "slash/scene3d/terrain.hpp"
#include "slash/world/cloth2d.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/damage.hpp"
#include "slash/world/world.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace asset = slash::asset;
namespace canvas = slash::canvas;
namespace json = slash::json;
namespace physics2d = slash::physics2d;
namespace physics3d = slash::physics3d;
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

world::DamageProfile profile_of(std::string_view text) {
    auto parsed = world::parse_damage(parse(text));
    REQUIRE_MESSAGE(parsed.ok(), (parsed.ok() ? "" : parsed.error().message));
    return std::move(parsed).value();
}

// A world with files in memory, drawing (for the cuts), both physics and its events.
struct Scene {
    std::map<std::string, std::string, std::less<>> files;
    asset::Assets assets{asset::FileSource{
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
        }}};
    world::ComponentRegistry components;
    world::World world{components};
    std::vector<world::Event> events;

    Scene() {
        world::register_builtin_components(components);
        canvas::install(world, assets);
        physics3d::install(world);
        physics2d::install(world);
        world.set_event_listener([this](const world::Event& event) { events.push_back(event); });
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

    void step(int ticks) {
        for (int i = 0; i < ticks; ++i) {
            world.step(1.0 / 60.0);
        }
    }

    [[nodiscard]] int count(std::string_view kind) const {
        return static_cast<int>(std::ranges::count_if(
            events, [&](const world::Event& event) { return event.kind == kind; }));
    }

    // Damage that a profile turns into dealt damage of a kind, for amounts that deal it.
    [[nodiscard]] float amount_for(std::string_view profile, std::string_view kind,
                                   float dealt) const {
        const world::DamageProfile p = profile_of(files.at(std::string(profile)));
        return dealt / world::damage_taken(p, 1.0F, kind);
    }
};

double polygon_area(const std::vector<Vec2>& points) {
    double twice = 0.0;
    for (std::size_t i = 0; i < points.size(); ++i) {
        const Vec2 a = points[i];
        const Vec2 b = points[(i + 1) % points.size()];
        twice += (static_cast<double>(a.x) * static_cast<double>(b.y)) -
                 (static_cast<double>(b.x) * static_cast<double>(a.y));
    }
    return std::abs(twice) * 0.5;
}

constexpr std::string_view kPlate3D = R"({
    "Transform3D": {"position": [0, 1, 0]},
    "StaticBody3D": {},
    "CollisionShape3D": {"shape": "box", "size": [2, 2, 0.3]},
    "BoxMesh": {"size": [2, 2, 0.3]},
    "MeshInstance3D": {},
    "StandardMaterial3D": {"albedo_color": [0.6, 0.6, 0.7, 1]},
    "Destructible3D": {"profile": "plate.damage.json"}})";

constexpr std::string_view kCrate2D = R"({
    "Transform2D": {"position": [0, 0]},
    "StaticBody2D": {},
    "CollisionShape2D": {"shape": "rectangle", "size": [64, 64]},
    "Polygon2D": {"polygon": [[-32, -32], [32, -32], [32, 32], [-32, 32]],
                  "color": [0.6, 0.4, 0.2, 1]},
    "Destructible2D": {"profile": "crate.damage.json"}})";

const std::vector<Vec2>& outline(const Scene& scene, entt::entity entity) {
    const auto* shape = scene.world.ecs().try_get<const world::CollisionPolygon2D>(entity);
    REQUIRE(shape != nullptr);
    return shape->polygon;
}

} // namespace

TEST_CASE("deform: materials say how damage that doesn't break changes their shape") {
    const world::DamageProfile metal = profile_of(R"({"format": 1, "material": "metal"})");
    CHECK(metal.deform.dent > 0.0F);
    CHECK(metal.deform.carve == 0.0F);
    const world::DamageProfile soil = profile_of(R"({"format": 1, "material": "soil"})");
    CHECK(soil.deform.carve > 0.0F);
    CHECK(soil.fracture.pattern == "none");
    const world::DamageProfile custom = profile_of(
        R"({"format": 1, "material": "wood", "deform": {"hole": 0.01, "max_hole": 0.05}})");
    CHECK(custom.deform.hole == doctest::Approx(0.01));
    CHECK(custom.deform.max_hole == doctest::Approx(0.05));
    CHECK(custom.deform.carve > 0.0F); // wood's, kept
    CHECK_FALSE(world::validate_damage(parse(R"({"format": 1, "deform": {"dnet": 1}})")).empty());
}

TEST_CASE("deform: a blow dents a 3D plate, a shot holes it and a blast carves it") {
    Scene scene;
    scene.files["plate.damage.json"] = R"({"format": 1, "material": "metal", "health": 100000,
        "deform": {"carve": 0.004, "max_carve": 0.5}})";
    const entt::entity plate = scene.create("plate", kPlate3D);
    scene.step(1);
    REQUIRE_FALSE(scene3d::draws_damaged(scene.world, plate));

    auto dented = physics3d::damage_body(scene.world, plate, 50.0F, "blunt", Vec3{0, 1, 0.15F},
                                         0.0F, Vec3{0, 0, -1});
    REQUIRE(dented.ok());
    CHECK(dented.value().deformed == "dent");
    CHECK_FALSE(dented.value().broke);
    const auto& destructible = scene.world.ecs().get<const world::Destructible3D>(plate);
    REQUIRE(destructible.dents.size() == 1);
    CHECK(destructible.dents[0].depth > 0.0F);
    CHECK(scene3d::draws_damaged(scene.world, plate));
    auto piece = scene3d::damaged_piece_of(scene.world, plate);
    REQUIRE_MESSAGE(piece.ok(), (piece.ok() ? "" : piece.error().message));
    CHECK(piece.value()->volume < 2.0 * 2.0 * 0.3);
    CHECK(piece.value()->mesh.triangles > 12);
    // Dents don't change what it collides as.
    CHECK(scene.world.ecs().get<const world::CollisionShape3D>(plate).shape == "box");

    auto holed = physics3d::damage_body(scene.world, plate, 30.0F, "pierce", Vec3{0.5F, 1, 0.15F},
                                        0.0F, Vec3{0, 0, -1});
    REQUIRE(holed.ok());
    CHECK(holed.value().deformed == "hole");
    REQUIRE(destructible.carves.size() == 1);
    // A holed static body collides as its triangles, so things go through the hole.
    CHECK(scene.world.ecs().get<const world::CollisionShape3D>(plate).shape == "concave_polygon");
    const double after_hole = scene3d::damaged_piece_of(scene.world, plate).value()->volume;

    auto carved = physics3d::damage_body(scene.world, plate, 50.0F, "explosive", Vec3{-1, 1, 0.2F});
    REQUIRE(carved.ok());
    CHECK(carved.value().deformed == "carve");
    CHECK(destructible.carves.size() == 2);
    const double after_carve = scene3d::damaged_piece_of(scene.world, plate).value()->volume;
    CHECK(after_carve < after_hole);
    CHECK(scene.count("damaged") == 3);
}

TEST_CASE("deform: a carved 3D wall breaks into pieces that keep its carve") {
    Scene scene;
    scene.files["plate.damage.json"] = R"({"format": 1, "material": "concrete", "health": 100})";
    const entt::entity wall = scene.create("wall", kPlate3D);
    scene.step(1);
    const float blast = scene.amount_for("plate.damage.json", "explosive", 50.0F);
    auto carved = physics3d::damage_body(scene.world, wall, blast, "explosive", Vec3{1, 2, 0.15F});
    REQUIRE(carved.ok());
    CHECK(carved.value().deformed == "carve");
    const double whole = scene3d::damaged_piece_of(scene.world, wall).value()->volume;
    CHECK(whole < 2.0 * 2.0 * 0.3);

    auto broken = physics3d::damage_body(scene.world, wall, 1000.0F, "blunt", Vec3{-0.5F, 1, 0});
    REQUIRE(broken.ok());
    REQUIRE(broken.value().broke);
    double pieces = 0.0;
    for (const entt::entity piece : scene.world.ecs().view<const world::FracturePiece3D>()) {
        const auto& cut = scene.world.ecs().get<const world::FracturePiece3D>(piece);
        REQUIRE_FALSE(cut.cuts.empty());
        CHECK(cut.cuts.front().carves.size() == 1);
        auto made = scene3d::damaged_piece_of(scene.world, piece);
        REQUIRE(made.ok());
        pieces += made.value()->volume;
    }
    CHECK(pieces == doctest::Approx(whole).epsilon(0.02));
}

TEST_CASE("deform: a blast digs a crater in an HTerrain made of soil") {
    Scene scene;
    scene.files["ground.damage.json"] = R"({"format": 1, "material": "soil"})";
    const entt::entity ground = scene.create("ground", R"({"Transform3D": {},
        "HTerrain": {"resolution": 65},
        "Destructible3D": {"profile": "ground.damage.json"}})");
    scene.step(1);
    const float blast = scene.amount_for("ground.damage.json", "explosive", 100.0F);
    const auto hurt =
        physics3d::damage_area(scene.world, Vec3{16, 0, 16}, 5.0F, blast, "explosive");
    REQUIRE(hurt.size() == 1);
    CHECK(hurt[0].deformed == "crater");
    const auto& terrain = scene.world.ecs().get<const world::HTerrain>(ground);
    REQUIRE(terrain.craters.size() == 1);
    const world::TerrainCrater& crater = terrain.craters[0];
    CHECK(crater.radius == doctest::Approx(2.0)); // 100 x 0.02 meters, a meter a cell
    const auto data = scene3d::terrain_data(scene.world, ground);
    REQUIRE(data != nullptr);
    const auto height = [&](int x, int z) {
        return data->heights[(static_cast<std::size_t>(z) * 65) + static_cast<std::size_t>(x)];
    };
    CHECK(height(16, 16) == doctest::Approx(-0.8));
    CHECK(height(17, 16) < 0.0F);
    CHECK(height(18, 16) > 0.0F); // the rim
    CHECK(height(4, 4) == doctest::Approx(0.0));
    // A blast high above the ground doesn't reach it.
    CHECK(physics3d::damage_area(scene.world, Vec3{8, 10, 8}, 5.0F, blast, "explosive").empty());
}

TEST_CASE("deform: 2D outlines are carved, holed and dented, and cut-off parts fall") {
    Scene scene;
    scene.files["crate.damage.json"] =
        R"({"format": 1, "material": "stone", "health": 100000,
            "deform": {"hole": 0.003, "max_hole": 0.2, "dent": 0.002, "max_dent": 0.2}})";
    const entt::entity crate = scene.create("crate", kCrate2D);
    scene.step(1);

    // A blast at its right edge takes a round bite out of it: 20 pixels across.
    const float bite = scene.amount_for("crate.damage.json", "explosive", 20.0F / (0.004F * 64.0F));
    auto carved = physics2d::damage_body_2d(scene.world, crate, bite, "explosive", Vec2{32, 0});
    REQUIRE(carved.ok());
    CHECK(carved.value().deformed == "carve");
    CHECK_FALSE(scene.world.ecs().all_of<world::CollisionShape2D>(crate));
    const double half_disc = 3.14159265 * 20.0 * 20.0 / 2.0;
    CHECK(polygon_area(outline(scene, crate)) == doctest::Approx(4096.0 - half_disc).epsilon(0.03));
    CHECK(scene.world.ecs().get<const world::Polygon2D>(crate).polygon.size() > 4);

    // A shot from the left leaves a round hole just inside, opened to the edge by a slot.
    const float shot = scene.amount_for("crate.damage.json", "pierce", 5.0F / (0.003F * 64.0F));
    auto holed = physics2d::damage_body_2d(scene.world, crate, shot, "pierce", Vec2{-32, 0}, 0.0F,
                                           Vec2{1, 0});
    REQUIRE(holed.ok());
    CHECK(holed.value().deformed == "hole");
    const double disc = 3.14159265 * 5.0 * 5.0;
    CHECK(polygon_area(outline(scene, crate)) ==
          doctest::Approx(4096.0 - half_disc - disc).epsilon(0.03));

    // A blow on top dents it in.
    const double before_dent = polygon_area(outline(scene, crate));
    auto dented = physics2d::damage_body_2d(scene.world, crate, 2000.0F, "blunt", Vec2{0, -32});
    REQUIRE(dented.ok());
    CHECK(dented.value().deformed == "dent");
    CHECK(polygon_area(outline(scene, crate)) < before_dent - 10.0);

    // A blast through the middle of a bar cuts it in two: one part falls away.
    const entt::entity bar = scene.create("bar", R"({"Transform2D": {"position": [200, 0]},
        "StaticBody2D": {}, "CollisionShape2D": {"shape": "rectangle", "size": [128, 16]},
        "Polygon2D": {"polygon": [[-64, -8], [64, -8], [64, 8], [-64, 8]]},
        "Destructible2D": {"profile": "crate.damage.json"}})");
    auto split = physics2d::damage_body_2d(scene.world, bar, bite, "explosive", Vec2{200, 0});
    REQUIRE(split.ok());
    CHECK(split.value().deformed == "carve");
    std::vector<entt::entity> debris;
    for (const entt::entity piece : scene.world.ecs().view<const world::FracturePiece2D>()) {
        debris.push_back(piece);
    }
    REQUIRE(debris.size() == 1);
    CHECK(scene.world.ecs().all_of<world::RigidBody2D>(debris[0]));
    const double kept = polygon_area(outline(scene, bar));
    const double fell =
        polygon_area(scene.world.ecs().get<const world::CollisionPolygon2D>(debris[0]).polygon);
    CHECK(kept + fell == doctest::Approx(128.0 * 16.0 - (16.0 * 40.0)).epsilon(0.05));
}

TEST_CASE("deform: a blast takes the cells it reaches out of a destructible tile map") {
    Scene scene;
    scene.files["rock.tileset.json"] = R"({"format": 1, "tile_size": [16, 16],
        "sources": {"rock": {"texture": "rock.png", "tiles": {"0,0": {"name": "rock"}}}}})";
    scene.files["ground.damage.json"] = R"({"format": 1, "material": "soil"})";
    const entt::entity layer = scene.create("cave", R"({"TileMapLayer": {
        "tile_set": "rock.tileset.json", "legend": {"r": "rock"},
        "rows": ["rrrrrrrr", "rrrrrrrr", "rrrrrrrr", "rrrrrrrr",
                 "rrrrrrrr", "rrrrrrrr", "rrrrrrrr", "rrrrrrrr"]},
        "Destructible2D": {"profile": "ground.damage.json"}})");
    scene.step(1);
    // 20 pixels of reach: the four cells round (64, 64).
    const float blast =
        scene.amount_for("ground.damage.json", "explosive", 20.0F / (0.02F * 64.0F));
    const auto hurt =
        physics2d::damage_area_2d(scene.world, Vec2{64, 64}, 100.0F, blast, "explosive");
    REQUIRE(hurt.size() == 1);
    CHECK(hurt[0].deformed == "carve");
    const auto& rows = scene.world.ecs().get<const world::TileMapLayer>(layer).rows;
    std::size_t cells = 0;
    for (const std::string& row : rows) {
        cells += static_cast<std::size_t>(std::ranges::count(row, 'r'));
    }
    CHECK(cells == 60);
    CHECK(rows[3] == "rrr..rrr");
    CHECK(rows[4] == "rrr..rrr");
}

TEST_CASE("deform: 2D cloth tears where it is cut, and the part torn off falls") {
    Scene scene;
    const entt::entity cape = scene.create("cape", R"({"Transform2D": {},
        "Cloth2D": {"size": [48, 96], "columns": 4, "rows": 8}})");
    scene.step(1);
    const auto torn = physics2d::tear_cloth_2d(scene.world, cape, Vec2{0, 48}, 30.0F);
    REQUIRE_FALSE(torn.empty());
    CHECK(scene.count("torn") == 1);
    const auto& cloth = scene.world.ecs().get<const world::Cloth2D>(cape);
    CHECK(cloth.torn == torn);
    const world::ClothGrid2D grid = world::cloth_grid_2d(cloth);
    const std::vector<bool> drawn = world::cloth_triangles_2d(grid);
    CHECK(std::ranges::count(drawn, false) > 0);
    CHECK(std::ranges::count(drawn, true) > 0);
    // Nothing joins the bottom half to the held top any more: it falls past its max distance.
    scene.step(60);
    const world::ClothPoints2D* points = world::cloth_points_2d(scene.world, cape);
    REQUIRE(points != nullptr);
    CHECK(points->current.back().y > 300.0F);
    CHECK(points->current.front().y == doctest::Approx(0.0));
    // Tearing again where it is already torn tears nothing more.
    CHECK(physics2d::tear_cloth_2d(scene.world, cape, Vec2{0, 48}, 1.0F).empty());
}

TEST_CASE("deform: 2D cloth pulled past its tear_stretch tears by itself") {
    Scene scene;
    const entt::entity sheet = scene.create("sheet", R"({"Transform2D": {},
        "Cloth2D": {"size": [48, 96], "columns": 4, "rows": 8, "stiffness": 0.05, "long_range_attachment": false,
                    "gravity_scale": 40, "max_distance": 400, "tear_stretch": 1.05}})");
    scene.step(60);
    CHECK_FALSE(scene.world.ecs().get<const world::Cloth2D>(sheet).torn.empty());
    CHECK(scene.count("torn") > 0);
    // Without a limit it only stretches.
    Scene loose;
    const entt::entity whole = loose.create("sheet", R"({"Transform2D": {},
        "Cloth2D": {"size": [48, 96], "columns": 4, "rows": 8, "stiffness": 0.05, "long_range_attachment": false,
                    "gravity_scale": 40, "max_distance": 400}})");
    loose.step(60);
    CHECK(loose.world.ecs().get<const world::Cloth2D>(whole).torn.empty());
}

TEST_CASE("deform: a 3D soft body tears where it is cut and is made again without the faces") {
    Scene scene;
    const entt::entity flag = scene.create("flag", R"({"Transform3D": {"position": [0, 2, 0]},
        "MeshInstance3D": {},
        "PlaneMesh": {"size": [2, 2], "subdivide_width": 3, "subdivide_depth": 3},
        "SoftBody3D": {"pinned_points": [{"point": 0}, {"point": 4}]}})");
    scene.step(2);
    const auto torn = physics3d::tear_soft_body(scene.world, flag, Vec3{0, 2, 0}, 0.4F);
    REQUIRE_FALSE(torn.empty());
    CHECK(scene.count("torn") == 1);
    CHECK(scene.world.ecs().get<const world::SoftBody3D>(flag).torn == torn);
    scene.step(2);
    const auto pose = scene3d::soft_body_pose(scene.world, flag);
    REQUIRE(pose != nullptr);
    CHECK(pose->torn == torn);
    // Its points are where they were: it hangs on, falling no faster.
    const auto points = physics3d::soft_body_points(scene.world, flag);
    REQUIRE(points.has_value());
    CHECK(points->points.size() == 25);
    CHECK(physics3d::tear_soft_body(scene.world, flag, Vec3{0, 2, 0}, 0.01F).empty());
}
