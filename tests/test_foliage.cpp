// Foliage, after Unreal's Foliage tool: .foliage.json types painted as Foliage3D and Foliage2D
// instances, bending in the wind and parting for characters, drawn, blocking and harvested.

#include "slash/canvas/canvas.hpp"
#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/platform/file.hpp"
#include "slash/procedural/foliage.hpp"
#include "slash/render/image.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/foliage.hpp"
#include "slash/world/tree3d.hpp"
#include "slash/world/wind.hpp"
#include "slash/world/world.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

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

// A blade of grass a meter tall and a rock, unshaded so their pixels are their colors.
constexpr std::string_view kBlade = R"({"format": 1,
    "materials": {"leaf": {"albedo_color": [0, 1, 0, 1], "shading_mode": "unshaded"}},
    "parts": [{"material": "leaf", "name": "blade", "shape": "box", "size": [0.4, 1, 0.4],
               "position": [0, 0.5, 0]}]})";
constexpr std::string_view kGrass = R"({"format": 1, "kind": "grass",
    "mesh": "blade.model.json", "density": 4, "spacing": 0.2, "scale": [0.8, 1.2],
    "wind": {"bend": 0.05}, "interaction": {"radius": 0.8, "bend": 0.6}})";
constexpr std::string_view kRock = R"({"format": 1, "kind": "rock",
    "mesh": "blade.model.json", "density": 1, "collision": {"shape": "sphere", "radius": 0.4},
    "harvest": {"item": "stone", "amount": 2}})";

// A 3D project: a ground plane 20 meters across, the types above and whatever else entities
// holds.
struct Project3D {
    std::string root;
    world::ComponentRegistry components;
    command::Registry commands;
    std::unique_ptr<command::Session> session;

    explicit Project3D(std::string name, std::string_view entities = "") : root(std::move(name)) {
        std::filesystem::remove_all(root);
        REQUIRE(slash::platform::create_directories(root).ok());
        REQUIRE(slash::platform::write_file(root + "/blade.model.json", kBlade).ok());
        REQUIRE(slash::platform::write_file(root + "/grass.foliage.json", kGrass).ok());
        REQUIRE(slash::platform::write_file(root + "/rock.foliage.json", kRock).ok());
        REQUIRE(slash::platform::write_file(root + "/main.scene.json",
                                            R"({"format": 1, "entities": {
              "ground": {"components": {"Transform3D": {}, "MeshInstance3D": {},
                                        "PlaneMesh": {"size": [20, 20]}}})" +
                                                std::string(entities) + "}}")
                    .ok());
        world::register_builtin_components(components);
        command::register_builtin_commands(commands);
        command::SessionOptions options;
        options.project_root = root;
        options.capture_logs = false;
        session = std::make_unique<command::Session>(components, commands, std::move(options));
        ok("scene.open", R"({"path": "main.scene.json"})");
    }
    Project3D(const Project3D&) = delete;
    Project3D& operator=(const Project3D&) = delete;
    Project3D(Project3D&&) = delete;
    Project3D& operator=(Project3D&&) = delete;
    ~Project3D() {
        session.reset();
        std::filesystem::remove_all(root);
    }

    json::Value ok(std::string_view name, std::string_view params = "{}") {
        command::Reply reply = session->call(name, parse(params));
        REQUIRE_MESSAGE(reply.ok, reply.message);
        return std::move(reply.value);
    }

    void steps(int ticks) const {
        for (int i = 0; i < ticks; ++i) {
            session->world().step(1.0 / 60.0);
        }
    }

    [[nodiscard]] const world::Foliage3D& foliage() const {
        const world::World& w = session->world();
        return w.ecs().get<const world::Foliage3D>(w.find("flora"));
    }
};

float planar(Vec3 a, Vec3 b) {
    const float dx = a.x - b.x;
    const float dz = a.z - b.z;
    return std::sqrt((dx * dx) + (dz * dz));
}

} // namespace

