#include "slash/core/json.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/core/math.hpp"
#include "slash/physics3d/physics3d.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/tree3d.hpp"
#include "slash/world/world.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <functional>
#include <numbers>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace json = slash::json;
namespace physics3d = slash::physics3d;
namespace world = slash::world;
using slash::Vec3;

namespace {

constexpr double kTick = 1.0 / 60.0;

world::CollisionShape3D box_shape(Vec3 size) {
    world::CollisionShape3D shape;
    shape.size = size;
    return shape;
}

world::CollisionShape3D sphere_shape(float radius) {
    world::CollisionShape3D shape;
    shape.shape = "sphere";
    shape.radius = radius;
    return shape;
}

world::CollisionShape3D capsule_shape(float radius, float height) {
    world::CollisionShape3D shape;
    shape.shape = "capsule";
    shape.radius = radius;
    shape.height = height;
    return shape;
}

// A world with 3D physics and the events it emits.
struct Physics {
    world::ComponentRegistry components;
    world::World world{components};
    std::vector<world::Event> events;

    explicit Physics(const physics3d::Settings& settings = {}) {
        world::register_builtin_components(components);
        physics3d::install(world, settings);
        world.set_event_listener([this](const world::Event& event) { events.push_back(event); });
    }

    entt::entity add(std::string_view id, Vec3 position) {
        const entt::entity entity = world.create(id).value();
        world.ecs().emplace<world::Transform3D>(entity).position = position;
        return entity;
    }

    template <typename Component>
    Component& get(std::string_view id) {
        return world.ecs().get<Component>(world.find(id));
    }

    Vec3 position(std::string_view id) { return get<world::Transform3D>(id).position; }

    void step(int ticks = 1, const std::function<void()>& before = {}) {
        for (int i = 0; i < ticks; ++i) {
            if (before) {
                before();
            }
            world.step(kTick);
        }
    }

    // A static box between two corners.
    entt::entity block(std::string_view id, Vec3 low, Vec3 high) {
        const Vec3 size = high - low;
        const entt::entity entity = add(id, low + (size * 0.5F));
        world.ecs().emplace<world::StaticBody3D>(entity);
        world.ecs().emplace<world::CollisionShape3D>(entity, box_shape(size));
        return entity;
    }

    // A floor whose top is at y 0, 20 meters across.
    entt::entity floor() { return block("floor", {-10.0F, -1.0F, -10.0F}, {10.0F, 0.0F, 10.0F}); }

    entt::entity crate(std::string_view id, Vec3 position, float size = 1.0F) {
        const entt::entity entity = add(id, position);
        world.ecs().emplace<world::RigidBody3D>(entity);
        world.ecs().emplace<world::CollisionShape3D>(entity, box_shape({size, size, size}));
        return entity;
    }

    // A capsule character 1.8 meters tall whose velocity gains gravity off the floor, as
    // Godot's template script does.
    entt::entity hero(std::string_view id, Vec3 position) {
        const entt::entity entity = add(id, position);
        world.ecs().emplace<world::CharacterBody3D>(entity);
        world.ecs().emplace<world::CollisionShape3D>(entity, capsule_shape(0.4F, 1.8F));
        return entity;
    }

    void fall(std::string_view id) {
        auto& character = get<world::CharacterBody3D>(id);
        if (!character.on_floor) {
            character.velocity.y -= 9.8F * static_cast<float>(kTick);
        }
    }

    [[nodiscard]] std::vector<std::string> event_list() const {
        std::vector<std::string> out;
        for (const world::Event& event : events) {
            if (!event.kind.starts_with("entity.")) {
                out.push_back(event.kind + " " + event.id + " " +
                              json::write(event.data, json::Style::Compact));
            }
        }
        return out;
    }

    [[nodiscard]] std::vector<std::string> ids(const std::vector<entt::entity>& entities) const {
        std::vector<std::string> out;
        for (const entt::entity entity : entities) {
            out.emplace_back(world.id_of(entity));
        }
        return out;
    }
};

using Ids = std::vector<std::string>;

// Settings with no gravity, and no damping unless damped.
physics3d::Settings weightless(bool damped = true) {
    physics3d::Settings settings;
    settings.gravity = 0.0F;
    if (!damped) {
        settings.linear_damp = 0.0F;
        settings.angular_damp = 0.0F;
    }
    return settings;
}

float length(Vec3 v) {
    return std::sqrt((v.x * v.x) + (v.y * v.y) + (v.z * v.z));
}

} // namespace

TEST_CASE("a rigid body falls and comes to rest on a static floor") {
    Physics physics;
    physics.floor();
    physics.crate("crate", {0.0F, 3.0F, 0.0F});
    physics.step(30);
    CHECK(physics.position("crate").y < 3.0F);
    CHECK(physics.get<world::RigidBody3D>("crate").linear_velocity.y < -1.0F);

    physics.step(240);
    const auto& rigid = physics.get<world::RigidBody3D>("crate");
    CHECK(physics.position("crate").y == doctest::Approx(0.5F).epsilon(0.02));
    CHECK(physics.position("crate").x == doctest::Approx(0.0F));
    CHECK(length(rigid.linear_velocity) < 0.01F);
    CHECK(rigid.sleeping);
}

TEST_CASE("physics settings set gravity and damping") {
    physics3d::Settings settings;
    settings.gravity = 2.0F;
    settings.gravity_vector = {1.0F, 0.0F, 0.0F};
    settings.linear_damp = 0.0F;
    Physics physics(settings);
    physics.crate("crate", {});
    physics.step(60);
    // One second at 2 m/s² sideways, with no damping.
    CHECK(physics.get<world::RigidBody3D>("crate").linear_velocity.x ==
          doctest::Approx(2.0F).epsilon(0.01));
    CHECK(physics.position("crate").x == doctest::Approx(1.0F).epsilon(0.05));
    CHECK(physics.position("crate").y == doctest::Approx(0.0F));

    const auto gravity = physics3d::gravity(physics.world, physics.world.find("crate"));
    REQUIRE(gravity.has_value());
    CHECK(gravity->gravity == Vec3{2.0F, 0.0F, 0.0F});
    CHECK(gravity->angular_damp == doctest::Approx(0.1F));

    const json::Value schema = physics3d::settings_schema();
    const physics3d::Settings parsed = physics3d::parse_settings(
        json::parse(R"({"gravity": 3, "gravity_vector": [0, 0, -1], "linear_damp": 0.5,
                        "areas_detect_static_bodies": true})")
            .value());
    CHECK(json::validate(json::parse(R"({"gravity": 3, "gravity_vector": [0, 0, -1]})").value(),
                         schema)
              .empty());
    CHECK(parsed.gravity == 3.0F);
    CHECK(parsed.gravity_vector == Vec3{0.0F, 0.0F, -1.0F});
    CHECK(parsed.linear_damp == 0.5F);
    CHECK(parsed.areas_detect_static_bodies);
}

