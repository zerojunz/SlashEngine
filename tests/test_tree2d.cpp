#include "slash/canvas/particles.hpp"
#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/core/math.hpp"
#include "slash/physics2d/physics2d.hpp"
#include "slash/platform/file.hpp"
#include "slash/render/image.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/scene.hpp"
#include "slash/world/tree2d.hpp"
#include "slash/world/world.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <cstddef>
#include <filesystem>
#include <limits>
#include <memory>
#include <numbers>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace canvas = slash::canvas;
namespace command = slash::command;
namespace json = slash::json;
namespace physics2d = slash::physics2d;
namespace render = slash::render;
namespace world = slash::world;
using slash::Vec2;

namespace {

constexpr float kPi = std::numbers::pi_v<float>;
constexpr double kTick = 1.0 / 60.0;

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE_MESSAGE(parsed.ok(), (parsed.ok() ? std::string() : parsed.error().message));
    return std::move(parsed).value();
}

std::string compact(const json::Value& value) {
    return json::write(value, json::Style::Compact);
}

void check_near(Vec2 got, Vec2 want, float tolerance = 1e-4F) {
    CHECK(got.x == doctest::Approx(want.x).epsilon(tolerance));
    CHECK(got.y == doctest::Approx(want.y).epsilon(tolerance));
}

// A world with the built-in components, and entities with a Transform2D in it.
struct Scene {
    world::ComponentRegistry components;
    world::World world{components};

    Scene() { world::register_builtin_components(components); }

    entt::entity add(std::string_view id, Vec2 position = {}, float rotation = 0.0F,
                     Vec2 scale = {1.0F, 1.0F}) {
        const entt::entity entity = world.create(id).value();
        auto& transform = world.ecs().emplace<world::Transform2D>(entity);
        transform.position = position;
        transform.rotation = rotation;
        transform.scale = scale;
        return entity;
    }

    world::Transform2D& transform(entt::entity entity) {
        return world.ecs().get<world::Transform2D>(entity);
    }

    void adopt(entt::entity parent, std::vector<entt::entity> children) {
        transform(parent).children = std::move(children);
    }
};

// A 4x4 opaque white texture.
void write_texture(const std::string& path) {
    render::Image image{.width = 4, .height = 4, .pixels = {}};
    image.pixels.assign(std::size_t{4} * 4 * 4, 255);
    REQUIRE(slash::platform::write_file(path, render::encode_png(image).value()).ok());
}

// A 160x90 project with a white texture, and a session over it.
struct Project {
    std::string root;
    world::ComponentRegistry components;
    command::Registry commands;
    std::unique_ptr<command::Session> session;

    explicit Project(std::string folder) : root(std::move(folder)) {
        std::filesystem::remove_all(root);
        REQUIRE(slash::platform::create_directories(root).ok());
        REQUIRE(slash::platform::write_file(root + "/project.json", R"({"format": 1,
            "name": "Tree", "main_scene": "main.scene.json", "version": "1.0.0",
            "window": {"width": 160, "height": 90}})")
                    .ok());
        write_texture(root + "/white.png");
        world::register_builtin_components(components);
        command::register_builtin_commands(commands);
        command::SessionOptions options;
        options.project_root = root;
        options.capture_logs = false;
        session = std::make_unique<command::Session>(components, commands, options);
    }
    ~Project() { std::filesystem::remove_all(root); }
    Project(const Project&) = delete;
    Project& operator=(const Project&) = delete;
    Project(Project&&) = delete;
    Project& operator=(Project&&) = delete;

    json::Value ok(std::string_view name, std::string_view params = "{}") const {
        command::Reply reply = session->call(name, parse(params));
        REQUIRE_MESSAGE(reply.ok, reply.message);
        return std::move(reply.value);
    }
};

} // namespace

