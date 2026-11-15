// Oceans, lakes and rivers (slash/world/water.hpp): recipes and the waves a wind raises,
// Gerstner surfaces sampled under a point, ripples and wakes, foam along shores, buoyancy and
// drag in 3D and 2D physics, rivers' currents and liquids falling into water.

#include "slash/asset/assets.hpp"
#include "slash/canvas/canvas.hpp"
#include "slash/core/json.hpp"
#include "slash/core/math.hpp"
#include "slash/physics2d/physics2d.hpp"
#include "slash/physics3d/physics3d.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/fluid.hpp"
#include "slash/world/water.hpp"
#include "slash/world/world.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
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

constexpr std::string_view kCalm = R"({"format": 1, "name": "Pond"})";

constexpr std::string_view kSwell = R"({"format": 1, "name": "Swell", "kind": "ocean",
    "waves": [{"direction": [1, 0], "wavelength": 12, "amplitude": 0.4, "steepness": 0.9},
              {"direction": [0.6, 0.8], "wavelength": 5, "amplitude": 0.12, "steepness": 0.5}]})";

constexpr std::string_view kRiver = R"({"format": 1, "name": "Brook", "kind": "river",
    "flow": 2, "drag": 2})";

constexpr std::string_view kCalm2D = R"({"format": 1, "name": "Pool", "space": "2d"})";

constexpr std::string_view kSwell2D = R"({"format": 1, "name": "Sea", "space": "2d",
    "waves": [{"wavelength": 300, "amplitude": 12, "steepness": 0.6}]})";

constexpr std::string_view kWaterFluid = R"({"format": 1, "name": "Drops",
    "particle_radius": 0.05, "max_particles": 500})";

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

    explicit Scene(bool physics = true) {
        world::register_builtin_components(components);
        files["calm.water.json"] = std::string(kCalm);
        files["swell.water.json"] = std::string(kSwell);
        files["river.water.json"] = std::string(kRiver);
        files["calm2d.water.json"] = std::string(kCalm2D);
        files["swell2d.water.json"] = std::string(kSwell2D);
        files["drops.fluid.json"] = std::string(kWaterFluid);
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

    [[nodiscard]] slash::Vec3 position(std::string_view id) const {
        return world.ecs().get<world::Transform3D>(world.find(id)).position;
    }

    [[nodiscard]] world::WaterBody body(std::string_view id) {
        auto found = world::water_body(world, world.find(id));
        REQUIRE(found.has_value());
        return std::move(*found);
    }

    [[nodiscard]] std::size_t count(std::string_view name) const {
        return static_cast<std::size_t>(std::ranges::count_if(
            events, [&](const world::Event& event) { return event.kind == name; }));
    }
};

float length(Vec3f v) {
    return std::sqrt((v.x * v.x) + (v.y * v.y) + (v.z * v.z));
}

} // namespace

TEST_CASE("water: recipes take their kind's defaults, a wind raises a sea, bad fields fail") {
    auto calm = world::parse_water(parse(kCalm));
    REQUIRE(calm.ok());
    CHECK(calm.value().kind == world::WaterRecipe::Kind::Lake);
    CHECK(calm.value().waves.empty());
    CHECK(calm.value().density == doctest::Approx(1000.0F));
    CHECK(calm.value().flow == 0.0F);

    auto flat = world::parse_water(parse(kCalm2D));
    REQUIRE(flat.ok());
    CHECK(flat.value().flat);
    CHECK(flat.value().density == doctest::Approx(0.001F));
    CHECK(flat.value().ripple_wavelength == doctest::Approx(30.0F));

    auto river = world::parse_water(parse(kRiver));
    REQUIRE(river.ok());
    CHECK(river.value().flow == doctest::Approx(2.0F));

    // An 8 m/s wind: a fully developed sea's peak near 53 m and significant height near 1.4 m.
    auto windy = world::parse_water(parse(R"({"format": 1, "kind": "ocean",
        "wind": {"speed": 8, "count": 6, "seed": 3}})"));
    REQUIRE(windy.ok());
    const auto& waves = windy.value().waves;
    REQUIRE(waves.size() == 6);
    float energy = 0.0F;
    float longest = 0.0F;
    for (const world::WaterWave& wave : waves) {
        energy += wave.amplitude * wave.amplitude;
        longest = std::max(longest, wave.wavelength);
        CHECK(std::abs((wave.direction.x * wave.direction.x) +
                       (wave.direction.y * wave.direction.y) - 1.0F) < 1e-4F);
    }
    CHECK(4.0F * std::sqrt(energy / 2.0F) == doctest::Approx(0.21F * 64.0F / 9.8F).epsilon(0.01));
    CHECK(longest > 53.0F);
    CHECK(longest < 53.0F * 1.8F);
    // The same seed raises the same sea.
    auto again = world::parse_water(parse(R"({"format": 1, "kind": "ocean",
        "wind": {"speed": 8, "count": 6, "seed": 3}})"));
    CHECK(again.value().waves.front().phase == waves.front().phase);

    CHECK_FALSE(world::validate_water(parse(R"({"format": 1, "kind": "pond"})")).empty());
    CHECK_FALSE(world::validate_water(parse(R"({"format": 1, "waves": [{"wavelength": 0,
        "amplitude": 1}]})"))
                    .empty());
    CHECK_FALSE(world::parse_water(parse(R"({"format": 1, "wavez": []})")).ok());
}

