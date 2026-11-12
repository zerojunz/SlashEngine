#include "slash/asset/assets.hpp"
#include "slash/canvas/canvas.hpp"
#include "slash/canvas/particles.hpp"
#include "slash/core/jobs.hpp"
#include "slash/core/json.hpp"
#include "slash/core/math.hpp"
#include "slash/scene3d/particles.hpp"
#include "slash/ui/ui.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/particle_sim.hpp"
#include "slash/world/world.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace asset = slash::asset;
namespace canvas = slash::canvas;
namespace json = slash::json;
namespace scene3d = slash::scene3d;
namespace world = slash::world;
using slash::Vec3f;

namespace {

constexpr double kTick = 1.0 / 60.0;

json::Value parse(std::string_view text) {
    auto value = json::parse(text);
    REQUIRE(value.ok());
    return std::move(value).value();
}

asset::FileSource no_files() {
    return {
        .read = [](std::string_view /*path*/) -> slash::Result<std::string> {
            return slash::make_error(slash::ErrorCode::NotFound, "no such file");
        },
        .stamp = [](std::string_view /*path*/) -> std::optional<asset::FileStamp> {
            return std::nullopt;
        },
    };
}

// A world with the canvas and 3D systems and the events it emits.
struct Scene {
    asset::Assets assets{no_files()};
    world::ComponentRegistry components;
    world::World world{components};
    std::vector<world::Event> events;

