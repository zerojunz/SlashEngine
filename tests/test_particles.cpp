#include "slash/canvas/particles.hpp"
#include "slash/core/json.hpp"
#include "slash/core/math.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/world.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <cstddef>
#include <limits>
#include <numbers>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace canvas = slash::canvas;
namespace json = slash::json;
namespace world = slash::world;
using slash::Vec2;
using slash::Vec4;

namespace {

constexpr double kTick = 1.0 / 60.0;

json::Value parse(std::string_view text) {
    auto value = json::parse(text);
    REQUIRE(value.ok());
    return std::move(value).value();
}

// A world with the particles system and the events it emits.
struct Particles {
    world::ComponentRegistry components;
    world::World world{components};
    std::vector<world::Event> events;

    Particles() {
        world::register_builtin_components(components);
        canvas::add_particles_system(world);
        world.set_event_listener([this](const world::Event& event) { events.push_back(event); });
    }

    // An entity at a position with a CPUParticles2D whose fields the JSON sets.
    entt::entity add(std::string_view id, Vec2 position, std::string_view fields) {
        const entt::entity entity = world.create(id).value();
        world.ecs().emplace<world::Transform2D>(entity, world::Transform2D{.position = position});
        std::string path;
        REQUIRE(components.find("CPUParticles2D")->assign(world, entity, parse(fields), path).ok());
        return entity;
    }

    void step(int ticks = 1) {
        for (int i = 0; i < ticks; ++i) {
            world.step(kTick);
        }
    }

    std::vector<canvas::Particle> live(entt::entity entity) const {
        return canvas::particles(world, entity);
    }

    world::CPUParticles2D& emitter(entt::entity entity) {
        return world.ecs().get<world::CPUParticles2D>(entity);
    }

    std::size_t count(std::string_view kind) const {
        std::size_t n = 0;
        for (const world::Event& event : events) {
            if (event.kind == kind) {
                ++n;
            }
        }
        return n;
    }
};

float distance(Vec2 a, Vec2 b) {
    return std::sqrt(((a.x - b.x) * (a.x - b.x)) + ((a.y - b.y) * (a.y - b.y)));
}

// A sparkler: particles straight along x at 100 pixels a second, with no gravity.
constexpr std::string_view kSparkler =
    R"({"amount": 10, "lifetime": 1, "spread": 0, "gravity": [0, 0],
        "initial_velocity_min": 100, "initial_velocity_max": 100})";

} // namespace

TEST_CASE("particles emit evenly over their lifetime, move and die at its end") {
    Particles scene;
    const entt::entity entity = scene.add("sparkler", {100.0F, 0.0F}, kSparkler);
    scene.step(45);
    // Slots emit a tenth of a second apart, each moved on from the moment it was due.
    const std::vector<canvas::Particle> live = scene.live(entity);
    REQUIRE(live.size() == 8);
    for (std::size_t i = 0; i < live.size(); ++i) {
        const canvas::Particle& p = live[i];
        CAPTURE(i);
        CHECK(p.index == i);
        CHECK(p.age == doctest::Approx(0.75 - (0.1 * static_cast<double>(i))).epsilon(1e-4));
        CHECK(p.position.x == doctest::Approx(100.0F + (100.0F * p.age)).epsilon(1e-4));
        CHECK(p.position.y == 0.0F);
        CHECK(p.velocity.x == doctest::Approx(100.0F));
        CHECK(p.scale == 1.0F);
        CHECK(p.color == Vec4{1.0F, 1.0F, 1.0F, 1.0F});
    }

    // Once a cycle has passed, each dies as its slot emits again.
    scene.step(48);
    REQUIRE(scene.live(entity).size() == 10);
    for (const canvas::Particle& p : scene.live(entity)) {
        CHECK(p.age < 1.0F);
        CHECK(p.position.x < 200.0F);
    }

    // The "lifetime" draw order puts the oldest first.
    scene.emitter(entity).draw_order = "lifetime";
    const std::vector<canvas::Particle> oldest_first = scene.live(entity);
    for (std::size_t i = 1; i < oldest_first.size(); ++i) {
        CHECK(oldest_first[i - 1].age >= oldest_first[i].age);
    }

    // Not emitting, the last particles live out their lives.
    scene.emitter(entity).emitting = false;
    scene.step(30);
    CHECK(!scene.live(entity).empty());
    scene.step(30);
    CHECK(scene.live(entity).empty());
    // Only a one_shot reports finished.
    CHECK(scene.count("finished") == 0);
}