TEST_CASE("foliage: types are checked and read with their defaults") {
    // A 3D type needs a mesh, ranges go low to high and shapes belong to their space.
    const auto errors = world::validate_foliage(parse(R"({"format": 1, "kind": "grass",
        "scale": [2, 1], "collision": {"shape": "circle"}})"));
    REQUIRE(errors.size() >= 3);
    bool mesh = false;
    bool scale = false;
    bool shape = false;
    for (const json::ValidationError& error : errors) {
        mesh = mesh || error.path == "/mesh";
        scale = scale || error.path == "/scale";
        shape = shape || error.path == "/collision/shape";
    }
    CHECK(mesh);
    CHECK(scale);
    CHECK(shape);
    CHECK(world::validate_foliage(parse(R"({"format": 1, "kind": "fern",
        "mesh": "a.model.json"})"))
              .front()
              .path == "/kind");

    // A capsule stands on the ground by default; a 2D circle sits up off it.
    auto tree = world::parse_foliage(parse(R"({"format": 1, "kind": "tree",
        "mesh": "tree.model.json", "collision": {"shape": "capsule", "radius": 0.3,
        "height": 4}, "climbable": true})"));
    REQUIRE(tree.ok());
    CHECK(tree.value().collision.offset.y == doctest::Approx(2.0));
    CHECK(tree.value().climbable);
    CHECK(tree.value().density == doctest::Approx(1.0));
    auto bush = world::parse_foliage(parse(R"({"format": 1, "space": "2d", "kind": "bush",
        "texture": "bush.png", "collision": {"shape": "circle", "radius": 6}})"));
    REQUIRE(bush.ok());
    CHECK(bush.value().collision.offset.y == doctest::Approx(-6.0));
}

TEST_CASE("foliage: a brush paints a type onto the ground within its radius, spaced") {
    Project3D project("foliage_paint_project");
    const json::Value painted = project.ok(
        "foliage.paint",
        R"({"entity": "flora", "type": "grass.foliage.json", "at": [1, 0, -2], "radius": 2})");
    const std::int64_t added = painted.find("added")->as_int();
    // Four a square meter over a circle of 2 meters, less what spacing turns away.
    CHECK(added > 20);
    CHECK(added <= 60);
    const world::Foliage3D& foliage = project.foliage();
    REQUIRE(foliage.groups.size() == 1);
    const world::FoliageGroup3D& group = foliage.groups[0];
    REQUIRE(procedural::foliage_count(group) == static_cast<std::size_t>(added));
    for (std::size_t i = 0; i < procedural::foliage_count(group); ++i) {
        const procedural::FoliagePlace3D place = procedural::foliage_place(group, i);
        CHECK(planar(place.position, {1.0F, 0.0F, -2.0F}) <= 2.0F + 1e-3F);
        CHECK(place.position.y == doctest::Approx(0.0).epsilon(0.01));
        CHECK(place.scale >= 0.8F - 1e-3F);
        CHECK(place.scale <= 1.2F + 1e-3F);
        for (std::size_t k = 0; k < i; ++k) {
            CHECK(planar(place.position, procedural::foliage_place(group, k).position) >=
                  0.2F - 1e-3F);
        }
    }
    // The same stroke again finds the room taken.
    const json::Value again = project.ok(
        "foliage.paint",
        R"({"entity": "flora", "type": "grass.foliage.json", "at": [1, 0, -2], "radius": 2})");
    CHECK(again.find("added")->as_int() < added);

    // Erasing a smaller circle removes only what is in it.
    const json::Value near = project.ok(
        "foliage.get", R"({"entity": "flora", "at": [1, 0, -2], "radius": 1, "limit": 0})");
    const std::int64_t within = near.find("within")->as_int();
    CHECK(within > 0);
    const std::int64_t before = project.ok("foliage.get", R"({"entity": "flora"})")
                                    .find("groups")
                                    ->as_array()[0]
                                    .find("count")
                                    ->as_int();
    const json::Value erased =
        project.ok("foliage.erase", R"({"entity": "flora", "at": [1, 0, -2], "radius": 1})");
    CHECK(erased.find("removed")->as_int() == within);
    const json::Value after = project.ok("foliage.get", R"({"entity": "flora"})");
    CHECK(after.find("groups")->as_array()[0].find("count")->as_int() == before - within);
    CHECK(after.find("groups")->as_array()[0].find("kind")->as_string() == "grass");

    // Undo takes the erase back.
    project.ok("undo");
    CHECK(project.ok("foliage.get", R"({"entity": "flora"})")
              .find("groups")
              ->as_array()[0]
              .find("count")
              ->as_int() == before);
}

