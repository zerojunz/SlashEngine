// Character cloth: a Cloth3D's cape on a model's skin, skinned where its map says and free to
// swing up to its max distance elsewhere, as Chaos Cloth, and a Cloth2D's on a cut-out.

#include "slash/canvas/canvas.hpp"
#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/platform/file.hpp"
#include "slash/scene3d/cloth.hpp"
#include "slash/scene3d/model.hpp"
#include "slash/scene3d/scene3d.hpp"
#include "slash/scene3d/soft_body.hpp"
#include "slash/world/cloth2d.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
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

constexpr int kColumns = 5;
constexpr int kRows = 6;

// A figure with one bone, Spine, a meter and a half up: a small body and a cape behind it from
// its shoulders (y 1.5) down to y 0.5, both skinned to Spine. The cape's vertices go row by row
// from the top.
scene3d::Model caped_figure() {
    scene3d::Model model;
    for (const char* name : {"body", "cape"}) {
        scene3d::ModelMaterial material;
        material.name = name;
        model.materials.push_back(material);
    }
    const auto node = [&](const char* name, int parent, Vec3 position) {
        scene3d::ModelNode added;
        added.name = name;
        added.parent = parent;
        added.position = position;
        model.nodes.push_back(added);
    };
    node("Root", -1, {});
    node("Spine", 0, {0.0F, 1.5F, 0.0F});
    node("Figure", 0, {});
    world::Affine3D unbind;
    unbind.origin = {0.0F, -1.5F, 0.0F};
    model.skins.push_back({.name = "Skin", .joints = {1}, .inverse_binds = {unbind}});
    const auto weighted = [](scene3d::ModelSurface& surface) {
        scene3d::VertexWeights weights;
        weights.weights[0] = 1.0F;
        surface.weights.assign(surface.vertices.size(), weights);
    };
    scene3d::ModelSurface body;
    body.material = 0;
    for (const auto& [x, y] : {std::pair{-0.2F, 0.6F}, {0.2F, 0.6F}, {0.2F, 1.5F}, {-0.2F, 1.5F}}) {
        body.vertices.push_back({.position = {x, y, 0.0F}, .normal = {0.0F, 0.0F, 1.0F}});
    }
    body.indices = {0, 1, 2, 0, 2, 3};
    weighted(body);
    scene3d::ModelSurface cape;
    cape.material = 1;
    for (int row = 0; row < kRows; ++row) {
        for (int column = 0; column < kColumns; ++column) {
            const float x = -0.3F + (0.6F * static_cast<float>(column) / (kColumns - 1));
            const float y = 1.5F - (1.0F * static_cast<float>(row) / (kRows - 1));
            cape.vertices.push_back({.position = {x, y, -0.25F},
                                     .normal = {0.0F, 0.0F, -1.0F},
                                     .uv = {static_cast<float>(column) / (kColumns - 1),
                                            static_cast<float>(row) / (kRows - 1)}});
        }
    }
    for (int row = 0; row + 1 < kRows; ++row) {
        for (int column = 0; column + 1 < kColumns; ++column) {
            const auto at = [](int r, int c) {
                return static_cast<std::uint32_t>((r * kColumns) + c);
            };
            cape.indices.insert(cape.indices.end(),
                                {at(row, column), at(row + 1, column), at(row + 1, column + 1),
                                 at(row, column), at(row + 1, column + 1), at(row, column + 1)});
        }
    }
    weighted(cape);
    scene3d::ModelMesh mesh;
    mesh.name = "Figure";
    mesh.surfaces = {body, cape};
    model.meshes.push_back(std::move(mesh));
    scene3d::ModelPart part;
    part.node = "Figure";
    part.node_index = 2;
    part.skin = 0;
    model.parts.push_back(part);
    return model;
}

struct Project {
    std::string root;
    world::ComponentRegistry components;
    command::Registry commands;
    std::unique_ptr<command::Session> session;