TEST_CASE("atan2 matches the standard library and its special cases") {
    for (int degrees = -180; degrees <= 180; ++degrees) {
        for (const double radius : {1e-3, 1.0, 3e4}) {
            const double angle = degrees * std::numbers::pi / 180.0;
            const auto x = static_cast<float>(radius * std::cos(angle));
            const auto y = static_cast<float>(radius * std::sin(angle));
            CAPTURE(degrees);
            CAPTURE(radius);
            CHECK(slash::atan2(y, x) ==
                  doctest::Approx(std::atan2(double{y}, double{x})).epsilon(2e-7));
        }
    }
    CHECK(slash::atan2(1.0F, 1.0F) == kPi / 4.0F);
    CHECK(slash::atan2(1.0F, 0.0F) == kPi / 2.0F);
    CHECK(slash::atan2(-1.0F, 0.0F) == -kPi / 2.0F);
    CHECK(slash::atan2(0.0F, -1.0F) == kPi);
    CHECK(slash::atan2(-0.0F, -1.0F) == -kPi);
    CHECK(slash::atan2(0.0F, 0.0F) == 0.0F);
    const float infinity = std::numeric_limits<float>::infinity();
    CHECK(slash::atan2(infinity, infinity) == kPi / 4.0F);
    CHECK(slash::atan2(-1.0F, -infinity) == -kPi);
    CHECK(slash::atan2(infinity, 5.0F) == kPi / 2.0F);
    CHECK(slash::atan2(std::numeric_limits<float>::quiet_NaN(), 1.0F) == 0.0F);
}

TEST_CASE("affine and parts turn a Transform2D into Godot's matrix and back") {
    world::Transform2D transform;
    transform.position = {10.0F, 20.0F};
    transform.rotation = 0.5F;
    transform.scale = {2.0F, 3.0F};
    transform.skew = 0.25F;
    const world::Affine2D matrix = world::affine(transform);
    check_near(matrix.x, {std::cos(0.5F) * 2.0F, std::sin(0.5F) * 2.0F});
    check_near(matrix.y, {-std::sin(0.75F) * 3.0F, std::cos(0.75F) * 3.0F});
    CHECK(matrix.origin == transform.position);

    const world::Transform2D back = world::parts(matrix);
    CHECK(back.position == transform.position);
    CHECK(back.rotation == doctest::Approx(0.5F));
    check_near(back.scale, transform.scale);
    CHECK(back.skew == doctest::Approx(0.25F));

    // A flip on y comes back as a negative y scale, as Godot's get_scale gives it.
    transform.scale = {2.0F, -3.0F};
    const world::Transform2D flipped = world::parts(world::affine(transform));
    check_near(flipped.scale, {2.0F, -3.0F});
    CHECK(flipped.skew == doctest::Approx(0.25F));
    // A flip on x comes back turned half round and flipped on y: the same matrix.
    transform.scale = {-2.0F, 3.0F};
    const world::Affine2D mirrored = world::affine(transform);
    const world::Affine2D again = world::affine(world::parts(mirrored));
    check_near(again.x, mirrored.x);
    check_near(again.y, mirrored.y);

    // A matrix after its inverse puts every point back.
    const world::Affine2D round_trip = matrix.inverse() * matrix;
    check_near(round_trip.apply({7.0F, -4.0F}), {7.0F, -4.0F});
    check_near((matrix * matrix.inverse()).apply({-3.0F, 11.0F}), {-3.0F, 11.0F});
}

