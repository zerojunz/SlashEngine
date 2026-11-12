#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/core/math.hpp"
#include "slash/platform/file.hpp"
#include "slash/procedural/pcg.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/tree3d.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <numbers>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace command = slash::command;
namespace json = slash::json;
namespace procedural = slash::procedural;
namespace world = slash::world;
using slash::Vec2;
using slash::Vec3;

namespace {

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE_MESSAGE(parsed.ok(), text);
    return std::move(parsed).value();
}

std::vector<std::string> error_paths(std::string_view text) {
    std::vector<std::string> out;
    for (const json::ValidationError& error : procedural::validate_pcg(parse(text))) {
        out.push_back(error.path);
    }
    return out;
}

bool has_error(std::string_view text, std::string_view path) {
    const std::vector<std::string> paths = error_paths(text);
    return std::ranges::find(paths, path) != paths.end();
}

float across(Vec3 a, Vec3 b) {
    const auto dx = static_cast<float>(a.x - b.x);
    const auto dz = static_cast<float>(a.z - b.z);
    return std::sqrt((dx * dx) + (dz * dz));
}

std::vector<procedural::PcgPoint> line_of(int count, float step) {
    std::vector<procedural::PcgPoint> points;
    for (int i = 0; i < count; ++i) {
        procedural::PcgPoint point;
        point.position = {static_cast<float>(i) * step, 0.0F, 0.0F};
        point.seed = procedural::pcg_hash(9U, static_cast<std::uint32_t>(i));
        points.push_back(point);
    }
    return points;
}

struct Project {
    std::string root;
    world::ComponentRegistry components;
    command::Registry commands;
    std::unique_ptr<command::Session> session;

    explicit Project(std::string name) : root(std::move(name)) {
        std::filesystem::remove_all(root);
        REQUIRE(slash::platform::create_directories(root).ok());
        world::register_builtin_components(components);
        command::register_builtin_commands(commands);
        command::SessionOptions options;
        options.project_root = root;
        options.capture_logs = false;
        session = std::make_unique<command::Session>(components, commands, options);
    }
    Project(const Project&) = delete;
    Project& operator=(const Project&) = delete;
    Project(Project&&) = delete;
    Project& operator=(Project&&) = delete;
    ~Project() { std::filesystem::remove_all(root); }

    void write(const std::string& file, std::string_view text) const {
        REQUIRE(slash::platform::write_file(root + "/" + file, text).ok());
    }
    command::Reply call(std::string_view name, std::string_view params) const {
        return session->call(name, parse(params));
    }
    [[nodiscard]] world::World& world() const { return session->world(); }
};

} // namespace