    explicit Project(std::string name, std::string_view cloth) : root(std::move(name)) {
        std::filesystem::remove_all(root);
        REQUIRE(slash::platform::create_directories(root).ok());
        const scene3d::ModelReader none = [](std::string_view) -> slash::Result<std::string> {
            return slash::make_error(slash::ErrorCode::NotFound, "no files");
        };
        REQUIRE(
            slash::platform::write_file(
                root + "/hero.glb", scene3d::write_glb(caped_figure(), "hero.glb", none).value())
                .ok());
        REQUIRE(slash::platform::write_file(root + "/main.scene.json",
                                            R"({"format": 1, "entities": {
              "hero": {"components": {"Transform3D": {},
                                      "MeshInstance3D": {"mesh": "hero.glb"},
                                      "Cloth3D": )" +
                                                std::string(cloth) + R"(}}}})")
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

    void steps(int ticks) {
        for (int i = 0; i < ticks; ++i) {
            session->world().step(1.0 / 60.0);
        }
    }

    // The cape's vertices where physics put them, in the model's space.
    [[nodiscard]] std::vector<Vec3> cape() const {
        const world::World& w = session->world();
        const auto pose = scene3d::soft_body_pose(w, w.find("hero"));
        REQUIRE(pose != nullptr);
        REQUIRE(pose->surfaces.size() == 1);
        CHECK(pose->surfaces[0].part == 0);
        CHECK(pose->surfaces[0].surface == 1);
        return pose->surfaces[0].positions;
    }
};

float distance(Vec3 a, Vec3 b) {
    const Vec3 d = a - b;
    return static_cast<float>(std::sqrt((d.x * d.x) + (d.y * d.y) + (d.z * d.z)));
}

Vec3 rest(int row, int column) {
    return {-0.3F + (0.6F * static_cast<float>(column) / (kColumns - 1)),
            1.5F - (1.0F * static_cast<float>(row) / (kRows - 1)), -0.25F};
}

} // namespace

TEST_CASE("cloth: a cape hangs from its skin and swings within its max distance") {
    Project project("cloth_cape_project",
                    R"({"section": "cape", "max_distance": 0.4, "gradient": [0, -1, 0]})");
    project.steps(30);
    std::vector<Vec3> cape = project.cape();
    REQUIRE(cape.size() == static_cast<std::size_t>(kColumns * kRows));
    // The top row has no share of the max distance, so the skin holds it exactly.
    for (int column = 0; column < kColumns; ++column) {
        const Vec3 at = cape[static_cast<std::size_t>(column)];
        CHECK(distance(at, rest(0, column)) < 0.001F);
    }
    // The rest hangs where it was made: gravity can't stretch it past its edges.
    const Vec3 hem = cape[static_cast<std::size_t>(((kRows - 1) * kColumns) + 2)];
    CHECK(distance(hem, rest(kRows - 1, 2)) < 0.05F);

    // The spine steps half a meter forward: the top follows at once, the hem trails behind and
    // never strays past its max distance from where the skin puts it.
    project.ok("component.set", R"({"id": "hero", "components": {
        "Skeleton3D": {"bone_positions": {"Spine": [0, 1.5, 0.5]}}}})");
    project.steps(1);
    cape = project.cape();
    for (int column = 0; column < kColumns; ++column) {
        CHECK(cape[static_cast<std::size_t>(column)].z == doctest::Approx(0.25).epsilon(0.001));
    }
    const Vec3 moved_hem = cape[static_cast<std::size_t>(((kRows - 1) * kColumns) + 2)];
    const Vec3 skinned_hem = rest(kRows - 1, 2) + Vec3{0.0F, 0.0F, 0.5F};
    CHECK(moved_hem.z < skinned_hem.z - 0.05F);
    CHECK(distance(moved_hem, skinned_hem) <= 0.4F + 0.01F);
    for (int row = 1; row < kRows; ++row) {
        const float share = static_cast<float>(row) / (kRows - 1);
        for (int column = 0; column < kColumns; ++column) {
            const Vec3 at = cape[static_cast<std::size_t>((row * kColumns) + column)];
            CHECK(distance(at, rest(row, column) + Vec3{0.0F, 0.0F, 0.5F}) <=
                  (0.4F * share) + 0.01F);
        }
    }
    // It swings to and fro about where the skin puts it, past it and back, and with damping
    // comes to rest there.
    bool past = false;
    for (int i = 0; i < 60; ++i) {
        project.steps(1);
        past = past || project.cape()[static_cast<std::size_t>(((kRows - 1) * kColumns) + 2)].z >
                           skinned_hem.z + 0.05F;
    }
    CHECK(past);
    project.ok("component.set", R"({"id": "hero", "components": {
        "Cloth3D": {"section": "cape", "max_distance": 0.4, "damping_coefficient": 2}}})");
    project.steps(240);
    cape = project.cape();
    const Vec3 settled = cape[static_cast<std::size_t>(((kRows - 1) * kColumns) + 2)];
    CHECK(distance(settled, skinned_hem) < 0.1F);
}