TEST_CASE("water: a sampled height is the displaced surface's, under the point asked about") {
    Scene scene(false);
    scene.create("sea", R"({"Transform3D": {"position": [0, 2, 0]},
        "Water3D": {"recipe": "swell.water.json"}})");
    const world::WaterBody sea = scene.body("sea");
    CHECK_FALSE(sea.bounded);
    for (const double time : {0.0, 1.3, 7.9}) {
        for (const float x : {-4.0F, 0.5F, 3.25F}) {
            for (const float z : {-2.0F, 1.5F}) {
                // Where a bit of the surface at rest at (x, z) has moved to, and the height found
                // under that place.
                const world::WaterSample moved = world::displace_water(sea, {x, 2.0F, z}, time);
                const world::WaterSample found =
                    world::sample_water(sea, {moved.position.x, 0.0F, moved.position.z}, time);
                CHECK(found.inside);
                CHECK(found.height == doctest::Approx(moved.position.y).epsilon(0.002));
                CHECK(length(found.normal) == doctest::Approx(1.0F));
                CHECK(found.normal.y > 0.5F);
                CHECK(std::abs(found.height - 2.0F) <= 0.52F + 1e-3F);
            }
        }
    }
    // Sharp crests foam; the surface moves as its waves do.
    float most_foam = 0.0F;
    float fastest = 0.0F;
    for (int i = 0; i < 48; ++i) {
        for (int j = 0; j < 20; ++j) {
            const world::WaterSample s = world::sample_water(
                sea, {static_cast<float>(i) * 0.25F, 0.0F, static_cast<float>(j) * 0.25F}, 0.0);
            most_foam = std::max(most_foam, s.foam);
            fastest = std::max(fastest, length(s.velocity));
        }
    }
    CHECK(most_foam > 0.2F);
    CHECK(fastest > 0.5F);
    CHECK(fastest < 3.0F);
}

TEST_CASE("water: 2D waves raise the surface up the screen, along x") {
    Scene scene(false);
    scene.create("sea", R"({"Transform2D": {"position": [0, 300]},
        "Water2D": {"recipe": "swell2d.water.json", "width": 1200, "depth": 300}})");
    const world::WaterBody sea = scene.body("sea");
    CHECK(sea.flat);
    float highest = 1e9F;
    float lowest = -1e9F;
    for (int i = 0; i < 60; ++i) {
        const float x = -300.0F + (static_cast<float>(i) * 10.0F);
        const world::WaterSample s = world::sample_water(sea, {x, 0.0F, 0.0F}, 0.5);
        CHECK(s.inside);
        CHECK(s.normal.y < 0.0F); // up the screen
        CHECK(s.normal.z == 0.0F);
        highest = std::min(highest, s.height);
        lowest = std::max(lowest, s.height);
        const world::WaterSample moved = world::displace_water(sea, {x, 300.0F, 0.0F}, 0.5);
        const world::WaterSample under =
            world::sample_water(sea, {moved.position.x, 0.0F, 0.0F}, 0.5);
        CHECK(under.height == doctest::Approx(moved.position.y).epsilon(0.001));
    }
    CHECK(highest < 290.0F);
    CHECK(highest > 287.0F);
    CHECK(lowest > 310.0F);
    CHECK_FALSE(world::sample_water(sea, {700.0F, 0.0F, 0.0F}, 0.0).inside);
}

