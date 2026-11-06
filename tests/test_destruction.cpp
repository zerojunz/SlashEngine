#include "slash/asset/assets.hpp"
#include "slash/canvas/canvas.hpp"
#include "slash/core/json.hpp"
#include "slash/core/math.hpp"
#include "slash/physics3d/physics3d.hpp"
#include "slash/render/device.hpp"
#include "slash/scene3d/fracture.hpp"
#include "slash/scene3d/scene3d.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/world.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace asset = slash::asset;
namespace canvas = slash::canvas;
namespace json = slash::json;
namespace physics3d = slash::physics3d;
namespace render = slash::render;
namespace scene3d = slash::scene3d;
namespace world = slash::world;
using slash::Vec3;

namespace {

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE(parsed.ok());
    return std::move(parsed).value();
}

// A world as sessions make one: drawing, 3D physics and the events it emits.
struct Scene {
    asset::Assets assets{asset::FileSource{
        .read = [](std::string_view /*path*/) -> slash::Result<std::string> {
            return slash::make_error(slash::ErrorCode::NotFound, "no files here");
        },
        .stamp = [](std::string_view /*path*/) -> std::optional<asset::FileStamp> {
            return std::nullopt;
        }}};
    world::ComponentRegistry components;
    world::World world{components};
    std::vector<world::Event> events;

    Scene() {
        world::register_builtin_components(components);
        canvas::install(world, assets);
        physics3d::install(world);
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

    std::vector<std::string> pieces() const {
        std::vector<std::string> ids;
        for (const entt::entity entity : world.ecs().view<const world::FracturePiece3D>()) {
            ids.emplace_back(world.id_of(entity));
        }
        std::sort(ids.begin(), ids.end());
        return ids;
    }
};

constexpr std::string_view kWall = R"({
    "Transform3D": {"position": [0, 1, 0]},
    "StaticBody3D": {},
    "CollisionShape3D": {"shape": "box", "size": [2, 2, 0.3]},
    "BoxMesh": {"size": [2, 2, 0.3]},
    "MeshInstance3D": {},
    "StandardMaterial3D": {"albedo_color": [0.8, 0.4, 0.3, 1]},
    "Destructible3D": {"pieces": 6, "damage_threshold": 10, "mass": 20}})";

// A ball of 4 kg flying at the wall at speed meters per second.
std::string ball(float speed) {
    return R"({"Transform3D": {"position": [0, 1, 1.5]},
        "RigidBody3D": {"mass": 4, "gravity_scale": 0, "linear_velocity": [0, 0, -)" +
           std::to_string(speed) + R"(]},
        "CollisionShape3D": {"shape": "sphere", "radius": 0.3}})";
}

} // namespace

TEST_CASE("destruction: a mesh breaks into Voronoi cells that share its volume and repeat") {
    scene3d::CsgTree box;
    box.shape = world::CSGMesh3D{};
    box.primitive = world::BoxMesh{.size = {2.0F, 2.0F, 0.3F}};
    const std::shared_ptr<const scene3d::Fracture> fracture = scene3d::fracture(box, 6, 0);
    REQUIRE(fracture->problems.empty());
    REQUIRE(fracture->pieces.size() >= 2);
    CHECK(fracture->pieces.size() <= 6);
    double volume = 0.0;
    for (const scene3d::FracturePiece& piece : fracture->pieces) {
        CHECK(piece.volume > 0.0);
        CHECK(piece.hull.size() >= 4);
        CHECK(!piece.mesh.surfaces.empty());
        // Every piece lies in the box.
        CHECK(piece.mesh.min.x >= -1.0001F);
        CHECK(piece.mesh.max.x <= 1.0001F);
        CHECK(piece.mesh.min.z >= -0.1501F);
        CHECK(piece.mesh.max.z <= 0.1501F);
        volume += piece.volume;
    }
    CHECK(volume == doctest::Approx(2.0 * 2.0 * 0.3).epsilon(1.0e-4));

    // The same seed cuts the same pieces; another seed, others.
    const auto again = scene3d::fracture(box, 6, 0);
    REQUIRE(again->pieces.size() == fracture->pieces.size());
    for (std::size_t i = 0; i < again->pieces.size(); ++i) {
        CHECK(again->pieces[i].hull == fracture->pieces[i].hull);
    }
    const auto other = scene3d::fracture(box, 6, 7);
    CHECK(other->pieces.front().hull != fracture->pieces.front().hull);

    // A flat mesh has no inside.
    scene3d::CsgTree plane;
    plane.shape = world::CSGMesh3D{};
    plane.primitive = world::PlaneMesh{};
    const auto flat = scene3d::fracture(plane, 4, 0);
    REQUIRE(flat->problems.size() == 1);
    CHECK(flat->problems.front().message.find("doesn't close") != std::string::npos);
}