TEST_CASE("foliage: grass leans with the wind and parts for a character, then springs back") {
    Project3D project("foliage_bend_project",
                      R"(, "flora": {"components": {"Transform3D": {}, "Foliage3D": {"groups": [
                             {"type": "grass.foliage.json",
                              "instances": [0, 0, 0, 0, 0, 0, 1, 3, 0, 0, 0, 0, 0, 1]}]}}},
                           "breeze": {"components": {"Transform3D": {}, "WindSource3D": {
                             "strength": 4, "min_gust_amount": 0, "max_gust_amount": 0,
                             "turbulence": 0}}},
                           "walker": {"components": {"Transform3D": {"position": [0.3, 0.5, 0]},
                             "CharacterBody3D": {}}})");
    world::World& w = project.session->world();
    const entt::entity flora = w.find("flora");
    const auto type = procedural::foliage_type(w, "grass.foliage.json");
    REQUIRE(type != nullptr);
    // The wind blows along -z at 4 m/s: 0.05 radians a meter a second of it.
    const world::WindField wind(w, world::Tree3D(w));
    const Vec3 base{3.0F, 0.0F, 0.0F};
    const Vec2 blown = procedural::foliage_lean_3d(w, flora, 0, 1, *type, base, wind.at(base, 0.0));
    CHECK(blown.x == doctest::Approx(0.0).epsilon(0.001));
    CHECK(blown.y == doctest::Approx(-0.2).epsilon(0.01));

    // The walker beside the first blade pushes it away from it, along +x.
    project.steps(30);
    const std::vector<procedural::FoliagePush> pushes = procedural::foliage_pushes(w);
    REQUIRE(pushes.size() == 1);
    CHECK(pushes[0].index == 0);
    CHECK(pushes[0].lean.x < -0.1F);
    // 0.3 m into a 0.8 m radius: 0.6 times five eighths, the spring settled near it.
    CHECK(-pushes[0].lean.x == doctest::Approx(0.375).epsilon(0.1));
    CHECK(std::abs(pushes[0].lean.y) < 1e-3F);
    const json::Value got = project.ok("foliage.get", R"({"entity": "flora"})");
    CHECK(got.find("pushed")->as_int() == 1);

    // The walker leaves and the blade springs back.
    w.ecs().get<world::Transform3D>(w.find("walker")).position = {-5.0F, 0.5F, 0.0F};
    project.steps(120);
    CHECK(procedural::foliage_pushes(w).empty());
}

TEST_CASE("foliage: rocks block rays, and harvesting one removes it and says what it gave") {
    Project3D project("foliage_harvest_project",
                      R"(, "flora": {"components": {"Transform3D": {}, "Foliage3D": {"groups": [
                             {"type": "grass.foliage.json", "instances": [0, 0, 0, 0, 0, 0, 1]},
                             {"type": "rock.foliage.json",
                              "instances": [2, 0, 0, 0, 0, 0, 1, 4, 0, 0, 0, 0, 0, 2]}]}}})");
    project.steps(1);
    // The second rock, twice the size, stands with its sphere's top 1.6 m up.
    const json::Value hit =
        project.ok("physics.raycast", R"({"from": [4, 5, 0], "to": [4, -1, 0]})");
    REQUIRE(hit.find("hit")->is_object());
    CHECK(hit.find("hit")->find("id")->as_string() == "flora");
    CHECK(hit.find("hit")->find("instance")->as_array()[0].as_int() == 1);
    CHECK(hit.find("hit")->find("instance")->as_array()[1].as_int() == 1);
    CHECK(hit.find("hit")->find("point")->as_array()[1].as_double() ==
          doctest::Approx(1.6).epsilon(0.02));
    // Grass doesn't block.
    const json::Value grass =
        project.ok("physics.raycast", R"({"from": [0, 5, 0], "to": [0, -1, 0]})");
    CHECK(grass.find("hit")->is_null());

    const std::string subscription =
        project.ok("events.subscribe", R"({"kinds": ["foliage_harvested"]})")
            .find("subscription")
            ->as_string();
    const json::Value harvested =
        project.ok("foliage.harvest", R"({"entity": "flora", "at": [3.8, 0, 0], "radius": 1})");
    CHECK(harvested.find("item")->as_string() == "stone");
    CHECK(harvested.find("amount")->as_int() == 2);
    CHECK(harvested.find("type")->as_string() == "rock.foliage.json");
    CHECK(procedural::foliage_count(project.foliage().groups[1]) == 1);
    const json::Value events =
        project.ok("events.read", R"({"subscription": ")" + subscription + "\"}");
    CHECK(json::write(events).find("foliage_harvested") != std::string::npos);
    // Grass gives nothing.
    command::Reply nothing = project.session->call(
        "foliage.harvest", parse(R"({"entity": "flora", "at": [0, 0, 0], "radius": 0.5})"));
    CHECK_FALSE(nothing.ok);
    project.steps(1);
    const json::Value gone =
        project.ok("physics.raycast", R"({"from": [4, 5, 0], "to": [4, -1, 0]})");
    CHECK(gone.find("hit")->is_null());
}

