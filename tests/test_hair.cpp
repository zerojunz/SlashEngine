// Hair and fur: groom files, the strand solver, guides following, 2D growth along a line, and
// grooms grown over a model, stepped by 3D and 2D physics and drawn.
#include "slash/canvas/canvas.hpp"
#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/platform/file.hpp"
#include "slash/render/image.hpp"
#include "slash/scene3d/hair.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/hair.hpp"
#include "slash/world/world.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace command = slash::command;
namespace json = slash::json;
namespace scene3d = slash::scene3d;
namespace world = slash::world;
using slash::Vec2;
using slash::Vec3;

namespace {

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE_MESSAGE(parsed.ok(), text);
    return std::move(parsed).value();
}

float distance(Vec3 a, Vec3 b) {
    const Vec3 d = a - b;
    return static_cast<float>(std::sqrt((d.x * d.x) + (d.y * d.y) + (d.z * d.z)));
}

// A strand of points straight along way from root, spaced by gap.
std::vector<Vec3> straight(Vec3 root, Vec3 way, int points, float gap) {
    std::vector<Vec3> out;
    for (int k = 0; k < points; ++k) {
        out.push_back(root + (way * (gap * static_cast<float>(k))));
    }
    return out;
}

world::StrandStep loose_step() {
    world::StrandStep step;
    step.seconds = 1.0F / 60.0F;
    step.steps = 4;
    step.stiffness = 0.0F;
    step.tip_stiffness = 0.0F;
    step.bend_stiffness = 0.0F;
    step.damping = 1.0F;
    return step;
}

// A head of two spheres, its scalp a material of its own, and a ball covered in fur.
constexpr std::string_view kHead = R"({"format": 1,
    "materials": {"skin": {"albedo_color": [0.8, 0.6, 0.5, 1]},
                  "scalp": {"albedo_color": [0.3, 0.2, 0.1, 1]}},
    "parts": [{"name": "head", "shape": "sphere", "radius": 0.12, "material": "skin",
               "children": [{"name": "scalp", "shape": "sphere", "radius": 0.118,
                             "position": [0, 0.03, 0], "material": "scalp",
                             "operation": "union"}]}]})";
constexpr std::string_view kLong = R"({"format": 1, "guides": 12, "density": 3000,
    "max_strands": 200, "segments": 5, "length": 0.2, "lift": 0.2, "comb": [0, -1, 0],
    "simulation": {"stiffness": 0.1, "tip_stiffness": 0.0}})";
constexpr std::string_view kFur = R"({"format": 1, "render": "shells", "guides": 8,
    "length": 0.03, "shell_count": 6})";
constexpr std::string_view kMane = R"({"format": 1, "space": "2d", "guides": 4,
    "density": 0.5, "segments": 4, "length": 30, "lift": 1,
    "root_color": [1, 0, 0, 1], "tip_color": [1, 0, 0, 1], "width": 4, "tip_width": 4,
    "simulation": {"stiffness": 0.1, "tip_stiffness": 0.0}})";

// A project with the files above, the scene's entities and a session on it.
struct Project {
    std::string root;
    world::ComponentRegistry components;
    command::Registry commands;
    std::unique_ptr<command::Session> session;

    Project(std::string name, std::string_view entities) : root(std::move(name)) {
        std::filesystem::remove_all(root);
        REQUIRE(slash::platform::create_directories(root).ok());
        const std::map<std::string, std::string_view> files{{"head.model.json", kHead},
                                                            {"long.groom.json", kLong},
                                                            {"fur.groom.json", kFur},
                                                            {"mane.groom.json", kMane}};
        for (const auto& [file, text] : files) {
            REQUIRE(slash::platform::write_file(root + "/" + file, text).ok());
        }
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
    Project(const Project&) = delete;
    Project& operator=(const Project&) = delete;
    Project(Project&&) = delete;
    Project& operator=(Project&&) = delete;
    ~Project() {
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
};

constexpr std::string_view kHeadEntity = R"("head": {"components": {
    "Transform3D": {}, "MeshInstance3D": {"mesh": "head.model.json"},
    "StaticBody3D": {}, "CollisionShape3D": {"shape": "sphere", "radius": 0.12},
    "HairInstance3D": {"groom": "long.groom.json", "section": "scalp"}}},
  "camera": {"components": {"Transform3D": {"position": [0, 0, 0.8]},
    "Camera3D": {"current": true}}})";

} // namespace