TEST_CASE("an entity in another's children is placed in its space, as Godot's Node2D") {
    Scene scene;
    const entt::entity ship = scene.add("ship", {100.0F, 50.0F}, kPi / 2.0F, {2.0F, 2.0F});
    const entt::entity gun = scene.add("gun", {10.0F, 0.0F}, 0.25F, {1.0F, 0.5F});
    const entt::entity muzzle = scene.add("muzzle", {5.0F, 0.0F});
    const entt::entity loose = scene.world.create("loose").value();
    scene.adopt(ship, {gun});
    scene.adopt(gun, {muzzle});
    const world::Tree2D tree(scene.world);
    CHECK(tree.nested());
    CHECK(tree.parent(gun) == ship);
    CHECK(tree.parent(muzzle) == gun);
    CHECK((tree.parent(ship) == entt::null));

    // A root's global transform is its own, to the bit.
    const world::Transform2D at_root = tree.global(ship);
    CHECK(at_root.position == Vec2{100.0F, 50.0F});
    CHECK(at_root.rotation == kPi / 2.0F);
    CHECK(at_root.scale == Vec2{2.0F, 2.0F});
    CHECK(at_root.children.empty());

    // Turned a quarter clockwise and doubled, the ship puts the gun 20 pixels below it.
    const world::Transform2D placed = tree.global(gun);
    check_near(placed.position, {100.0F, 70.0F});
    CHECK(placed.rotation == doctest::Approx(kPi / 2.0F + 0.25F));
    check_near(placed.scale, {2.0F, 1.0F});
    CHECK(placed.skew == 0.0F);

    // The gun scales unevenly, so the muzzle goes through the matrices.
    const float turn = (kPi / 2.0F) + 0.25F;
    const world::Transform2D tip = tree.global(muzzle);
    check_near(tip.position, {100.0F + (10.0F * std::cos(turn)), 70.0F + (10.0F * std::sin(turn))});
    CHECK(tip.rotation == doctest::Approx(turn));
    check_near(tip.scale, {2.0F, 1.0F});
    const world::Affine2D multiplied = world::affine(scene.transform(ship)) *
                                       world::affine(scene.transform(gun)) *
                                       world::affine(scene.transform(muzzle));
    check_near(tree.global_affine(muzzle).x, multiplied.x);
    check_near(tree.global_affine(muzzle).y, multiplied.y);

    // A skewed parent leans its children.
    scene.transform(gun).skew = 0.3F;
    const world::Affine2D leaning = world::affine(scene.transform(ship)) *
                                    world::affine(scene.transform(gun)) *
                                    world::affine(scene.transform(muzzle));
    check_near(tree.global_affine(muzzle).origin, leaning.origin);
    check_near(tree.global_affine(muzzle).y, leaning.y);

    // An entity with no Transform2D is at the origin.
    CHECK(tree.global(loose).position == Vec2{});
    CHECK((tree.parent(loose) == entt::null));

    Scene flat;
    flat.add("a");
    CHECK_FALSE(world::Tree2D(flat.world).nested());
}

TEST_CASE("a child listed twice keeps its first parent, and a cycle is broken") {
    Scene scene;
    const entt::entity a = scene.add("a");
    const entt::entity b = scene.add("b");
    const entt::entity c = scene.add("c");
    scene.adopt(b, {c});
    scene.adopt(a, {c});
    const entt::entity x = scene.add("x");
    const entt::entity y = scene.add("y");
    scene.adopt(x, {y});
    scene.adopt(y, {x});
    const entt::entity self = scene.add("self");
    scene.adopt(self, {self});

    const world::Tree2D tree(scene.world);
    CHECK(tree.parent(c) == a);
    CHECK(tree.parent(y) == x);
    CHECK((tree.parent(x) == entt::null));
    CHECK((tree.parent(self) == entt::null));
}

TEST_CASE("children draw after their parent, before the next root") {
    Scene scene;
    const entt::entity first = scene.add("first");
    const entt::entity second = scene.add("second");
    const entt::entity child = scene.add("child");
    const entt::entity grandchild = scene.add("grandchild");
    const entt::entity sibling = scene.add("sibling");
    scene.adopt(first, {child, sibling});
    scene.adopt(child, {grandchild});
    const world::Tree2D tree(scene.world);
    const std::vector<entt::entity> order{first, child, grandchild, sibling, second};
    for (std::size_t i = 1; i < order.size(); ++i) {
        CAPTURE(i);
        CHECK(tree.draw_key(order[i - 1]) < tree.draw_key(order[i]));
    }
}

