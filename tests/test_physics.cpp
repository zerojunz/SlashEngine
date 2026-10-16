#include "slash/core/json.hpp"
#include "slash/core/math.hpp"
#include "slash/physics2d/physics2d.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/tree2d.hpp"
#include "slash/world/world.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <functional>
#include <numbers>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace json = slash::json;
namespace physics2d = slash::physics2d;
namespace world = slash::world;
using slash::Vec2;

namespace {

constexpr double kTick = 1.0 / 60.0;

world::CollisionShape2D rectangle(Vec2 size) {
    world::CollisionShape2D shape;
    shape.size = size;
    return shape;
}

world::CollisionShape2D circle(float radius) {
    world::CollisionShape2D shape;
    shape.shape = "circle";
    shape.radius = radius;
    return shape;
}

// A world with 2D physics and the events it emits.
struct Physics {
    world::ComponentRegistry components;
    world::World world{components};
    std::vector<world::Event> events;

    explicit Physics(const physics2d::Settings& settings = {}) {
        world::register_builtin_components(components);
        physics2d::install(world, settings);
        world.set_event_listener([this](const world::Event& event) { events.push_back(event); });
    }

    entt::entity add(std::string_view id, Vec2 position) {
        const entt::entity entity = world.create(id).value();
        world.ecs().emplace<world::Transform2D>(entity).position = position;
        return entity;
    }

    template <typename Component>
    Component& get(std::string_view id) {
        return world.ecs().get<Component>(world.find(id));
    }

    Vec2 position(std::string_view id) { return get<world::Transform2D>(id).position; }

    void step(int ticks = 1, const std::function<void()>& before = {}) {
        for (int i = 0; i < ticks; ++i) {
            if (before) {
                before();
            }
            world.step(kTick);
        }
    }

    // A static box from left, top to right, bottom.
    entt::entity box(std::string_view id, Vec2 top_left, Vec2 bottom_right) {
        const Vec2 size = bottom_right - top_left;
        const entt::entity entity = add(id, top_left + (size * 0.5F));
        world.ecs().emplace<world::StaticBody2D>(entity);
        world.ecs().emplace<world::CollisionShape2D>(entity, rectangle(size));
        return entity;
    }

    entt::entity crate(std::string_view id, Vec2 position, float size = 20.0F) {
        const entt::entity entity = add(id, position);
        world.ecs().emplace<world::RigidBody2D>(entity);
        world.ecs().emplace<world::CollisionShape2D>(entity, rectangle({size, size}));
        return entity;
    }

    // A 20 by 40 character whose velocity gains gravity off the floor, as Godot's template
    // script does.
    entt::entity hero(std::string_view id, Vec2 position) {
        const entt::entity entity = add(id, position);
        world.ecs().emplace<world::CharacterBody2D>(entity);
        world.ecs().emplace<world::CollisionShape2D>(entity, rectangle({20.0F, 40.0F}));
        return entity;
    }