TEST_CASE("hair: groom files are checked and read with their defaults") {
    auto groom = world::parse_groom(parse(R"({"format": 1})"));
    REQUIRE(groom.ok());
    CHECK(groom.value().space == "3d");
    CHECK(groom.value().length == doctest::Approx(0.1));
    CHECK(groom.value().guides == 32);
    // A 2D groom's lengths are pixels.
    auto flat = world::parse_groom(parse(R"({"format": 1, "space": "2d"})"));
    REQUIRE(flat.ok());
    CHECK(flat.value().length == doctest::Approx(24.0));
    CHECK(flat.value().comb.y > 0.0F);
    auto shells =
        world::validate_groom(parse(R"({"format": 1, "space": "2d", "render": "shells"})"));
    REQUIRE(shells.size() == 1);
    CHECK(shells[0].path == "/render");
    auto wide = world::validate_groom(parse(R"({"format": 1, "width": 0.001, "tip_width": 0.01})"));
    REQUIRE(wide.size() == 1);
    CHECK(wide[0].path == "/tip_width");
    CHECK_FALSE(world::validate_groom(parse(R"({"format": 1, "lenght": 0.2})")).empty());
    CHECK(world::is_groom_path("hair/long.groom.json"));
    CHECK_FALSE(world::is_groom_path("long.json"));
}

TEST_CASE("hair: strands start at their groomed shape, sag under gravity and keep lengths") {
    const std::vector<Vec3> targets = straight({0, 0, 0}, {1, 0, 0}, 5, 0.1F);
    world::Strands strands;
    world::StrandStep step = loose_step();
    step.seconds = 0.0F;
    world::step_strands(strands, 5, targets, step);
    REQUIRE(strands.current.size() == 5);
    CHECK(distance(strands.current[4], targets[4]) == doctest::Approx(0.0));
    step = loose_step();
    step.gravity = {0, -9.8F, 0};
    for (int i = 0; i < 120; ++i) {
        world::step_strands(strands, 5, targets, step);
    }
    // The root stays, the tip hangs below it, and each segment keeps its length.
    CHECK(distance(strands.current[0], targets[0]) == doctest::Approx(0.0));
    CHECK(strands.current[4].y < -0.3F);
    for (std::size_t k = 1; k < 5; ++k) {
        CHECK(distance(strands.current[k], strands.current[k - 1]) ==
              doctest::Approx(0.1).epsilon(0.02));
    }
    // A stiff groom holds its shape against gravity.
    world::Strands stiff;
    step.stiffness = 1.0F;
    step.tip_stiffness = 1.0F;
    for (int i = 0; i < 120; ++i) {
        world::step_strands(stiff, 5, targets, step);
    }
    CHECK(distance(stiff.current[4], targets[4]) < 0.01F);
}

TEST_CASE("hair: strands keep off spheres, capsules and boxes, and blow downwind") {
    const std::vector<Vec3> targets = straight({0, 0, 0}, {1, 0, 0}, 6, 0.1F);
    const auto hang = [&](const world::StrandCollider& collider) {
        world::Strands strands;
        world::StrandStep step = loose_step();
        step.gravity = {0, -9.8F, 0};
        step.point_radius = 0.01F;
        const std::vector<world::StrandCollider> colliders{collider};
        step.colliders = colliders;
        for (int i = 0; i < 180; ++i) {
            world::step_strands(strands, 6, targets, step);
        }
        return strands.current;
    };
    world::StrandCollider sphere;
    sphere.a = {0.25F, -0.15F, 0.0F};
    sphere.b = sphere.a;
    sphere.radius = 0.1F;
    for (const Vec3 point : hang(sphere)) {
        CHECK(distance(point, sphere.a) >= 0.1F);
    }
    world::StrandCollider box;
    box.box = true;
    box.place.origin = {0.3F, -0.2F, 0.0F};
    box.half = {0.1F, 0.1F, 0.1F};
    for (const Vec3 point : hang(box)) {
        const bool inside = std::abs(point.x - 0.3F) < 0.1F && std::abs(point.y + 0.2F) < 0.1F &&
                            std::abs(point.z) < 0.1F;
        CHECK_FALSE(inside);
    }
    // The wind carries a strand standing up over toward where it blows.
    const std::vector<Vec3> up = straight({0, 0, 0}, {0, 1, 0}, 5, 0.1F);
    world::Strands blown;
    world::StrandStep step = loose_step();
    step.stiffness = 0.1F;
    step.tip_stiffness = 0.0F;
    step.drag_coefficient = 1.0F;
    step.wind = [](Vec3 /*at*/) { return Vec3{4.0F, 0.0F, 0.0F}; };
    for (int i = 0; i < 120; ++i) {
        world::step_strands(blown, 5, up, step);
    }
    CHECK(blown.current[4].x > 0.05F);
    CHECK(std::abs(blown.current[4].z) < 1e-4F);
}