TEST_CASE("destruction: an impact past the damage threshold breaks a body into pieces that fly "
          "on") {
    Scene scene;
    scene.create("wall", kWall);
    scene.create("ball", ball(10.0F));
    // 4 kg against a static wall at 10 m/s is 40 kg m/s, past the threshold of 10.
    scene.step(12);
    const bool wall_gone = scene.world.find("wall") == entt::null;
    REQUIRE(wall_gone);
    const std::vector<std::string> pieces = scene.pieces();
    REQUIRE(pieces.size() >= 2);
    CHECK(pieces.front() == "wall_1");
    std::size_t broken = 0;
    for (const world::Event& event : scene.events) {
        if (event.kind == "broken") {
            ++broken;
            CHECK(event.id == "wall");
            CHECK(event.data.find("pieces")->as_array().size() == pieces.size());
        }
    }
    CHECK(broken == 1);
    // The pieces share the wall's 20 kg by volume and keep its look and layers.
    float mass = 0.0F;
    for (const std::string& id : pieces) {
        const entt::entity piece = scene.world.find(id);
        const auto& ecs = scene.world.ecs();
        mass += ecs.get<world::RigidBody3D>(piece).mass;
        CHECK(ecs.get<world::CollisionShape3D>(piece).shape == "convex_polygon");
        CHECK(ecs.all_of<world::BoxMesh, world::MeshInstance3D, world::StandardMaterial3D>(piece));
        CHECK(ecs.get<world::FracturePiece3D>(piece).pieces == 6);
    }
    CHECK(mass == doctest::Approx(20.0F).epsilon(1.0e-4));
    // The ball goes on into the pieces rather than bouncing off the wall, and pushes them back.
    scene.step(30);
    CHECK(scene.world.ecs().get<world::Transform3D>(scene.world.find("ball")).position.z < 1.0F);
    float pushed = 0.0F;
    for (const std::string& id : pieces) {
        pushed += scene.world.ecs().get<world::RigidBody3D>(scene.world.find(id)).linear_velocity.z;
    }
    CHECK(pushed < 0.0F);

    // A gentler ball bounces off a wall that holds.
    Scene gentle;
    gentle.create("wall", kWall);
    gentle.create("ball", ball(2.0F));
    gentle.step(60);
    const bool wall_kept = gentle.world.find("wall") != entt::null;
    CHECK(wall_kept);
    CHECK(gentle.pieces().empty());
}