TEST_CASE("bodies collide when either one's mask has the other's layer") {
    Physics physics;
    physics.floor();
    // On layer 2 and looking at layer 2 only: the floor is on layer 1 and looks at layer 1.
    physics.crate("ghost", {0.0F, 2.0F, 0.0F});
    physics.get<world::RigidBody3D>("ghost").collision_layer = 2;
    physics.get<world::RigidBody3D>("ghost").collision_mask = 2;
    // Looking at layer 1 only, but on layer 2: it still lands, since its mask has the floor's.
    physics.crate("lander", {3.0F, 2.0F, 0.0F});
    physics.get<world::RigidBody3D>("lander").collision_layer = 2;
    physics.step(120);
    CHECK(physics.position("ghost").y < -2.0F);
    CHECK(physics.position("lander").y == doctest::Approx(0.5F).epsilon(0.02));
}

TEST_CASE("an area reports bodies and areas entering and leaving") {
    Physics physics;
    const entt::entity zone = physics.add("zone", {0.0F, -3.0F, 0.0F});
    physics.world.ecs().emplace<world::Area3D>(zone);
    physics.world.ecs().emplace<world::CollisionShape3D>(zone, box_shape({4.0F, 2.0F, 4.0F}));
    const entt::entity sensor = physics.add("sensor", {1.5F, -3.0F, 0.0F});
    physics.world.ecs().emplace<world::Area3D>(sensor);
    physics.world.ecs().emplace<world::CollisionShape3D>(sensor, sphere_shape(0.25F));
    physics.crate("crate", {0.0F, 0.0F, 0.0F}, 0.5F);

    physics.step();
    CHECK(physics.event_list() ==
          Ids{R"(area_entered sensor {"area":"zone"})", R"(area_entered zone {"area":"sensor"})"});
    physics.events.clear();

    // In and out as it falls; nothing collides with an area.
    std::vector<Ids> inside;
    physics.step(90, [&] {
        inside.push_back(physics.ids(physics3d::contacts(physics.world, zone).bodies));
    });
    CHECK(physics.event_list() ==
          Ids{R"(body_entered zone {"body":"crate"})", R"(body_exited zone {"body":"crate"})"});
    CHECK(std::find(inside.begin(), inside.end(), Ids{"crate"}) != inside.end());
    CHECK(physics.position("crate").y < -5.0F);

    // A body's contacts list the areas it is in.
    physics.get<world::Transform3D>("crate").position = {0.0F, -3.0F, 0.0F};
    physics.get<world::RigidBody3D>("crate").gravity_scale = 0.0F;
    physics.get<world::RigidBody3D>("crate").linear_velocity = {};
    physics.step();
    CHECK(physics.ids(physics3d::contacts(physics.world, physics.world.find("crate")).areas) ==
          Ids{"zone"});

    // An area that isn't monitorable is not seen by others.
    physics.events.clear();
    physics.get<world::Area3D>("sensor").monitorable = false;
    physics.step();
    CHECK(physics.event_list() == Ids{R"(area_exited zone {"area":"sensor"})"});

    // Static bodies are left out, as Godot's Jolt module leaves them out by default.
    physics.events.clear();
    physics.block("wall", {-0.2F, -3.2F, -0.2F}, {0.2F, -2.8F, 0.2F});
    physics.step();
    CHECK(physics.event_list().empty());
    physics3d::Settings settings;
    settings.areas_detect_static_bodies = true;
    physics3d::set_settings(physics.world, settings);
    physics.step();
    CHECK(physics.event_list() == Ids{R"(body_entered zone {"body":"wall"})"});
}

TEST_CASE("contact_monitor reports the bodies a rigid body touches") {
    Physics physics;
    physics.floor();
    physics.crate("crate", {0.0F, 1.0F, 0.0F});
    physics.get<world::RigidBody3D>("crate").contact_monitor = true;
    physics.step(60);
    CHECK(physics.event_list() == Ids{R"(body_entered crate {"body":"floor"})"});
    CHECK(physics.ids(physics3d::contacts(physics.world, physics.world.find("crate")).bodies) ==
          Ids{"floor"});
    CHECK(physics.ids(physics3d::contacts(physics.world, physics.world.find("floor")).bodies) ==
          Ids{"crate"});

    physics.events.clear();
    physics.get<world::RigidBody3D>("crate").linear_velocity = {0.0F, 8.0F, 0.0F};
    physics.step(5);
    CHECK(physics.event_list() == Ids{R"(body_exited crate {"body":"floor"})"});
}

TEST_CASE("a character lands, walks, stops at walls and stays put on slopes") {
    Physics physics;
    physics.floor();
    physics.block("wall", {3.0F, 0.0F, -10.0F}, {4.0F, 3.0F, 10.0F});
    physics.hero("hero", {0.0F, 2.0F, 0.0F});
    physics.step(60, [&] { physics.fall("hero"); });
    const auto& hero = physics.get<world::CharacterBody3D>("hero");
    REQUIRE(hero.on_floor);
    // The capsule's bottom rests on the floor, within the safe margin.
    CHECK(physics.position("hero").y == doctest::Approx(0.9F).epsilon(0.01));
    CHECK(hero.floor_normal.y == doctest::Approx(1.0F));
    CHECK(hero.velocity.y == 0.0F);
    CHECK(physics.ids(physics3d::contacts(physics.world, physics.world.find("hero")).bodies) ==
          Ids{"floor"});

    // Walking right at 4 m/s for a second, it stops against the wall.
    physics.step(60, [&] {
        physics.fall("hero");
        physics.get<world::CharacterBody3D>("hero").velocity.x = 4.0F;
    });
    CHECK(hero.on_floor);
    CHECK(hero.on_wall);
    CHECK(hero.wall_normal.x == doctest::Approx(-1.0F));
    CHECK(physics.position("hero").x == doctest::Approx(2.6F).epsilon(0.01));
    CHECK(physics.position("hero").y == doctest::Approx(0.9F).epsilon(0.01));

    // On a 30 degree slope, pulled down every tick, it stays put, as floor_stop_on_slope says.
    Physics slope;
    slope.block("ramp", {-5.0F, -0.5F, -5.0F}, {5.0F, 0.5F, 5.0F});
    slope.get<world::Transform3D>("ramp").rotation = {0.0F, 0.0F, 0.5235988F};
    slope.hero("hero", {0.0F, 2.5F, 0.0F});
    const auto pull = [&] {
        slope.get<world::CharacterBody3D>("hero").velocity.y -= 9.8F * static_cast<float>(kTick);
    };
    slope.step(90, pull);
    REQUIRE(slope.get<world::CharacterBody3D>("hero").on_floor);
    const Vec3 rest = slope.position("hero");
    slope.step(60, pull);
    CHECK(slope.position("hero").x == doctest::Approx(rest.x).epsilon(0.001));
    CHECK(slope.position("hero").y == doctest::Approx(rest.y).epsilon(0.001));

    // Without floor_stop_on_slope it slides down.
    slope.get<world::CharacterBody3D>("hero").floor_stop_on_slope = false;
    slope.step(60, pull);
    CHECK(slope.position("hero").y < rest.y - 0.02F);
}