TEST_CASE("cloth: a uniform map lets every point swing, and the backstop keeps it behind") {
    // With every point free and none skinned exactly, only max distance holds the cape up.
    Project project("cloth_uniform_project",
                    R"({"section": "cape", "max_distance": 0.2, "max_distance_map": "uniform",
                        "long_range_attachment": false})");
    project.steps(60);
    const std::vector<Vec3> cape = project.cape();
    for (int row = 0; row < kRows; ++row) {
        for (int column = 0; column < kColumns; ++column) {
            const Vec3 at = cape[static_cast<std::size_t>((row * kColumns) + column)];
            CHECK(distance(at, rest(row, column)) <= 0.2F + 0.01F);
            // Gravity pulls each point to the bottom of its reach.
            CHECK(at.y < rest(row, column).y - 0.1F);
        }
    }
}

TEST_CASE("cloth: scene validate names a section the model lacks") {
    Project project("cloth_section_project", R"({"section": "cpae"})");
    const auto problems = scene3d::check_cloths(project.session->world());
    REQUIRE(problems.size() == 1);
    CHECK(problems[0].path == "/components/Cloth3D/section");
    CHECK(problems[0].message == R"(the model has no material "cpae"; did you mean "cape"?)");
    // And no body is made of it.
    project.steps(2);
    const world::World& w = project.session->world();
    CHECK(scene3d::soft_body_pose(w, w.find("hero")) == nullptr);
}

namespace {

// A 2D scene of a cut-out hero at (100, 40) wearing a Cloth2D cape 4 points across and 8 down,
// 40 by 70 pixels, and whatever else entities holds.
struct Project2D {
    std::string root;
    world::ComponentRegistry components;
    command::Registry commands;
    std::unique_ptr<command::Session> session;