TEST_CASE("procedural graphs are checked with the path of each mistake") {
    CHECK(error_paths(R"({"format": 1, "nodes": [
        {"name": "ground", "op": "grid", "area": [0, 0, 10, 10], "spacing": 1},
        {"name": "thin", "op": "filter", "from": ["ground"], "keep": 0.5},
        {"name": "rocks", "op": "spawn", "from": ["thin"], "prefab": "rock.prefab.json"}]})")
              .empty());
    // A field the op doesn't take, and a sampler given from.
    CHECK(has_error(R"({"format": 1, "nodes": [
        {"name": "g", "op": "grid", "area": [0, 0, 1, 1], "spacing": 1, "keep": 0.5}]})",
                    "/nodes/0/keep"));
    CHECK(has_error(R"({"format": 1, "nodes": [
        {"name": "a", "op": "points", "points": [[0, 0, 0]]},
        {"name": "g", "op": "grid", "from": ["a"], "area": [0, 0, 1, 1], "spacing": 1}]})",
                    "/nodes/1/from"));
    // Other ops need from, naming earlier nodes.
    CHECK(has_error(R"({"format": 1, "nodes": [{"name": "f", "op": "filter", "keep": 0.5}]})",
                    "/nodes/0"));
    CHECK(has_error(R"({"format": 1, "nodes": [
        {"name": "f", "op": "filter", "from": ["later"], "keep": 0.5},
        {"name": "later", "op": "points", "points": [[0, 0, 0]]}]})",
                    "/nodes/0/from/0"));
    CHECK(has_error(R"({"format": 1, "nodes": [
        {"name": "a", "op": "points", "points": [[0, 0, 0]]},
        {"name": "n", "op": "near", "from": ["a"], "to": ["nobody"], "range": [0, 1]}]})",
                    "/nodes/1/to/0"));
    // Names used twice, ranges out of order, 3D-only fields in 2D, points of the wrong size.
    CHECK(has_error(R"({"format": 1, "nodes": [
        {"name": "a", "op": "points", "points": [[0, 0, 0]]},
        {"name": "a", "op": "points", "points": [[0, 0, 0]]}]})",
                    "/nodes/1/name"));
    CHECK(has_error(R"({"format": 1, "nodes": [
        {"name": "a", "op": "points", "points": [[0, 0, 0]]},
        {"name": "f", "op": "filter", "from": ["a"], "height": [5, 1]}]})",
                    "/nodes/1/height"));
    CHECK(has_error(R"({"format": 1, "space": "2d", "nodes": [
        {"name": "a", "op": "points", "points": [[0, 0]]},
        {"name": "f", "op": "filter", "from": ["a"], "slope": [0, 30]}]})",
                    "/nodes/1/slope"));
    CHECK(has_error(R"({"format": 1, "space": "2d", "nodes": [
        {"name": "a", "op": "points", "points": [[0, 0, 0]]}]})",
                    "/nodes/0/points/0"));
    CHECK(has_error(R"({"format": 1, "space": "2d", "nodes": [
        {"name": "a", "op": "points", "points": [[0, 0]]},
        {"name": "m", "op": "multimesh", "from": ["a"], "mesh": "rock.glb"}]})",
                    "/nodes/1/op"));
    // What each op needs, and the schema's own checks.
    CHECK(has_error(R"({"format": 1, "nodes": [{"name": "s", "op": "surface", "count": 3}]})",
                    "/nodes/0"));
    CHECK(has_error(R"({"format": 1, "nodes": [
        {"name": "a", "op": "points", "points": [[0, 0, 0]]},
        {"name": "c", "op": "command", "from": ["a"], "command": "creature.place"}]})",
                    "/nodes/1"));
    CHECK(has_error(R"({"format": 1, "nodes": [{"name": "x", "op": "explode"}]})", "/nodes/0/op"));
    CHECK(has_error(R"({"format": 1, "nodes": [
        {"name": "a", "op": "points", "points": [[0, 0, 0]], "expect": {"min": 5, "max": 2}}]})",
                    "/nodes/0/expect/max"));
    CHECK_FALSE(procedural::parse_pcg(parse(R"({"format": 1, "nodes": []})")).ok());
}