TEST_CASE("a floating character slides along walls and has no floor") {
    Physics physics(weightless());
    physics.block("wall", {1.0F, -5.0F, -5.0F}, {2.0F, 5.0F, 5.0F});
    const entt::entity diver = physics.add("diver", {0.0F, 0.0F, 0.0F});
    auto& character = physics.world.ecs().emplace<world::CharacterBody3D>(diver);
    character.motion_mode = "floating";
    character.velocity = {2.0F, 0.0F, 2.0F};
    physics.world.ecs().emplace<world::CollisionShape3D>(diver, sphere_shape(0.5F));
    physics.step(60);
    const auto& diver_body = physics.get<world::CharacterBody3D>("diver");
    CHECK_FALSE(diver_body.on_floor);
    CHECK(diver_body.on_wall);
    CHECK(physics.position("diver").x == doctest::Approx(0.5F).epsilon(0.01));
    // It slid along the wall rather than stopping.
    CHECK(physics.position("diver").z > 1.0F);
}

TEST_CASE("a character rides a moving platform and keeps its velocity when it leaves") {
    Physics physics;
    const entt::entity lift = physics.add("lift", {0.0F, 0.0F, 0.0F});
    physics.world.ecs().emplace<world::AnimatableBody3D>(lift);
    physics.world.ecs().emplace<world::CollisionShape3D>(lift, box_shape({20.0F, 0.5F, 20.0F}));
    physics.hero("hero", {0.0F, 1.5F, 0.0F});
    physics.step(60, [&] { physics.fall("hero"); });
    REQUIRE(physics.get<world::CharacterBody3D>("hero").on_floor);
    const Vec3 start = physics.position("hero");

    // The lift moves along x 0.05 meters a tick, and the hero with it.
    physics.step(30, [&] {
        physics.fall("hero");
        physics.get<world::Transform3D>("lift").position.x += 0.05F;
    });
    const auto& hero = physics.get<world::CharacterBody3D>("hero");
    CHECK(hero.on_floor);
    CHECK(physics.position("hero").x == doctest::Approx(start.x + 1.5F).epsilon(0.02));
    CHECK(hero.platform_velocity.x == doctest::Approx(3.0F).epsilon(0.01));

    // Jumping off keeps the lift's velocity, as Godot's platform_on_leave add_velocity.
    physics.get<world::CharacterBody3D>("hero").velocity = {0.0F, 5.0F, 0.0F};
    physics.step(1, [&] { physics.get<world::Transform3D>("lift").position.x += 0.05F; });
    CHECK_FALSE(hero.on_floor);
    CHECK(hero.velocity.x == doctest::Approx(3.0F).epsilon(0.01));
}

TEST_CASE("a character stops at a rigid body, as Godot's, until an impulse pushes it") {
    Physics physics;
    physics.floor();
    physics.crate("crate", {1.5F, 0.5F, 0.0F});
    physics.hero("hero", {0.0F, 0.95F, 0.0F});
    const auto walk = [&] {
        physics.fall("hero");
        physics.get<world::CharacterBody3D>("hero").velocity.x = 2.0F;
    };
    physics.step(60, walk);
    CHECK(physics.position("hero").x == doctest::Approx(0.6F).epsilon(0.01));
    CHECK(physics.get<world::CharacterBody3D>("hero").on_wall);
    CHECK(physics.ids(physics3d::contacts(physics.world, physics.world.find("hero")).bodies) ==
          Ids{"crate", "floor"});

    // Gameplay code pushes what the character walks into, as Godot's scripts do.
    physics3d::apply_impulse(physics.world, physics.world.find("crate"), {3.0F, 0.0F, 0.0F});
    physics.step(30, walk);
    CHECK(physics.position("crate").x > 1.8F);
    CHECK(physics.position("hero").x > 0.7F);
}