TEST_CASE("water: a ring spreads from a splash and dies away") {
    Scene scene(false);
    const entt::entity pond = scene.create("pond", R"({"Transform3D": {},
        "Water3D": {"recipe": "calm.water.json", "size": [10, 10]}})");
    scene.step(1);
    CHECK(
        world::sample_water(scene.body("pond"), {1.0F, 0.0F, 0.0F}, world::water_time(scene.world))
            .height == doctest::Approx(0.0F));
    world::add_water_ripple(scene.world, pond, {0.0F, 0.0F, 0.0F}, 0.1F);
    CHECK(scene.body("pond").ripples.size() == 1);
    // A half-meter ring spreads at about 0.88 m/s.
    scene.step(60);
    const world::WaterBody body = scene.body("pond");
    const double now = world::water_time(scene.world);
    float on_ring = 0.0F;
    for (int i = 0; i < 40; ++i) {
        const float d = 0.5F + (static_cast<float>(i) * 0.025F);
        on_ring =
            std::max(on_ring, std::abs(world::sample_water(body, {d, 0.0F, 0.0F}, now).height));
    }
    CHECK(on_ring > 0.005F);
    CHECK(std::abs(world::sample_water(body, {4.0F, 0.0F, 0.0F}, now).height) < 1e-4F);
    // Rings left close by at once add to each other.
    world::add_water_ripple(scene.world, pond, {3.0F, 0.0F, 3.0F}, 0.02F);
    world::add_water_ripple(scene.world, pond, {3.1F, 0.0F, 3.0F}, 0.02F);
    CHECK(scene.body("pond").ripples.size() == 2);
    scene.step(60 * 12);
    CHECK(scene.body("pond").ripples.empty());
}

TEST_CASE("water: a crate floats at the depth its weight displaces, and a stone sinks") {
    Scene scene;
    scene.create("pond", R"({"Transform3D": {},
        "Water3D": {"recipe": "calm.water.json", "size": [20, 20]}})");
    // 400 kg in a cubic meter floats 0.4 m deep; 3000 kg doesn't.
    scene.create("crate", R"({"Transform3D": {"position": [0, 1, 0]},
        "RigidBody3D": {"mass": 400}, "CollisionShape3D": {"size": [1, 1, 1]}})");
    scene.create("stone", R"({"Transform3D": {"position": [4, 1, 0]},
        "RigidBody3D": {"mass": 3000}, "CollisionShape3D": {"size": [1, 1, 1]}})");
    scene.step(60);
    // Their splashes left rings.
    CHECK_FALSE(scene.body("pond").ripples.empty());
    scene.step(540);
    const slash::Vec3 crate = scene.position("crate");
    CHECK(crate.y == doctest::Approx(0.1F).epsilon(0.05));
    CHECK(std::abs(crate.x) < 0.05F);
    CHECK(scene.position("stone").y < -3.0F);
    CHECK(scene.count("water_entered") == 2);
}

TEST_CASE("water: a river carries what floats on it along its path, fastest midstream") {
    Scene scene;
    scene.create("course", R"({"Transform3D": {}, "Path3D": {"points": [
        {"position": [0, 0, 0]}, {"position": [30, 0, 0]}]}})");
    scene.create("brook", R"({"Transform3D": {},
        "Water3D": {"recipe": "river.water.json", "path": "course", "width": 6}})");
    scene.create("log", R"({"Transform3D": {"position": [3, 0.2, 0]},
        "RigidBody3D": {"mass": 50}, "CollisionShape3D": {"size": [0.6, 0.4, 0.6]}})");
    const world::WaterBody brook = scene.body("brook");
    REQUIRE(brook.river.size() > 2);
    const world::WaterSample middle = world::sample_water(brook, {10.0F, 0.0F, 0.0F}, 0.0);
    const world::WaterSample bank = world::sample_water(brook, {10.0F, 0.0F, 2.5F}, 0.0);
    CHECK(middle.inside);
    CHECK(bank.inside);
    CHECK(middle.velocity.x == doctest::Approx(2.0F));
    CHECK(bank.velocity.x < middle.velocity.x * 0.5F);
    CHECK_FALSE(world::sample_water(brook, {10.0F, 0.0F, 3.5F}, 0.0).inside);
    CHECK_FALSE(world::sample_water(brook, {-2.0F, 0.0F, 0.0F}, 0.0).inside);
    scene.step(240);
    const slash::Vec3 log = scene.position("log");
    // Carried well over 3 m downstream in 4 s, gathering speed.
    CHECK(log.x > 7.0F);
    CHECK(std::abs(log.y) < 0.3F);
}

