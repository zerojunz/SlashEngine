// Damage profiles (.damage.json): materials, resistances and damage states, fracture patterns,
// linked pieces and the load through them, and bodies breaking by them in 3D and 2D.

#include "slash/asset/assets.hpp"
#include "slash/canvas/canvas.hpp"
#include "slash/core/json.hpp"
#include "slash/core/math.hpp"
#include "slash/physics2d/physics2d.hpp"
#include "slash/physics3d/physics3d.hpp"
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

    template <typename Piece>
    [[nodiscard]] std::vector<entt::entity> pieces() const {
        std::vector<entt::entity> out;
        for (const entt::entity entity : world.ecs().view<const Piece>()) {
            out.push_back(entity);
        }
        return out;
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

constexpr std::string_view kWall3D = R"({
    "Transform3D": {"position": [0, 1, 0]},
    "StaticBody3D": {},
    "CollisionShape3D": {"shape": "box", "size": [2, 2, 0.3]},
    "BoxMesh": {"size": [2, 2, 0.3]},
    "MeshInstance3D": {},
    "StandardMaterial3D": {"albedo_color": [0.8, 0.4, 0.3, 1]},
    "Destructible3D": {"profile": "wall.damage.json"}})";

constexpr std::string_view kCrate2D = R"({
    "Transform2D": {"position": [0, 0]},
    "StaticBody2D": {},
    "CollisionShape2D": {"shape": "rectangle", "size": [64, 64]},
    "Polygon2D": {"polygon": [[-32, -32], [32, -32], [32, 32], [-32, 32]],
                  "color": [0.6, 0.4, 0.2, 1]},
    "Destructible2D": {"profile": "crate.damage.json"}})";

} // namespace

TEST_CASE("damage: profiles fill what they leave out from their material") {
    const world::DamageProfile glass = profile_of(R"({"format": 1, "material": "glass"})");
    CHECK(glass.health == doctest::Approx(10.0));
    CHECK(glass.fracture.pattern == "radial");
    CHECK(world::damage_taken(glass, 10.0F, "blunt") == doctest::Approx(15.0));
    CHECK(world::damage_taken(glass, 10.0F, "fire") == doctest::Approx(2.0));
    CHECK(world::damage_taken(glass, 10.0F, "cut") == doctest::Approx(10.0));

    const world::DamageProfile door = profile_of(R"({"format": 1, "material": "wood",
        "health": 80, "resist": {"fire": 3},
        "states": [{"below": 0.75, "tint": [0.8, 0.8, 0.8, 1]}, {"below": 0.3}]})");
    CHECK(door.health == doctest::Approx(80.0));
    CHECK(world::damage_taken(door, 1.0F, "fire") == doctest::Approx(3.0));
    CHECK(world::damage_taken(door, 1.0F, "pierce") == doctest::Approx(0.6)); // wood's, kept
    CHECK(world::damage_state(door, 80.0F) == 0);
    CHECK(world::damage_state(door, 50.0F) == 1);
    CHECK(world::damage_state(door, 10.0F) == 2);
    CHECK(door.states.front().tinted);

    // Every material makes a profile, and the schema holds every file to its names.
    for (const std::string& material : world::damage_materials()) {
        json::Value doc;
        doc["format"] = 1;
        doc["material"] = material;
        CHECK(world::validate_damage(doc).empty());
        CHECK(world::parse_damage(doc).ok());
    }
    const auto typo = world::validate_damage(parse(R"({"format": 1, "resist": {"peirce": 0.5}})"));
    REQUIRE(!typo.empty());
    CHECK(typo.front().message.find("pierce") != std::string::npos);
    CHECK_FALSE(world::validate_damage(parse(R"({"format": 1, "anchors": "top"})")).empty());
    CHECK_FALSE(world::validate_damage(
                    parse(R"({"format": 1, "states": [{"below": 0.2}, {"below": 0.6}]})"))
                    .empty());
    CHECK(world::is_damage_path("props/door.damage.json"));
    CHECK_FALSE(world::is_damage_path("door.json"));
}