TEST_CASE("every shape kind collides") {
    Physics physics;
    physics.floor();
    world::CollisionShape3D shapes[5];
    shapes[0] = sphere_shape(0.5F);
    shapes[1] = capsule_shape(0.3F, 1.2F);
    shapes[2].shape = "cylinder";
    shapes[2].radius = 0.4F;
    shapes[2].height = 1.0F;
    shapes[3].shape = "convex_polygon";
    shapes[3].points = {
        {-0.5F, 0.0F, -0.5F}, {0.5F, 0.0F, -0.5F}, {0.0F, 0.0F, 0.5F}, {0.0F, 0.8F, 0.0F}};
    shapes[4] = box_shape({1.0F, 0.6F, 1.0F});
    for (int i = 0; i < 5; ++i) {
        const std::string id = "body" + std::to_string(i);
        const entt::entity entity =
            physics.add(id, {(static_cast<float>(i) * 2.0F) - 4.0F, 2.0F, 0.0F});
        auto& rigid = physics.world.ecs().emplace<world::RigidBody3D>(entity);
        rigid.lock_rotation = true;
        physics.world.ecs().emplace<world::CollisionShape3D>(entity, shapes[i]);
    }
    physics.step(180);
    CHECK(physics.position("body0").y == doctest::Approx(0.5F).epsilon(0.02));
    CHECK(physics.position("body1").y == doctest::Approx(0.6F).epsilon(0.02));
    CHECK(physics.position("body2").y == doctest::Approx(0.5F).epsilon(0.02));
    CHECK(physics.position("body3").y == doctest::Approx(0.0F).epsilon(0.02));
    CHECK(physics.position("body4").y == doctest::Approx(0.3F).epsilon(0.02));

    // A concave mesh holds a ball in its bowl; a world boundary holds what falls through all.
    Physics bowl;
    const entt::entity ground = bowl.add("ground", {});
    bowl.world.ecs().emplace<world::StaticBody3D>(ground);
    world::CollisionShape3D mesh;
    mesh.shape = "concave_polygon";
    // Two triangles making a V-shaped valley along z, sloping down to x 0.
    mesh.faces = {{-4.0F, 2.0F, -4.0F}, {0.0F, 0.0F, -4.0F}, {0.0F, 0.0F, 4.0F},
                  {-4.0F, 2.0F, -4.0F}, {0.0F, 0.0F, 4.0F},  {-4.0F, 2.0F, 4.0F},
                  {0.0F, 0.0F, -4.0F},  {4.0F, 2.0F, -4.0F}, {4.0F, 2.0F, 4.0F},
                  {0.0F, 0.0F, -4.0F},  {4.0F, 2.0F, 4.0F},  {0.0F, 0.0F, 4.0F}};
    bowl.world.ecs().emplace<world::CollisionShape3D>(ground, mesh);
    const entt::entity ball = bowl.add("ball", {2.0F, 3.0F, 0.0F});
    bowl.world.ecs().emplace<world::RigidBody3D>(ball);
    bowl.world.ecs().emplace<world::CollisionShape3D>(ball, sphere_shape(0.25F));
    const entt::entity limit = bowl.add("limit", {0.0F, -20.0F, 0.0F});
    bowl.world.ecs().emplace<world::StaticBody3D>(limit);
    world::CollisionShape3D boundary;
    boundary.shape = "world_boundary";
    bowl.world.ecs().emplace<world::CollisionShape3D>(limit, boundary);
    bowl.crate("stray", {8.0F, 0.0F, 0.0F});
    bowl.step(600);
    CHECK(std::abs(bowl.position("ball").x) < 0.3F);
    CHECK(bowl.position("ball").y > 0.0F);
    CHECK(bowl.position("stray").y == doctest::Approx(-19.5F).epsilon(0.01));

    // A shape that can't be made is left out with a warning; its body falls, touching nothing.
    world::CollisionShape3D bad;
    bad.shape = "convex_polygon";
    bad.points = {{0.0F, 0.0F, 0.0F}, {1.0F, 0.0F, 0.0F}};
    const entt::entity broken = bowl.add("broken", {0.0F, 5.0F, 0.0F});
    bowl.world.ecs().emplace<world::RigidBody3D>(broken);
    bowl.world.ecs().emplace<world::CollisionShape3D>(broken, bad);
    bowl.step(10);
    CHECK(bowl.position("broken").y < 5.0F);
}

TEST_CASE("raycast finds the first hit and follows masks, areas and exclusions") {
    Physics physics;
    physics.floor();
    physics.crate("near", {0.0F, 0.5F, 2.0F});
    physics.get<world::RigidBody3D>("near").collision_layer = 2;
    physics.get<world::RigidBody3D>("near").freeze = true;
    const entt::entity zone = physics.add("zone", {0.0F, 0.5F, 4.0F});
    physics.world.ecs().emplace<world::Area3D>(zone);
    physics.world.ecs().emplace<world::CollisionShape3D>(zone, box_shape({1.0F, 1.0F, 1.0F}));

    const auto hit = physics3d::raycast(physics.world, {0.0F, 0.5F, 10.0F}, {0.0F, 0.5F, 0.0F});
    REQUIRE(hit.has_value());
    CHECK(physics.world.id_of(hit->entity) == "near");
    CHECK(hit->point.z == doctest::Approx(2.5F).epsilon(0.001));
    CHECK(hit->normal.z == doctest::Approx(1.0F));
    CHECK(hit->fraction == doctest::Approx(0.75F).epsilon(0.001));

    physics3d::QueryFilter areas;
    areas.collide_with_areas = true;
    const auto area_hit =
        physics3d::raycast(physics.world, {0.0F, 0.5F, 10.0F}, {0.0F, 0.5F, 0.0F}, areas);
    REQUIRE(area_hit.has_value());
    CHECK(physics.world.id_of(area_hit->entity) == "zone");

    physics3d::QueryFilter layer_one;
    layer_one.collision_mask = 1;
    CHECK_FALSE(
        physics3d::raycast(physics.world, {0.0F, 0.5F, 10.0F}, {0.0F, 0.5F, 0.0F}, layer_one));
    const auto down =
        physics3d::raycast(physics.world, {0.0F, 3.0F, 2.0F}, {0.0F, -3.0F, 2.0F}, layer_one);
    REQUIRE(down.has_value());
    CHECK(physics.world.id_of(down->entity) == "floor");
    CHECK(down->normal.y == doctest::Approx(1.0F));

    physics3d::QueryFilter exclude;
    exclude.exclude = {physics.world.find("near"), physics.world.find("floor")};
    CHECK_FALSE(
        physics3d::raycast(physics.world, {0.0F, 3.0F, 2.0F}, {0.0F, -3.0F, 2.0F}, exclude));
    // A ray starting inside a shape doesn't hit it.
    CHECK_FALSE(
        physics3d::raycast(physics.world, {0.0F, 0.5F, 2.0F}, {0.0F, 0.5F, 3.0F}, layer_one));
}

TEST_CASE("overlap_point and overlap_shape find what is there") {
    Physics physics;
    physics.floor();
    physics.crate("a", {0.0F, 0.5F, 0.0F});
    physics.crate("b", {3.0F, 0.5F, 0.0F});
    physics.get<world::RigidBody3D>("a").freeze = true;
    physics.get<world::RigidBody3D>("b").freeze = true;
    CHECK(physics.ids(physics3d::overlap_point(physics.world, {0.1F, 0.4F, 0.1F})) == Ids{"a"});
    CHECK(physics.ids(physics3d::overlap_point(physics.world, {0.0F, -0.5F, 0.0F})) ==
          Ids{"floor"});
    CHECK(physics3d::overlap_point(physics.world, {1.5F, 2.0F, 0.0F}).empty());

    // A sphere between the two crates touches both and the floor.
    CHECK(physics.ids(physics3d::overlap_shape(physics.world, sphere_shape(1.2F),
                                               {1.5F, 0.5F, 0.0F}, {})) == Ids{"a", "b", "floor"});
    physics3d::QueryFilter only_bodies;
    only_bodies.collision_mask = 1;
    only_bodies.exclude = {physics.world.find("floor")};
    CHECK(physics
              .ids(physics3d::overlap_shape(physics.world, box_shape({1.0F, 0.2F, 0.2F}),
                                            {0.0F, 1.5F, 0.0F}, {}, only_bodies))
              .empty());
    // Turned upright, the long box reaches down into a.
    CHECK(physics.ids(physics3d::overlap_shape(physics.world, box_shape({2.0F, 0.2F, 0.2F}),
                                               {0.0F, 1.5F, 0.0F}, {0.0F, 0.0F, 1.5707964F},
                                               only_bodies)) == Ids{"a"});
}