TEST_CASE("water: shores are measured under the water and foam where it is shallow") {
    Scene scene;
    // A beach rising from 2 m under the surface at x -5 to the surface at x 5.
    scene.create("beach", R"({"Transform3D": {"position": [0, -1, 0], "rotation": [0, 0, 0.1974]},
        "StaticBody3D": {}, "CollisionShape3D": {"size": [12, 0.1, 12]}})");
    scene.create("bay", R"({"Transform3D": {},
        "Water3D": {"recipe": "calm.water.json", "size": [8, 8]}})");
    scene.step(1);
    const world::WaterBody bay = scene.body("bay");
    REQUIRE_FALSE(bay.ground.empty());
    const world::WaterSample deep = world::sample_water(bay, {-3.0F, 0.0F, 0.0F}, 0.0);
    const world::WaterSample shallow = world::sample_water(bay, {3.0F, 0.0F, 0.0F}, 0.0);
    REQUIRE(std::isfinite(deep.depth));
    CHECK(deep.depth > shallow.depth);
    CHECK(deep.depth == doctest::Approx(1.6F).epsilon(0.1));
    CHECK(shallow.foam > deep.foam);
    CHECK(shallow.foam > 0.0F);
    // Shallow water shows less of the deep color.
    const auto& recipe = *bay.recipe;
    CHECK(world::water_color(recipe, shallow.depth, 0.0F).w <
          world::water_color(recipe, deep.depth, 0.0F).w);
}

TEST_CASE("water: liquid falling into it becomes it and rings where it lands") {
    Scene scene;
    scene.create("pond", R"({"Transform3D": {},
        "Water3D": {"recipe": "calm.water.json", "size": [10, 10]}})");
    scene.create("rain", R"({"Transform3D": {"position": [0, 1, 0]},
        "Fluid3D": {"recipe": "drops.fluid.json", "fill_size": [0.3, 0.3, 0.3]}})");
    scene.step(1);
    const world::FluidParticles* drops =
        world::fluid_particles(scene.world, scene.world.find("rain"));
    REQUIRE(drops != nullptr);
    CHECK(drops->size() == 27);
    scene.step(90);
    CHECK(drops->size() == 0);
    CHECK_FALSE(scene.body("pond").ripples.empty());
}

TEST_CASE("water: a 2D crate floats and its wake rings as it is pushed along") {
    Scene scene;
    scene.create("pool", R"({"Transform2D": {"position": [0, 300]},
        "Water2D": {"recipe": "calm2d.water.json", "width": 2000, "depth": 400}})");
    // 64 by 64 px at 1 kg: 0.001 kg/px^2 holds it with 1 kg / (0.001 * 64) = 15.6 px under.
    scene.create("crate", R"({"Transform2D": {"position": [0, 200]},
        "RigidBody2D": {"mass": 1}, "CollisionShape2D": {"size": [64, 64]}})");
    scene.step(600);
    const auto& crate = scene.world.ecs().get<world::Transform2D>(scene.world.find("crate"));
    CHECK(crate.position.y == doctest::Approx(300.0F - 32.0F + 15.6F).epsilon(0.03));
    CHECK(scene.count("water_entered") == 1);
    auto& rigid = scene.world.ecs().get<world::RigidBody2D>(scene.world.find("crate"));
    rigid.linear_velocity = {300.0F, 0.0F};
    scene.events.clear();
    scene.step(30);
    CHECK(scene.body("pool").ripples.size() >= 2);
    CHECK(crate.position.x > 20.0F);
}

TEST_CASE("water: drawing reads the water without changing it or measuring") {
    Scene scene;
    scene.create("beach", R"({"Transform3D": {"position": [0, -1, 0]},
        "StaticBody3D": {}, "CollisionShape3D": {"size": [12, 0.1, 12]}})");
    scene.create("bay", R"({"Transform3D": {},
        "Water3D": {"recipe": "calm.water.json", "size": [8, 8]}})");
    const world::World& seen = scene.world;
    // Before the water system has run, nothing is measured.
    const std::vector<world::WaterBody> before = world::water_bodies(seen, false);
    REQUIRE(before.size() == 1);
    CHECK(before.front().ground.empty());
    scene.step(1);
    const std::vector<world::WaterBody> after = world::water_bodies(seen, false);
    CHECK_FALSE(after.front().ground.empty());
    // A recipe that can't be read draws nothing and warns once, from the system.
    scene.create("bad", R"({"Transform3D": {}, "Water3D": {"recipe": "missing.water.json"}})");
    CHECK(world::water_bodies(seen, false).size() == 1);
    CHECK(scene.count("water_failed") == 0);
    scene.step(2);
    CHECK(scene.count("water_failed") == 1);
}