TEST_CASE("damage: a profile extends another as a merge patch") {
    std::map<std::string, std::string, std::less<>> files{
        {"base.damage.json", R"({"format": 1, "material": "brick", "health": 300})"}};
    const world::FileReader read = [&](std::string_view path) -> slash::Result<std::string> {
        const auto it = files.find(path);
        if (it == files.end()) {
            return slash::make_error(slash::ErrorCode::NotFound, "no such file");
        }
        return it->second;
    };
    auto resolved = world::resolve_damage(
        parse(R"({"format": 1, "extends": "base.damage.json", "anchors": "ends"})"), read);
    REQUIRE(resolved.ok());
    const world::DamageProfile wall = world::parse_damage(resolved.value()).value();
    CHECK(wall.material == "brick");
    CHECK(wall.health == doctest::Approx(300.0));
    CHECK(wall.anchors == "ends");
    CHECK(wall.connected); // brick's default
    files["loop.damage.json"] = R"({"format": 1, "extends": "loop.damage.json"})";
    CHECK_FALSE(
        world::resolve_damage(parse(R"({"format": 1, "extends": "loop.damage.json"})"), read).ok());
}

TEST_CASE("damage: fracture patterns place their sites the same on every machine") {
    const Vec3 low{-1.0F, -1.0F, -0.1F};
    const Vec3 high{1.0F, 1.0F, 0.1F};
    world::FractureCut cut{.pattern = "voronoi",
                           .cells = 12,
                           .seed = 7,
                           .at = {},
                           .fine = 0.0F,
                           .index = 0,
                           .dents = {},
                           .carves = {}};
    const world::FractureSites a = world::fracture_sites(cut, low, high, false);
    const world::FractureSites b = world::fracture_sites(cut, low, high, false);
    REQUIRE(a.points.size() == 12);
    CHECK(a.points == b.points);
    CHECK(a.groups.empty());
    for (const auto& point : a.points) {
        CHECK(point[0] >= -1.0);
        CHECK(point[0] <= 1.0);
        CHECK(point[2] >= -0.1);
        CHECK(point[2] <= 0.1);
    }

    // An impact gathers most of its sites near where it hit.
    cut.pattern = "impact";
    cut.at = {0.8F, 0.8F, 0.0F};
    cut.fine = 0.3F;
    const world::FractureSites impact = world::fracture_sites(cut, low, high, false);
    const auto near = std::ranges::count_if(
        impact.points, [](const auto& p) { return std::hypot(p[0] - 0.8, p[1] - 0.8) <= 0.31; });
    CHECK(near >= 6);

    // Slices are nearly even along the longest axis (a meter apart here, a little off).
    cut.pattern = "slices";
    cut.cells = 4;
    const world::FractureSites slices =
        world::fracture_sites(cut, {-2.0F, -0.5F, -0.5F}, {2.0F, 0.5F, 0.5F}, false);
    REQUIRE(slices.points.size() == 4);
    std::vector<double> xs;
    for (const auto& point : slices.points) {
        xs.push_back(point[0]);
    }
    std::ranges::sort(xs);
    for (std::size_t i = 1; i < xs.size(); ++i) {
        CHECK(xs[i] - xs[i - 1] > 0.55);
        CHECK(xs[i] - xs[i - 1] < 1.45);
    }

    // Clusters join sub-cells into pieces; flat cuts stay in their plane.
    cut.pattern = "clusters";
    cut.cells = 5;
    const world::FractureSites clusters = world::fracture_sites(cut, low, high, true);
    REQUIRE(clusters.groups.size() == clusters.points.size());
    CHECK(clusters.points.size() > 5);
    CHECK(*std::ranges::max_element(clusters.groups) == 4);
    for (const auto& point : clusters.points) {
        CHECK(point[2] == doctest::Approx(-0.1));
    }

    // A level's cut seeds by its level and the hit, so pieces break apart differently.
    const world::DamageProfile stone = profile_of(R"({"format": 1, "material": "stone"})");
    const world::FractureCut first = world::damage_cut(stone, {}, 0, 3);
    const world::FractureCut again = world::damage_cut(stone, {}, 1, 3);
    CHECK(first.cells == 8);
    CHECK(again.cells == 4);
    CHECK(first.seed != again.seed);
}