TEST_CASE("set_global_position and set_global_rotation place a child in the world") {
    Scene scene;
    const entt::entity parent = scene.add("parent", {100.0F, 0.0F}, kPi / 2.0F, {2.0F, 2.0F});
    const entt::entity child = scene.add("child");
    scene.adopt(parent, {child});
    const world::Tree2D tree(scene.world);
    tree.set_global_position(scene.world, child, {100.0F, 40.0F});
    check_near(scene.transform(child).position, {20.0F, 0.0F});
    check_near(tree.global(child).position, {100.0F, 40.0F});
    tree.set_global_rotation(scene.world, child, 1.0F);
    CHECK(scene.transform(child).rotation == doctest::Approx(1.0F - (kPi / 2.0F)));
    CHECK(tree.global(child).rotation == doctest::Approx(1.0F));
    // A root's are its own.
    tree.set_global_position(scene.world, parent, {3.0F, 4.0F});
    CHECK(scene.transform(parent).position == Vec2{3.0F, 4.0F});
}

TEST_CASE("scene validate reports Transform2D children that don't make a tree") {
    world::ComponentRegistry components;
    world::register_builtin_components(components);
    const auto errors = world::validate_scene(components, parse(R"({
        "format": 1,
        "entities": {
            "a": {"components": {"Transform2D": {"children": ["b", "a", "label"]}}},
            "b": {"components": {"Transform2D": {"children": ["c"]}}},
            "c": {"components": {"Transform2D": {"children": ["b"]}}},
            "d": {"components": {"Transform2D": {"children": ["c", null]}}},
            "e": {"components": {"Transform2D": {"children": ["f"]}}},
            "f": {"components": {"Transform2D": {"children": ["e"]}}},
            "label": {}
        }
    })"));
    std::vector<std::string> found;
    for (const json::ValidationError& error : errors) {
        found.push_back(error.path + ": " + error.message);
    }
    const std::string at = "/components/Transform2D/children/";
    CHECK(found ==
          std::vector<std::string>{
              "/entities/a" + at + "1: an entity can't be its own child",
              "/entities/a" + at + R"(2: "label" has no Transform2D to place in "a"'s space)",
              "/entities/c" + at + R"(0: "b" is already a child of "a")",
              "/entities/d" + at + R"(0: "c" is already a child of "b")",
              "/entities/d" + at + "1: children can't be null",
              "/entities/f" + at + R"(0: "e" is an ancestor of "f", so it can't be its child)",
          });

    // A tree is fine, whatever order its entities are listed in.
    CHECK(world::validate_scene(components, parse(R"({
        "format": 1,
        "entities": {
            "arm": {"components": {"Transform2D": {"children": ["hand"]}}},
            "body": {"components": {"Transform2D": {"children": ["arm", "leg"]}}},
            "hand": {"components": {"Transform2D": {}}},
            "leg": {"components": {"Transform2D": {}}}
        }
    })"))
              .empty());
}

TEST_CASE("children draw after their parent with its z_index, visibility and modulate") {
    const Project project("tree2d_draw_project");
    // A 16-pixel ship with a flag 8 pixels to its right, and a crate 8 pixels right of that,
    // made between them.
    project.ok("batch", R"({"commands": [
        {"command": "entity.create", "params": {"id": "ship", "components": {
            "Transform2D": {"position": [40, 45], "scale": [4, 4]},
            "Sprite2D": {"texture": "white.png"}}}},
        {"command": "entity.create", "params": {"id": "crate", "components": {
            "Transform2D": {"position": [56, 45], "scale": [4, 4]},
            "Sprite2D": {"texture": "white.png"}}}},
        {"command": "entity.create", "params": {"id": "flag", "components": {
            "Transform2D": {"position": [2, 0]}, "Sprite2D": {"texture": "white.png"}}}},
        {"command": "component.set", "params": {"id": "ship", "components": {
            "Transform2D": {"children": ["flag"]}}}}]})");
    const auto drawn = [&] { return compact(project.ok("frame.summary")); };
    // The flag draws after the ship and under the crate.
    CHECK(drawn() == compact(parse(R"({"viewport": [160, 90], "drawn": [
        {"id": "ship", "kind": "Sprite2D", "rect": [32, 37, 16, 16], "shown": 0.5},
        {"id": "flag", "kind": "Sprite2D", "rect": [40, 37, 16, 16], "shown": 0.5},
        {"id": "crate", "kind": "Sprite2D", "rect": [48, 37, 16, 16]}]})")));

    // The ship's z_index lifts the flag over the crate too.
    project.ok("component.set", R"({"id": "ship", "components": {"Sprite2D": {"z_index": 1}}})");
    CHECK(drawn() == compact(parse(R"({"viewport": [160, 90], "drawn": [
        {"id": "crate", "kind": "Sprite2D", "rect": [48, 37, 16, 16], "shown": 0.5},
        {"id": "ship", "kind": "Sprite2D", "rect": [32, 37, 16, 16], "shown": 0.5},
        {"id": "flag", "kind": "Sprite2D", "rect": [40, 37, 16, 16]}]})")));

    // A clear ship makes its flag clear, and a hidden one hides it.
    project.ok("component.set",
               R"({"id": "ship", "components": {"Sprite2D": {"modulate": [1, 1, 1, 0]}}})");
    CHECK(drawn() == compact(parse(R"({"viewport": [160, 90], "drawn": [
        {"id": "crate", "kind": "Sprite2D", "rect": [48, 37, 16, 16]}],
        "clear": ["ship", "flag"]})")));
    project.ok("component.set", R"({"id": "ship", "components": {"Sprite2D": {
        "modulate": [1, 1, 1, 1], "visible": false}}})");
    CHECK(drawn() == compact(parse(R"({"viewport": [160, 90], "drawn": [
        {"id": "crate", "kind": "Sprite2D", "rect": [48, 37, 16, 16]}]})")));
}