TEST_CASE("procedural graph steps keep, thin, move and order points the same everywhere") {
    // Hashes repeat and spread.
    CHECK(procedural::pcg_hash(1, 2, 3) == procedural::pcg_hash(1, 2, 3));
    CHECK(procedural::pcg_hash(1, 2, 3) != procedural::pcg_hash(1, 2, 4));
    CHECK(procedural::pcg_unit(5, 6) >= 0.0F);
    CHECK(procedural::pcg_unit(5, 6) < 1.0F);

    // A grid: whole cells centered in the area, jittered by the seed alone.
    const auto grid = procedural::pcg_grid({0, 0, 10, 4}, 2.0F, 0.0F, 3, false);
    REQUIRE(grid.size() == 10);
    CHECK(grid[0].position.x == doctest::Approx(1.0F));
    CHECK(grid[0].position.z == doctest::Approx(1.0F));
    CHECK(grid[9].position.x == doctest::Approx(9.0F));
    const auto jittered = procedural::pcg_grid({0, 0, 10, 4}, 2.0F, 0.5F, 3, false);
    const auto again = procedural::pcg_grid({0, 0, 10, 4}, 2.0F, 0.5F, 3, false);
    REQUIRE(jittered.size() == 10);
    for (std::size_t i = 0; i < jittered.size(); ++i) {
        CHECK(jittered[i].position == again[i].position);
        CHECK(std::abs(jittered[i].position.x - grid[i].position.x) <= 1.0F);
    }
    const auto flat = procedural::pcg_grid({0, 0, 4, 4}, 2.0F, 0.0F, 3, true);
    REQUIRE(flat.size() == 4);
    CHECK(flat[3].position == Vec3{3.0F, 3.0F, 0.0F});

    // Filters: area, height, keep (by seed), density and a limit, in order.
    std::vector<procedural::PcgPoint> line = line_of(100, 1.0F);
    procedural::PcgFilter area;
    area.area = std::array<float, 4>{10, -1, 19, 1};
    CHECK(procedural::pcg_filter(line, area, 1, false).size() == 10);
    procedural::PcgFilter keep;
    keep.keep = 0.3F;
    const auto kept = procedural::pcg_filter(line, keep, 7, false);
    CHECK(kept.size() > 15);
    CHECK(kept.size() < 45);
    CHECK(procedural::pcg_filter(line, keep, 7, false).size() == kept.size());
    CHECK(procedural::pcg_filter(line, keep, 8, false).front().position != kept.front().position);
    procedural::PcgFilter limit;
    limit.limit = 4;
    CHECK(procedural::pcg_filter(line, limit, 1, false).back().position.x == doctest::Approx(3.0F));
    line[5].density = 0.2F;
    procedural::PcgFilter dense;
    dense.density = Vec2{0.5F, 1.0F};
    CHECK(procedural::pcg_filter(line, dense, 1, false).size() == 99);

    // Pruning keeps the densest first, then puts them back in order.
    std::vector<procedural::PcgPoint> pair = line_of(2, 0.5F);
    pair[1].density = 0.9F;
    pair[0].density = 0.1F;
    const auto pruned = procedural::pcg_prune(pair, 1.0F, false);
    REQUIRE(pruned.size() == 1);
    CHECK(pruned[0].position.x == doctest::Approx(0.5F));
    CHECK(procedural::pcg_prune(line_of(20, 0.5F), 1.0F, false).size() == 10);

    // Difference and near measure across the ground.
    const std::vector<Vec3> house{{10.0F, 50.0F, 0.0F}};
    const auto away = procedural::pcg_difference(line_of(21, 1.0F), house, 3.0F, false);
    CHECK(away.size() == 16);
    const auto close = procedural::pcg_near(line_of(21, 1.0F), house, {2.0F, 4.0F}, false);
    CHECK(close.size() == 6);
    CHECK(procedural::pcg_near(line_of(5, 1.0F), {}, {0.0F, 100.0F}, false).empty());

    // Transforms: offsets, up, scale and a turn that repeats by seed.
    std::vector<procedural::PcgPoint> moved = line_of(3, 1.0F);
    procedural::PcgTransform transform;
    transform.offset = {0.0F, 1.0F, 0.0F};
    transform.offset_up = 0.5F;
    transform.scale = 2.0F;
    transform.rotate_random = 1.0F;
    procedural::pcg_transform(moved, transform, 11, false);
    CHECK(moved[1].position.y == doctest::Approx(1.5F));
    CHECK(moved[1].scale == doctest::Approx(2.0F));
    std::vector<procedural::PcgPoint> moved_again = line_of(3, 1.0F);
    procedural::pcg_transform(moved_again, transform, 11, false);
    CHECK(moved_again[2].rotation == moved[2].rotation);
    CHECK(moved[0].rotation.y != moved[1].rotation.y);
    procedural::PcgTransform tilt;
    tilt.align_to_normal = true;
    std::vector<procedural::PcgPoint> leaning = line_of(1, 1.0F);
    leaning[0].normal = {1.0F, 0.0F, 0.0F};
    procedural::pcg_transform(leaning, tilt, 1, false);
    CHECK(std::abs(leaning[0].rotation.z) ==
          doctest::Approx(std::numbers::pi_v<float> * 0.5F).epsilon(1e-4));

    // Routes go to the nearest point not yet visited.
    std::vector<procedural::PcgPoint> stops = line_of(4, 1.0F);
    std::swap(stops[1], stops[3]);
    const auto route = procedural::pcg_route_order(stops, false);
    CHECK(route[1].position.x == doctest::Approx(1.0F));
    CHECK(route[3].position.x == doctest::Approx(3.0F));
    const auto bounds = procedural::pcg_bounds(route, false);
    CHECK(bounds == std::array<float, 4>{0.0F, 0.0F, 3.0F, 0.0F});
}