TEST_CASE("damage: the load through linked pieces breaks the links too weak for it") {
    // A column of three 1 kN blocks on an anchored base, linked by 0.1 m^2 faces.
    std::vector<world::StressPiece> column(4);
    for (std::size_t i = 0; i < column.size(); ++i) {
        column[i].center = {0.0F, static_cast<float>(i), 0.0F};
        column[i].weight = 1000.0F;
        if (i > 0) {
            column[i].links.emplace_back(i - 1, 0.1F);
            column[i - 1].links.emplace_back(i, 0.1F);
        }
    }
    column[0].anchored = true;
    const Vec3 up{0.0F, 1.0F, 0.0F};
    const world::StressResult held = world::settle_stress(column, 20000.0F, 10.0F, up);
    CHECK(held.broken.empty());
    CHECK(std::ranges::none_of(held.falls, [](bool f) { return f; }));
    CHECK(held.loads[1] == doctest::Approx(3000.0)); // what stands on the lowest link

    // Hung from the anchor instead (no compression bonus), the same links give way.
    for (auto& piece : column) {
        piece.center.y = -piece.center.y;
    }
    const world::StressResult hung = world::settle_stress(column, 20000.0F, 10.0F, up);
    REQUIRE(!hung.broken.empty());
    CHECK(hung.broken.front() == std::pair<std::size_t, std::size_t>{0, 1});
    CHECK(hung.falls[1]);
    CHECK(hung.falls[3]);
    CHECK_FALSE(hung.falls[0]);

    // A bridge held at both ends: its middle hangs from both sides.
    std::vector<world::StressPiece> bridge(5);
    for (std::size_t i = 0; i < bridge.size(); ++i) {
        bridge[i].center = {static_cast<float>(i), 0.0F, 0.0F};
        bridge[i].weight = 500.0F;
        if (i > 0) {
            bridge[i].links.emplace_back(i - 1, 0.05F);
            bridge[i - 1].links.emplace_back(i, 0.05F);
        }
    }
    bridge.front().anchored = true;
    bridge.back().anchored = true;
    const world::StressResult span = world::settle_stress(bridge, 100000.0F, 10.0F, up);
    CHECK(span.broken.empty());
    const world::StressResult weak = world::settle_stress(bridge, 5000.0F, 10.0F, up);
    CHECK(!weak.broken.empty());
    CHECK(weak.falls[2]);
}

TEST_CASE("damage: a 3D wall shows its damage, then breaks where it is hit") {
    Scene scene;
    scene.files["wall.damage.json"] = R"({"format": 1, "material": "stone", "health": 100,
        "states": [{"below": 0.5, "tint": [0.5, 0.5, 0.5, 1]}],
        "fracture": {"pattern": "voronoi", "pieces": 6, "levels": 1}})";
    const entt::entity wall = scene.create("wall", kWall3D);
    scene.step(1);

    auto first = physics3d::damage_body(scene.world, wall, 40.0F, "blunt", Vec3{0, 1, 0.15F});
    REQUIRE(first.ok());
    CHECK(first.value().dealt == doctest::Approx(40.0));
    CHECK(first.value().health == doctest::Approx(60.0));
    CHECK(first.value().state == 0);
    CHECK_FALSE(first.value().broke);
    // Stone shrugs off fire.
    auto fire = physics3d::damage_body(scene.world, wall, 100.0F, "fire", std::nullopt);
    REQUIRE(fire.ok());
    CHECK(fire.value().dealt == doctest::Approx(0.0));
    auto second = physics3d::damage_body(scene.world, wall, 20.0F, "blunt", std::nullopt);
    REQUIRE(second.ok());
    CHECK(second.value().state == 1);
    const auto& material = scene.world.ecs().get<world::StandardMaterial3D>(wall);
    CHECK(material.albedo_color.x == doctest::Approx(0.4));
    CHECK(scene.count("damaged") == 2); // the fire did nothing

    auto last = physics3d::damage_body(scene.world, wall, 60.0F, "explosive", Vec3{0, 1, 0.15F});
    REQUIRE(last.ok());
    CHECK(last.value().broke);
    CHECK(last.value().pieces.size() >= 2);
    CHECK((scene.world.find("wall") == entt::null));
    CHECK(scene.count("broken") == 1);
    // Its pieces are Destructibles of the next level, with less health.
    const auto pieces = scene.pieces<world::FracturePiece3D>();
    REQUIRE(!pieces.empty());
    const auto& piece = scene.world.ecs().get<world::Destructible3D>(pieces.front());
    CHECK(piece.level == 1);
    CHECK(piece.health > 0.0F);
    CHECK(piece.health < 100.0F);
    scene.step(30);
}

