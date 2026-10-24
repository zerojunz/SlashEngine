#include "slash/asset/assets.hpp"
#include "slash/canvas/canvas.hpp"
#include "slash/core/json.hpp"
#include "slash/core/math.hpp"
#include "slash/scene3d/csg.hpp"
#include "slash/scene3d/scene3d.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/path.hpp"
#include "slash/world/tree2d.hpp"
#include "slash/world/tree3d.hpp"
#include "slash/world/world.hpp"

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace asset = slash::asset;
namespace canvas = slash::canvas;
namespace json = slash::json;
namespace scene3d = slash::scene3d;
namespace world = slash::world;
using slash::Vec2;
using slash::Vec3;

namespace {

constexpr float kPi = 3.14159265F;

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE(parsed.ok());
    return std::move(parsed).value();
}

// Godot's tests compare with a tolerance of 0.001.
bool near(float a, float b, float tolerance = 0.001F) {
    return std::abs(a - b) < tolerance;
}

bool near(Vec2 a, Vec2 b, float tolerance = 0.001F) {
    return near(a.x, b.x, tolerance) && near(a.y, b.y, tolerance);
}

bool near(Vec3 a, Vec3 b, float tolerance = 0.001F) {
    return near(a.x, b.x, tolerance) && near(a.y, b.y, tolerance) && near(a.z, b.z, tolerance);
}

world::CurvePoint2D point2(Vec2 position, Vec2 in = {}, Vec2 out = {}) {
    return {.position = position, .in = in, .out = out};
}

world::CurvePoint3D point(Vec3 position, Vec3 in = {}, Vec3 out = {}, float tilt = 0.0F) {
    return {.position = position, .in = in, .out = out, .tilt = tilt};
}

// A world with the built-in components, installed as sessions and games install it.
struct Scene {
    asset::Assets assets{asset::FileSource{
        .read = [](std::string_view) -> slash::Result<std::string> {
            return slash::make_error(slash::ErrorCode::NotFound, "no such file");
        },
        .stamp = [](std::string_view) -> std::optional<asset::FileStamp> { return std::nullopt; },
    }};
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

    std::shared_ptr<const scene3d::CsgMesh> mesh(entt::entity root) {
        const world::Tree3D tree(world);
        return scene3d::csg_mesh(world, tree, root);
    }
};

std::vector<std::string> messages(const std::vector<world::PathProblem>& problems,
                                  const world::World& world) {
    std::vector<std::string> out;
    out.reserve(problems.size());
    for (const world::PathProblem& problem : problems) {
        out.push_back(std::string(world.id_of(problem.entity)) + problem.path + ": " +
                      problem.message);
    }
    return out;
}

// The volume a CSG mesh's collision triangles enclose; positive when they face out.
double volume(const scene3d::CsgMesh& mesh) {
    double sum = 0.0;
    const std::vector<Vec3>& faces = *mesh.collision;
    for (std::size_t i = 0; i + 2 < faces.size(); i += 3) {
        const Vec3 a = faces[i];
        const Vec3 b = faces[i + 1];
        const Vec3 c = faces[i + 2];
        sum -= (static_cast<double>(a.x) * ((b.y * c.z) - (b.z * c.y))) +
               (static_cast<double>(a.y) * ((b.z * c.x) - (b.x * c.z))) +
               (static_cast<double>(a.z) * ((b.x * c.y) - (b.y * c.x)));
    }
    return sum / 6.0;
}

} // namespace