TEST_CASE("pcg.run fills a 3D world from a graph and replaces what it made before") {
    const Project project("pcg_test_project");
    project.write("rock.prefab.json", R"({"format": 1, "components": {
        "BoxMesh": {"size": [0.5, 0.5, 0.5]}, "MeshInstance3D": {}, "Transform3D": {}}})");
    project.write("grass.foliage.json", R"({"format": 1, "kind": "grass", "mesh": "made:blade",
        "density": 2})");
    project.write("valley.pcg.json", R"({"format": 1, "seed": 7, "budget": {"entities": 200},
      "nodes": [
        {"name": "ground", "op": "surface", "on": ["valley"], "density": 0.5, "spacing": 0.5},
        {"name": "meadow", "op": "filter", "from": ["ground"], "area": [-10, -10, 0, 10]},
        {"name": "grass", "op": "foliage", "from": ["meadow"], "type": "grass.foliage.json"},
        {"name": "east", "op": "filter", "from": ["ground"], "area": [0, -10, 10, 10]},
        {"name": "spread", "op": "prune", "from": ["east"], "spacing": 3},
        {"name": "rocks", "op": "spawn", "from": ["spread"], "prefab": "rock.prefab.json",
         "components": {"StoryRef": {"ids": ["$node"]}}},
        {"name": "trees", "op": "multimesh", "from": ["spread"], "mesh": "made:tree"},
        {"name": "lamps", "op": "transform", "from": ["spread"], "offset_up": 2},
        {"name": "lights", "op": "command", "from": ["lamps"], "keep": 0.5,
         "command": "entity.create",
         "params": {"id": "$id", "components": {
            "Transform3D": {"position": "$position"},
            "OmniLight3D": {"light_energy": {"$pick": [1, 2]}}}}},
        {"name": "walk", "op": "route", "from": ["spread"], "closed": true}]})");
    REQUIRE(project
                .call("entity.create", R"({"id": "valley", "components": {
        "PlaneMesh": {"size": [20, 20]}, "MeshInstance3D": {}, "Transform3D": {}}})")
                .ok);
    world::World& world = project.world();

    command::Reply reply = project.call("pcg.run", R"({"path": "valley.pcg.json", "into": "v"})");
    REQUIRE_MESSAGE(reply.ok, reply.message);
    const json::Array& nodes = reply.value.find("nodes")->as_array();
    REQUIRE(nodes.size() == 10);
    const auto points = [&](std::size_t i) { return nodes[i].find("points")->as_int(); };
    CHECK(points(0) > 100);
    CHECK(points(1) + points(3) >= points(0));
    CHECK(points(4) < points(3));
    CHECK(points(4) > 5);
    const std::int64_t rocks = points(5);
    CHECK(rocks == points(4));
    CHECK(nodes[5].find("made")->as_int() == rocks);
    CHECK(nodes[6].find("made")->as_int() == 1);
    const std::int64_t lights = nodes[8].find("made")->as_int();
    CHECK(lights > 0);
    CHECK(lights < rocks);

    const entt::entity v = world.find("v");
    REQUIRE(world.valid(v));
    const std::vector<entt::entity> children = world.ecs().get<world::Transform3D>(v).children;
    // grass, the rocks, trees, the lights and the route, all under v.
    CHECK(std::cmp_equal(children.size(), 1 + rocks + 1 + lights + 1));

    // Foliage instances lie west, rocks east and apart, on the plane.
    const auto& grass = world.ecs().get<world::Foliage3D>(world.find("v_grass"));
    REQUIRE(grass.groups.size() == 1);
    CHECK(grass.groups[0].type == "grass.foliage.json");
    CHECK(std::cmp_equal(grass.groups[0].instances.size(), points(2) * 7));
    for (std::size_t i = 0; i < grass.groups[0].instances.size(); i += 7) {
        CHECK(grass.groups[0].instances[i] <= 0.0F);
    }
    std::vector<Vec3> rock_places;
    for (std::int64_t k = 1; k <= rocks; ++k) {
        const entt::entity rock = world.find("v_rocks_" + std::to_string(k));
        REQUIRE(world.valid(rock));
        CHECK(world.prefab_of(rock)->path == "rock.prefab.json");
        CHECK(world.ecs().get<world::StoryRef>(rock).ids == std::vector<std::string>{"rocks"});
        const Vec3 at = world.ecs().get<world::Transform3D>(rock).position;
        CHECK(at.x >= 0.0F);
        CHECK(at.y == doctest::Approx(0.0F).epsilon(1e-4));
        for (const Vec3 other : rock_places) {
            CHECK(across(at, other) >= 3.0F);
        }
        rock_places.push_back(at);
    }
    CHECK(std::cmp_equal(
        world.ecs().get<world::MultiMeshInstance3D>(world.find("v_trees")).instances.size(),
        rocks));
    // Commands' entities: lifted 2 m, energies picked by seed, and adopted by v.
    std::set<float> energies;
    for (const entt::entity child : children) {
        if (const auto* light = world.ecs().try_get<world::OmniLight3D>(child); light != nullptr) {
            CHECK(world.ecs().get<world::Transform3D>(child).position.y == doctest::Approx(2.0F));
            energies.insert(light->light_energy);
        }
    }
    CHECK_FALSE(energies.empty());
    CHECK(energies.size() <= 2);
    const auto& walk = world.ecs().get<world::Path3D>(world.find("v_walk"));
    CHECK(std::cmp_equal(walk.points.size(), rocks));
    CHECK(walk.closed);

    // The same graph again makes the same world, replacing what was there; one undo takes it
    // back.
    const json::Value first = reply.value;
    const Vec3 first_rock = world.ecs().get<world::Transform3D>(world.find("v_rocks_1")).position;
    reply = project.call("pcg.run", R"({"path": "valley.pcg.json", "into": "v"})");
    REQUIRE_MESSAGE(reply.ok, reply.message);
    CHECK(reply.value == first);
    CHECK(world.ecs().get<world::Transform3D>(world.find("v_rocks_1")).position == first_rock);
    CHECK(world.ecs().get<world::Transform3D>(v).children.size() == children.size());
    reply = project.call("pcg.run", R"({"path": "valley.pcg.json", "into": "v", "seed": 8})");
    REQUIRE_MESSAGE(reply.ok, reply.message);
    CHECK(world.ecs().get<world::Transform3D>(world.find("v_rocks_1")).position != first_rock);
    REQUIRE(project.call("undo", "{}").ok);
    CHECK(world.ecs().get<world::Transform3D>(world.find("v_rocks_1")).position == first_rock);

    // Over the budget, a dry run, and a command that isn't one.
    reply = project.call("pcg.run", R"({"into": "x", "graph": {"format": 1,
        "budget": {"points": 3}, "nodes": [
        {"name": "g", "op": "grid", "area": [0, 0, 4, 4], "spacing": 1}]}})");
    CHECK_FALSE(reply.ok);
    CHECK(reply.message.find("budget") != std::string::npos);
    CHECK_FALSE(world.valid(world.find("x")));
    const std::size_t before = world.ids().size();
    reply = project.call("pcg.run", R"({"into": "dry", "dry_run": true, "graph": {"format": 1,
        "nodes": [{"name": "g", "op": "grid", "area": [0, 0, 4, 4], "spacing": 1},
                  {"name": "p", "op": "spawn", "from": ["g"], "components": {}}]}})");
    REQUIRE_MESSAGE(reply.ok, reply.message);
    CHECK(world.ids().size() == before);
    reply = project.call("pcg.run", R"({"into": "x", "graph": {"format": 1, "nodes": [
        {"name": "g", "op": "points", "points": [[0, 0, 0]]},
        {"name": "c", "op": "command", "from": ["g"], "command": "creature.plase",
         "params": {}}]}})");
    CHECK_FALSE(reply.ok);
    CHECK(reply.message.find("/graph/nodes/1/command") != std::string::npos);
    reply = project.call("pcg.run", R"({"into": "x", "graph": {"format": 1, "nodes": [
        {"name": "g", "op": "points", "points": [[0, 0, 0]]},
        {"name": "c", "op": "command", "from": ["g"], "command": "entity.create",
         "params": {"id": "$id", "components": {"Nope": {}}}}]}})");
    CHECK_FALSE(reply.ok);
    CHECK(reply.message.find("point 1") != std::string::npos);
    CHECK_FALSE(world.valid(world.find("x")));
}