    Scene() {
        world::register_builtin_components(components);
        canvas::install(world, assets);
        slash::ui::Settings ui;
        ui.viewport = {32.0F, 32.0F};
        slash::ui::set_settings(world, ui);
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

    void step(int ticks = 1) {
        for (int i = 0; i < ticks; ++i) {
            world.step(kTick);
        }
    }

    [[nodiscard]] std::size_t count(std::string_view kind) const {
        return static_cast<std::size_t>(std::ranges::count_if(
            events, [&](const world::Event& event) { return event.kind == kind; }));
    }
};

float length(Vec3f v) {
    return std::sqrt((v.x * v.x) + (v.y * v.y) + (v.z * v.z));
}

world::ParticleStages compiled(const world::ParticleSimulation& settings) {
    auto stages = world::compile_stages(settings);
    REQUIRE_MESSAGE(stages.ok(), (stages.ok() ? "" : stages.error().message));
    return std::move(stages).value();
}

world::ParticleEventRule rule_on(std::string on, std::string name) {
    world::ParticleEventRule rule;
    rule.on = std::move(on);
    rule.name = std::move(name);
    return rule;
}

world::SimParticle particle_at(Vec3f position, Vec3f velocity, std::uint32_t index) {
    world::SimParticle p;
    p.position = position;
    p.velocity = velocity;
    p.index = index;
    p.lifetime = 10.0F;
    return p;
}

} // namespace

TEST_CASE("the neighbor index finds every point within its radius by Morton cells") {
    CHECK(world::morton_code(0, 0, 0) < world::morton_code(1, 0, 0));
    CHECK(world::morton_code(-1, 0, 0) < world::morton_code(0, 0, 0));
    CHECK(world::morton_code(1, 0, 0) == (world::morton_code(0, 0, 0) ^ 1U));

    std::vector<Vec3f> points;
    for (std::uint32_t i = 0; i < 300; ++i) {
        points.push_back({(world::sim_random(7, i, 0) * 8.0F) - 4.0F,
                          (world::sim_random(7, i, 1) * 8.0F) - 4.0F,
                          (world::sim_random(7, i, 2) * 8.0F) - 4.0F});
    }
    constexpr float kRadius = 0.9F;
    world::NeighborIndex index;
    index.build(points, kRadius, false);
    CHECK(index.size() == points.size());
    for (std::size_t n = 0; n < points.size(); n += 7) {
        std::set<std::uint32_t> found;
        index.around(points[n], [&](std::uint32_t other) {
            if (length(points[other] - points[n]) < kRadius) {
                found.insert(other);
            }
        });
        std::set<std::uint32_t> expected;
        for (std::uint32_t other = 0; other < points.size(); ++other) {
            if (length(points[other] - points[n]) < kRadius) {
                expected.insert(other);
            }
        }
        CHECK(found == expected);
    }
    // Along the curve, every point once.
    std::vector<std::uint32_t> order = index.order();
    std::ranges::sort(order);
    CHECK(std::ranges::adjacent_find(order) == order.end());
    CHECK(order.size() == points.size());

    // Flat: z is ignored, so points above one another are neighbors.
    const std::vector<Vec3f> stacked{{0.1F, 0.1F, 0.0F}, {0.1F, 0.1F, 50.0F}};
    world::NeighborIndex flat;
    flat.build(stacked, 1.0F, true);
    int visits = 0;
    flat.around(stacked[0], [&](std::uint32_t /*other*/) { ++visits; });
    CHECK(visits == 2);
}

TEST_CASE("curl noise is the same each call, smooth and has no divergence") {
    const Vec3f p{0.3F, 1.7F, -2.2F};
    const Vec3f a = world::curl_noise(p, 0.5F, 11, false);
    CHECK(a == world::curl_noise(p, 0.5F, 11, false));
    CHECK(!(a == world::curl_noise(p, 0.5F, 12, false)));
    CHECK(length(a) > 0.0F);
    // Its divergence, by central differences, is near zero next to its size.
    constexpr float kH = 1e-2F;
    const auto at = [](Vec3f q) { return world::curl_noise(q, 0.5F, 11, false); };
    const float divergence = ((at(p + Vec3f{kH, 0.0F, 0.0F}).x - at(p - Vec3f{kH, 0.0F, 0.0F}).x) +
                              (at(p + Vec3f{0.0F, kH, 0.0F}).y - at(p - Vec3f{0.0F, kH, 0.0F}).y) +
                              (at(p + Vec3f{0.0F, 0.0F, kH}).z - at(p - Vec3f{0.0F, 0.0F, kH}).z)) /
                             (2.0F * kH);
    const float spread =
        (length(at(p + Vec3f{kH, 0.0F, 0.0F}) - at(p - Vec3f{kH, 0.0F, 0.0F}))) / (2.0F * kH);
    CHECK(std::abs(divergence) < 0.05F * std::max(spread, 1.0F));
    // Flat curl lies in the plane.
    const Vec3f flat = world::curl_noise(p, 0.5F, 11, true);
    CHECK(flat.z == 0.0F);
    CHECK(length(flat) > 0.0F);
}

TEST_CASE("simulation stages separate, gather, slow and limit particles") {
    world::ParticleSimulation settings;
    settings.neighbor_radius = 1.0F;
    settings.separation = 4.0F;
    const world::ParticleStages apart = compiled(settings);
    world::SimInputs inputs;
    inputs.delta = 0.1F;
    std::vector<world::SimParticle> particles{particle_at({0.0F, 0.0F, 0.0F}, {}, 0),
                                              particle_at({0.5F, 0.0F, 0.0F}, {}, 1),
                                              particle_at({5.0F, 0.0F, 0.0F}, {}, 2)};
    std::vector<world::SimEvent> events;
    std::string failed;
    world::run_stages(apart, particles, inputs, events, failed);
    CHECK(failed.empty());
    CHECK(particles[0].velocity.x < 0.0F);
    CHECK(particles[1].velocity.x > 0.0F);
    CHECK(particles[0].velocity.x == doctest::Approx(-particles[1].velocity.x));
    CHECK(particles[0].neighbors == 1);
    CHECK(particles[2].neighbors == 0);
    CHECK(particles[2].velocity == Vec3f{});
    // The change moves them this tick too.
    CHECK(particles[0].position.x == doctest::Approx(particles[0].velocity.x * 0.1F));

    settings.separation = 0.0F;
    settings.cohesion = 2.0F;
    const world::ParticleStages together = compiled(settings);
    particles = {particle_at({0.0F, 0.0F, 0.0F}, {}, 0), particle_at({0.5F, 0.0F, 0.0F}, {}, 1)};
    world::run_stages(together, particles, inputs, events, failed);
    CHECK(particles[0].velocity.x > 0.0F);
    CHECK(particles[1].velocity.x < 0.0F);

    world::ParticleSimulation slow;
    slow.drag = 5.0F;
    slow.max_speed = 3.0F;
    particles = {particle_at({}, {10.0F, 0.0F, 0.0F}, 0), particle_at({}, {1.0F, 0.0F, 0.0F}, 1)};
    world::run_stages(compiled(slow), particles, inputs, events, failed);
    CHECK(particles[0].velocity.x == doctest::Approx(3.0F));
    CHECK(particles[1].velocity.x == doctest::Approx(0.5F));

    world::ParticleSimulation fast;
    fast.min_speed = 2.0F;
    particles = {particle_at({}, {0.0F, 1.0F, 0.0F}, 0)};
    world::run_stages(compiled(fast), particles, inputs, events, failed);
    CHECK(particles[0].velocity.y == doctest::Approx(2.0F));

    world::ParticleSimulation swirl;
    swirl.curl_strength = 4.0F;
    particles = {particle_at({0.2F, 0.4F, 0.6F}, {}, 0)};
    inputs.flat = true;
    world::run_stages(compiled(swirl), particles, inputs, events, failed);
    CHECK(length(particles[0].velocity) > 0.0F);
    CHECK(particles[0].velocity.z == 0.0F);
}

TEST_CASE("simulation stages collide through the contact they are given") {
    world::ParticleSimulation settings;
    settings.collide = "colliders";
    settings.bounce = 0.5F;
    settings.friction = 0.0F;
    settings.events.push_back(rule_on("collision", "splash"));
    world::SimInputs inputs;
    inputs.delta = 0.1F;
    // The floor is y = 0.
    inputs.contact = [](Vec3f p, float radius) {
        world::SimContact c;
        if (p.y < radius) {
            c = {.collided = true, .normal = {0.0F, 1.0F, 0.0F}, .depth = radius - p.y};
        }
        return c;
    };
    std::vector<world::SimParticle> particles{
        particle_at({0.0F, -0.1F, 0.0F}, {1.0F, -4.0F, 0.0F}, 0),
        particle_at({0.0F, 2.0F, 0.0F}, {0.0F, -1.0F, 0.0F}, 1)};
    std::vector<world::SimEvent> events;
    std::string failed;
    world::run_stages(compiled(settings), particles, inputs, events, failed);
    CHECK(particles[0].collided);
    CHECK(particles[0].position.y == doctest::Approx(settings.collision_radius));
    CHECK(particles[0].velocity.y == doctest::Approx(2.0F));
    CHECK(particles[0].velocity.x == doctest::Approx(1.0F));
    CHECK_FALSE(particles[1].collided);
    REQUIRE(events.size() == 1);
    CHECK(events[0].particle == 0);

    settings.response = "stick";
    particles = {particle_at({0.0F, -0.1F, 0.0F}, {1.0F, -4.0F, 0.0F}, 0)};
    world::run_stages(compiled(settings), particles, inputs, events, failed);
    CHECK(particles[0].velocity == Vec3f{});

    settings.response = "kill";
    particles = {particle_at({0.0F, -0.1F, 0.0F}, {1.0F, -4.0F, 0.0F}, 0)};
    world::run_stages(compiled(settings), particles, inputs, events, failed);
    CHECK_FALSE(particles[0].alive);
}

TEST_CASE("attributes are worked out by Expressions when particles start and each tick") {
    world::ParticleSimulation settings;
    settings.attributes = {
        {.name = "heat", .value = "random * 100", .every_tick = false},
        {.name = "size", .value = "1 + age", .every_tick = true},
        {.name = "kill", .value = "heat > 50 and age > 1", .every_tick = true},
    };
    const world::ParticleStages stages = compiled(settings);
    CHECK(stages.size_attribute == 1);
    CHECK(stages.kill_attribute == 2);
    CHECK(stages.alpha_attribute == -1);
    world::SimInputs inputs;
    inputs.delta = 0.1F;
    std::vector<world::SimParticle> particles{particle_at({}, {}, 0), particle_at({}, {}, 1)};
    particles[0].random = 0.2F;
    particles[1].random = 0.9F;
    for (world::SimParticle& p : particles) {
        p.born = true;
        p.age = 1.5F;
    }
    std::vector<world::SimEvent> events;
    std::string failed;
    world::run_stages(stages, particles, inputs, events, failed);
    CHECK(failed.empty());
    CHECK(particles[0].attributes[0] == doctest::Approx(20.0F));
    CHECK(particles[1].attributes[0] == doctest::Approx(90.0F));
    CHECK(world::attribute_or(stages, particles[0], stages.size_attribute, 1.0F) ==
          doctest::Approx(2.5F));
    CHECK(world::attribute_or(stages, particles[0], stages.alpha_attribute, 0.75F) == 0.75F);
    CHECK(particles[0].alive);
    CHECK_FALSE(particles[1].alive);
    // Kept, not worked out again: random changed but heat stays.
    particles[0].born = false;
    particles[0].random = 0.7F;
    world::run_stages(stages, std::span(particles).first(1), inputs, events, failed);
    CHECK(particles[0].attributes[0] == doctest::Approx(20.0F));

    // An expression that fails at run time says so once and leaves the value at 0.
    world::ParticleSimulation broken;
    broken.attributes = {{.name = "odd", .value = "age.nothing", .every_tick = true}};
    const auto parsed = world::compile_stages(broken);
    if (parsed.ok()) {
        particles = {particle_at({}, {}, 0)};
        world::run_stages(parsed.value(), particles, inputs, events, failed);
        CHECK_FALSE(failed.empty());
    }
}

TEST_CASE("compile_stages refuses attributes it can't run, with their JSON Pointers") {
    const auto error_of = [](const world::ParticleSimulation& settings) {
        const auto stages = world::compile_stages(settings);
        REQUIRE_FALSE(stages.ok());
        return stages.error().message;
    };
    world::ParticleSimulation settings;
    settings.attributes.resize(world::kMaxParticleAttributes + 1);
    CHECK(error_of(settings).starts_with("/attributes: "));

    settings.attributes = {{.name = "a", .value = "1", .every_tick = false},
                           {.name = "a", .value = "2", .every_tick = false}};
    CHECK(error_of(settings).starts_with("/attributes/1/name: "));

    settings.attributes = {{.name = "age", .value = "1", .every_tick = false}};
    CHECK(error_of(settings).starts_with("/attributes/0/name: "));

    settings.attributes = {{.name = "a", .value = "1 +", .every_tick = false}};
    CHECK(error_of(settings).starts_with("/attributes/0/value: "));

    settings.attributes = {{.name = "a", .value = "b * 2", .every_tick = false},
                           {.name = "b", .value = "1", .every_tick = false}};
    CHECK(error_of(settings).starts_with("/attributes/0/value: "));

    settings.attributes = {};
    settings.events = {rule_on("landing", "")};
    CHECK(error_of(settings).starts_with("/events/0/on: "));
}

TEST_CASE("3D particles flock and collide with particle colliders in a world") {
    Scene scene;
    scene.create("floor", R"({"Transform3D": {"position": [0, -1, 0]},
        "GPUParticlesCollisionBox3D": {"size": [20, 2, 20]}})");
    const entt::entity rain = scene.create("rain", R"({"Transform3D": {"position": [0, 3, 0]},
        "CPUParticles3D": {"amount": 40, "lifetime": 4, "explosiveness": 1, "spread": 180,
                           "initial_velocity_min": 1, "initial_velocity_max": 2,
                           "gravity": [0, -9.8, 0]},
        "ParticleSimulation3D": {"collide": "colliders", "bounce": 0, "friction": 1,
                                 "neighbor_radius": 0.5, "separation": 1,
                                 "events": [{"on": "collision", "name": "landed"}]}})");
    scene.step(120);
    const std::vector<scene3d::Particle3D> live = scene3d::particles_3d(scene.world, rain);
    REQUIRE(live.size() == 40);
    for (const scene3d::Particle3D& p : live) {
        CHECK(p.transform.origin.y > -0.01F);
        CHECK(p.transform.origin.y < 0.2F);
    }
    CHECK(scene.count("particle_event") > 0);
    // At most 8 world events an emitter a tick.
    std::size_t first_tick = 0;
    for (const world::Event& event : scene.events) {
        if (event.kind == "particle_event") {
            CHECK(event.id == "rain");
            CHECK(event.data.find("name")->as_string() == "landed");
            first_tick = first_tick == 0 ? event.tick : first_tick;
        }
    }
    CHECK(std::ranges::count_if(scene.events, [&](const world::Event& event) {
              return event.kind == "particle_event" && event.tick == first_tick;
          }) <= 8);
}