TEST_CASE("entity.get gives a child's parent and global transform") {
    const Project project("tree2d_get_project");
    project.ok("batch", R"({"commands": [
        {"command": "entity.create", "params": {"id": "ship", "components": {
            "Transform2D": {"position": [100, 50], "rotation": 1.5707964, "scale": [2, 2]}}}},
        {"command": "entity.create", "params": {"id": "gun", "components": {
            "Transform2D": {"position": [10, 0], "skew": 0.25}}}},
        {"command": "component.set", "params": {"id": "ship", "components": {
            "Transform2D": {"children": ["gun"]}}}}]})");
    CHECK(compact(project.ok("entity.get", R"({"id": "gun", "omit_defaults": true})")) ==
          compact(parse(R"({"id": "gun",
            "components": {"Transform2D": {"position": [10.0, 0.0], "skew": 0.25}},
            "parent": "ship",
            "global": {"position": [100, 70], "rotation": 1.5708, "scale": [2, 2],
                       "skew": 0.25}})")));
    // A root has neither.
    CHECK(compact(project.ok("entity.get", R"({"id": "ship", "components": ["Transform2D"]})"))
              .find("\"global\"") == std::string::npos);
}

TEST_CASE("entity.destroy takes the entity's children with it, as Godot frees a node's") {
    const Project project("tree2d_destroy_project");
    project.ok("batch", R"({"commands": [
        {"command": "entity.create", "params": {"id": "ship", "components": {"Transform2D": {}}}},
        {"command": "entity.create", "params": {"id": "gun", "components": {"Transform2D": {}}}},
        {"command": "entity.create", "params": {"id": "muzzle", "components": {"Transform2D": {}}}},
        {"command": "entity.create", "params": {"id": "sail", "components": {"Transform2D": {}}}},
        {"command": "component.set", "params": {"id": "ship", "components": {
            "Transform2D": {"children": ["gun", "sail"]}}}},
        {"command": "component.set", "params": {"id": "gun", "components": {
            "Transform2D": {"children": ["muzzle"]}}}}]})");
    // The gun goes with its muzzle and leaves the ship's list.
    CHECK(compact(project.ok("entity.destroy", R"({"id": "gun"})")) ==
          R"({"descendants":["muzzle"],"destroyed":"gun"})");
    CHECK(compact(project.ok("entity.get",
                             R"({"id": "ship"})")["components"]["Transform2D"]["children"]) ==
          R"(["sail"])");

    // Undo brings both back, in the list again.
    project.ok("undo");
    CHECK(compact(project.ok("entity.get",
                             R"({"id": "ship"})")["components"]["Transform2D"]["children"]) ==
          R"(["gun","sail"])");
    CHECK(project.ok("entity.get", R"({"id": "muzzle"})")["parent"].as_string() == "gun");

    // The ship takes everything.
    CHECK(compact(project.ok("entity.destroy", R"({"id": "ship"})")) ==
          R"({"descendants":["gun","sail","muzzle"],"destroyed":"ship"})");
    CHECK(compact(project.ok("world.query", "{}")) ==
          R"({"count":0,"entities":[],"truncated":false})");
}