TEST_CASE("simulate runs settings apart from a world and gives what the system would") {
    Particles scene;
    const std::string fields =
        R"({"amount": 24, "lifetime": 0.8, "spread": 60, "initial_velocity_min": 40,
            "initial_velocity_max": 90, "randomness": 0.5, "lifetime_randomness": 0.3,
            "angular_velocity_min": -90, "angular_velocity_max": 90, "damping_min": 5,
            "damping_max": 10, "radial_accel_min": 20, "radial_accel_max": 30,
            "tangential_accel_min": -10, "tangential_accel_max": 10, "scale_amount_min": 0.5,
            "scale_amount_max": 2, "hue_variation_min": -0.1, "hue_variation_max": 0.1,
            "emission_shape": "sphere", "emission_sphere_radius": 6, "preprocess": 0.45})";
    const entt::entity entity = scene.add("fountain", {50.0F, 80.0F}, fields);
    scene.step(37);
    const std::vector<canvas::Particle> stepped = scene.live(entity);
    const std::vector<canvas::Particle> simulated =
        canvas::simulate(scene.emitter(entity), scene.world.ecs().get<world::Transform2D>(entity),
                         canvas::particle_seed("fountain", 0), 37 * kTick, kTick);
    REQUIRE(!stepped.empty());
    REQUIRE(stepped.size() == simulated.size());
    for (std::size_t i = 0; i < stepped.size(); ++i) {
        CHECK(stepped[i].index == simulated[i].index);
        CHECK(stepped[i].position == simulated[i].position);
        CHECK(stepped[i].color == simulated[i].color);
        CHECK(stepped[i].rotation == simulated[i].rotation);
        // lifetime_randomness shortens lives by up to 30%.
        CHECK(stepped[i].lifetime <= 0.8F);
        CHECK(stepped[i].lifetime >= 0.8F * 0.7F);
    }

    // Preprocess runs the emitter on before the first tick, and time 0 gives what it made.
    world::CPUParticles2D sparkler = scene.emitter(entity);
    sparkler.amount = 10;
    sparkler.lifetime = 1.0F;
    sparkler.randomness = 0.0F;
    sparkler.lifetime_randomness = 0.0F;
    CHECK(canvas::simulate(sparkler, {}, 1, 0.0, kTick).size() == 5);
    sparkler.preprocess = 0.0F;
    CHECK(canvas::simulate(sparkler, {}, 1, 0.0, kTick).empty());
    CHECK(canvas::simulate(sparkler, {}, 1, 1.0, 0.0).empty());
}

TEST_CASE("a one_shot emits one cycle, stops emitting, says it finished and restarts") {
    Particles scene;
    const entt::entity entity = scene.add(
        "burst", {0.0F, 0.0F},
        R"({"amount": 5, "lifetime": 0.5, "one_shot": true, "explosiveness": 1, "spread": 180,
            "initial_velocity_min": 50, "initial_velocity_max": 100, "gravity": [0, 0]})");
    scene.step();
    // Explosiveness 1 emits them all at once, in every direction.
    std::vector<canvas::Particle> live = scene.live(entity);
    REQUIRE(live.size() == 5);
    std::set<float> directions;
    for (const canvas::Particle& p : live) {
        CHECK(p.age == doctest::Approx(kTick));
        const float speed = distance(p.velocity, {});
        CHECK(speed >= 50.0F - 1e-3F);
        CHECK(speed <= 100.0F + 1e-3F);
        directions.insert(std::atan2(p.velocity.y, p.velocity.x));
    }
    CHECK(directions.size() == 5);
    const std::vector<canvas::Particle> first = live;

    scene.step(20);
    CHECK(scene.live(entity).size() == 5);
    CHECK(scene.emitter(entity).emitting);
    CHECK(scene.count("finished") == 0);
    scene.step(20);
    CHECK(scene.live(entity).empty());
    CHECK(!scene.emitter(entity).emitting);
    REQUIRE(scene.count("finished") == 1);
    CHECK(scene.events.back().id == "burst");
    scene.step(60);
    CHECK(scene.live(entity).empty());
    CHECK(scene.count("finished") == 1);

    // Emitting again starts a new cycle, with new random values.
    scene.emitter(entity).emitting = true;
    scene.step();
    live = scene.live(entity);
    REQUIRE(live.size() == 5);
    CHECK(live[0].velocity != first[0].velocity);
    scene.step(40);
    CHECK(scene.count("finished") == 2);
}