TEST_CASE("Curve2D bakes, samples and finds the nearest point as Godot's") {
    // The cases of Godot's tests/scene/test_curve_2d.h.
    const world::Curve2D single({point2({})}, 5.0F);
    CHECK(single.length() == 0.0F);
    CHECK(single.points().size() == 1);

    const world::Curve2D line({point2({0, 0}), point2({0, 50})}, 5.0F);
    CHECK(line.length() == doctest::Approx(50.0F));
    CHECK(line.points().size() == 15);
    for (const float y : {0.0F, 25.0F, 50.0F}) {
        CHECK(near(line.sample(line.closest_offset({0, y})), {0, y}));
        CHECK(near(line.sample(line.closest_offset({0, y}), true), {0, y}));
    }
    CHECK(near(line.closest_point({50, 25}), {0, 25}));
    CHECK(near(line.closest_point({50, 50}), {0, 50}));
    CHECK(near(line.closest_point({0, 100}), {0, 50}));

    // The way it goes is the x axis.
    const std::array<std::pair<Vec2, float>, 4> ways{
        {{{50, 0}, 0.0F}, {{0, 50}, kPi / 2.0F}, {{-50, 0}, kPi}, {{0, -50}, -kPi / 2.0F}}};
    for (const auto& [end, angle] : ways) {
        const world::Curve2D curve({point2({}), point2(end)}, 5.0F);
        for (const bool cubic : {false, true}) {
            const world::Affine2D t = curve.sample_with_rotation(25.0F, cubic);
            CHECK(near(t.origin, end * 0.5F));
            CHECK(near(t.x, slash::cos_sin(angle)));
            CHECK(near(t.y, {-t.x.y, t.x.x}));
        }
    }

    // Handles that point at each other's ends make a line (Godot's issue 88923).
    const world::Curve2D crossed({point2({}, {-2, 0}, {1, 0}), point2({1, 0}, {-1, 0}, {2, 0})},
                                 0.5F);
    CHECK(crossed.points().size() >= 3);
    const world::Affine2D middle = crossed.sample_with_rotation(crossed.closest_offset({0.5F, 0}));
    CHECK(near(middle.x, {1, 0}));
    CHECK(near(middle.origin, {0.5F, 0}));

    // A Bézier curve is longer than the line between its ends; baked coarser, it is shorter.
    const std::vector<world::CurvePoint2D> bent{point2({0, 0}, {}, {50, 0}),
                                                point2({0, 50}, {-50, 0})};
    const world::Curve2D fine(bent, 5.0F);
    const world::Curve2D coarse(bent, 10.0F);
    CHECK(fine.length() > 50.0F);
    CHECK(coarse.length() < fine.length());
    CHECK(coarse.points().size() < fine.points().size());
}