    Project2D(std::string name, std::string_view cloth, std::string_view entities = "")
        : root(std::move(name)) {
        std::filesystem::remove_all(root);
        REQUIRE(slash::platform::create_directories(root).ok());
        REQUIRE(slash::platform::write_file(root + "/main.scene.json",
                                            R"({"format": 1, "entities": {
              "hero": {"components": {"Transform2D": {"position": [100, 40]},
                                      "Cloth2D": )" +
                                                std::string(cloth) + "}}" + std::string(entities) +
                                                "}}")
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

    void steps(int ticks) {
        for (int i = 0; i < ticks; ++i) {
            session->world().step(1.0 / 60.0);
        }
    }

    [[nodiscard]] std::vector<Vec2> points() const {
        const world::World& w = session->world();
        const world::ClothPoints2D* points = world::cloth_points_2d(w, w.find("hero"));
        REQUIRE(points != nullptr);
        return points->current;
    }
};

constexpr int kSheetColumns = 4;
constexpr int kSheetRows = 8;
constexpr std::string_view kCape = R"({"columns": 4, "rows": 8, "size": [40, 70],
    "max_distance": 30, "color": [1, 0, 0, 1], "damping_coefficient": 2)";

// Where the hero's transform puts a point, its top edge's middle at the hero.
Vec2 rest_2d(Vec2 hero, int row, int column) {
    return {hero.x - 20.0F + (40.0F * static_cast<float>(column) / (kSheetColumns - 1)),
            hero.y + (70.0F * static_cast<float>(row) / (kSheetRows - 1))};
}

float distance_2d(Vec2 a, Vec2 b) {
    const Vec2 d = a - b;
    return static_cast<float>(std::sqrt((d.x * d.x) + (d.y * d.y)));
}

std::size_t at_2d(int row, int column) {
    return static_cast<std::size_t>((row * kSheetColumns) + column);
}

} // namespace

TEST_CASE("cloth 2D: a cut-out's cape follows its transform and trails within its max distance") {
    Project2D project("cloth2d_cape_project", std::string(kCape) + "}");
    project.steps(30);
    std::vector<Vec2> points = project.points();
    REQUIRE(points.size() == static_cast<std::size_t>(kSheetColumns * kSheetRows));
    const Vec2 hero{100.0F, 40.0F};
    for (int column = 0; column < kSheetColumns; ++column) {
        CHECK(distance_2d(points[at_2d(0, column)], rest_2d(hero, 0, column)) < 0.001F);
    }
    // Hanging still, edges keep it from stretching under gravity.
    CHECK(distance_2d(points[at_2d(kSheetRows - 1, 1)], rest_2d(hero, kSheetRows - 1, 1)) < 3.0F);

    // The hero runs 40 pixels right in a tick: the top row goes with it, the hem trails.
    project.ok("component.set",
               R"({"id": "hero", "components": {"Transform2D": {"position": [140, 40]}}})");
    project.steps(1);
    points = project.points();
    const Vec2 moved{140.0F, 40.0F};
    for (int column = 0; column < kSheetColumns; ++column) {
        CHECK(points[at_2d(0, column)].x == doctest::Approx(rest_2d(moved, 0, column).x));
    }
    const Vec2 hem = points[at_2d(kSheetRows - 1, 1)];
    CHECK(hem.x < rest_2d(moved, kSheetRows - 1, 1).x - 5.0F);
    for (int row = 0; row < kSheetRows; ++row) {
        const float share = static_cast<float>(row) / (kSheetRows - 1);
        for (int column = 0; column < kSheetColumns; ++column) {
            CHECK(distance_2d(points[at_2d(row, column)], rest_2d(moved, row, column)) <=
                  (30.0F * share) + 0.01F);
        }
    }
    // With damping it settles under the hero again.
    project.steps(240);
    CHECK(distance_2d(project.points()[at_2d(kSheetRows - 1, 1)],
                      rest_2d(moved, kSheetRows - 1, 1)) < 3.0F);
}

TEST_CASE("cloth 2D: the wind lifts a cape and the ground stops it") {
    // A floor just under the hem, and a cape free all over that gravity drops onto it.
    Project2D floored("cloth2d_floor_project",
                      R"({"columns": 4, "rows": 8, "size": [40, 70], "max_distance": 60,
                          "max_distance_map": "uniform", "long_range_attachment": false,
                          "damping_coefficient": 2})",
                      R"(, "floor": {"components": {"Transform2D": {"position": [100, 130]},
                           "StaticBody2D": {},
                           "CollisionShape2D": {"shape": "rectangle", "size": [400, 20]}}})");
    floored.steps(120);
    for (const Vec2 point : floored.points()) {
        // The floor's top is at y 120; each point keeps its radius from it.
        CHECK(point.y <= 118.0F + 0.01F);
    }
    // The bottom row lies on the floor rather than hanging at its max distance.
    CHECK(floored.points()[at_2d(kSheetRows - 1, 1)].y > 115.0F);

    // A steady wind to the right carries the hem along.
    Project2D windy("cloth2d_wind_project", std::string(kCape) + R"(, "drag_coefficient": 1})",
                    R"(, "breeze": {"components": {"Transform2D": {},
                           "WindSource2D": {"strength": 400, "min_gust_amount": 0,
                                            "max_gust_amount": 0, "turbulence": 0}}})");
    windy.steps(120);
    const Vec2 hem = windy.points()[at_2d(kSheetRows - 1, 1)];
    CHECK(hem.x > rest_2d({100.0F, 40.0F}, kSheetRows - 1, 1).x + 10.0F);
    CHECK(windy.points()[at_2d(0, 1)].x == doctest::Approx(rest_2d({100.0F, 40.0F}, 0, 1).x));
}

TEST_CASE("cloth 2D: the canvas draws the sheet where physics put it") {
    Project2D project("cloth2d_draw_project", std::string(kCape) + R"(, "drag_coefficient": 1})",
                      R"(, "breeze": {"components": {"Transform2D": {},
                           "WindSource2D": {"strength": 400, "min_gust_amount": 0,
                                            "max_gust_amount": 0, "turbulence": 0}}})");
    const auto red_at = [&](Vec2 point) {
        auto captured = slash::canvas::capture(project.session->world(), project.session->assets());
        REQUIRE(captured.ok());
        const slash::render::Image& image = captured.value().image;
        const auto x = static_cast<std::size_t>(point.x);
        const auto y = static_cast<std::size_t>(point.y);
        REQUIRE(x < image.width);
        REQUIRE(y < image.height);
        const std::size_t at = ((y * image.width) + x) * 4;
        return image.pixels[at] > 200 && image.pixels[at + 1] < 50;
    };
    // At rest it hangs under the hero.
    CHECK(red_at({100.0F, 100.0F}));
    CHECK_FALSE(red_at({140.0F, 100.0F}));
    // Blown right, its lower half is drawn where the wind carried it.
    project.steps(120);
    const Vec2 hem = project.points()[at_2d(kSheetRows - 2, 1)];
    CHECK(hem.x > 105.0F);
    CHECK(red_at({hem.x + 5.0F, hem.y}));
}