TEST_CASE("impulses, forces, freezing and removing bodies") {
    Physics physics(weightless(false));
    physics.crate("crate", {});
    physics.step();
    physics3d::apply_impulse(physics.world, physics.world.find("crate"), {2.0F, 0.0F, 0.0F});
    physics.step();
    CHECK(physics.get<world::RigidBody3D>("crate").linear_velocity.x ==
          doctest::Approx(2.0F).epsilon(0.001));
    physics3d::apply_torque_impulse(physics.world, physics.world.find("crate"), {0.0F, 0.1F, 0.0F});
    physics.step();
    CHECK(physics.get<world::RigidBody3D>("crate").angular_velocity.y > 0.1F);

    // A constant force accelerates it: 1 N on 1 kg for a second.
    physics.get<world::RigidBody3D>("crate").linear_velocity = {};
    physics.get<world::RigidBody3D>("crate").angular_velocity = {};
    physics.get<world::RigidBody3D>("crate").constant_force = {0.0F, 0.0F, 1.0F};
    physics.step(60);
    CHECK(physics.get<world::RigidBody3D>("crate").linear_velocity.z ==
          doctest::Approx(1.0F).epsilon(0.02));

    // Frozen, it stays where it is.
    physics.get<world::RigidBody3D>("crate").freeze = true;
    const Vec3 frozen = physics.position("crate");
    physics.step(10);
    CHECK(physics.position("crate") == frozen);

    // Removed, it is no body any more.
    physics.world.ecs().remove<world::RigidBody3D>(physics.world.find("crate"));
    physics.step();
    CHECK_FALSE(physics3d::has_body(physics.world, physics.world.find("crate")));
    CHECK_FALSE(physics3d::gravity(physics.world, physics.world.find("crate")).has_value());
}

TEST_CASE("the same scene simulates the same every time") {
    const auto run = [] {
        Physics physics;
        physics.floor();
        for (int i = 0; i < 12; ++i) {
            const float f = static_cast<float>(i);
            physics.crate("crate" + std::to_string(i),
                          {(f * 0.37F) - 2.0F, 1.0F + (f * 1.1F), (f * 0.21F) - 1.0F}, 0.8F);
        }
        physics.hero("hero", {-4.0F, 1.0F, 0.0F});
        physics.step(240, [&] {
            physics.fall("hero");
            physics.get<world::CharacterBody3D>("hero").velocity.x = 2.0F;
        });
        std::vector<std::uint32_t> bits;
        for (int i = 0; i < 12; ++i) {
            const Vec3 p = physics.position("crate" + std::to_string(i));
            bits.push_back(std::bit_cast<std::uint32_t>(p.x));
            bits.push_back(std::bit_cast<std::uint32_t>(p.y));
            bits.push_back(std::bit_cast<std::uint32_t>(p.z));
        }
        bits.push_back(std::bit_cast<std::uint32_t>(physics.position("hero").x));
        return bits;
    };
    CHECK(run() == run());
}

TEST_CASE("a body takes the shapes of child entities with no body of their own, as Godot's") {
    Physics physics;
    physics.floor();
    // A dumbbell: a body with two sphere children a meter either side.
    const entt::entity bell = physics.add("bell", {0.0F, 2.0F, 0.0F});
    auto& rigid = physics.world.ecs().emplace<world::RigidBody3D>(bell);
    rigid.lock_rotation = true;
    for (const float side : {-1.0F, 1.0F}) {
        const std::string id = side < 0.0F ? "left" : "right";
        const entt::entity end = physics.add(id, {side, 0.0F, 0.0F});
        physics.world.ecs().emplace<world::CollisionShape3D>(end, sphere_shape(0.3F));
        physics.world.ecs().get<world::Transform3D>(bell).children.push_back(end);
    }
    physics.step(120);
    CHECK(physics.position("bell").y == doctest::Approx(0.3F).epsilon(0.02));
    CHECK(physics.ids(physics3d::overlap_point(physics.world, {1.0F, 0.3F, 0.0F})) == Ids{"bell"});
    CHECK(physics3d::overlap_point(physics.world, {0.0F, 0.3F, 0.0F}).empty());
}

TEST_CASE("an area's space overrides change gravity and damping, by priority") {
    Physics physics;
    const entt::entity low = physics.add("low", {});
    auto& low_area = physics.world.ecs().emplace<world::Area3D>(low);
    low_area.gravity_space_override = "replace";
    low_area.gravity_direction = {1.0F, 0.0F, 0.0F};
    low_area.gravity = 4.0F;
    physics.world.ecs().emplace<world::CollisionShape3D>(low, box_shape({10.0F, 10.0F, 10.0F}));
    const entt::entity high = physics.add("high", {});
    auto& high_area = physics.world.ecs().emplace<world::Area3D>(high);
    high_area.priority = 1;
    high_area.gravity_space_override = "combine";
    high_area.gravity_direction = {0.0F, 0.0F, 1.0F};
    high_area.gravity = 1.0F;
    high_area.linear_damp_space_override = "replace";
    high_area.linear_damp = 2.0F;
    physics.world.ecs().emplace<world::CollisionShape3D>(high, box_shape({10.0F, 10.0F, 10.0F}));
    physics.crate("crate", {});
    physics.step(2);
    const auto gravity = physics3d::gravity(physics.world, physics.world.find("crate"));
    REQUIRE(gravity.has_value());
    // high combines first, then low replaces what it gave and leaves no room for the project's.
    CHECK(gravity->gravity == Vec3{4.0F, 0.0F, 0.0F});
    CHECK(gravity->linear_damp == doctest::Approx(2.0F));
    CHECK(gravity->angular_damp == doctest::Approx(0.1F));
    CHECK(physics.ids(gravity->areas) == Ids{"high", "low"});
    physics.step(30);
    // Overrides apply from the step after a body enters, so it fell for its first step.
    CHECK(physics.position("crate").x > 0.1F);
    CHECK(physics.position("crate").y > -0.1F);
    CHECK(physics.position("crate").z == 0.0F);
}