TEST_CASE("3D particles collide with what the current camera sees") {
    Scene scene;
    scene.create("camera",
                 R"({"Transform3D": {"position": [0, 6, 0], "rotation": [-1.5707964, 0, 0]},
        "Camera3D": {"current": true}})");
    scene.create("ground", R"({"Transform3D": {"position": [0, -0.5, 0]},
        "MeshInstance3D": {}, "BoxMesh": {"size": [20, 1, 20]}})");
    const entt::entity sand = scene.create("sand", R"({"Transform3D": {"position": [0, 2, 0]},
        "CPUParticles3D": {"amount": 12, "lifetime": 5, "explosiveness": 1, "spread": 30,
                           "direction": [0, -1, 0], "initial_velocity_min": 0.5,
                           "initial_velocity_max": 1, "gravity": [0, -9.8, 0]},
        "ParticleSimulation3D": {"collide": "depth", "response": "stick"}})");
    scene.step(90);
    const std::vector<scene3d::Particle3D> live = scene3d::particles_3d(scene.world, sand);
    REQUIRE(live.size() == 12);
    for (const scene3d::Particle3D& p : live) {
        CHECK(p.transform.origin.y > -0.15F);
        CHECK(p.transform.origin.y < 0.3F);
    }
}

TEST_CASE("particle events start particles in another emitter") {
    Scene scene;
    scene.create("floor", R"({"Transform3D": {"position": [0, -1, 0]},
        "GPUParticlesCollisionBox3D": {"size": [20, 2, 20]}})");
    scene.create("drops", R"({"Transform3D": {"position": [0, 1, 0]},
        "CPUParticles3D": {"amount": 4, "lifetime": 3, "explosiveness": 1, "one_shot": true,
                           "spread": 0, "direction": [0, -1, 0], "initial_velocity_min": 1,
                           "initial_velocity_max": 1, "gravity": [0, -9.8, 0]},
        "ParticleSimulation3D": {"collide": "colliders", "response": "kill",
            "events": [{"on": "collision", "spawn": "splashes", "count": 3, "speed": 2}]}})");
    const entt::entity splashes = scene.create("splashes", R"({"Transform3D": {},
        "CPUParticles3D": {"emitting": false, "amount": 1, "lifetime": 0.5,
                           "gravity": [0, 0, 0]}})");
    scene.step(40);
    const std::vector<scene3d::Particle3D> live = scene3d::particles_3d(scene.world, splashes);
    CHECK(live.size() == 12);
    for (const scene3d::Particle3D& p : live) {
        CHECK(std::abs(p.transform.origin.y) < 1.1F);
        CHECK(length(slash::to_float(p.velocity)) == doctest::Approx(2.0F).epsilon(0.01));
    }
    scene.step(40);
    CHECK(scene3d::particles_3d(scene.world, splashes).empty());
}