TEST_CASE("an edit that breaks the tree fails and changes nothing, as Godot's add_child") {
    const Project project("tree2d_refuse_project");
    project.ok("batch", R"({"commands": [
        {"command": "entity.create", "params": {"id": "ship", "components": {"Transform2D": {}}}},
        {"command": "entity.create", "params": {"id": "gun", "components": {"Transform2D": {}}}},
        {"command": "entity.create", "params": {"id": "note"}},
        {"command": "component.set", "params": {"id": "ship", "components": {
            "Transform2D": {"children": ["gun"]}}}}]})");
    const auto refused = [&](std::string_view name, std::string_view params) {
        const command::Reply reply = project.session->call(name, parse(params));
        CHECK_FALSE(reply.ok);
        CHECK(reply.code == command::code::kInvalidArgument);
        return reply.message;
    };
    CHECK(
        refused("component.set",
                R"({"id": "gun", "components": {"Transform2D": {"children": ["ship"]}}})") ==
        R"(/entities/gun/components/Transform2D/children/0: "ship" is an ancestor of "gun", so it can't be its child)");
    CHECK(
        refused("entity.create", R"({"id": "boat", "components": {
                      "Transform2D": {"children": ["gun"]}}})") ==
        R"(/entities/boat/components/Transform2D/children/0: "gun" is already a child of "ship")");
    CHECK(
        refused(
            "component.set",
            R"({"id": "ship", "components": {"Transform2D": {"children": ["gun", "note"]}}})") ==
        R"(/entities/ship/components/Transform2D/children/1: "note" has no Transform2D to place in "ship"'s space)");
    // A dry run says so too.
    CHECK_FALSE(project.session
                    ->call("component.set", parse(R"({"id": "ship", "dry_run": true, "components": {
                               "Transform2D": {"children": ["ship"]}}})"))
                    .ok);
    CHECK(compact(project.ok("entity.get",
                             R"({"id": "ship"})")["components"]["Transform2D"]["children"]) ==
          R"(["gun"])");
    CHECK(project.ok("entity.get", R"({"id": "gun"})")["components"]["Transform2D"]["children"]
              .as_array()
              .empty());
}