TEST_CASE("hair: a strand follows its guide, gathered toward it at the tip") {
    const std::vector<Vec3> guide = straight({0, 0, 0}, {0, 1, 0}, 5, 0.1F);
    std::vector<Vec3> out;
    world::follow_guide(guide, {0.05F, 0, 0}, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, {1, 0, 0}, false, out);
    REQUIRE(out.size() == 5);
    CHECK(distance(out[0], {0.05F, 0, 0}) == doctest::Approx(0.0));
    CHECK(distance(out[4], {0.05F, 0.4F, 0}) == doctest::Approx(0.0).epsilon(1e-5));
    // Half as long, and clumped onto the guide at its tip.
    world::follow_guide(guide, {0.05F, 0, 0}, 0.5F, 1.0F, 0.0F, 0.0F, 0.0F, {1, 0, 0}, false, out);
    CHECK(distance(out[4], {0.0F, 0.2F, 0}) < 1e-4F);
    // A curl in a plane stays in it.
    world::follow_guide(guide, {}, 1.0F, 0.0F, 0.02F, 2.0F, 0.0F, {1, 0, 0}, true, out);
    for (const Vec3 point : out) {
        CHECK(point.z == doctest::Approx(0.0));
    }
}

TEST_CASE("hair: a 2D groom grows guides and strands along its line, to its left") {
    auto groom = world::parse_groom(parse(kMane));
    REQUIRE(groom.ok());
    const std::vector<Vec2> line{{0, 0}, {100, 0}};
    const world::HairGrowth2D growth = world::grow_hair_2d(groom.value(), line, {}, {}, {});
    REQUIRE(growth.guides.size() == 4);
    CHECK(growth.strands.size() == 50);
    for (const world::HairGuide2D& guide : growth.guides) {
        CHECK(guide.root.y == doctest::Approx(0.0));
        CHECK(guide.root.x >= 0.0F);
        CHECK(guide.root.x <= 100.0F);
        // Drawn left to right, up the screen.
        CHECK(guide.normal.y < -0.9F);
        CHECK(guide.rest.size() == 5);
        CHECK(guide.rest.back().y == doctest::Approx(-30.0).epsilon(0.25));
    }
}

TEST_CASE("hair: a HairInstance3D grows over its section, steps in physics and swings") {
    Project project("hair_head_project", kHeadEntity);
    world::World& w = project.session->world();
    const entt::entity head = w.find("head");
    auto grown = scene3d::hair_growth(w, head);
    REQUIRE(grown.ok());
    const scene3d::HairGrowth3D& growth = *grown.value();
    CHECK(growth.guides.size() == 12);
    CHECK(growth.points == 6);
    CHECK(growth.strands.size() > 12);
    // Grown on the scalp only: every root is on its sphere.
    for (const scene3d::HairRoot& root : growth.guides) {
        CHECK(distance(root.position, {0, 0.03F, 0}) == doctest::Approx(0.118).epsilon(0.05));
    }
    // Grown once, kept by its key.
    CHECK(scene3d::hair_growth(w, head).value() == grown.value());
    json::Value before = project.ok("hair.get", R"({"entity": "head"})");
    CHECK_FALSE(before["simulated"].as_bool());
    CHECK(before["guides"].as_int() == 12);
    project.steps(30);
    json::Value hanging = project.ok("hair.get", R"({"entity": "head"})");
    CHECK(hanging["simulated"].as_bool());
    CHECK(hanging["tip_moved"]["max"].as_double() > 0.005);
    // Moving the head leaves its hair trailing, then it settles.
    project.ok("component.set",
               R"({"id": "head", "components": {"Transform3D": {"position": [0.3, 0, 0]}}})");
    project.steps(3);
    const double trailing =
        project.ok("hair.get", R"({"entity": "head"})")["tip_moved"]["max"].as_double();
    CHECK(trailing > 0.1);
    project.steps(240);
    CHECK(project.ok("hair.get", R"({"entity": "head"})")["tip_moved"]["max"].as_double() <
          trailing / 4.0);
}

