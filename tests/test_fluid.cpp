// Fluids and smoke as particles with no grid (slash/world/fluid.hpp): recipes, the
// position-based liquid solver, gases that rise, burn and cool, emitters and sinks, collision
// with 3D and 2D bodies, and steps that repeat on any number of threads.

#include "slash/asset/assets.hpp"
#include "slash/canvas/canvas.hpp"
#include "slash/core/jobs.hpp"
#include "slash/core/json.hpp"
#include "slash/core/math.hpp"
#include "slash/physics2d/physics2d.hpp"
#include "slash/physics3d/physics3d.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/fluid.hpp"
#include "slash/world/world.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace asset = slash::asset;
namespace canvas = slash::canvas;
namespace json = slash::json;
namespace physics2d = slash::physics2d;
namespace physics3d = slash::physics3d;
namespace world = slash::world;
using slash::Vec3f;

namespace {

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE(parsed.ok());
    return std::move(parsed).value();
}

constexpr std::string_view kWater = R"({"format": 1, "name": "Water", "particle_radius": 0.05,
    "iterations": 4, "max_particles": 2000})";

constexpr std::string_view kSmoke = R"({"format": 1, "name": "Smoke", "kind": "gas",
    "particle_radius": 0.05, "lifetime": 0,
    "gas": {"buoyancy": 2, "cooling": 0.4, "ignition": 200, "burn_rate": 2, "heat": 400}})";

constexpr std::string_view kWater2D = R"({"format": 1, "name": "Water", "space": "2d",
    "particle_radius": 4, "max_particles": 2000})";

// A world with files in memory and every canvas system, with physics or not.
struct Scene {
    std::map<std::string, std::string, std::less<>> files;
    asset::Assets assets{asset::FileSource{
        .read = [this](std::string_view path) -> slash::Result<std::string> {
            const auto it = files.find(path);
            if (it == files.end()) {
                return slash::make_error(slash::ErrorCode::NotFound, "no such file");
            }
            return it->second;
        },
        .stamp = [this](std::string_view path) -> std::optional<asset::FileStamp> {
            const auto it = files.find(path);
            if (it == files.end()) {
                return std::nullopt;
            }
            return asset::FileStamp{.modified = 1, .size = it->second.size()};
        }}};
    world::ComponentRegistry components;
    world::World world{components};
    std::vector<world::Event> events;

    explicit Scene(bool physics = false) {
        world::register_builtin_components(components);
        files["water.fluid.json"] = std::string(kWater);
        files["smoke.fluid.json"] = std::string(kSmoke);
        files["water2d.fluid.json"] = std::string(kWater2D);
        canvas::install(world, assets);
        if (physics) {
            physics3d::install(world);
            physics2d::install(world);
        }
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

    [[nodiscard]] const world::FluidParticles& particles(std::string_view id) const {
        const world::FluidParticles* found = world::fluid_particles(world, world.find(id));
        REQUIRE(found != nullptr);
        return *found;
    }
};

float lowest(const world::FluidParticles& particles, int axis) {
    float low = 1e30F;
    for (const Vec3f& p : particles.position) {
        low = std::min(low, axis == 0 ? p.x : (axis == 1 ? p.y : p.z));
    }
    return low;
}

float mean(const std::vector<float>& values) {
    float sum = 0.0F;
    for (const float v : values) {
        sum += v;
    }
    return values.empty() ? 0.0F : sum / static_cast<float>(values.size());
}

} // namespace

TEST_CASE("fluid: recipes take their kind's defaults, and validation finds bad fields") {
    const auto water = world::parse_fluid(parse(kWater));
    REQUIRE(water.ok());
    CHECK_FALSE(water.value().gas);
    CHECK_FALSE(water.value().flat);
    CHECK(water.value().rest_density == doctest::Approx(1000.0F));
    CHECK(water.value().gravity_scale == doctest::Approx(1.0F));
    CHECK(water.value().iterations == 4);

    const auto smoke = world::parse_fluid(parse(kSmoke));
    REQUIRE(smoke.ok());
    CHECK(smoke.value().gas);
    CHECK(smoke.value().gravity_scale == doctest::Approx(0.0F));
    CHECK(smoke.value().ignition == doctest::Approx(200.0F));

    const auto flat = world::parse_fluid(parse(kWater2D));
    REQUIRE(flat.ok());
    CHECK(flat.value().flat);
    CHECK(flat.value().substeps == 2);

    const auto errors = world::validate_fluid(
        parse(R"({"format": 1, "kind": "plasma", "iterations": 40, "gas": {"heat": -1}})"));
    std::vector<std::string> paths;
    for (const auto& error : errors) {
        paths.push_back(error.path);
    }
    CHECK(std::ranges::find(paths, "/kind") != paths.end());
    CHECK(std::ranges::find(paths, "/iterations") != paths.end());
    CHECK(std::ranges::find(paths, "/gas/heat") != paths.end());
    CHECK(world::is_fluid_path("liquids/water.fluid.json"));
    CHECK_FALSE(world::is_fluid_path("water.json"));
}