TEST_CASE("foliage: instances draw their type's model and fade out past their cull distance") {
    const std::string scene = R"(, "flora": {"components": {"Transform3D": {}, "Foliage3D": {
                                    "groups": [{"type": "grass.foliage.json",
                                                "instances": [0, 0, 0, 0, 0, 0, 1]}]}}},
                                  "camera": {"components": {"Transform3D": {"position": [0, 0.5, 3]},
                                    "Camera3D": {"current": true}}})";
    const auto green_middle = [](Project3D& project) {
        auto captured = slash::canvas::capture(project.session->world(), project.session->assets());
        REQUIRE(captured.ok());
        const slash::render::Image& image = captured.value().image;
        const std::size_t at = (((image.height / 2) * image.width) + (image.width / 2)) * 4;
        return image.pixels[at] < 40 && image.pixels[at + 1] > 200;
    };
    {
        Project3D project("foliage_draw_project", scene);
        CHECK(green_middle(project));
    }
    {
        Project3D project("foliage_cull_project", scene);
        REQUIRE(slash::platform::write_file(project.root + "/grass.foliage.json",
                                            R"({"format": 1, "kind": "grass",
            "mesh": "blade.model.json", "cull_distance": [1, 2]})")
                    .ok());
        project.session->poll_assets();
        CHECK_FALSE(green_middle(project));
    }
}

namespace {

constexpr std::string_view kTuft = R"({"format": 1, "space": "2d", "kind": "grass",
    "texture": "tuft.png", "size": [8, 16], "density": 0.01, "random_flip": false,
    "wind": {"bend": 0.2}, "interaction": {"radius": 20, "bend": 0.5}})";
constexpr std::string_view kStone = R"({"format": 1, "space": "2d", "kind": "rock",
    "texture": "tuft.png", "size": [8, 8], "collision": {"shape": "circle", "radius": 4},
    "harvest": {"item": "pebble"}})";

// A 2D project with a green tuft texture and the 2D types above.
struct Project2D {
    std::string root;
    world::ComponentRegistry components;
    command::Registry commands;
    std::unique_ptr<command::Session> session;

    explicit Project2D(std::string name, std::string_view entities) : root(std::move(name)) {
        std::filesystem::remove_all(root);
        REQUIRE(slash::platform::create_directories(root).ok());
        slash::render::Image image{.width = 8, .height = 16, .pixels = {}};
        for (int i = 0; i < 8 * 16; ++i) {
            image.pixels.insert(image.pixels.end(), {0, 255, 0, 255});
        }
        REQUIRE(slash::platform::write_file(root + "/tuft.png",
                                            slash::render::encode_png(image).value())
                    .ok());
        REQUIRE(slash::platform::write_file(root + "/tuft.foliage.json", kTuft).ok());
        REQUIRE(slash::platform::write_file(root + "/stone.foliage.json", kStone).ok());
        REQUIRE(slash::platform::write_file(root + "/main.scene.json",
                                            R"({"format": 1, "entities": {)" +
                                                std::string(entities) + "}}")
                    .ok());
        world::register_builtin_components(components);
        command::register_builtin_commands(commands);
        command::SessionOptions options;
        options.project_root = root;
        options.capture_logs = false;
        session = std::make_unique<command::Session>(components, commands, std::move(options));
        ok("scene.open", R"({"path": "main.scene.json"})");
    }
    Project2D(const Project2D&) = delete;
    Project2D& operator=(const Project2D&) = delete;
    Project2D(Project2D&&) = delete;
    Project2D& operator=(Project2D&&) = delete;
    ~Project2D() {
        session.reset();
        std::filesystem::remove_all(root);
    }

    json::Value ok(std::string_view name, std::string_view params = "{}") {
        command::Reply reply = session->call(name, parse(params));
        REQUIRE_MESSAGE(reply.ok, reply.message);
        return std::move(reply.value);
    }

    void steps(int ticks) const {
        for (int i = 0; i < ticks; ++i) {
            session->world().step(1.0 / 60.0);
        }
    }

    [[nodiscard]] bool green_at(Vec2 point) const {
        auto captured = slash::canvas::capture(session->world(), session->assets());
        REQUIRE(captured.ok());
        const slash::render::Image& image = captured.value().image;
        const auto x = static_cast<std::size_t>(point.x);
        const auto y = static_cast<std::size_t>(point.y);
        REQUIRE(x < image.width);
        REQUIRE(y < image.height);
        const std::size_t at = ((y * image.width) + x) * 4;
        return image.pixels[at] < 40 && image.pixels[at + 1] > 200;
    }
};

} // namespace