TEST_CASE("destruction: break_body pushes pieces away and keeps a rigid body's motion, and debris "
          "goes when its lifetime ends") {
    Scene scene;
    const entt::entity crate = scene.create("crate", R"({
        "Transform3D": {"position": [0, 5, 0]},
        "RigidBody3D": {"mass": 8, "gravity_scale": 0, "linear_velocity": [3, 0, 0]},
        "CollisionShape3D": {"shape": "box"},
        "BoxMesh": {},
        "MeshInstance3D": {},
        "Destructible3D": {"pieces": 4, "seed": 3, "debris_lifetime": 0.5}})");
    auto broken = physics3d::break_body(scene.world, crate, 2.0F, Vec3{0.0F, 5.0F, 0.0F});
    REQUIRE(broken.ok());
    const std::vector<entt::entity>& pieces = broken.value();
    REQUIRE(pieces.size() >= 2);
    CHECK(!scene.world.valid(crate));
    for (const entt::entity piece : pieces) {
        const auto& body = scene.world.ecs().get<world::RigidBody3D>(piece);
        CHECK(body.gravity_scale == 0.0F);
        // Each moves as the crate did, plus 2 m/s away from its middle.
        const Vec3 away = body.linear_velocity - Vec3{3.0F, 0.0F, 0.0F};
        CHECK(std::sqrt((away.x * away.x) + (away.y * away.y) + (away.z * away.z)) ==
              doctest::Approx(2.0F).epsilon(1.0e-4));
        CHECK(scene.world.ecs().get<world::FracturePiece3D>(piece).lifetime == 0.5F);
    }
    scene.step(29);
    CHECK(scene.pieces().size() == pieces.size());
    scene.step(2);
    CHECK(scene.pieces().empty());

    // What can't break says why, and changes nothing.
    const entt::entity rock = scene.create("rock", R"({"Transform3D": {}, "StaticBody3D": {},
        "CollisionShape3D": {"shape": "box"}, "BoxMesh": {}, "MeshInstance3D": {}})");
    auto refused = physics3d::break_body(scene.world, rock);
    REQUIRE_FALSE(refused.ok());
    CHECK(refused.error().message == "has no Destructible3D");
    const entt::entity ghost = scene.create("ghost", R"({"Transform3D": {}, "BoxMesh": {},
        "MeshInstance3D": {}, "Destructible3D": {}})");
    refused = physics3d::break_body(scene.world, ghost);
    REQUIRE_FALSE(refused.ok());
    CHECK(refused.error().message ==
          "is no StaticBody3D or RigidBody3D, so it has no body to break");
    const entt::entity sheet = scene.create("sheet", R"({"Transform3D": {}, "StaticBody3D": {},
        "PlaneMesh": {}, "MeshInstance3D": {}, "Destructible3D": {}})");
    refused = physics3d::break_body(scene.world, sheet);
    REQUIRE_FALSE(refused.ok());
    CHECK(refused.error().message.find("can't break: its PlaneMesh is flat") == 0);
    CHECK(scene.world.valid(sheet));

    // scene validate reports both, through check_meshes.
    const std::vector<scene3d::Problem> problems = scene3d::check_destructibles(scene.world);
    REQUIRE(problems.size() == 2);
    CHECK(problems[0].entity == ghost);
    CHECK(problems[0].path == "/components/Destructible3D");
    CHECK(problems[0].message == "breaks only a StaticBody3D or RigidBody3D: give it one");
    CHECK(problems[1].entity == sheet);
    CHECK(problems[1].path == "/components/MeshInstance3D");
    CHECK(problems[1].message.find("can't break: its PlaneMesh is flat") == 0);
}

TEST_CASE("destruction: each piece draws its own part of the mesh") {
    Scene scene;
    scene.create("camera", R"({"Transform3D": {"position": [0, 1, 4]}, "Camera3D": {}})");
    const entt::entity wall = scene.create("wall", kWall);
    const auto device = render::create_null_device();
    scene3d::Renderer renderer(*device, nullptr);
    REQUIRE(renderer.prepare(scene.world, 64.0F, 64.0F, 1.0, {}).has_value());
    CHECK(renderer.meshes() == 1);
    auto broken = physics3d::break_body(scene.world, wall);
    REQUIRE(broken.ok());
    REQUIRE(renderer.prepare(scene.world, 64.0F, 64.0F, 1.0, {}).has_value());
    CHECK(renderer.problems().empty());
    // A mesh per piece, the cut faces in the same material as the outside.
    CHECK(renderer.meshes() == broken.value().size());
    // A piece that doesn't exist is a problem at its index.
    scene.world.ecs().get<world::FracturePiece3D>(broken.value().front()).index = 40;
    REQUIRE(renderer.prepare(scene.world, 64.0F, 64.0F, 1.0, {}).has_value());
    REQUIRE(renderer.problems().size() == 1);
    CHECK(renderer.problems().front().path == "/components/FracturePiece3D/index");
}