TEST_CASE("pcg.run follows entities, storyboard places and paths, in 3D and 2D") {
    const Project project("pcg_places_project");
    world::World& world = project.world();
    REQUIRE(project
                .call("batch", R"({"commands": [
        {"command": "entity.create", "params": {"id": "inn", "components": {
            "Transform3D": {"position": [10, 0, 5]}, "StoryRef": {"ids": ["place/inn"]}}}},
        {"command": "entity.create", "params": {"id": "well", "components": {
            "Transform3D": {"position": [-10, 0, 0]}, "OmniLight3D": {}}}},
        {"command": "entity.create", "params": {"id": "road", "components": {
            "Transform3D": {"position": [0, 0, 5]},
            "Path3D": {"points": [{"position": [0, 0, 0]}, {"position": [10, 0, 0]}]}}}}]})")
                .ok);
    command::Reply reply = project.call("pcg.run", R"({"into": "town", "graph": {"format": 1,
      "nodes": [
        {"name": "places", "op": "entities", "story": ["place/inn"], "with": ["OmniLight3D"]},
        {"name": "posts", "op": "path", "path": "road", "spacing": 2.5, "side": 1},
        {"name": "near_inn", "op": "near", "from": ["posts"], "story": ["place/inn"],
         "range": [0, 6]},
        {"name": "far", "op": "difference", "from": ["posts"], "to": ["places"], "distance": 7},
        {"name": "signs", "op": "spawn", "from": ["near_inn"], "components": {}}]}})");
    REQUIRE_MESSAGE(reply.ok, reply.message);
    const json::Array& nodes = reply.value.find("nodes")->as_array();
    CHECK(nodes[0].find("points")->as_int() == 2);
    CHECK(nodes[1].find("points")->as_int() == 5);
    // Posts at x 5, 7.5 and 10 are within 6 m of the inn; those at 0 and 2.5 more than 7 m
    // from both places.
    CHECK(nodes[2].find("points")->as_int() == 3);
    CHECK(nodes[3].find("points")->as_int() == 2);
    // The road runs along +x, so its right (Godot's: forward is -z) is +z.
    const Vec3 sign = world.ecs().get<world::Transform3D>(world.find("town_signs_1")).position;
    CHECK(sign.x == doctest::Approx(5.0F));
    CHECK(sign.z == doctest::Approx(6.0F));

    // 2D: a grid over a tile map's grass cells, foliage and a route along a Path2D.
    project.write("ground.tileset.json", R"({"format": 1, "tile_size": [16, 16], "sources": {
        "a": {"texture": "tiles.png", "tiles": {"0,0": {"name": "grass"},
                                                "1,0": {"name": "rock"}}}}})");
    project.write("tuft.foliage.json", R"({"format": 1, "space": "2d", "kind": "grass",
        "texture": "made:tuft", "size": [8, 16]})");
    REQUIRE(project
                .call("batch", R"({"commands": [
        {"command": "entity.create", "params": {"id": "map", "components": {
            "TileMapLayer": {"tile_set": "ground.tileset.json", "legend": {"g": "grass",
                             "r": "rock"}, "rows": ["ggrr", "ggrr"]},
            "Transform2D": {}}}},
        {"command": "entity.create", "params": {"id": "trail", "components": {
            "Transform2D": {"position": [0, 100]},
            "Path2D": {"points": [{"position": [0, 0]}, {"position": [64, 0]}]}}}}]})")
                .ok);
    reply = project.call("pcg.run", R"({"into": "meadow", "graph": {"format": 1, "space": "2d",
      "nodes": [
        {"name": "cells", "op": "grid", "area": [0, 0, 64, 32], "spacing": 8, "on": ["map"],
         "tiles": ["grass"]},
        {"name": "tufts", "op": "foliage", "from": ["cells"], "type": "tuft.foliage.json"},
        {"name": "steps", "op": "path", "path": "trail", "spacing": 16},
        {"name": "spun", "op": "transform", "from": ["steps"], "rotation_degrees": [0, 0, 90]},
        {"name": "stones", "op": "spawn", "from": ["spun"], "components": {}}]}})");
    REQUIRE_MESSAGE(reply.ok, reply.message);
    const json::Array& flat_nodes = reply.value.find("nodes")->as_array();
    CHECK(flat_nodes[0].find("points")->as_int() == 16); // the 32 by 32 grass, every 8 px
    const auto& tufts = world.ecs().get<world::Foliage2D>(world.find("meadow_tufts"));
    REQUIRE(tufts.groups[0].instances.size() == 16 * 4);
    for (std::size_t i = 0; i < tufts.groups[0].instances.size(); i += 4) {
        CHECK(tufts.groups[0].instances[i] < 32.0F);
    }
    CHECK(flat_nodes[2].find("points")->as_int() == 5);
    const auto& stone = world.ecs().get<world::Transform2D>(world.find("meadow_stones_2"));
    CHECK(stone.position.x == doctest::Approx(16.0F));
    CHECK(stone.position.y == doctest::Approx(100.0F));
    CHECK(stone.rotation == doctest::Approx(std::numbers::pi_v<float> * 0.5F));
}