TEST_CASE("foliage 2D: tufts painted on an area stand on their origin and lean in the wind") {
    Project2D project("foliage2d_paint_project", R"("meadow": {"components": {
                           "Transform2D": {"position": [100, 100]}}})");
    const json::Value painted = project.ok(
        "foliage.paint",
        R"({"entity": "meadow", "type": "tuft.foliage.json", "at": [200, 200], "radius": 60})");
    // A hundredth a square pixel over a circle of 60 pixels: about 113.
    CHECK(painted.find("added")->as_int() > 80);
    const world::World& w = project.session->world();
    const auto& foliage = w.ecs().get<const world::Foliage2D>(w.find("meadow"));
    REQUIRE(foliage.groups.size() == 1);
    for (std::size_t i = 0; i < procedural::foliage_count(foliage.groups[0]); ++i) {
        // In the meadow's space, 100 pixels up and left of the world's.
        const Vec2 at = procedural::foliage_place(foliage.groups[0], i).position;
        const float dx = at.x - 100.0F;
        const float dy = at.y - 100.0F;
        CHECK(std::sqrt((dx * dx) + (dy * dy)) <= 60.0F + 1e-3F);
    }
    // A 3D type can't go in it.
    REQUIRE(slash::platform::write_file(project.root + "/blade.foliage.json",
                                        R"({"format": 1, "kind": "grass", "mesh": "b.glb"})")
                .ok());
    command::Reply wrong = project.session->call(
        "foliage.paint",
        parse(R"({"entity": "meadow", "type": "blade.foliage.json", "at": [0, 0, 0]})"));
    CHECK_FALSE(wrong.ok);
}

TEST_CASE("foliage 2D: the canvas draws each tuft, bent by the wind and parted by a character") {
    Project2D project("foliage2d_draw_project",
                      R"("meadow": {"components": {"Transform2D": {}, "Foliage2D": {"groups": [
                             {"type": "tuft.foliage.json", "instances": [100, 100, 0, 2]}]}}},
                         "breeze": {"components": {"Transform2D": {}, "WindSource2D": {
                             "enabled": false, "strength": 300, "min_gust_amount": 0,
                             "max_gust_amount": 0, "turbulence": 0}}})");
    // Sixteen pixels across and 32 up from its foot at (100, 100).
    CHECK(project.green_at({100.0F, 90.0F}));
    CHECK(project.green_at({100.0F, 70.0F}));
    CHECK_FALSE(project.green_at({112.0F, 70.0F}));
    // Blown right at 3 meters a second's worth: 0.6 radians, its top 18 pixels over and down to
    // y 74, so 24 pixels up it spans x 106 to 122 at y 80.
    world::World& w = project.session->world();
    w.ecs().get<world::WindSource2D>(w.find("breeze")).enabled = true;
    CHECK(project.green_at({113.0F, 80.0F}));
    CHECK_FALSE(project.green_at({97.0F, 80.0F}));
    CHECK_FALSE(project.green_at({112.0F, 71.0F}));
    w.ecs().get<world::WindSource2D>(w.find("breeze")).enabled = false;

    // A character just left of it pushes it right.
    project.ok("entity.create", R"({"id": "kid", "components": {
        "Transform2D": {"position": [90, 95]}, "CharacterBody2D": {}}})");
    project.steps(30);
    const std::vector<procedural::FoliagePush> pushes = procedural::foliage_pushes(w);
    REQUIRE(pushes.size() == 1);
    CHECK(pushes[0].lean.x > 0.1F);
}

TEST_CASE("foliage 2D: stones block bodies and rays and can be harvested") {
    Project2D project("foliage2d_block_project",
                      R"("yard": {"components": {"Transform2D": {}, "Foliage2D": {"groups": [
                             {"type": "stone.foliage.json", "instances": [100, 100, 0, 1]}]}}})");
    project.steps(1);
    // Its circle sits on its foot, 4 pixels up, so its top is at y 92.
    const json::Value hit =
        project.ok("physics.raycast", R"({"from": [100, 50], "to": [100, 150]})");
    REQUIRE(hit.find("hit")->is_object());
    CHECK(hit.find("hit")->find("id")->as_string() == "yard");
    CHECK(hit.find("hit")->find("instance")->as_array()[0].as_int() == 0);
    CHECK(hit.find("hit")->find("point")->as_array()[1].as_double() ==
          doctest::Approx(92.0).epsilon(0.01));
    const json::Value harvested =
        project.ok("foliage.harvest", R"({"entity": "yard", "at": [102, 98], "radius": 8})");
    CHECK(harvested.find("item")->as_string() == "pebble");
    CHECK(harvested.find("amount")->as_int() == 1);
    project.steps(1);
    CHECK(project.ok("physics.raycast", R"({"from": [100, 50], "to": [100, 150]})")
              .find("hit")
              ->is_null());
}