TEST_CASE("Curve3D bakes up vectors and tilts along it as Godot's") {
    // The cases of Godot's tests/scene/test_curve_3d.h.
    const world::Curve3D single({point({})}, false, 0.2F, true);
    CHECK(single.length() == 0.0F);
    CHECK(single.points().size() == 1);

    const world::Curve3D line({point({}), point({0, 50, 0})}, false, 0.2F, true);
    CHECK(line.length() == doctest::Approx(50.0F));
    // Halved by Bézier time, whose pieces are longest in the middle of a straight segment.
    CHECK(line.points().size() == 369);
    for (const float y : {0.0F, 25.0F, 50.0F}) {
        const float offset = line.closest_offset({0, y, 0});
        CHECK(near(line.sample(offset), {0, y, 0}));
        CHECK(near(line.sample(offset, true), {0, y, 0}));
        for (const bool cubic : {false, true}) {
            for (const bool tilt : {false, true}) {
                const world::Affine3D t = line.sample_with_rotation(offset, cubic, tilt);
                CHECK(near(t.x, {0, 0, -1}));
                CHECK(near(t.y, {1, 0, 0}));
                CHECK(near(t.z, {0, -1, 0}));
                CHECK(near(t.origin, {0, y, 0}));
            }
        }
        CHECK(line.sample_tilt(offset) == 0.0F);
        // Straight up, the first frame looks along it with x as up.
        CHECK(near(line.sample_up_vector(offset), {1, 0, 0}));
        CHECK(near(line.sample_up_vector(offset, true), {1, 0, 0}));
    }
    CHECK(near(line.closest_point({50, 25, 0}), {0, 25, 0}));
    CHECK(near(line.closest_point({0, 100, 0}), {0, 50, 0}));
    const world::Curve3D flat({point({}), point({0, 50, 0})}, false, 0.2F, false);
    CHECK(near(flat.sample_up_vector(25.0F), {0, 1, 0}));

    // Godot's issue 81879: up vectors on a curve a little off an axis.
    const world::Curve3D off_axis({point({}), point({0, 0.1F, 1})}, false, 0.2F, true);
    CHECK(near(off_axis.sample_up_vector(off_axis.closest_offset({0, 0, 0.9F})),
               {0, 0.995037F, -0.099504F}, 0.01F));

    // Godot's issue 88923, in 3D.
    const world::Curve3D crossed(
        {point({}, {-1, 0, 2}, {1, 0, 0}), point({1, 0, 0}, {-1, 0, 0}, {1, 0, 2})}, false, 0.2F,
        true);
    CHECK(crossed.points().size() >= 3);
    const world::Affine3D middle =
        crossed.sample_with_rotation(crossed.closest_offset({0.5F, 0, 0}));
    CHECK(near(middle.x, {0, 0, 1}));
    CHECK(near(middle.y, {0, 1, 0}));
    CHECK(near(middle.z, {-1, 0, 0}));
    CHECK(near(middle.origin, {0.5F, 0, 0}));

    // Round a corner on the ground, the up vector stays up.
    const world::Curve3D corner({point({}), point({10, 0, 0}), point({10, 0, -10})}, false, 0.2F,
                                true);
    for (const float offset : {0.0F, 5.0F, 10.0F, 15.0F, 20.0F}) {
        CHECK(near(corner.sample_up_vector(offset), {0, 1, 0}));
    }

    // Tilt turns the up vector about the way the curve goes.
    const world::Curve3D tilted({point({}, {}, {}, 0.0F), point({0, 0, -10}, {}, {}, kPi / 2.0F)},
                                false, 0.2F, true);
    CHECK(tilted.sample_tilt(5.0F) == doctest::Approx(kPi / 4.0F));
    CHECK(near(tilted.sample_up_vector(10.0F), {0, 1, 0}));
    CHECK(near(tilted.sample_up_vector(10.0F, true), {1, 0, 0}));
    CHECK(near(tilted.sample_with_rotation(10.0F, false, true).x, {0, -1, 0}));

    // A closed curve has a segment back to the start.
    const world::Curve3D open({point({}), point({10, 0, 0}), point({10, 0, -10})}, false, 1.0F,
                              true);
    const world::Curve3D closed({point({}), point({10, 0, 0}), point({10, 0, -10})}, true, 1.0F,
                                true);
    CHECK(open.length() == doctest::Approx(20.0F));
    CHECK(closed.length() == doctest::Approx(20.0F + (10.0F * std::sqrt(2.0F))));
    CHECK(near(closed.sample(closed.length()), {}));
}

TEST_CASE("progress wraps round a loop and stops at the ends without one, as Godot's") {
    // Godot's "Progress out of range" and "Progress ratio out of range" tests.
    CHECK(world::wrap_progress(-50.0F, 100.0F, true) == doctest::Approx(50.0F));
    CHECK(world::wrap_progress(150.0F, 100.0F, true) == doctest::Approx(50.0F));
    CHECK(world::wrap_progress(-30.0F, 100.0F, true) == doctest::Approx(70.0F));
    CHECK(world::wrap_progress(130.0F, 100.0F, true) == doctest::Approx(30.0F));
    CHECK(world::wrap_progress(-50.0F, 100.0F, false) == 0.0F);
    CHECK(world::wrap_progress(150.0F, 100.0F, false) == 100.0F);
    // A whole number of laps ends at the end, not back at the start.
    CHECK(world::wrap_progress(200.0F, 100.0F, true) == 100.0F);
    CHECK(world::wrap_progress(0.0F, 100.0F, true) == 0.0F);
    CHECK(world::wrap_progress(5.0F, 0.0F, true) == 0.0F);
}