TEST_CASE("death and location events fire where particles end and pass") {
    Scene scene;
    scene.create("sparks", R"({"Transform3D": {},
        "CPUParticles3D": {"amount": 3, "lifetime": 0.5, "explosiveness": 1, "one_shot": true,
                           "spread": 0, "direction": [1, 0, 0], "initial_velocity_min": 4,
                           "initial_velocity_max": 4, "gravity": [0, 0, 0]},
        "ParticleSimulation3D": {"events": [
            {"on": "death", "name": "out"},
            {"on": "location", "name": "gate", "region_position": [1, -1, -1],
             "region_size": [0.5, 2, 2], "kill": true}]}})");
    scene.step(60);
    std::size_t gate = 0;
    std::size_t out = 0;
    for (const world::Event& event : scene.events) {
        if (event.kind != "particle_event") {
            continue;
        }
        const std::string name = event.data.find("name")->as_string();
        if (name == "gate") {
            ++gate;
            const double x = event.data.find("position")->as_array()[0].as_double();
            CHECK(x >= 1.0);
            CHECK(x <= 1.5);
        } else if (name == "out") {
            ++out;
        }
    }
    // Each passes the gate once, which kills it, and dies once.
    CHECK(gate == 3);
    CHECK(out == 3);
}

TEST_CASE("2D particles collide with occluders and change size and alpha by attributes") {
    Scene scene;
    scene.create("ground", R"({"Transform2D": {"position": [0, 100]},
        "LightOccluder2D": {"polygon": [[-500, 0], [500, 0], [500, 40], [-500, 40]]}})");
    const entt::entity dust = scene.create("dust", R"({"Transform2D": {},
        "CPUParticles2D": {"amount": 20, "lifetime": 4, "explosiveness": 1, "spread": 45,
                           "initial_velocity_min": 50, "initial_velocity_max": 80,
                           "gravity": [0, 300]},
        "ParticleSimulation2D": {"collide": "occluders", "bounce": 0.2, "friction": 0.5,
            "attributes": [{"name": "size", "value": "2", "every_tick": true},
                           {"name": "alpha", "value": "0.5", "every_tick": true}]}})");
    scene.step(150);
    const std::vector<canvas::Particle> live = canvas::particles(scene.world, dust);
    REQUIRE(live.size() == 20);
    for (const canvas::Particle& p : live) {
        CHECK(p.position.y < 100.5F);
        CHECK(p.position.y > 90.0F);
        CHECK(p.scale == doctest::Approx(2.0F));
        CHECK(p.color.w == doctest::Approx(0.5F));
    }
}