TEST_CASE("hair: grooms that can't grow say why") {
    Project project("hair_problems_project", R"("a": {"components": {"Transform3D": {},
        "MeshInstance3D": {"mesh": "head.model.json"},
        "HairInstance3D": {"groom": "long.groom.json", "section": "scalpp"}}},
      "b": {"components": {"Transform3D": {}, "MeshInstance3D": {"mesh": "head.model.json"},
        "HairInstance3D": {"groom": "mane.groom.json"}}},
      "c": {"components": {"Transform3D": {},
        "HairInstance3D": {"groom": "long.groom.json"}}})");
    const std::vector<slash::scene3d::Problem> problems =
        scene3d::check_hair(project.session->world());
    REQUIRE(problems.size() == 3);
    std::map<std::string, std::string> by_entity;
    for (const auto& problem : problems) {
        by_entity[std::string(project.session->world().id_of(problem.entity))] = problem.message;
    }
    CHECK(by_entity["a"].find("did you mean \"scalp\"") != std::string::npos);
    CHECK(by_entity["b"].find("2D groom") != std::string::npos);
    CHECK(by_entity["c"].find("MeshInstance3D") != std::string::npos);
    command::Reply reply = project.session->call("hair.get", parse(R"({"entity": "a"})"));
    CHECK_FALSE(reply.ok);
    reply = project.session->call("hair.get", parse(R"({"entity": "heda"})"));
    CHECK_FALSE(reply.ok);
}

TEST_CASE("hair: strands, cards and fur shells draw over the model") {
    const auto meshes = [](Project& project) {
        return project.ok("frame.capture", R"({"points": [[0, 0]]})")["drawn"]["meshes"].as_int();
    };
    Project bald("hair_bald_project", R"("head": {"components": {"Transform3D": {},
        "MeshInstance3D": {"mesh": "head.model.json"}}},
      "camera": {"components": {"Transform3D": {"position": [0, 0, 0.8]},
        "Camera3D": {"current": true}}})");
    const std::int64_t alone = meshes(bald);
    Project hairy("hair_strands_project", kHeadEntity);
    CHECK(meshes(hairy) == alone + 1);
    // Fur draws a layer of shells each.
    hairy.ok("component.set",
             R"({"id": "head", "components": {"HairInstance3D": {"groom": "fur.groom.json",
                 "section": ""}}})");
    CHECK(meshes(hairy) == alone + 6);
    // The pattern the shells cut out is the same everywhere.
    const slash::render::Image image = scene3d::hair_shell_image();
    CHECK(image.width == 128);
    std::size_t bare = 0;
    for (std::size_t at = 3; at < image.pixels.size(); at += 4) {
        bare += image.pixels[at] == 0 ? 1U : 0U;
    }
    CHECK(bare > 128 * 128 / 4);
    CHECK(bare < 128 * 128 * 3 / 4);
}

TEST_CASE("hair: a Hair2D hangs, blows in a 2D draft and draws its strands") {
    Project project("hair_2d_project", R"("doll": {"components": {
        "Transform2D": {"position": [80, 20]},
        "Hair2D": {"groom": "mane.groom.json", "root_line": [[10, 0], [-10, 0]]}}},
      "draft": {"components": {"Transform2D": {},
        "WindSource2D": {"strength": 400, "min_gust_amount": 0, "max_gust_amount": 0,
                         "turbulence": 0}}})");
    json::Value at_rest = project.ok("hair.get", R"({"entity": "doll", "points": true})");
    CHECK_FALSE(at_rest["simulated"].as_bool());
    // Drawn right to left, the line hangs its hair down the screen.
    CHECK(at_rest["bounds"]["max"].as_array()[1].as_double() ==
          doctest::Approx(50.0).epsilon(0.05));
    project.steps(60);
    json::Value blown = project.ok("hair.get", R"({"entity": "doll"})");
    CHECK(blown["simulated"].as_bool());
    CHECK(blown["tip_moved"]["max"].as_double() > 2.0);
    // Its strands draw in its colors below the line.
    json::Value captured = project.ok("frame.capture", R"({"points": [[80, 30]]})");
    CHECK(captured["colors"].as_array()[0].as_array()[0].as_double() > 0.9);
    CHECK(captured["colors"].as_array()[0].as_array()[1].as_double() < 0.1);
}