TEST_CASE("damage: a linked 3D wall stands until the load through its pieces breaks it") {
    Scene scene;
    scene.files["wall.damage.json"] = R"({"format": 1, "material": "concrete", "health": 10,
        "fracture": {"pattern": "voronoi", "pieces": 10, "levels": 1, "fine": 0.4},
        "connected": true, "anchors": "bottom", "strength": 1e9})";
    const entt::entity wall = scene.create("wall", kWall3D);
    scene.step(1);
    auto hit = physics3d::damage_body(scene.world, wall, 50.0F, "blunt", Vec3{0.9F, 1.9F, 0.15F});
    REQUIRE(hit.ok());
    REQUIRE(hit.value().broke);
    scene.step(2);
    int standing = 0;
    int anchored = 0;
    for (const entt::entity entity : scene.pieces<world::FracturePiece3D>()) {
        const auto& piece = scene.world.ecs().get<world::FracturePiece3D>(entity);
        CHECK(piece.group == "wall");
        if (scene.world.ecs().get<world::RigidBody3D>(entity).freeze) {
            ++standing;
            anchored += piece.anchored ? 1 : 0;
        }
    }
    CHECK(standing > anchored);
    CHECK(anchored > 0);
    CHECK(scene.count("collapsed") == 0);

    // Weakened past what the pieces weigh, all but what the ground holds comes down at the
    // next settle.
    scene.files["wall.damage.json"] = R"({"format": 1, "material": "concrete", "health": 10,
        "fracture": {"pattern": "voronoi", "pieces": 10, "levels": 1, "fine": 0.4},
        "connected": true, "anchors": "bottom", "strength": 1})";
    const std::vector<std::string> fell = physics3d::settle_group(scene.world, "wall");
    CHECK(fell.size() == static_cast<std::size_t>(standing - anchored));
    for (const entt::entity entity : scene.pieces<world::FracturePiece3D>()) {
        const auto& piece = scene.world.ecs().get<world::FracturePiece3D>(entity);
        CHECK(scene.world.ecs().get<world::RigidBody3D>(entity).freeze == piece.anchored);
    }
}

TEST_CASE("damage: an explosion damages every 3D Destructible in reach, less with distance") {
    Scene scene;
    scene.files["wall.damage.json"] = R"({"format": 1, "material": "stone", "health": 1000})";
    scene.create("wall", kWall3D);
    std::string far(kWall3D);
    far.replace(far.find("[0, 1, 0]"), 9, "[0, 1, 9]");
    scene.create("far", far);
    scene.step(1);
    const auto reports =
        physics3d::damage_area(scene.world, Vec3{0, 1, 1}, 5.0F, 100.0F, "explosive");
    REQUIRE(reports.size() == 1);
    CHECK(reports.front().id == "wall");
    CHECK(reports.front().dealt > 0.0F);
    CHECK(reports.front().dealt < 150.0F); // stone takes 1.5 times, less a meter off
}