TEST_CASE("2D particles flock in the plane") {
    Scene scene;
    const entt::entity birds = scene.create("birds", R"({"Transform2D": {},
        "CPUParticles2D": {"amount": 30, "lifetime": 10, "explosiveness": 1, "spread": 180,
                           "initial_velocity_min": 20, "initial_velocity_max": 60,
                           "gravity": [0, 0]},
        "ParticleSimulation2D": {"neighbor_radius": 40, "separation": 1, "cohesion": 0.5,
                                 "alignment": 2, "min_speed": 30, "max_speed": 60,
                                 "curl_strength": 10}})");
    scene.step(120);
    const std::vector<canvas::Particle> live = canvas::particles(scene.world, birds);
    REQUIRE(live.size() == 30);
    for (const canvas::Particle& p : live) {
        const float speed =
            std::sqrt((p.velocity.x * p.velocity.x) + (p.velocity.y * p.velocity.y));
        CHECK(speed <= 60.01F);
        CHECK(speed >= 29.99F);
    }
}

TEST_CASE("particle simulations don't depend on the number of threads") {
    const auto run = [](int threads) {
        slash::jobs::Pool pool(threads);
        Scene scene;
        scene.world.set_pool(pool);
        scene.create("floor", R"({"Transform3D": {"position": [0, -1, 0]},
            "GPUParticlesCollisionBox3D": {"size": [20, 2, 20]}})");
        for (const std::string_view id : {"a", "b", "c"}) {
            scene.create(id, R"({"Transform3D": {"position": [0, 2, 0]},
                "CPUParticles3D": {"amount": 30, "lifetime": 2, "spread": 90,
                                   "initial_velocity_min": 1, "initial_velocity_max": 3},
                "ParticleSimulation3D": {"collide": "colliders", "neighbor_radius": 0.4,
                                         "separation": 2, "alignment": 1, "curl_strength": 3,
                    "attributes": [{"name": "size", "value": "1 + random", "every_tick": false}]}})");
        }
        scene.create("flat", R"({"Transform2D": {},
            "CPUParticles2D": {"amount": 30, "lifetime": 2, "spread": 180,
                               "initial_velocity_min": 20, "initial_velocity_max": 60},
            "ParticleSimulation2D": {"neighbor_radius": 30, "separation": 1, "cohesion": 1,
                                     "curl_strength": 20}})");
        scene.step(90);
        std::vector<float> out;
        for (const std::string_view id : {"a", "b", "c"}) {
            for (const scene3d::Particle3D& p :
                 scene3d::particles_3d(scene.world, scene.world.find(id))) {
                out.insert(out.end(), {p.transform.origin.x, p.transform.origin.y,
                                       p.transform.origin.z, p.transform.x.x});
            }
        }
        for (const canvas::Particle& p : canvas::particles(scene.world, scene.world.find("flat"))) {
            out.insert(out.end(), {p.position.x, p.position.y});
        }
        return out;
    };
    const std::vector<float> alone = run(0);
    CHECK(alone.size() > 100);
    CHECK(alone == run(3));
}