TEST_CASE("constant velocities carry what touches a body without moving it") {
    Physics physics;
    const entt::entity belt = physics.floor();
    physics.world.ecs().get<world::StaticBody3D>(belt).constant_linear_velocity = {1.0F, 0.0F,
                                                                                   0.0F};
    physics.crate("crate", {0.0F, 0.6F, 0.0F});
    physics.hero("hero", {0.0F, 1.0F, 3.0F});
    physics.step(120, [&] { physics.fall("hero"); });
    CHECK(physics.position("floor").x == doctest::Approx(0.0F));
    CHECK(physics.position("crate").x > 1.0F);
    CHECK(physics.get<world::CharacterBody3D>("hero").platform_velocity.x ==
          doctest::Approx(1.0F).epsilon(0.01));
    CHECK(physics.position("hero").x > 1.0F);
}

TEST_CASE("3D physics reports its memory") {
    Physics physics;
    physics.floor();
    physics.crate("crate", {0.0F, 2.0F, 0.0F});
    const entt::entity pin = physics.add("pin", {0.0F, 3.0F, 0.0F});
    physics.world.ecs().emplace<world::PinJoint3D>(pin).node_a = physics.world.find("crate");
    physics.step();
    const auto parts = physics.world.memory();
    const auto found = std::find_if(parts.begin(), parts.end(), [](const world::MemoryPart& part) {
        return part.name == "physics3d";
    });
    REQUIRE(found != parts.end());
    CHECK(found->bytes > 0);
    CHECK(found->counts.at("bodies") == 2);
    CHECK(found->counts.at("active") == 1);
    CHECK(found->counts.at("joints") == 1);
}

namespace {

// The angle between two directions.
float angle_between(Vec3 a, Vec3 b) {
    const float d = ((a.x * b.x) + (a.y * b.y) + (a.z * b.z)) / (length(a) * length(b));
    return std::acos(std::clamp(d, -1.0F, 1.0F));
}

float distance(Vec3 a, Vec3 b) {
    return length(a - b);
}

} // namespace

TEST_CASE("a pin joint swings a body from the world and holds two bodies together") {
    Physics physics;
    // A pendulum: a ball a meter from a pin that holds it to the world.
    const entt::entity ball = physics.crate("ball", {1.0F, 0.0F, 0.0F}, 0.2F);
    const entt::entity pin = physics.add("pin", {});
    physics.world.ecs().emplace<world::PinJoint3D>(pin).node_a = ball;
    physics.step(30);
    CHECK(physics.position("ball").y < -0.5F);
    CHECK(distance(physics.position("ball"), {}) == doctest::Approx(1.0F).epsilon(0.02));
    physics.step(30);
    CHECK(distance(physics.position("ball"), {}) == doctest::Approx(1.0F).epsilon(0.02));

    // Two crates pinned halfway between them move as one when one is pushed.
    Physics weightless_physics(weightless(false));
    const entt::entity a = weightless_physics.crate("a", {-1.0F, 0.0F, 0.0F}, 0.5F);
    const entt::entity b = weightless_physics.crate("b", {1.0F, 0.0F, 0.0F}, 0.5F);
    auto& joint = weightless_physics.world.ecs().emplace<world::PinJoint3D>(
        weightless_physics.add("joint", {}));
    joint.node_a = a;
    joint.node_b = b;
    weightless_physics.step();
    physics3d::apply_impulse(weightless_physics.world, a, {1.0F, 0.0F, 0.0F});
    weightless_physics.step(60);
    CHECK(weightless_physics.position("b").x > 1.3F);
    CHECK(distance(weightless_physics.position("a"), weightless_physics.position("b")) ==
          doctest::Approx(2.0F).epsilon(0.02));
}

TEST_CASE("a hinge turns about its z axis within its limits, and its motor drives it") {
    // A door: a slab whose hinge's z axis points up.
    const auto door = [](Physics& physics) {
        const entt::entity slab = physics.add("door", {0.5F, 0.0F, 0.0F});
        physics.world.ecs().emplace<world::RigidBody3D>(slab);
        physics.world.ecs().emplace<world::CollisionShape3D>(slab, box_shape({1.0F, 2.0F, 0.1F}));
        const entt::entity hinge = physics.add("hinge", {});
        physics.get<world::Transform3D>("hinge").rotation.x = -std::numbers::pi_v<float> / 2.0F;
        auto& joint = physics.world.ecs().emplace<world::HingeJoint3D>(hinge);
        joint.node_a = slab;
        return &joint;
    };
    {
        Physics physics(weightless(false));
        door(physics);
        physics.step();
        physics3d::apply_impulse(physics.world, physics.world.find("door"), {0.0F, 0.0F, 1.0F},
                                 {0.5F, 0.0F, 0.0F});
        physics.step(30);
        // It turned about the hinge: its middle stays half a meter from the axis, at its height.
        const Vec3 p = physics.position("door");
        CHECK(p.z > 0.2F);
        CHECK(p.y == doctest::Approx(0.0F).epsilon(0.01));
        CHECK(length({p.x, 0.0F, p.z}) == doctest::Approx(0.5F).epsilon(0.02));
        const Vec3 spin = physics.get<world::RigidBody3D>("door").angular_velocity;
        CHECK(std::abs(spin.x) < 0.01F);
        CHECK(std::abs(spin.z) < 0.01F);
    }
    {
        // Limited to half a radian either way, a hard push stops at the limit.
        Physics physics(weightless(false));
        world::HingeJoint3D* joint = door(physics);
        joint->angular_limit_enabled = true;
        joint->angular_limit_lower = -0.5F;
        joint->angular_limit_upper = 0.5F;
        physics.step();
        physics3d::apply_impulse(physics.world, physics.world.find("door"), {0.0F, 0.0F, 3.0F},
                                 {0.5F, 0.0F, 0.0F});
        physics.step(60);
        const Vec3 p = physics.position("door");
        CHECK(angle_between({p.x, 0.0F, p.z}, {1.0F, 0.0F, 0.0F}) < 0.55F);
    }
    {
        // Its motor turns it at its target velocity.
        Physics physics(weightless(false));
        world::HingeJoint3D* joint = door(physics);
        joint->motor_enabled = true;
        joint->motor_target_velocity = 2.0F;
        joint->motor_max_impulse = 10.0F;
        physics.step(30);
        CHECK(std::abs(physics.get<world::RigidBody3D>("door").angular_velocity.y) ==
              doctest::Approx(2.0F).epsilon(0.02));
    }
}