    void fall(std::string_view id) {
        auto& character = get<world::CharacterBody2D>(id);
        if (!character.on_floor) {
            character.velocity.y += 980.0F * static_cast<float>(kTick);
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

} // namespace

TEST_CASE("a rigid body falls and comes to rest on a static floor") {
    Physics physics;
    physics.box("floor", {-200.0F, 200.0F}, {200.0F, 220.0F});
    physics.crate("crate", {0.0F, 0.0F});

    physics.step();
    const auto& rigid = physics.get<world::RigidBody2D>("crate");
    CHECK(rigid.linear_velocity.y > 15.0F);
    CHECK(physics.position("crate").y > 0.0F);

    physics.step(180);
    CHECK(physics.position("crate").y == doctest::Approx(190.0F).epsilon(0.002));
    CHECK(std::abs(physics.position("crate").x) < 0.05F);
    CHECK(std::abs(rigid.linear_velocity.y) < 1.0F);
    CHECK(physics.get<world::RigidBody2D>("crate").sleeping);
}

TEST_CASE("physics settings set gravity and damping") {
    Physics physics(physics2d::Settings{.gravity = 0.0F, .gravity_vector = {0.0F, 1.0F}});
    physics.crate("crate", {0.0F, 0.0F});
    physics.step(30);
    CHECK(physics.position("crate").y == 0.0F);

    physics2d::set_settings(physics.world, {.gravity = 490.0F, .gravity_vector = {-1.0F, 0.0F}});
    physics.step();
    CHECK(physics.get<world::RigidBody2D>("crate").linear_velocity.x ==
          doctest::Approx(-490.0F / 60.0F).epsilon(0.01));

    const physics2d::Settings parsed = physics2d::parse_settings(
        json::parse(R"({"gravity": 100, "gravity_vector": [1, 0], "linear_damp": 0})").value());
    CHECK(parsed == physics2d::Settings{.gravity = 100.0F,
                                        .gravity_vector = {1.0F, 0.0F},
                                        .linear_damp = 0.0F,
                                        .angular_damp = 1.0F});
    CHECK(
        json::validate(json::parse(R"({"gravity": "down"})").value(), physics2d::settings_schema())
            .size() == 1);
}

TEST_CASE("bodies collide when either one's mask has the other's layer") {
    const auto lands = [](std::uint32_t crate_layer, std::uint32_t crate_mask,
                          std::uint32_t floor_layer, std::uint32_t floor_mask) {
        Physics physics;
        auto& floor = physics.world.ecs().get<world::StaticBody2D>(
            physics.box("floor", {-100.0F, 100.0F}, {100.0F, 120.0F}));
        floor.collision_layer = floor_layer;
        floor.collision_mask = floor_mask;
        auto& crate = physics.world.ecs().get<world::RigidBody2D>(physics.crate("crate", {}));
        crate.collision_layer = crate_layer;
        crate.collision_mask = crate_mask;
        physics.step(90);
        return physics.position("crate").y < 100.0F;
    };
    CHECK(lands(1, 1, 1, 1));
    CHECK(lands(2, 0, 1, 2)); // the floor's mask has the crate's layer
    CHECK(lands(2, 1, 1, 0)); // the crate's mask has the floor's layer
    CHECK_FALSE(lands(2, 0, 1, 0));
    CHECK_FALSE(lands(2, 4, 1, 1));
}

TEST_CASE("a one-way platform holds what lands on it and lets what rises pass") {
    Physics physics;
    const entt::entity platform = physics.box("platform", {-50.0F, 100.0F}, {50.0F, 110.0F});
    physics.get<world::CollisionShape2D>("platform").one_way_collision = true;
    (void)platform;
    physics.crate("crate", {0.0F, 150.0F}, 10.0F);
    physics.get<world::RigidBody2D>("crate").linear_velocity = {0.0F, -600.0F};

    // Up through the platform, over the top, then down onto it.
    physics.step(10);
    CHECK(physics.position("crate").y < 100.0F);
    physics.step(120);
    CHECK(physics.position("crate").y == doctest::Approx(95.0F).epsilon(0.002));

    // Turned upside down, the platform lets the crate fall through it.
    physics.get<world::Transform2D>("platform").rotation = std::numbers::pi_v<float>;
    physics.step(60);
    CHECK(physics.position("crate").y > 110.0F);
}

TEST_CASE("an area reports bodies and areas entering and leaving") {
    Physics physics;
    const entt::entity zone = physics.add("zone", {0.0F, 100.0F});
    physics.world.ecs().emplace<world::Area2D>(zone);
    physics.world.ecs().emplace<world::CollisionShape2D>(zone, rectangle({100.0F, 40.0F}));
    const entt::entity sensor = physics.add("sensor", {40.0F, 100.0F});
    physics.world.ecs().emplace<world::Area2D>(sensor);
    physics.world.ecs().emplace<world::CollisionShape2D>(sensor, circle(5.0F));
    physics.crate("crate", {0.0F, 0.0F}, 10.0F);

    physics.step();
    CHECK(physics.event_list() ==
          Ids{R"(area_entered sensor {"area":"zone"})", R"(area_entered zone {"area":"sensor"})"});
    physics.events.clear();

    // In and out as it falls; nothing collides with an area.
    std::vector<std::vector<std::string>> inside;
    physics.step(60, [&] {
        inside.push_back(physics.ids(physics2d::contacts(physics.world, zone).bodies));
    });
    CHECK(physics.event_list() ==
          Ids{R"(body_entered zone {"body":"crate"})", R"(body_exited zone {"body":"crate"})"});
    CHECK(std::find(inside.begin(), inside.end(), Ids{"crate"}) != inside.end());
    CHECK(physics.position("crate").y > 200.0F);

    // A body's contacts list the areas it is in.
    physics.get<world::Transform2D>("crate").position = {0.0F, 100.0F};
    physics.get<world::RigidBody2D>("crate").gravity_scale = 0.0F;
    physics.get<world::RigidBody2D>("crate").linear_velocity = {};
    physics.step();
    CHECK(physics.ids(physics2d::contacts(physics.world, physics.world.find("crate")).areas) ==
          Ids{"zone"});

    // An area that isn't monitorable is not seen by others; one not monitoring sees nothing.
    physics.events.clear();
    physics.get<world::Area2D>("sensor").monitorable = false;
    physics.step();
    CHECK(physics.event_list() == Ids{R"(area_exited zone {"area":"sensor"})"});
}

TEST_CASE("contact_monitor reports the bodies a rigid body touches") {
    Physics physics;
    physics.box("floor", {-100.0F, 100.0F}, {100.0F, 120.0F});
    physics.crate("crate", {0.0F, 60.0F});
    physics.get<world::RigidBody2D>("crate").contact_monitor = true;
    physics.step(60);
    CHECK(physics.event_list() == Ids{R"(body_entered crate {"body":"floor"})"});
    CHECK(physics.ids(physics2d::contacts(physics.world, physics.world.find("crate")).bodies) ==
          Ids{"floor"});

    physics.events.clear();
    physics.get<world::RigidBody2D>("crate").linear_velocity = {0.0F, -500.0F};
    physics.step(5);
    CHECK(physics.event_list() == Ids{R"(body_exited crate {"body":"floor"})"});
}

TEST_CASE("a character lands, walks, stops at walls and stays put on slopes") {
    Physics physics;
    physics.box("floor", {-300.0F, 100.0F}, {300.0F, 120.0F});
    physics.box("wall", {100.0F, 0.0F}, {120.0F, 100.0F});
    physics.hero("hero", {0.0F, 0.0F});

    physics.step(60, [&] { physics.fall("hero"); });
    const auto& hero = physics.get<world::CharacterBody2D>("hero");
    CHECK(hero.on_floor);
    CHECK_FALSE(hero.on_wall);
    CHECK(physics.position("hero").y == doctest::Approx(80.0F).epsilon(0.002));
    CHECK(hero.floor_normal == Vec2{0.0F, -1.0F});
    CHECK(hero.velocity == Vec2{});

    // Walk right into the wall.
    physics.step(60, [&] {
        physics.fall("hero");
        physics.get<world::CharacterBody2D>("hero").velocity.x = 300.0F;
    });
    CHECK(hero.on_floor);
    CHECK(hero.on_wall);
    CHECK(hero.wall_normal.x == doctest::Approx(-1.0F));
    CHECK(physics.position("hero").x == doctest::Approx(90.0F).epsilon(0.002));
    CHECK(physics.position("hero").y == doctest::Approx(80.0F).epsilon(0.002));
    CHECK(physics2d::contacts(physics.world, physics.world.find("hero")).bodies.size() == 2);

    // A jump leaves the floor.
    physics.get<world::CharacterBody2D>("hero").velocity = {0.0F, -400.0F};
    physics.step();
    CHECK_FALSE(hero.on_floor);
    CHECK(physics.position("hero").y < 80.0F);

    // A 30 degree slope: standing still stays still, walking climbs it.
    Physics slope;
    const entt::entity ramp = slope.add("ramp", {});
    slope.world.ecs().emplace<world::StaticBody2D>(ramp);
    slope.world.ecs().emplace<world::CollisionPolygon2D>(
        ramp, world::CollisionPolygon2D{.polygon = {{-200.0F, 200.0F},
                                                    {200.0F, 200.0F - 230.94F},
                                                    {200.0F, 300.0F},
                                                    {-200.0F, 300.0F}}});
    slope.hero("hero", {0.0F, 60.0F});
    slope.step(90, [&] { slope.fall("hero"); });
    const auto& climber = slope.get<world::CharacterBody2D>("hero");
    CHECK(climber.on_floor);
    const Vec2 rest = slope.position("hero");
    slope.step(60, [&] { slope.fall("hero"); });
    CHECK(slope.position("hero").x == doctest::Approx(rest.x).epsilon(0.0001));
    CHECK(slope.position("hero").y == doctest::Approx(rest.y).epsilon(0.0001));
    slope.step(30, [&] {
        slope.fall("hero");
        slope.get<world::CharacterBody2D>("hero").velocity.x = 100.0F;
    });
    CHECK(climber.on_floor);
    // Godot's floor_constant_speed is off, so the slope slows it: 50 pixels becomes 37.
    CHECK(slope.position("hero").x > rest.x + 35.0F);
    CHECK(slope.position("hero").y < rest.y - 20.0F);
}

TEST_CASE("a character jumps up through a one-way platform and lands on it") {
    Physics physics;
    physics.box("floor", {-300.0F, 200.0F}, {300.0F, 220.0F});
    physics.box("ledge", {-50.0F, 100.0F}, {50.0F, 110.0F});
    physics.get<world::CollisionShape2D>("ledge").one_way_collision = true;
    // A capsule this time.
    physics.hero("hero", {0.0F, 180.0F});
    auto& shape = physics.get<world::CollisionShape2D>("hero");
    shape.shape = "capsule";
    shape.radius = 10.0F;
    shape.height = 40.0F;
    physics.step(30, [&] { physics.fall("hero"); });
    REQUIRE(physics.get<world::CharacterBody2D>("hero").on_floor);
    CHECK(physics.position("hero").y == doctest::Approx(180.0F).epsilon(0.002));

    physics.get<world::CharacterBody2D>("hero").velocity.y = -600.0F;
    physics.step(90, [&] { physics.fall("hero"); });
    const auto& hero = physics.get<world::CharacterBody2D>("hero");
    CHECK(hero.on_floor);
    CHECK(physics.position("hero").y == doctest::Approx(80.0F).epsilon(0.002));
    CHECK(physics.ids(physics2d::contacts(physics.world, physics.world.find("hero")).bodies) ==
          Ids{"ledge"});

    // Under a solid ceiling a jump stops at it.
    physics.get<world::CollisionShape2D>("ledge").one_way_collision = false;
    physics.get<world::Transform2D>("hero").position = {0.0F, 180.0F};
    physics.get<world::CharacterBody2D>("hero").velocity = {};
    physics.step(30, [&] { physics.fall("hero"); });
    physics.get<world::CharacterBody2D>("hero").velocity.y = -600.0F;
    bool bumped = false;
    physics.step(30, [&] {
        physics.fall("hero");
        bumped = bumped || physics.get<world::CharacterBody2D>("hero").on_ceiling;
    });
    CHECK(bumped);
    CHECK(physics.position("hero").y == doctest::Approx(180.0F).epsilon(0.002));
}

TEST_CASE("a floating character slides along walls and has no floor") {
    Physics physics;
    physics.box("wall", {100.0F, -200.0F}, {120.0F, 200.0F});
    const entt::entity ship = physics.add("ship", {0.0F, 0.0F});
    physics.world.ecs().emplace<world::CharacterBody2D>(ship, world::CharacterBody2D{});
    physics.get<world::CharacterBody2D>("ship").motion_mode = "floating";
    physics.get<world::CharacterBody2D>("ship").velocity = {300.0F, 120.0F};
    physics.world.ecs().emplace<world::CollisionShape2D>(ship, circle(10.0F));
    physics.step(30);
    const auto& character = physics.get<world::CharacterBody2D>("ship");
    CHECK(physics.position("ship").x == doctest::Approx(90.0F).epsilon(0.002));
    // 18 ticks to reach the wall, then the rest along it at full speed.
    CHECK(physics.position("ship").y == doctest::Approx(100.6F).epsilon(0.01));
    CHECK(character.on_wall);
    CHECK_FALSE(character.on_floor);
    CHECK(character.velocity == Vec2{300.0F, 120.0F}); // floating keeps its velocity

    // Head-on, it stops rather than slides.
    physics.get<world::CharacterBody2D>("ship").velocity = {300.0F, 10.0F};
    const float y = physics.position("ship").y;
    physics.step(10);
    CHECK(physics.position("ship").y == doctest::Approx(y));
}

TEST_CASE("a character rides a moving platform and keeps its velocity when it leaves") {
    Physics physics;
    const entt::entity lift = physics.add("lift", {0.0F, 100.0F});
    physics.world.ecs().emplace<world::AnimatableBody2D>(lift);
    physics.world.ecs().emplace<world::CollisionShape2D>(lift, rectangle({200.0F, 20.0F}));
    physics.hero("hero", {0.0F, 60.0F});
    physics.step(30, [&] { physics.fall("hero"); });
    REQUIRE(physics.get<world::CharacterBody2D>("hero").on_floor);
    const Vec2 start = physics.position("hero");

    // The lift moves right 2 pixels a tick, and the hero with it.
    physics.step(30, [&] {
        physics.fall("hero");
        physics.get<world::Transform2D>("lift").position.x += 2.0F;
    });
    const auto& hero = physics.get<world::CharacterBody2D>("hero");
    CHECK(hero.on_floor);
    CHECK(physics.position("hero").x == doctest::Approx(start.x + 60.0F).epsilon(0.001));
    CHECK(hero.platform_velocity.x == doctest::Approx(120.0F).epsilon(0.01));

    // Jumping off keeps the lift's velocity, as Godot's platform_on_leave add_velocity.
    physics.get<world::CharacterBody2D>("hero").velocity = {0.0F, -300.0F};
    physics.step(1, [&] { physics.get<world::Transform2D>("lift").position.x += 2.0F; });
    CHECK_FALSE(hero.on_floor);
    CHECK(hero.velocity.x == doctest::Approx(120.0F).epsilon(0.01));
}

TEST_CASE("concave polygons are split, and polygon segments are only edges") {
    Physics physics;
    // A U: two arms and a base, with a notch between the arms.
    const entt::entity cup = physics.add("cup", {});
    physics.world.ecs().emplace<world::StaticBody2D>(cup);
    physics.world.ecs().emplace<world::CollisionPolygon2D>(
        cup, world::CollisionPolygon2D{.polygon = {{0.0F, 0.0F},
                                                   {20.0F, 0.0F},
                                                   {20.0F, 80.0F},
                                                   {80.0F, 80.0F},
                                                   {80.0F, 0.0F},
                                                   {100.0F, 0.0F},
                                                   {100.0F, 100.0F},
                                                   {0.0F, 100.0F}}});
    CHECK(physics.ids(physics2d::overlap_point(physics.world, {10.0F, 10.0F})) == Ids{"cup"});
    CHECK(physics.ids(physics2d::overlap_point(physics.world, {50.0F, 90.0F})) == Ids{"cup"});
    CHECK(physics2d::overlap_point(physics.world, {50.0F, 40.0F}).empty());

    // A crate dropped into the notch rests on the base.
    physics.crate("crate", {50.0F, -50.0F});
    physics.step(120);
    CHECK(physics.position("crate").y == doctest::Approx(70.0F).epsilon(0.002));

    // As segments, the outline is hollow: a point inside the base hits nothing.
    physics.get<world::CollisionPolygon2D>("cup").build_mode = "segments";
    CHECK(physics2d::overlap_point(physics.world, {50.0F, 90.0F}).empty());
    const auto hit = physics2d::raycast(physics.world, {50.0F, 200.0F}, {50.0F, 50.0F});
    REQUIRE(hit);
    CHECK(hit->point.y == doctest::Approx(100.0F));
}

TEST_CASE("raycast finds the first hit and follows masks, areas and exclusions") {
    Physics physics;
    const entt::entity near = physics.box("near", {100.0F, -50.0F}, {120.0F, 50.0F});
    physics.box("far", {200.0F, -50.0F}, {220.0F, 50.0F});
    physics.get<world::StaticBody2D>("far").collision_layer = 2;
    const entt::entity zone = physics.add("zone", {50.0F, 0.0F});
    physics.world.ecs().emplace<world::Area2D>(zone);
    physics.world.ecs().emplace<world::CollisionShape2D>(zone, circle(10.0F));

    const auto hit = physics2d::raycast(physics.world, {0.0F, 0.0F}, {300.0F, 0.0F});
    REQUIRE(hit);
    CHECK(physics.world.id_of(hit->entity) == "near");
    CHECK(hit->point.x == doctest::Approx(100.0F));
    CHECK(hit->normal == Vec2{-1.0F, 0.0F});
    CHECK(hit->fraction == doctest::Approx(1.0F / 3.0F));

    const auto areas = physics2d::raycast(physics.world, {0.0F, 0.0F}, {300.0F, 0.0F},
                                          {.collide_with_areas = true});
    REQUIRE(areas);
    CHECK(physics.world.id_of(areas->entity) == "zone");

    const auto past =
        physics2d::raycast(physics.world, {0.0F, 0.0F}, {300.0F, 0.0F}, {.exclude = {near}});
    REQUIRE(past);
    CHECK(physics.world.id_of(past->entity) == "far");
    CHECK_FALSE(
        physics2d::raycast(physics.world, {0.0F, 0.0F}, {300.0F, 0.0F}, {.collision_mask = 4}));
    // A ray that starts inside a shape doesn't hit it.
    const auto inside = physics2d::raycast(physics.world, {110.0F, 0.0F}, {300.0F, 0.0F});
    REQUIRE(inside);
    CHECK(physics.world.id_of(inside->entity) == "far");
}

TEST_CASE("overlap_shape finds what a shape would touch") {
    Physics physics;
    physics.box("a", {0.0F, 0.0F}, {20.0F, 20.0F});
    physics.box("b", {40.0F, 0.0F}, {60.0F, 20.0F});
    physics.box("c", {200.0F, 0.0F}, {220.0F, 20.0F});
    const world::CollisionShape2D wide = rectangle({60.0F, 10.0F});
    CHECK(physics.ids(physics2d::overlap_shape(physics.world, wide, {30.0F, 10.0F}, 0.0F)) ==
          Ids{"a", "b"});
    CHECK(physics.ids(physics2d::overlap_shape(physics.world, wide, {30.0F, 10.0F}, 0.0F, {}, 1)) ==
          Ids{"a"});
    // Turned upright it fits between them.
    CHECK(physics2d::overlap_shape(physics.world, wide, {30.0F, 10.0F},
                                   std::numbers::pi_v<float> / 2.0F)
              .empty());
    const world::CollisionShape2D ball = circle(15.0F);
    CHECK(physics.ids(physics2d::overlap_shape(physics.world, ball, {210.0F, 30.0F}, 0.0F)) ==
          Ids{"c"});
    world::CollisionShape2D star;
    star.shape = "polygon";
    star.points = {{0.0F, -30.0F}, {8.0F, -8.0F},  {30.0F, 0.0F},  {8.0F, 8.0F},  {0.0F, 30.0F},
                   {-8.0F, 8.0F},  {-30.0F, 0.0F}, {-8.0F, -8.0F}, {0.0F, -25.0F}};
    CHECK(physics.ids(physics2d::overlap_shape(physics.world, star, {30.0F, 10.0F}, 0.0F)) ==
          Ids{"a", "b"});
}

TEST_CASE("joints pin, spring and slide bodies") {
    Physics physics;
    // A pendulum pinned to the world at (0, 0), its bob at (100, 0).
    physics.crate("bob", {100.0F, 0.0F}, 10.0F);
    const entt::entity pin = physics.add("pin", {0.0F, 0.0F});
    physics.world.ecs().emplace<world::PinJoint2D>(
        pin, world::PinJoint2D{.node_b = physics.world.find("bob")});
    // A weight on a spring hanging from (300, 0), resting 50 pixels down at first.
    physics.crate("weight", {300.0F, 50.0F}, 10.0F);
    const entt::entity spring = physics.add("spring", {300.0F, 0.0F});
    physics.world.ecs().emplace<world::DampedSpringJoint2D>(
        spring, world::DampedSpringJoint2D{
                    .node_b = physics.world.find("weight"), .stiffness = 200.0F, .damping = 5.0F});
    // A bead on a groove from (500, 0) 100 pixels down, tilted 45 degrees, 25 pixels along it.
    physics.crate("bead", {517.678F, 17.678F}, 10.0F);
    const entt::entity groove = physics.add("groove", {500.0F, 0.0F});
    physics.get<world::Transform2D>("groove").rotation = -std::numbers::pi_v<float> / 4.0F;
    physics.world.ecs().emplace<world::GrooveJoint2D>(
        groove, world::GrooveJoint2D{.node_b = physics.world.find("bead"), .length = 100.0F});

    physics.step(120);
    const auto distance = [](Vec2 a, Vec2 b) { return std::hypot(a.x - b.x, a.y - b.y); };
    CHECK(distance(physics.position("bob"), {0.0F, 0.0F}) == doctest::Approx(100.0F).epsilon(0.01));
    CHECK(physics.position("bob").y > 50.0F); // it swung down
    // Stretched below 50 by its weight: 980 / 200 = 4.9 pixels.
    CHECK(physics.position("weight").x == doctest::Approx(300.0F).epsilon(0.001));
    CHECK(physics.position("weight").y == doctest::Approx(54.9F).epsilon(0.02));
    // At the groove's low end, (500 + 70.7, 70.7).
    CHECK(physics.position("bead").x == doctest::Approx(570.7F).epsilon(0.002));
    CHECK(physics.position("bead").y == doctest::Approx(70.7F).epsilon(0.002));

    // Removing the pin lets the bob fall.
    physics.world.destroy(pin);
    physics.step(60);
    CHECK(physics.position("bob").y > 150.0F);
}

TEST_CASE("a joint stays where it was made when a parent carries it") {
    Physics physics;
    // A pendulum whose pin is the bob's own child, 100 pixels left of it at (0, 0), as Godot
    // scenes often keep a joint: the bob swinging away doesn't take the pin along.
    const entt::entity bob = physics.crate("bob", {100.0F, 0.0F}, 10.0F);
    const entt::entity pin = physics.add("pin", {-100.0F, 0.0F});
    physics.get<world::Transform2D>("bob").children.push_back(pin);
    physics.world.ecs().emplace<world::PinJoint2D>(pin, world::PinJoint2D{.node_b = bob});
    physics.step(120);
    const auto distance = [](Vec2 a, Vec2 b) { return std::hypot(a.x - b.x, a.y - b.y); };
    CHECK(distance(physics.position("bob"), {0.0F, 0.0F}) == doctest::Approx(100.0F).epsilon(0.01));
    CHECK(physics.position("bob").y > 50.0F);

    // Moving the pin itself makes the joint again where it now is: 50 pixels from the bob.
    physics.get<world::Transform2D>("pin").position = {-50.0F, 0.0F};
    const Vec2 pivot = world::Tree2D(physics.world).global(pin).position;
    physics.step(120);
    CHECK(distance(physics.position("bob"), pivot) == doctest::Approx(50.0F).epsilon(0.02));

    // A joint's lengths are in its own space, as Godot's: scaled twice, a 50 pixel groove from
    // (500, 0) reaches 100 pixels down, and the bead starts 25 * 2 along it.
    physics.crate("bead", {500.0F, 50.0F}, 10.0F);
    const entt::entity groove = physics.add("groove", {500.0F, 0.0F});
    physics.get<world::Transform2D>("groove").scale = {2.0F, 2.0F};
    physics.world.ecs().emplace<world::GrooveJoint2D>(
        groove, world::GrooveJoint2D{.node_b = physics.world.find("bead")});
    physics.step(120);
    CHECK(physics.position("bead").x == doctest::Approx(500.0F).epsilon(0.002));
    CHECK(physics.position("bead").y == doctest::Approx(100.0F).epsilon(0.002));
}

TEST_CASE("a pin's motor turns its body up to the limit and no further") {
    Physics physics(physics2d::Settings{.gravity = 0.0F,
                                        .gravity_vector = {0.0F, 1.0F},
                                        .linear_damp = 0.0F,
                                        .angular_damp = 0.0F});
    const entt::entity door = physics.crate("door", {0.0F, 0.0F}, 20.0F);
    const entt::entity hinge = physics.add("hinge", {0.0F, 0.0F});
    physics.world.ecs().emplace<world::PinJoint2D>(
        hinge, world::PinJoint2D{.node_b = door,
                                 .angular_limit_enabled = true,
                                 .angular_limit_lower = -0.5F,
                                 .angular_limit_upper = 1.0F,
                                 .motor_enabled = true,
                                 .motor_target_velocity = 2.0F});
    physics.step(30);
    CHECK(physics.get<world::Transform2D>("door").rotation == doctest::Approx(1.0F).epsilon(0.05));
    physics.step(120);
    CHECK(physics.get<world::Transform2D>("door").rotation == doctest::Approx(1.0F).epsilon(0.05));
}

TEST_CASE("impulses, forces, freezing and removing bodies") {
    Physics physics(physics2d::Settings{.gravity = 0.0F,
                                        .gravity_vector = {0.0F, 1.0F},
                                        .linear_damp = 0.0F,
                                        .angular_damp = 0.0F});
    const entt::entity crate = physics.crate("crate", {});
    physics.get<world::RigidBody2D>("crate").mass = 2.0F;
    physics2d::apply_impulse(physics.world, crate, {120.0F, 0.0F});
    physics.step();
    CHECK(physics.get<world::RigidBody2D>("crate").linear_velocity.x == doctest::Approx(60.0F));
    physics2d::apply_torque_impulse(physics.world, crate, 1000.0F);
    physics.step();
    CHECK(physics.get<world::RigidBody2D>("crate").angular_velocity > 0.0F);

    physics.get<world::RigidBody2D>("crate").freeze = true;
    physics.get<world::RigidBody2D>("crate").linear_velocity = {};
    physics.get<world::RigidBody2D>("crate").angular_velocity = 0.0F;
    const Vec2 frozen = physics.position("crate");
    physics2d::apply_impulse(physics.world, crate, {120.0F, 0.0F});
    physics.step(10);
    CHECK(physics.position("crate") == frozen);

    physics.get<world::RigidBody2D>("crate").freeze = false;
    physics.get<world::RigidBody2D>("crate").constant_force = {0.0F, 200.0F};
    physics.step(60);
    CHECK(physics.get<world::RigidBody2D>("crate").linear_velocity.y ==
          doctest::Approx(100.0F).epsilon(0.01));

    // Without its body component it stops moving and nothing finds it.
    physics.world.ecs().remove<world::RigidBody2D>(crate);
    const Vec2 left = physics.position("crate");
    physics.step(10);
    CHECK(physics.position("crate") == left);
    CHECK(physics2d::overlap_point(physics.world, left).empty());
}

TEST_CASE("the same scene simulates the same every time") {
    const auto run = [] {
        Physics physics;
        physics.box("floor", {-300.0F, 300.0F}, {300.0F, 320.0F});
        for (int i = 0; i < 12; ++i) {
            const std::string id = "crate" + std::to_string(i);
            physics.crate(
                id, {static_cast<float>((i % 4) * 25) - 40.0F, static_cast<float>(i / 4) * -30.0F});
            physics.get<world::Transform2D>(id).rotation = 0.1F * static_cast<float>(i);
        }
        physics.hero("hero", {150.0F, 200.0F});
        physics.step(240, [&] {
            physics.fall("hero");
            physics.get<world::CharacterBody2D>("hero").velocity.x = -120.0F;
        });
        std::string out;
        for (const std::string_view id : physics.world.ids()) {
            const auto& transform = physics.get<world::Transform2D>(id);
            out += std::string(id) + " " + std::to_string(transform.position.x) + " " +
                   std::to_string(transform.position.y) + " " + std::to_string(transform.rotation) +
                   "\n";
        }
        return out;
    };
    CHECK(run() == run());
}

TEST_CASE("a body takes the shapes of child entities with no body of their own, as Godot's") {
    Physics physics;
    // A wall with no shape of its own, built of two bricks: one beside it, one turned and
    // doubled further along.
    const entt::entity wall = physics.add("wall", {100.0F, 100.0F});
    physics.world.ecs().emplace<world::StaticBody2D>(wall);
    const entt::entity near = physics.add("near", {0.0F, 0.0F});
    physics.world.ecs().emplace<world::CollisionShape2D>(near, rectangle({20.0F, 20.0F}));
    const entt::entity far = physics.add("far", {60.0F, 0.0F});
    physics.get<world::Transform2D>("far").rotation = std::numbers::pi_v<float> / 4.0F;
    physics.get<world::Transform2D>("far").scale = {2.0F, 2.0F};
    physics.world.ecs().emplace<world::CollisionShape2D>(far, rectangle({10.0F, 10.0F}));
    physics.get<world::Transform2D>("wall").children = {near, far};
    physics.step();

    // Hits name the body. The far brick is a diamond 20 * sqrt(2) pixels across its corners.
    auto hit = physics2d::raycast(physics.world, {100.0F, 0.0F}, {100.0F, 200.0F});
    REQUIRE(hit.has_value());
    CHECK(hit->entity == wall);
    CHECK(hit->point.y == doctest::Approx(90.0F));
    hit = physics2d::raycast(physics.world, {160.0F, 0.0F}, {160.0F, 200.0F});
    REQUIRE(hit.has_value());
    CHECK(hit->entity == wall);
    CHECK(hit->point.y == doctest::Approx(100.0F - (10.0F * std::numbers::sqrt2_v<float>)));
    CHECK_FALSE(physics2d::raycast(physics.world, {130.0F, 0.0F}, {130.0F, 200.0F}).has_value());

    // Moving a brick moves its shape.
    physics.get<world::Transform2D>("near").position = {-40.0F, 0.0F};
    physics.step();
    CHECK_FALSE(physics2d::raycast(physics.world, {100.0F, 0.0F}, {100.0F, 200.0F}).has_value());
    hit = physics2d::raycast(physics.world, {60.0F, 0.0F}, {60.0F, 200.0F});
    REQUIRE(hit.has_value());
    CHECK(hit->entity == wall);

    // A child that is a body keeps its shapes.
    physics.world.ecs().emplace<world::StaticBody2D>(near);
    physics.step();
    hit = physics2d::raycast(physics.world, {60.0F, 0.0F}, {60.0F, 200.0F});
    REQUIRE(hit.has_value());
    CHECK(hit->entity == near);
}

TEST_CASE("rigid bodies, characters and areas use their children's shapes") {
    Physics physics;
    // A floor whose shape is a child's, and a crate whose shape is a child's, 10 pixels round.
    const entt::entity floor = physics.add("floor", {0.0F, 200.0F});
    physics.world.ecs().emplace<world::StaticBody2D>(floor);
    const entt::entity slab = physics.add("slab", {});
    physics.world.ecs().emplace<world::CollisionShape2D>(slab, rectangle({400.0F, 20.0F}));
    physics.get<world::Transform2D>("floor").children = {slab};
    const entt::entity ball = physics.add("ball", {-100.0F, 100.0F});
    physics.world.ecs().emplace<world::RigidBody2D>(ball);
    const entt::entity round = physics.add("round", {0.0F, 0.0F});
    physics.world.ecs().emplace<world::CollisionShape2D>(round, circle(10.0F));
    physics.get<world::Transform2D>("ball").children = {round};
    physics.step(120);
    CHECK(physics.position("ball").y == doctest::Approx(180.0F).epsilon(0.002));

    // A character stands on it on its child's shape.
    const entt::entity hero = physics.add("hero", {100.0F, 100.0F});
    physics.world.ecs().emplace<world::CharacterBody2D>(hero);
    const entt::entity feet = physics.add("feet", {0.0F, 10.0F});
    physics.world.ecs().emplace<world::CollisionShape2D>(feet, rectangle({20.0F, 20.0F}));
    physics.get<world::Transform2D>("hero").children = {feet};
    physics.step(60, [&] { physics.fall("hero"); });
    CHECK(physics.get<world::CharacterBody2D>("hero").on_floor);
    CHECK(physics.position("hero").y == doctest::Approx(170.0F).epsilon(0.002));

    // An area's child shape reports what enters it.
    physics.events.clear();
    const entt::entity zone = physics.add("zone", {-100.0F, 0.0F});
    physics.world.ecs().emplace<world::Area2D>(zone);
    const entt::entity field = physics.add("field", {0.0F, 175.0F});
    physics.world.ecs().emplace<world::CollisionShape2D>(field, rectangle({20.0F, 20.0F}));
    physics.get<world::Transform2D>("zone").children = {field};
    physics.step();
    CHECK(physics.event_list() == Ids{R"(body_entered zone {"body":"ball"})"});
    CHECK(physics.ids(physics2d::contacts(physics.world, ball).areas) == Ids{"zone"});
}