TEST_CASE("damage: a 2D crate breaks into polygons that cover it") {
    Scene scene;
    scene.files["crate.damage.json"] = R"({"format": 1, "material": "wood", "health": 30,
        "fracture": {"pattern": "voronoi", "pieces": 6, "levels": 1}})";
    const entt::entity crate = scene.create("crate", kCrate2D);
    scene.step(1);
    auto light = physics2d::damage_body_2d(scene.world, crate, 10.0F, "blunt", Vec2{0, 0});
    REQUIRE(light.ok());
    CHECK(light.value().health == doctest::Approx(20.0));
    CHECK_FALSE(light.value().broke);
    auto burn = physics2d::damage_body_2d(scene.world, crate, 10.0F, "fire", std::nullopt);
    REQUIRE(burn.ok());
    CHECK(burn.value().broke); // wood takes fire twice over
    CHECK((scene.world.find("crate") == entt::null));
    const auto pieces = scene.pieces<world::FracturePiece2D>();
    REQUIRE(pieces.size() >= 2);
    double area = 0.0;
    for (const entt::entity piece : pieces) {
        const auto& shape = scene.world.ecs().get<world::CollisionPolygon2D>(piece);
        const auto& look = scene.world.ecs().get<world::Polygon2D>(piece);
        CHECK(look.polygon.size() == shape.polygon.size());
        CHECK(scene.world.ecs().all_of<world::RigidBody2D>(piece));
        area += polygon_area(shape.polygon);
    }
    CHECK(area == doctest::Approx(64.0 * 64.0).epsilon(0.01));
    scene.step(30);
}

TEST_CASE("damage: a fast 2D ball breaks a glass pane it hits") {
    Scene scene;
    scene.files["crate.damage.json"] = R"({"format": 1, "material": "glass",
        "impact": {"threshold": 100, "damage": 1}})";
    scene.create("crate", kCrate2D);
    scene.create("ball", R"({"Transform2D": {"position": [-200, 0]},
        "RigidBody2D": {"mass": 2, "gravity_scale": 0, "linear_velocity": [900, 0]},
        "CollisionShape2D": {"shape": "circle", "radius": 8}})");
    scene.step(30);
    CHECK((scene.world.find("crate") == entt::null));
    CHECK(scene.count("broken") == 1);
    CHECK(!scene.pieces<world::FracturePiece2D>().empty());
}

TEST_CASE("damage: a linked 2D wall loses what no link holds to the ground") {
    Scene scene;
    scene.files["crate.damage.json"] = R"({"format": 1, "material": "brick", "health": 5,
        "fracture": {"pattern": "voronoi", "pieces": 8, "levels": 1, "fine": 10},
        "connected": true, "anchors": "bottom", "strength": 1e9})";
    const entt::entity wall = scene.create("crate", kCrate2D);
    scene.step(1);
    auto hit = physics2d::damage_body_2d(scene.world, wall, 50.0F, "blunt", Vec2{30, -30});
    REQUIRE(hit.ok());
    REQUIRE(hit.value().broke);
    scene.step(2);
    int standing = 0;
    int anchored = 0;
    for (const entt::entity entity : scene.pieces<world::FracturePiece2D>()) {
        const auto& piece = scene.world.ecs().get<world::FracturePiece2D>(entity);
        CHECK(piece.group == "crate");
        if (piece.anchored || !piece.links.empty()) {
            ++standing;
            anchored += piece.anchored ? 1 : 0;
        }
    }
    CHECK(standing > anchored);
    CHECK(anchored > 0);
    scene.files["crate.damage.json"] = R"({"format": 1, "material": "brick", "health": 5,
        "fracture": {"pattern": "voronoi", "pieces": 8, "levels": 1, "fine": 10},
        "connected": true, "anchors": "bottom", "strength": 1})";
    const std::vector<std::string> fell = physics2d::settle_group_2d(scene.world, "crate");
    CHECK(fell.size() == static_cast<std::size_t>(standing - anchored));
    CHECK(scene.count("collapsed") == 1);
}