TEST_CASE("a slider moves a body along its x axis within its limits and keeps it from turning") {
    Physics physics(weightless(false));
    const entt::entity crate = physics.crate("crate", {});
    auto& joint = physics.world.ecs().emplace<world::SliderJoint3D>(physics.add("slider", {}));
    joint.node_a = crate;
    physics.step();
    physics3d::apply_impulse(physics.world, crate, {0.5F, 0.5F, 0.5F}, {0.0F, 0.5F, 0.0F});
    physics.step(30);
    const Vec3 p = physics.position("crate");
    CHECK(p.x > 0.2F);
    CHECK(std::abs(p.y) < 0.01F);
    CHECK(std::abs(p.z) < 0.01F);
    CHECK(length(physics.get<world::RigidBody3D>("crate").angular_velocity) < 0.01F);
    // Its limits are a meter either way.
    physics.step(120);
    CHECK(physics.position("crate").x == doctest::Approx(1.0F).epsilon(0.03));
}

TEST_CASE("a cone twist joint keeps a hanging body's swing within its span") {
    Physics physics;
    const entt::entity weight = physics.crate("weight", {0.0F, -1.0F, 0.0F}, 0.2F);
    // Its twist axis, x, points down at the weight.
    const entt::entity cone = physics.add("cone", {});
    physics.get<world::Transform3D>("cone").rotation.z = -std::numbers::pi_v<float> / 2.0F;
    auto& joint = physics.world.ecs().emplace<world::ConeTwistJoint3D>(cone);
    joint.node_a = weight;
    joint.swing_span = 0.3F;
    physics.step();
    physics3d::apply_impulse(physics.world, weight, {1.5F, 0.0F, 0.0F});
    float widest = 0.0F;
    physics.step(60, [&] {
        widest = std::max(widest, angle_between(physics.position("weight"), {0.0F, -1.0F, 0.0F}));
    });
    // The limit gives a little under a push.
    CHECK(widest > 0.2F);
    CHECK(widest < 0.4F);
    CHECK(distance(physics.position("weight"), {}) == doctest::Approx(1.0F).epsilon(0.02));
}

TEST_CASE("a 6DOF joint locks its axes, frees the ones without limits and drives them") {
    {
        // Every axis locked, it welds two crates together.
        Physics physics(weightless(false));
        const entt::entity a = physics.crate("a", {0.0F, 0.0F, 0.0F}, 0.5F);
        const entt::entity b = physics.crate("b", {1.0F, 0.0F, 0.0F}, 0.5F);
        auto& joint =
            physics.world.ecs().emplace<world::Generic6DOFJoint3D>(physics.add("weld", {}));
        joint.node_a = a;
        joint.node_b = b;
        physics.step();
        physics3d::apply_impulse(physics.world, a, {0.0F, 1.0F, 0.0F}, {0.0F, 0.0F, 0.25F});
        physics.step(60);
        const Vec3 offset = physics.position("b") - physics.position("a");
        CHECK(physics.position("a").y > 0.3F);
        CHECK(length(offset) == doctest::Approx(1.0F).epsilon(0.02));
        CHECK(
            physics.get<world::RigidBody3D>("a").angular_velocity.x ==
            doctest::Approx(physics.get<world::RigidBody3D>("b").angular_velocity.x).epsilon(0.02));
    }
    {
        // With y free, a crate held to the world falls straight down.
        Physics physics;
        const entt::entity crate = physics.crate("crate", {});
        auto& joint =
            physics.world.ecs().emplace<world::Generic6DOFJoint3D>(physics.add("rail", {}));
        joint.node_a = crate;
        joint.linear_y.limit_enabled = false;
        physics.step(30,
                     [&] { physics.get<world::RigidBody3D>("crate").linear_velocity.x += 0.1F; });
        CHECK(physics.position("crate").y < -0.5F);
        CHECK(std::abs(physics.position("crate").x) < 0.01F);
    }
    {
        // A motor on its free x axis drives it at its target velocity.
        Physics physics(weightless(false));
        const entt::entity crate = physics.crate("crate", {});
        auto& joint =
            physics.world.ecs().emplace<world::Generic6DOFJoint3D>(physics.add("drive", {}));
        joint.node_a = crate;
        joint.linear_x.limit_enabled = false;
        joint.linear_x.motor_enabled = true;
        joint.linear_x.motor_target_velocity = 1.5F;
        joint.linear_x.motor_force_limit = 100.0F;
        physics.step(30);
        CHECK(std::abs(physics.get<world::RigidBody3D>("crate").linear_velocity.x) ==
              doctest::Approx(1.5F).epsilon(0.02));
    }
}

TEST_CASE("jointed bodies pass through each other unless the joint lets them collide") {
    const auto run = [](bool exclude) {
        Physics physics(weightless(false));
        const entt::entity a = physics.crate("a", {0.0F, 0.0F, 0.0F});
        const entt::entity b = physics.crate("b", {0.5F, 0.0F, 0.0F});
        // A joint that only keeps them from turning.
        auto& joint =
            physics.world.ecs().emplace<world::Generic6DOFJoint3D>(physics.add("joint", {}));
        joint.node_a = a;
        joint.node_b = b;
        joint.exclude_nodes_from_collision = exclude;
        joint.linear_x.limit_enabled = false;
        joint.linear_y.limit_enabled = false;
        joint.linear_z.limit_enabled = false;
        physics.step(60);
        return distance(physics.position("a"), physics.position("b"));
    };
    CHECK(run(true) == doctest::Approx(0.5F).epsilon(0.01));
    CHECK(run(false) > 0.95F);
}

TEST_CASE("a joint is made again when its transform or bodies change, and goes with them") {
    Physics physics;
    const entt::entity first = physics.crate("first", {0.0F, 5.0F, 0.0F}, 0.5F);
    const entt::entity second = physics.crate("second", {3.0F, 5.0F, 0.0F}, 0.5F);
    // Pinned at its center, the first stays where it is while the second falls.
    const entt::entity pin = physics.add("pin", {0.0F, 5.0F, 0.0F});
    physics.world.ecs().emplace<world::PinJoint3D>(pin).node_a = first;
    physics.step(30);
    CHECK(physics.position("first").y == doctest::Approx(5.0F).epsilon(0.01));
    CHECK(physics.position("second").y < 4.5F);

    // Moved to the second and naming it, the pin holds the second where it is now.
    const float held = physics.position("second").y;
    physics.get<world::Transform3D>("pin").position = physics.position("second");
    physics.get<world::PinJoint3D>("pin").node_a = second;
    physics.step(30);
    CHECK(physics.position("second").y == doctest::Approx(held).epsilon(0.01));
    CHECK(physics.position("first").y < 4.5F);

    // Without the joint the second falls too.
    physics.world.ecs().remove<world::PinJoint3D>(pin);
    physics.step(30);
    CHECK(physics.position("second").y < held - 0.5F);
}