TEST_CASE("PathFollow2D goes along its curve, turned with it, offsets along and across it") {
    // Godot's tests/scene/test_path_follow_2d.h.
    const world::Curve2D square(
        {point2({0, 0}), point2({100, 0}), point2({100, 100}), point2({0, 100}), point2({0, 0})},
        1.0F);
    REQUIRE(square.length() == doctest::Approx(400.0F));
    const std::array<Vec2, 9> places{
        {{0, 0}, {50, 0}, {100, 0}, {100, 50}, {100, 100}, {50, 100}, {0, 100}, {0, 50}, {0, 0}}};
    world::PathFollow2D follower;
    follower.loop = false;
    for (std::size_t i = 0; i < places.size(); ++i) {
        follower.progress = 50.0F * static_cast<float>(i);
        CHECK(near(world::follow(follower, square, {}).position, places[i]));
    }

    const world::Curve2D line({point2({0, 0}), point2({100, 0})}, 5.0F);
    follower.progress = 50.0F;
    CHECK(near(world::follow(follower, line, {}).position, {50, 0}));
    follower.h_offset = 25.0F;
    CHECK(near(world::follow(follower, line, {}).position, {75, 0}));
    follower.v_offset = 25.0F;
    CHECK(near(world::follow(follower, line, {}).position, {75, 25}));

    // Turned with the curve, or not: then the offsets are the world's axes and the rotation is
    // left as it was.
    const world::Curve2D down({point2({0, 0}), point2({0, 100})}, 5.0F);
    world::Transform2D turned = world::follow(follower, down, {});
    CHECK(near(turned.position, {-25, 75}));
    CHECK(turned.rotation == doctest::Approx(kPi / 2.0F));
    follower.rotates = false;
    world::Transform2D before;
    before.rotation = 1.0F;
    before.scale = {2, 2};
    turned = world::follow(follower, down, before);
    CHECK(near(turned.position, {25, 75}));
    CHECK(turned.rotation == 1.0F);
    CHECK(turned.scale == Vec2{2, 2});
}