TEST_CASE("particles repeat for the same entity and seed, and differ for others") {
    const std::string fields =
        R"({"amount": 16, "spread": 180, "initial_velocity_min": 10, "initial_velocity_max": 80,
            "randomness": 1, "emission_shape": "rectangle", "emission_rect_extents": [20, 10]})";
    const auto run = [&](std::string_view id, int seed) {
        Particles scene;
        const entt::entity entity = scene.add(id, {}, fields);
        scene.emitter(entity).seed = seed;
        scene.step(50);
        std::vector<Vec2> positions;
        for (const canvas::Particle& p : scene.live(entity)) {
            positions.push_back(p.position);
        }
        return positions;
    };
    const std::vector<Vec2> a = run("a", 0);
    CHECK(!a.empty());
    CHECK(run("a", 0) == a);
    CHECK(run("b", 0) != a);
    CHECK(run("a", 1) != a);
    CHECK(canvas::particle_seed("a", 0) != canvas::particle_seed("b", 0));
    CHECK(canvas::particle_seed("a", 0) != canvas::particle_seed("a", 1));
}

TEST_CASE("gravity, damping and the emitter's transform move particles as Godot's") {
    Particles scene;
    const entt::entity falling =
        scene.add("falling", {0.0F, 0.0F}, R"({"amount": 1, "lifetime": 10, "gravity": [0, 100]})");
    const entt::entity slowing =
        scene.add("slowing", {0.0F, 0.0F},
                  R"({"amount": 1, "lifetime": 10, "gravity": [0, 0], "spread": 0,
            "initial_velocity_min": 100, "initial_velocity_max": 100, "damping_min": 50,
            "damping_max": 50})");
    // Turned a quarter turn clockwise and doubled, so it emits down at twice the speed.
    const entt::entity turned =
        scene.add("turned", {10.0F, 20.0F},
                  R"({"amount": 1, "lifetime": 10, "gravity": [0, 0], "spread": 0,
            "initial_velocity_min": 100, "initial_velocity_max": 100})");
    auto& transform = scene.world.ecs().get<world::Transform2D>(turned);
    transform.rotation = std::numbers::pi_v<float> / 2.0F;
    transform.scale = {2.0F, 2.0F};
    // Its y axis along its velocity, which points along x: a quarter turn anticlockwise.
    const entt::entity aligned =
        scene.add("aligned", {0.0F, 0.0F},
                  R"({"amount": 1, "lifetime": 10, "gravity": [0, 0], "spread": 0,
            "initial_velocity_min": 100, "initial_velocity_max": 100,
            "particle_flag_align_y": true})");
    scene.step(60);

    // Gravity speeds it up before it moves each tick: 100 * (1/60)^2 * (1 + 2 + ... + 60).
    const canvas::Particle fall = scene.live(falling).at(0);
    CHECK(fall.velocity.y == doctest::Approx(100.0F).epsilon(1e-4));
    CHECK(fall.position.y == doctest::Approx(50.8333F).epsilon(1e-4));
    CHECK(fall.position.x == 0.0F);

    // Damping takes 50 pixels a second off its speed each second, down to a stop.
    const canvas::Particle slow = scene.live(slowing).at(0);
    CHECK(slow.velocity.x == doctest::Approx(50.0F).epsilon(1e-4));
    CHECK(slow.position.x == doctest::Approx(74.5833F).epsilon(1e-4));

    const canvas::Particle down = scene.live(turned).at(0);
    CHECK(down.position.x == doctest::Approx(10.0F));
    CHECK(down.position.y == doctest::Approx(220.0F).epsilon(1e-4));

    CHECK(scene.live(aligned).at(0).rotation == doctest::Approx(-std::numbers::pi_v<float> / 2.0F));

    scene.step(120);
    CHECK(scene.live(slowing).at(0).velocity == Vec2{});
}