TEST_CASE("fluid: a recipe extends another and takes overrides over both") {
    Scene scene;
    scene.files["muddy.fluid.json"] =
        R"({"format": 1, "extends": "water.fluid.json", "viscosity": 0.2})";
    const auto muddy = world::load_fluid(scene.world, "muddy.fluid.json",
                                         parse(R"({"look": {"color": [0.4, 0.3, 0.2, 1]}})"));
    REQUIRE(muddy.ok());
    CHECK(muddy.value()->viscosity == doctest::Approx(0.2F));
    CHECK(muddy.value()->iterations == 4); // from water
    CHECK(muddy.value()->color.x == doctest::Approx(0.4F));
    CHECK_FALSE(world::load_fluid(scene.world, "missing.fluid.json").ok());
}

TEST_CASE("fluid: water in a tank settles at rest density, keeping every particle in it") {
    Scene scene;
    scene.create("tank", R"({"Transform3D": {"position": [0, 1, 0]},
        "Fluid3D": {"recipe": "water.fluid.json", "fill_size": [0.6, 0.4, 0.6],
                    "bounds_size": [0.8, 2, 0.8]}})");
    scene.step(1);
    const std::size_t count = scene.particles("tank").size();
    CHECK(count == 6 * 4 * 6);
    scene.step(180);
    const auto& water = scene.particles("tank");
    CHECK(water.size() == count);
    // Fallen to the tank's floor, which is a meter below its middle.
    CHECK(lowest(water, 1) == doctest::Approx(0.05F).epsilon(0.02));
    for (const Vec3f& p : water.position) {
        CHECK(std::abs(p.x) <= 0.4F);
        CHECK(std::abs(p.z) <= 0.4F);
        CHECK(p.y >= 0.0F);
    }
    // Settled: slow, and not squashed far past rest density.
    float fastest = 0.0F;
    for (const Vec3f& v : water.velocity) {
        fastest = std::max(fastest, std::sqrt((v.x * v.x) + (v.y * v.y) + (v.z * v.z)));
    }
    CHECK(fastest < 0.5F);
    CHECK(mean(water.density) < 1.3F);
    CHECK(water.contacts > 0);
    CHECK(water.work > 0);
}

TEST_CASE("fluid: steps come out the same on any number of threads") {
    const auto run = [](int threads) {
        slash::jobs::Pool pool(threads);
        Scene scene;
        scene.world.set_pool(pool);
        scene.create("tank", R"({"Transform3D": {},
            "Fluid3D": {"recipe": "water.fluid.json", "fill_size": [0.5, 0.5, 0.5],
                        "bounds_size": [0.8, 2, 0.8]}})");
        scene.step(30);
        return scene.particles("tank").position;
    };
    const auto one = run(0);
    const auto many = run(4);
    REQUIRE(one.size() == many.size());
    CHECK(std::memcmp(one.data(), many.data(), one.size() * sizeof(Vec3f)) == 0);
}

TEST_CASE("fluid: hot smoke rises and cools; fuel above ignition burns into heat and smoke") {
    Scene scene;
    scene.create("puff", R"({"Transform3D": {}, "Fluid3D": {"recipe": "smoke.fluid.json"}})");
    scene.create("vent", R"({"Transform3D": {}, "FluidEmitter3D": {"fluid": "puff", "rate": 300,
        "direction": [0, 1, 0], "speed": 0.5, "temperature": 300, "fuel": 0.5}})");
    scene.step(30);
    const auto& smoke = scene.particles("puff");
    REQUIRE(smoke.size() > 100);
    float top = -1e30F;
    for (const Vec3f& p : smoke.position) {
        top = std::max(top, p.y);
    }
    CHECK(top > 1.0F); // risen far past what half a meter a second pours
    CHECK(mean(smoke.smoke) > 0.05F);
    // The oldest have burned their fuel and cooled below where they started burning hotter.
    const std::size_t oldest = 0;
    CHECK(smoke.fuel[oldest] < 0.5F);
    CHECK(smoke.temperature[oldest] < smoke.temperature[smoke.size() - 1] + 400.0F);
}