TEST_CASE("PathFollow3D goes along its curve, its rotation as its rotation_mode says") {
    // Godot's tests/scene/test_path_follow_3d.h.
    const world::Curve3D zigzag({point({0, 0, 0}), point({100, 0, 0}), point({100, 100, 0}),
                                 point({100, 100, 100}), point({100, 0, 100})},
                                false, 0.2F, true);
    REQUIRE(zigzag.length() == doctest::Approx(400.0F));
    const std::array<Vec3, 9> places{{{0, 0, 0},
                                      {50, 0, 0},
                                      {100, 0, 0},
                                      {100, 50, 0},
                                      {100, 100, 0},
                                      {100, 100, 50},
                                      {100, 100, 100},
                                      {100, 50, 100},
                                      {100, 0, 100}}};
    world::PathFollow3D follower;
    follower.loop = false;
    for (std::size_t i = 0; i < places.size(); ++i) {
        follower.progress = 50.0F * static_cast<float>(i);
        CHECK(near(world::follow(follower, zigzag, {}).position, places[i]));
    }

    // "Calculate forward vector": oriented followers look along the curve, y up.
    const float cube = 100.0F * std::sqrt(3.0F);
    const world::Curve3D bends({point({0, 0, 0}), point({100, 0, 0}), point({200, 100, -100}),
                                point({200, 100, 200}), point({100, 0, 100}), point({0, 0, 100})},
                               false, 0.2F, true);
    follower.rotation_mode = "oriented";
    const auto z_axis = [&](const world::Curve3D& curve, float progress) {
        follower.progress = world::wrap_progress(progress, curve.length(), false);
        return world::affine(world::follow(follower, curve, {})).z;
    };
    const float diagonal = 0.577348F;
    CHECK(near(z_axis(bends, -50.0F), {-1, 0, 0}));
    CHECK(near(z_axis(bends, 0.0F), {-1, 0, 0}));
    CHECK(near(z_axis(bends, 50.0F), {-1, 0, 0}));
    CHECK(near(z_axis(bends, 100.0F), {-1, 0, 0}));
    CHECK(near(z_axis(bends, 100.0F + (cube / 2.0F)), {-diagonal, -diagonal, diagonal}));
    CHECK(near(z_axis(bends, 100.0F + cube - 0.01F), {-diagonal, -diagonal, diagonal}));
    CHECK(near(z_axis(bends, 250.0F + cube), {0, 0, -1}));
    CHECK(near(z_axis(bends, 400.0F + cube - 0.01F), {0, 0, -1}));
    CHECK(near(z_axis(bends, 400.0F + (1.5F * cube)), {diagonal, diagonal, diagonal}));
    CHECK(near(z_axis(bends, 400.0F + (2.0F * cube) - 0.01F), {diagonal, diagonal, diagonal}));
    CHECK(near(z_axis(bends, 500.0F + (2.0F * cube)), {1, 0, 0}));

    // "Calculate forward vector with degenerate curves": handles of zero length.
    const world::Curve3D circle({point({0, 0, 1}, {}, {1, 0, 0}), point({1, 0, 0}),
                                 point({0, 0, -1}, {1, 0, 0}, {-1, 0, 0}), point({-1, 0, 0}),
                                 point({0, 0, 1}, {-1, 0, 0}, {})},
                                false, 0.2F, true);
    const auto at_ratio = [&](float ratio) { return z_axis(circle, ratio * circle.length()); };
    CHECK(near(at_ratio(0.0F), {-1, 0, 0}));
    CHECK(near(at_ratio(0.25F), {0, 0, 1}));
    CHECK(near(at_ratio(0.5F), {1, 0, 0}));
    CHECK(near(at_ratio(0.75F), {0, 0, -1}));
    CHECK(near(at_ratio(1.0F), {-1, 0, 0}));
    CHECK(near(at_ratio(0.125F), {-0.688375F, 0, 0.725355F}));
    CHECK(near(at_ratio(0.375F), {0.688375F, 0, 0.725355F}));
    CHECK(near(at_ratio(0.625F), {0.688375F, 0, -0.725355F}));
    CHECK(near(at_ratio(0.875F), {-0.688375F, 0, -0.725355F}));

    // Up a slope facing +x: "y" keeps only the turn about y, "xy" the pitch too.
    const world::Curve3D slope({point({}), point({10, 10, 0})}, false, 0.2F, true);
    follower = {};
    follower.progress = 5.0F;
    CHECK(near(world::follow(follower, slope, {}).rotation, {kPi / 4.0F, -kPi / 2.0F, 0}));
    follower.rotation_mode = "xy";
    CHECK(near(world::follow(follower, slope, {}).rotation, {kPi / 4.0F, -kPi / 2.0F, 0}));
    follower.rotation_mode = "y";
    CHECK(near(world::follow(follower, slope, {}).rotation, {0, -kPi / 2.0F, 0}));

    // Along -z: offsets along the follower's x and y, the model's front turned to face along
    // it, the tilt rolled in, and "none" leaving it unturned. Its scale is its own.
    const world::Curve3D ahead(
        {point({}, {}, {}, kPi / 2.0F), point({0, 0, -10}, {}, {}, kPi / 2.0F)}, false, 0.2F, true);
    follower = {};
    follower.progress = 5.0F;
    follower.h_offset = 1.0F;
    follower.v_offset = 2.0F;
    follower.tilt_enabled = false;
    world::Transform3D placed = world::follow(follower, ahead, {});
    CHECK(near(placed.position, {1, 2, -5}));
    CHECK(near(placed.rotation, {}));
    follower.use_model_front = true;
    placed = world::follow(follower, ahead, {});
    CHECK(near(placed.position, {-1, 2, -5}));
    CHECK(near(std::abs(placed.rotation.y), kPi));
    follower.use_model_front = false;
    follower.tilt_enabled = true;
    placed = world::follow(follower, ahead, {});
    CHECK(near(placed.rotation, {0, 0, -kPi / 2.0F}));
    CHECK(near(placed.position, {2, -1, -5}));
    follower.rotation_mode = "none";
    world::Transform3D before;
    before.rotation = {1, 1, 1};
    before.scale = {3, 3, 3};
    placed = world::follow(follower, ahead, before);
    CHECK(near(placed.position, {1, 2, -5}));
    CHECK(placed.rotation == Vec3{});
    CHECK(placed.scale == Vec3{3, 3, 3});
}