TEST_CASE("particle_flag_align_y turns particles' y axis along their velocity") {
    Particles scene;
    std::vector<std::pair<entt::entity, float>> emitters;
    for (int i = 0; i < 24; ++i) {
        const float angle = (static_cast<float>(i) * 0.27F) - 3.1F;
        const entt::entity entity =
            scene.add("e" + std::to_string(i), {},
                      R"({"amount": 1, "lifetime": 10, "gravity": [0, 0], "spread": 0,
                          "initial_velocity_min": 10, "initial_velocity_max": 10,
                          "particle_flag_align_y": true})");
        scene.emitter(entity).direction = canvas::cos_sin(angle);
        emitters.emplace_back(entity, angle);
    }
    scene.step();
    for (const auto& [entity, angle] : emitters) {
        CAPTURE(angle);
        // Its x axis is a quarter turn anticlockwise from its velocity.
        const auto expected = static_cast<float>(std::atan2(-std::cos(static_cast<double>(angle)),
                                                            std::sin(static_cast<double>(angle))));
        CHECK(scene.live(entity).at(0).rotation == doctest::Approx(expected).epsilon(1e-5));
    }
}

TEST_CASE("particles in local coordinates move with their emitter") {
    Particles scene;
    const std::string fields =
        R"({"amount": 1, "lifetime": 10, "gravity": [0, 0], "spread": 0,
            "initial_velocity_min": 100, "initial_velocity_max": 100, "local_coords": %})";
    std::string local = fields;
    local.replace(local.find('%'), 1, "true");
    std::string in_world = fields;
    in_world.replace(in_world.find('%'), 1, "false");
    const entt::entity a = scene.add("local", {100.0F, 0.0F}, local);
    const entt::entity b = scene.add("world", {100.0F, 0.0F}, in_world);
    scene.step(30);
    CHECK(scene.live(a).at(0).position.x == doctest::Approx(50.0F).epsilon(1e-4));
    CHECK(scene.live(b).at(0).position.x == doctest::Approx(150.0F).epsilon(1e-4));

    scene.world.ecs().get<world::Transform2D>(a).position = {200.0F, 50.0F};
    scene.world.ecs().get<world::Transform2D>(b).position = {200.0F, 50.0F};
    scene.step(30);
    // One stays where the entity puts it; the other carries on where it was.
    CHECK(scene.live(a).at(0).position.x == doctest::Approx(100.0F).epsilon(1e-4));
    CHECK(scene.live(a).at(0).position.y == 0.0F);
    CHECK(scene.live(b).at(0).position.x == doctest::Approx(200.0F).epsilon(1e-4));
    CHECK(scene.live(b).at(0).position.y == 0.0F);
}

TEST_CASE("curves and gradients change particles over their lives") {
    Particles scene;
    const entt::entity entity =
        scene.add("fading", {0.0F, 0.0F},
                  R"({"amount": 1, "lifetime": 1, "gravity": [0, 0], "color": [1, 1, 1, 0.5],
            "scale_amount_min": 2, "scale_amount_max": 2,
            "scale_amount_curve": [[1, 0], [0, 1]],
            "color_ramp": [{"offset": 0, "color": [1, 0, 0, 1]},
                           {"offset": 1, "color": [0, 0, 1, 1]}],
            "angle_min": 90, "angle_max": 90, "angular_velocity_min": 180,
            "angular_velocity_max": 180, "anim_speed_min": 1, "anim_speed_max": 1,
            "anim_offset_min": 0.25, "anim_offset_max": 0.25})");
    scene.step(30);
    const canvas::Particle p = scene.live(entity).at(0);
    REQUIRE(p.age == doctest::Approx(0.5F));
    // Halfway through its life: half its scale, halfway from red to blue, half a turn.
    CHECK(p.scale == doctest::Approx(1.0F).epsilon(1e-4));
    CHECK(p.color.x == doctest::Approx(0.5F).epsilon(1e-4));
    CHECK(p.color.y == 0.0F);
    CHECK(p.color.z == doctest::Approx(0.5F).epsilon(1e-4));
    CHECK(p.color.w == doctest::Approx(0.5F));
    CHECK(p.rotation == doctest::Approx(std::numbers::pi_v<float>).epsilon(1e-4));
    CHECK(p.frame == doctest::Approx(0.75F).epsilon(1e-4));

    // A full turn of hue gives the color back.
    auto& emitter = scene.emitter(entity);
    emitter.color_ramp.clear();
    emitter.color = {0.8F, 0.4F, 0.2F, 1.0F};
    emitter.hue_variation_min = 1.0F;
    emitter.hue_variation_max = 1.0F;
    scene.step();
    const Vec4 color = scene.live(entity).at(0).color;
    // Godot's hue matrix is rounded to three places, so it comes back to within 0.002.
    CHECK(std::abs(color.x - 0.8F) <= 0.002F);
    CHECK(std::abs(color.y - 0.4F) <= 0.002F);
    CHECK(std::abs(color.z - 0.2F) <= 0.002F);
}