namespace {

// A car on a floor 400 meters across: a 40 kilogram body, as Godot's VehicleBody3D, with four
// wheels, the rear ones driven and the front ones steering.
struct Car {
    Physics physics;

    Car() {
        physics.block("ground", {-200.0F, -1.0F, -200.0F}, {200.0F, 0.0F, 200.0F});
        const entt::entity car = physics.add("car", {0.0F, 1.0F, 0.0F});
        physics.world.ecs().emplace<world::RigidBody3D>(car).mass = 40.0F;
        physics.world.ecs().emplace<world::VehicleBody3D>(car);
        physics.world.ecs().emplace<world::CollisionShape3D>(car, box_shape({2.0F, 0.5F, 4.0F}));
        for (const auto& [id, x, z] :
             {std::tuple{"front_left", 1.0F, 1.5F}, std::tuple{"front_right", -1.0F, 1.5F},
              std::tuple{"rear_left", 1.0F, -1.5F}, std::tuple{"rear_right", -1.0F, -1.5F}}) {
            const entt::entity wheel = physics.add(id, {x, -0.3F, z});
            auto& component = physics.world.ecs().emplace<world::VehicleWheel3D>(wheel);
            component.use_as_steering = z > 0.0F;
            component.use_as_traction = z < 0.0F;
            component.wheel_radius = 0.4F;
            component.wheel_rest_length = 0.3F;
            component.suspension_travel = 0.3F;
            component.suspension_stiffness = 30.0F;
            physics.get<world::Transform3D>("car").children.push_back(wheel);
        }
    }

    world::VehicleBody3D& controls() { return physics.get<world::VehicleBody3D>("car"); }
    world::VehicleWheel3D& wheel(std::string_view id) {
        return physics.get<world::VehicleWheel3D>(id);
    }
    Vec3 velocity() { return physics.get<world::RigidBody3D>("car").linear_velocity; }
};

} // namespace

TEST_CASE("a vehicle settles on its suspension, drives, steers and brakes, as Godot's") {
    Car car;
    car.physics.step(120);
    // It rests on its springs, which hold its weight.
    for (const std::string_view id : {"front_left", "front_right", "rear_left", "rear_right"}) {
        const world::VehicleWheel3D& wheel = car.wheel(id);
        CHECK(wheel.in_contact);
        CHECK(car.physics.world.id_of(wheel.contact_body) == "ground");
        CHECK(wheel.contact_normal.y == doctest::Approx(1.0F));
        CHECK(wheel.suspension_length > 0.0F);
        CHECK(wheel.suspension_length < 0.3F);
        CHECK(wheel.skid_info == doctest::Approx(1.0F));
        // Its Transform3D shows it lowered by its suspension from where it is mounted.
        CHECK(car.physics.position(id).y ==
              doctest::Approx(-0.3F - wheel.suspension_length).epsilon(0.001));
    }
    // Where the springs balance its weight, 0.08 meters short of their rest length, or near.
    const float resting = car.physics.position("car").y;
    CHECK(resting > 0.85F);
    CHECK(resting < 0.95F);
    CHECK(length(car.velocity()) < 0.05F);

    // The engine drives it toward +z, and the wheels roll.
    car.controls().engine_force = 50.0F;
    car.physics.step(120);
    CHECK(car.wheel("rear_left").engine_force == 50.0F);
    CHECK(car.wheel("front_left").engine_force == 0.0F);
    CHECK(car.physics.position("car").z > 2.0F);
    CHECK(car.velocity().z > 2.0F);
    CHECK(std::abs(car.physics.position("car").x) < 0.05F);
    CHECK(car.wheel("rear_left").rpm > 10.0F);
    CHECK(car.wheel("front_left").rotation > 1.0F);

    // Steering left turns it toward +x.
    car.controls().steering = 0.3F;
    car.physics.step(60);
    CHECK(car.wheel("front_left").steering == doctest::Approx(0.3F));
    CHECK(car.wheel("rear_left").steering == 0.0F);
    CHECK(car.physics.position("car").x > 0.3F);
    CHECK(car.physics.get<world::Transform3D>("car").rotation.y > 0.1F);
    // The wheel turns with its steering about its suspension, rolling about its axle.
    const Vec3 axle = world::affine(car.physics.get<world::Transform3D>("front_left")).x;
    CHECK(axle.x == doctest::Approx(std::cos(0.3F)).epsilon(0.001));
    CHECK(axle.y == doctest::Approx(0.0F).epsilon(0.001));
    CHECK(axle.z == doctest::Approx(-std::sin(0.3F)).epsilon(0.001));

    // Braking stops it.
    car.controls().engine_force = 0.0F;
    car.controls().steering = 0.0F;
    car.controls().brake = 2.0F;
    car.physics.step(120);
    CHECK(car.wheel("rear_right").brake == 2.0F);
    CHECK(length(car.velocity()) < 0.1F);
    CHECK(car.physics.position("car").y > 0.85F);
    CHECK(car.physics.position("car").y < 0.95F);
}

TEST_CASE("a vehicle asleep wakes when it is given throttle, and drives the same every time") {
    const auto run = [] {
        Car car;
        car.physics.step(240);
        car.controls().engine_force = 50.0F;
        car.controls().steering = -0.2F;
        car.physics.step(90);
        CHECK(car.velocity().z > 1.0F);
        CHECK(car.physics.position("car").x < -0.1F);
        std::vector<std::uint32_t> bits;
        for (const std::string_view id : {"car", "front_left", "rear_right"}) {
            const world::Transform3D& transform = car.physics.get<world::Transform3D>(id);
            for (const float f : {transform.position.x, transform.position.y, transform.position.z,
                                  transform.rotation.x, transform.rotation.y}) {
                bits.push_back(std::bit_cast<std::uint32_t>(f));
            }
        }
        return bits;
    };
    CHECK(run() == run());
}

TEST_CASE("a wheel moved by hand is mounted where it was put") {
    Car car;
    car.physics.step(60);
    // Raised by a meter, the front left wheel's ray no longer reaches the ground.
    car.physics.get<world::Transform3D>("front_left").position.y += 1.0F;
    car.physics.step(2);
    CHECK_FALSE(car.wheel("front_left").in_contact);
    CHECK(car.wheel("front_left").suspension_length == doctest::Approx(0.3F));
    CHECK(car.wheel("rear_left").in_contact);
}