TEST_CASE("the path_follow system wraps followers' progress and places them on their path") {
    Scene scene;
    const entt::entity rail = scene.create("rail", R"({
        "Transform2D": {"position": [10, 0]},
        "Path2D": {"points": [{"position": [0, 0]}, {"position": [100, 0]}]}
    })");
    const entt::entity cart =
        scene.create("cart", R"({"Transform2D": {}, "PathFollow2D": {"progress": 150}})");
    scene.world.ecs().get<world::Transform2D>(rail).children.push_back(cart);
    const entt::entity lost =
        scene.create("lost", R"({"Transform2D": {"position": [7, 7]}, "PathFollow2D": {}})");
    scene.world.step(1.0 / 60.0);
    const auto& follower = scene.world.ecs().get<world::PathFollow2D>(cart);
    CHECK(follower.progress == doctest::Approx(50.0F));
    CHECK(near(scene.world.ecs().get<world::Transform2D>(cart).position, {50, 0}));
    CHECK(near(world::Tree2D(scene.world).global(cart).position, {60, 0}));
    CHECK(scene.world.ecs().get<world::Transform2D>(lost).position == Vec2{7, 7});

    // The curve is kept until the path changes.
    const auto first = world::curve_2d(scene.world, rail);
    CHECK(world::curve_2d(scene.world, rail) == first);
    scene.world.ecs().get<world::Path2D>(rail).points[1].position = {0, 100};
    scene.world.step(1.0 / 60.0);
    CHECK(world::curve_2d(scene.world, rail) != first);
    const auto& moved = scene.world.ecs().get<world::Transform2D>(cart);
    CHECK(near(moved.position, {0, 50}));
    CHECK(moved.rotation == doctest::Approx(kPi / 2.0F));

    const entt::entity track = scene.create("track", R"({
        "Transform3D": {},
        "Path3D": {"points": [{"position": [0, 0, 0]}, {"position": [0, 0, -10]}]}
    })");
    const entt::entity car = scene.create(
        "car", R"({"Transform3D": {}, "PathFollow3D": {"progress": -4, "loop": false}})");
    scene.world.ecs().get<world::Transform3D>(track).children.push_back(car);
    scene.world.ecs().get<world::PathFollow3D>(car).progress = 4.0F;
    scene.world.step(1.0 / 60.0);
    CHECK(near(scene.world.ecs().get<world::Transform3D>(car).position, {0, 0, -4}));
    scene.world.ecs().get<world::PathFollow3D>(car).progress = 40.0F;
    scene.world.step(1.0 / 60.0);
    CHECK(scene.world.ecs().get<world::PathFollow3D>(car).progress == 10.0F);
    CHECK(near(scene.world.ecs().get<world::Transform3D>(car).position, {0, 0, -10}));

    // Without add_path_systems, curves are baked on each call.
    Scene bare(false);
    const entt::entity path = bare.create("path", R"({"Path3D": {}})");
    CHECK(world::curve_3d(bare.world, path) != world::curve_3d(bare.world, path));
    CHECK(world::curve_3d(bare.world, bare.create("none", R"({"Transform3D": {}})")) == nullptr);
}

TEST_CASE("check_paths reports followers that can't follow, as Godot's warnings") {
    Scene scene;
    scene.create("drifter", R"({"Transform2D": {}, "PathFollow2D": {}})");
    const entt::entity node = scene.create("node", R"({"Transform3D": {}})");
    const entt::entity rider = scene.create("rider", R"({"Transform3D": {}, "PathFollow3D": {}})");
    scene.world.ecs().get<world::Transform3D>(node).children.push_back(rider);
    const entt::entity rail =
        scene.create("rail", R"({"Transform3D": {}, "Path3D": {"points": [{"position": [0, 0, 0]},
                    {"position": [1, 0, 0]}], "up_vector_enabled": false}})");
    const entt::entity looker = scene.create(
        "looker", R"({"Transform3D": {}, "PathFollow3D": {"rotation_mode": "oriented"}})");
    scene.world.ecs().get<world::Transform3D>(rail).children.push_back(looker);
    const std::vector<std::string> expected{
        "drifter/components/PathFollow2D: only follows a path as a Transform2D child of an "
        "entity with a Path2D",
        "rider/components/PathFollow3D: only follows a path as a Transform3D child of an entity "
        "with a Path3D",
        "looker/components/PathFollow3D/rotation_mode: \"oriented\" needs up vectors: set "
        "up_vector_enabled on the parent's Path3D"};
    CHECK(messages(world::check_paths(scene.world), scene.world) == expected);
    // scene validate and exports report them through check_meshes.
    CHECK(scene3d::check_meshes(scene.world).size() == 3);
}