TEST_CASE("emission shapes place new particles") {
    const auto emit = [](std::string_view shape_fields) {
        Particles scene;
        std::string fields = R"({"amount": 200, "explosiveness": 1, "gravity": [0, 0], )";
        fields += shape_fields;
        fields += "}";
        const entt::entity entity = scene.add("shape", {100.0F, 100.0F}, fields);
        scene.step();
        return scene.live(entity);
    };
    const Vec2 center{100.0F, 100.0F};

    SUBCASE("rectangle") {
        bool left = false;
        bool right = false;
        for (const canvas::Particle& p :
             emit(R"("emission_shape": "rectangle", "emission_rect_extents": [10, 5])")) {
            CHECK(std::abs(p.position.x - center.x) <= 10.0F);
            CHECK(std::abs(p.position.y - center.y) <= 5.0F);
            left = left || p.position.x < center.x - 5.0F;
            right = right || p.position.x > center.x + 5.0F;
        }
        CHECK((left && right));
    }
    SUBCASE("sphere") {
        bool inner = false;
        for (const canvas::Particle& p :
             emit(R"("emission_shape": "sphere", "emission_sphere_radius": 10)")) {
            CHECK(distance(p.position, center) <= 10.0F + 1e-3F);
            inner = inner || distance(p.position, center) < 5.0F;
        }
        CHECK(inner);
    }
    SUBCASE("sphere_surface") {
        for (const canvas::Particle& p :
             emit(R"("emission_shape": "sphere_surface", "emission_sphere_radius": 10)")) {
            CHECK(distance(p.position, center) == doctest::Approx(10.0F).epsilon(1e-4));
        }
    }
    SUBCASE("ring") {
        for (const canvas::Particle& p :
             emit(R"("emission_shape": "ring", "emission_ring_radius": 10,
                     "emission_ring_inner_radius": 8)")) {
            CHECK(distance(p.position, center) >= 8.0F - 1e-3F);
            CHECK(distance(p.position, center) <= 10.0F + 1e-3F);
        }
    }
    SUBCASE("points take their point's color") {
        std::set<std::pair<float, float>> used;
        for (const canvas::Particle& p :
             emit(R"("emission_shape": "points", "emission_points": [[1, 2], [3, 4]],
                     "emission_colors": [[1, 0, 0, 1], [0, 1, 0, 1]])")) {
            used.emplace(p.position.x, p.position.y);
            const bool first = p.position == Vec2{101.0F, 102.0F};
            CHECK((first || p.position == Vec2{103.0F, 104.0F}));
            CHECK(p.color == (first ? Vec4{1.0F, 0.0F, 0.0F, 1.0F} : Vec4{0.0F, 1.0F, 0.0F, 1.0F}));
        }
        CHECK(used.size() == 2);
    }
    SUBCASE("directed_points turn velocity to their normal") {
        for (const canvas::Particle& p :
             emit(R"("emission_shape": "directed_points", "emission_points": [[0, 0]],
                     "emission_normals": [[0, 2]], "spread": 0, "initial_velocity_min": 100,
                     "initial_velocity_max": 100)")) {
            CHECK(p.velocity.x == doctest::Approx(0.0F));
            CHECK(p.velocity.y == doctest::Approx(100.0F));
        }
    }
}

TEST_CASE("cos_sin matches the standard library to a float's precision") {
    CHECK(canvas::cos_sin(0.0F) == Vec2{1.0F, 0.0F});
    CHECK(canvas::cos_sin(std::numeric_limits<float>::quiet_NaN()) == Vec2{1.0F, 0.0F});
    for (int i = -2000; i <= 2000; ++i) {
        const float radians = static_cast<float>(i) * 0.0517F;
        const Vec2 cs = canvas::cos_sin(radians);
        CAPTURE(radians);
        CHECK(std::abs(cs.x - static_cast<float>(std::cos(static_cast<double>(radians)))) <= 1e-6F);
        CHECK(std::abs(cs.y - static_cast<float>(std::sin(static_cast<double>(radians)))) <= 1e-6F);
    }
}