TEST_CASE("pcg.preview and pcg.check look at a graph without placing anything") {
    const Project project("pcg_check_project");
    world::World& world = project.world();
    project.write("floor.navmesh.json", R"({"format": 1, "polygons": [[0, 1, 2, 3]],
        "vertices": [[0, 0, 0], [0, 0, 10], [10, 0, 10], [10, 0, 0]]})");
    REQUIRE(project
                .call("entity.create", R"({"id": "floor", "components": {
        "NavigationRegion3D": {"navigation_mesh": "floor.navmesh.json"}, "Transform3D": {}}})")
                .ok);
    const std::string graph = R"({"format": 1, "budget": {"points": 30}, "nodes": [
        {"name": "g", "op": "grid", "area": [0, 0, 20, 10], "spacing": 2,
         "expect": {"min": 10, "density": [0.2, 0.3], "area": [0, 0, 20, 10],
                    "reachable": true}},
        {"name": "few", "op": "filter", "from": ["g"], "limit": 2, "expect": {"min": 3}},
        {"name": "none", "op": "spawn", "from": ["few"], "keep": 0, "components": {}}]})";
    const std::size_t before = world.ids().size();

    command::Reply reply = project.call(
        "pcg.preview", R"({"image": "previews/g.png", "size": 64, "graph": )" + graph + "}");
    REQUIRE_MESSAGE(reply.ok, reply.message);
    const json::Array& nodes = reply.value.find("nodes")->as_array();
    CHECK(nodes[0].find("points")->as_int() == 50);
    CHECK(nodes[0].find("bounds")->as_array()[2].as_double() == doctest::Approx(19.0));
    CHECK(nodes[0].find("sample")->as_array().size() == 3);
    CHECK(nodes[2].find("makes")->as_int() == 0);
    CHECK(reply.value.find("legend")->as_array().size() == 3);
    CHECK(std::filesystem::file_size(project.root + "/previews/g.png") > 50);
    CHECK(world.ids().size() == before);

    reply = project.call("pcg.check", R"({"reach_from": [1, 0, 1], "graph": )" + graph + "}");
    REQUIRE_MESSAGE(reply.ok, reply.message);
    CHECK_FALSE(reply.value.find("passed")->as_bool());
    std::set<std::string> kinds;
    for (const json::Value& problem : reply.value.find("problems")->as_array()) {
        kinds.insert(problem.find("problem")->as_string());
        if (problem.find("problem")->as_string() == "unreachable") {
            // The grid's east half is off the floor.
            CHECK(problem.find("count")->as_int() == 25);
        }
    }
    CHECK(kinds == std::set<std::string>{"over_budget", "unreachable", "too_few", "empty"});
    CHECK(reply.value.find("nodes")->as_array()[0].find("density")->as_double() ==
          doctest::Approx(0.25));
    reply = project.call("pcg.check", R"({"graph": )" + graph + "}");
    CHECK_FALSE(reply.ok);
    CHECK(reply.message.find("/reach_from") != std::string::npos);
    CHECK(world.ids().size() == before);
}