TEST_CASE("CSGPolygon3D's path mode pushes its polygon along a Path3D, as Godot's") {
    Scene scene;
    const entt::entity rail = scene.create("rail", R"({
        "Transform3D": {},
        "Path3D": {"points": [{"position": [0, 0, 0]}, {"position": [0, 0, -10]}]}
    })");
    const entt::entity beam = scene.create("beam", R"({
        "Transform3D": {},
        "CSGPolygon3D": {"polygon": [[0, 0], [1, 0], [1, 1], [0, 1]], "mode": "path",
                         "path_node": "rail"}
    })");
    // A ring each unit along it, the rings facing the way it goes, capped at both ends.
    auto mesh = scene.mesh(beam);
    CHECK(mesh->problems.empty());
    CHECK(mesh->triangles == (10 * 4 * 2) + 4);
    CHECK(volume(*mesh) == doctest::Approx(10.0));
    CHECK(near(mesh->min, {0, 0, -10}));
    CHECK(near(mesh->max, {1, 1, 0}));

    auto& polygon = scene.world.ecs().get<world::CSGPolygon3D>(beam);
    // Rings that turn less than path_simplify_angle are merged.
    polygon.path_simplify_angle = 5.0F;
    mesh = scene.mesh(beam);
    CHECK(mesh->triangles == 12);
    CHECK(volume(*mesh) == doctest::Approx(10.0));
    polygon.path_simplify_angle = 0.0F;
    // Subdivided by the path's points rather than by distance.
    polygon.path_interval_type = "subdivide";
    mesh = scene.mesh(beam);
    CHECK(mesh->triangles == 12);
    scene.world.ecs().get<world::Path3D>(rail).points.insert(
        scene.world.ecs().get<world::Path3D>(rail).points.begin() + 1, point({0, 0, -5}));
    mesh = scene.mesh(beam);
    CHECK(mesh->triangles == (2 * 4 * 2) + 4);
    CHECK(volume(*mesh) == doctest::Approx(10.0));
    polygon.path_interval_type = "distance";

    // The path's place in the world moves the shape, unless path_local; moving it makes a new
    // mesh.
    const auto before = scene.mesh(beam);
    scene.world.ecs().get<world::Transform3D>(rail).position = {5, 0, 0};
    mesh = scene.mesh(beam);
    CHECK(mesh != before);
    CHECK(near(mesh->min, {5, 0, -10}));
    polygon.path_local = true;
    mesh = scene.mesh(beam);
    CHECK(near(mesh->min, {0, 0, -10}));
    polygon.path_local = false;
    scene.world.ecs().get<world::Transform3D>(rail).position = {};

    // Round a closed loop and joined, it has no ends.
    auto& path = scene.world.ecs().get<world::Path3D>(rail);
    path.points = {point({0, 0, 0}), point({10, 0, 0}), point({10, 0, -10}), point({0, 0, -10})};
    path.closed = true;
    polygon.path_joined = true;
    polygon.polygon = {{-0.5F, -0.5F}, {0.5F, -0.5F}, {0.5F, 0.5F}, {-0.5F, 0.5F}};
    mesh = scene.mesh(beam);
    CHECK(mesh->problems.empty());
    CHECK(volume(*mesh) == doctest::Approx(40.0).epsilon(0.05));
    CHECK(near(mesh->min, {-0.5F, -0.5F, -10.5F}, 0.1F));
    CHECK(near(mesh->max, {10.5F, 0.5F, 0.5F}, 0.1F));

    // What can't be pushed along a path.
    polygon.path_node = entt::null;
    mesh = scene.mesh(beam);
    REQUIRE(mesh->problems.size() >= 1);
    CHECK(mesh->problems[0].path == "/components/CSGPolygon3D/path_node");
    CHECK(mesh->problems[0].message ==
          "names no entity: name the entity with the Path3D to push the polygon along");
    polygon.path_node = scene.create("post", R"({"Transform3D": {}})");
    mesh = scene.mesh(beam);
    REQUIRE(mesh->problems.size() >= 1);
    CHECK(mesh->problems[0].message ==
          "\"post\" has no Path3D: give it one, or name another entity");
    polygon.path_node = rail;
    path.points.resize(1);
    mesh = scene.mesh(beam);
    REQUIRE(mesh->problems.size() >= 1);
    CHECK(mesh->problems[0].message == "names a Path3D with fewer than 2 points: give it more");
    CHECK(scene3d::check_csg(scene.world).size() >= 1);
}