TEST_CASE("physics bodies inside other entities are where their parents put them") {
    world::ComponentRegistry components;
    world::register_builtin_components(components);
    world::World game(components);
    physics2d::install(game, {});
    const auto add = [&](std::string_view id, Vec2 position, float rotation = 0.0F,
                         Vec2 scale = {1.0F, 1.0F}) {
        const entt::entity entity = game.create(id).value();
        auto& transform = game.ecs().emplace<world::Transform2D>(entity);
        transform.position = position;
        transform.rotation = rotation;
        transform.scale = scale;
        return entity;
    };
    const auto step = [&](int ticks) {
        for (int i = 0; i < ticks; ++i) {
            game.step(kTick);
        }
    };

    // A floor 50 pixels below a platform, which moves it.
    const entt::entity platform = add("platform", {100.0F, 100.0F});
    const entt::entity floor = add("floor", {0.0F, 50.0F});
    game.ecs().emplace<world::StaticBody2D>(floor);
    world::CollisionShape2D slab;
    slab.size = {200.0F, 20.0F};
    game.ecs().emplace<world::CollisionShape2D>(floor, slab);
    game.ecs().get<world::Transform2D>(platform).children = {floor};
    step(1);
    auto hit = physics2d::raycast(game, {100.0F, 0.0F}, {100.0F, 300.0F});
    REQUIRE(hit.has_value());
    CHECK(hit->entity == floor);
    CHECK(hit->point.y == doctest::Approx(140.0F));
    game.ecs().get<world::Transform2D>(platform).position = {400.0F, 100.0F};
    step(1);
    hit = physics2d::raycast(game, {400.0F, 0.0F}, {400.0F, 300.0F});
    REQUIRE(hit.has_value());
    CHECK(hit->point.y == doctest::Approx(140.0F));
    CHECK_FALSE(physics2d::raycast(game, {100.0F, 0.0F}, {100.0F, 300.0F}).has_value());

    // A ball in a turned carrier falls in the world; its own position is in the carrier's space,
    // and it comes to rest on the floor and sleeps.
    const entt::entity carrier = add("carrier", {400.0F, 0.0F}, 0.3F, {1.5F, 1.5F});
    const entt::entity ball = add("ball", {});
    game.ecs().emplace<world::RigidBody2D>(ball);
    world::CollisionShape2D round;
    round.shape = "circle";
    round.radius = 5.0F;
    game.ecs().emplace<world::CollisionShape2D>(ball, round);
    game.ecs().get<world::Transform2D>(carrier).children = {ball};
    world::Tree2D(game).set_global_position(game, ball, {413.7F, 60.0F});
    step(20);
    const Vec2 fell = world::Tree2D(game).global(ball).position;
    CHECK(fell.x == doctest::Approx(413.7F));
    CHECK(fell.y > 70.0F);
    for (int i = 0; i < 600 && !game.ecs().get<world::RigidBody2D>(ball).sleeping; ++i) {
        step(1);
    }
    const world::Transform2D rest = world::Tree2D(game).global(ball);
    CHECK(rest.position.x == doctest::Approx(413.7F));
    // The carrier's scale makes it 7.5 pixels round, on a floor 140 pixels down.
    CHECK(rest.position.y == doctest::Approx(132.5F).epsilon(0.001));
    // Asleep, it stays asleep: its own position, rounded through the carrier's, is no move.
    for (int i = 0; i < 30; ++i) {
        step(1);
        REQUIRE(game.ecs().get<world::RigidBody2D>(ball).sleeping);
    }

    // A character moves in the world: half as far in a carrier that doubles it.
    const entt::entity rig = add("rig", {1000.0F, 0.0F}, 0.0F, {2.0F, 2.0F});
    const entt::entity hero = add("hero", {});
    game.ecs().emplace<world::CharacterBody2D>(hero).velocity = {60.0F, 0.0F};
    game.ecs().emplace<world::CollisionShape2D>(hero, round);
    game.ecs().get<world::Transform2D>(rig).children = {hero};
    step(1);
    CHECK(game.ecs().get<world::Transform2D>(hero).position.x == doctest::Approx(0.5F));
    CHECK(world::Tree2D(game).global(hero).position.x == doctest::Approx(1001.0F));
}

TEST_CASE("particles emit where their emitter's parents put it") {
    world::ComponentRegistry components;
    world::register_builtin_components(components);
    world::World game(components);
    canvas::add_particles_system(game);
    const entt::entity rig = game.create("rig").value();
    auto& rig_transform = game.ecs().emplace<world::Transform2D>(rig);
    rig_transform.position = {100.0F, 0.0F};
    rig_transform.scale = {2.0F, 2.0F};
    const entt::entity torch = game.create("torch").value();
    game.ecs().emplace<world::Transform2D>(torch).position = {10.0F, 5.0F};
    std::string path;
    REQUIRE(components.find("CPUParticles2D")
                ->assign(game, torch, parse(R"({"amount": 1, "lifetime": 10, "gravity": [0, 0],
                                   "initial_velocity_min": 0, "initial_velocity_max": 0})"),
                         path)
                .ok());
    game.ecs().get<world::Transform2D>(rig).children = {torch};
    game.step(kTick);
    const std::vector<canvas::Particle> live = canvas::particles(game, torch);
    REQUIRE(live.size() == 1);
    check_near(live[0].position, {120.0F, 10.0F});
}