TEST_CASE("fluid: emitters pour until the recipe's limit and sinks drain") {
    Scene scene;
    scene.files["small.fluid.json"] =
        R"({"format": 1, "extends": "water.fluid.json", "max_particles": 150})";
    scene.create("pool", R"({"Transform3D": {},
        "Fluid3D": {"recipe": "small.fluid.json", "bounds_size": [2, 2, 2]}})");
    scene.create("tap", R"({"Transform3D": {"position": [0, 0.8, 0]},
        "FluidEmitter3D": {"fluid": "pool", "rate": 600, "speed": 1}})");
    scene.step(30);
    CHECK(scene.particles("pool").size() == 150);
    scene.world.ecs().get<world::FluidEmitter3D>(scene.world.find("tap")).enabled = false;
    scene.create("drain", R"({"Transform3D": {"position": [0, -1, 0]},
        "FluidSink3D": {"size": [2, 0.4, 2]}})");
    scene.step(120);
    CHECK(scene.particles("pool").size() < 30);
}

TEST_CASE("fluid: 2D water falls down the screen, stays at z 0 and fills its box") {
    Scene scene;
    scene.create("tank", R"({"Transform2D": {"position": [100, 100]},
        "Fluid2D": {"recipe": "water2d.fluid.json", "fill_size": [80, 40],
                    "bounds_size": [120, 200]}})");
    scene.step(120);
    const auto& water = scene.particles("tank");
    CHECK(water.size() == 10 * 5);
    float bottom = -1e30F;
    for (const Vec3f& p : water.position) {
        CHECK(p.z == 0.0F);
        bottom = std::max(bottom, p.y);
    }
    // The box's bottom is 100 px below its middle, at y 200.
    CHECK(bottom == doctest::Approx(196.0F).epsilon(0.01));
}

TEST_CASE("fluid: water lands on 3D bodies and pushes a box in it") {
    Scene scene(true);
    scene.create("floor", R"({"Transform3D": {"position": [0, -0.25, 0]},
        "StaticBody3D": {}, "CollisionShape3D": {"size": [4, 0.5, 4]}})");
    scene.create("crate", R"({"Transform3D": {"position": [0.3, 0.1, 0]},
        "RigidBody3D": {"mass": 0.2}, "CollisionShape3D": {"size": [0.2, 0.2, 0.2]}})");
    scene.create("splash", R"({"Transform3D": {"position": [0, 0.6, 0]},
        "Fluid3D": {"recipe": "water.fluid.json", "fill_size": [0.4, 0.4, 0.4]}})");
    scene.step(90);
    const auto& water = scene.particles("splash");
    REQUIRE(water.size() == 4 * 4 * 4);
    // Nothing fell through the floor.
    CHECK(lowest(water, 1) > -0.01F);
    CHECK(water.contacts > 0);
    // The crate was pushed away from the splash.
    const auto& crate = scene.world.ecs().get<world::Transform3D>(scene.world.find("crate"));
    CHECK(crate.position.x > 0.32F);
}

TEST_CASE("fluid: 2D water lands on a 2D body") {
    Scene scene(true);
    scene.create("ground", R"({"Transform2D": {"position": [0, 200]}, "StaticBody2D": {},
        "CollisionShape2D": {"size": [2000, 20]}})");
    scene.create("splash", R"({"Transform2D": {"position": [0, 100]},
        "Fluid2D": {"recipe": "water2d.fluid.json", "fill_size": [64, 64]}})");
    scene.step(120);
    const auto& water = scene.particles("splash");
    float bottom = -1e30F;
    for (const Vec3f& p : water.position) {
        bottom = std::max(bottom, p.y);
    }
    CHECK(bottom < 192.0F);
    CHECK(bottom > 180.0F);
    CHECK(water.contacts > 0);
}
