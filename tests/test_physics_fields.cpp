// Physics materials and fields: .physics_material.json files, and what PhysicsField3Ds and
// PhysicsField2Ds do to bodies and particles in 3D and 2D worlds.

#include "slash/asset/assets.hpp"
#include "slash/canvas/particles.hpp"
#include "slash/core/json.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/core/math.hpp"
#include "slash/physics2d/physics2d.hpp"
#include "slash/physics3d/physics3d.hpp"
#include "slash/scene3d/particles.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/physics_field.hpp"
#include "slash/world/physics_material.hpp"
#include "slash/world/tree2d.hpp"
#include "slash/world/tree3d.hpp"
#include "slash/world/world.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace asset = slash::asset;
namespace json = slash::json;
namespace physics2d = slash::physics2d;
namespace physics3d = slash::physics3d;
namespace world = slash::world;
using slash::Vec2;
using slash::Vec3;

namespace {

constexpr double kTick = 1.0 / 60.0;

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE(parsed.ok());
    return std::move(parsed).value();
}

float length(Vec3 v) {
    return std::sqrt((v.x * v.x) + (v.y * v.y) + (v.z * v.z));
}

// A world with files in memory, both physics and the events it emits.
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

    Scene(const physics3d::Settings& settings3d, const physics2d::Settings& settings2d) {
        world::register_builtin_components(components);
        asset::attach(world.ecs(), assets);
        physics3d::install(world, settings3d);
        physics2d::install(world, settings2d);
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
            world.step(kTick);
        }
    }

    template <typename Component>
    Component& get(std::string_view id) {
        return world.ecs().get<Component>(world.find(id));
    }

    [[nodiscard]] bool emitted(std::string_view kind, std::string_view id) const {
        for (const world::Event& event : events) {
            if (event.kind == kind && event.id == id) {
                return true;
            }
        }
        return false;
    }
};

// No gravity or damping, so only fields move things.
Scene still_scene() {
    physics3d::Settings settings3d;
    settings3d.gravity = 0.0F;
    settings3d.linear_damp = 0.0F;
    settings3d.angular_damp = 0.0F;
    physics2d::Settings settings2d;
    settings2d.gravity = 0.0F;
    settings2d.linear_damp = 0.0F;
    settings2d.angular_damp = 0.0F;
    return {settings3d, settings2d};
}

} // namespace

TEST_CASE("physics materials: files read as Godot's PhysicsMaterial with Chaos's additions") {
    const auto defaults = world::parse_physics_material(parse(R"({"format": 1})"));
    REQUIRE(defaults.ok());
    CHECK(defaults.value() == world::PhysicsMaterial{});

    const auto wood = world::parse_physics_material(parse(
        R"({"format": 1, "friction": 0.6, "rough": true, "bounce": 0.25, "absorbent": true,
            "density": 700, "strength": 30, "surface": "wood"})"));
    REQUIRE(wood.ok());
    CHECK(wood.value().friction == doctest::Approx(0.6));
    CHECK(wood.value().density == doctest::Approx(700.0));
    CHECK(wood.value().strength == doctest::Approx(30.0));
    CHECK(wood.value().surface == "wood");
    // Rough and absorbent materials hand physics negative values, as Godot's.
    CHECK(world::computed_friction(wood.value()) == doctest::Approx(-0.6));
    CHECK(world::computed_bounce(wood.value()) == doctest::Approx(-0.25));

    const auto bad = world::parse_physics_material(parse(R"({"format": 1, "bounce": 2})"));
    REQUIRE(!bad.ok());
    CHECK(bad.error().message.starts_with("/bounce"));
    CHECK(!world::parse_physics_material(parse(R"({"format": 1, "frction": 1})")).ok());
    CHECK(!world::parse_physics_material(parse(R"({"friction": 1})")).ok());
    CHECK(world::is_physics_material_path("props/wood.physics_material.json"));
    CHECK(!world::is_physics_material_path("props/wood.material.json"));
}

TEST_CASE("physics fields: reach, falloff and the way each type pushes") {
    Scene scene = still_scene();
    scene.create("blast", R"({"Transform3D": {"position": [0, 0, 0]},
        "PhysicsField3D": {"type": "radial_force", "magnitude": 100, "radius": 10,
                           "falloff": "linear"}})");
    scene.create("wind", R"({"Transform3D": {"rotation": [0, 1.5707963, 0]},
        "PhysicsField3D": {"type": "directional_force", "magnitude": 5, "shape": "everywhere",
                           "collision_mask": 2}})");
    scene.create("crusher", R"({"Transform3D": {"position": [0, 0, 20]},
        "PhysicsField3D": {"type": "strain", "magnitude": 40, "shape": "box",
                           "size": [4, 4, 4]}})");
    scene.create("off", R"({"PhysicsField3D": {"enabled": false, "type": "kill",
                                               "shape": "everywhere"}})");
    const world::Tree3D tree(scene.world);
    const world::PhysicsFields3D fields(scene.world, tree);

    // Halfway out of the sphere on x, a linear falloff gives half the push, outward.
    const world::FieldEffect half = fields.at({5.0F, 0.0F, 0.0F}, 1U, 0U, 0.0);
    CHECK(half.force.x == doctest::Approx(50.0));
    CHECK(half.force.y == doctest::Approx(0.0));
    CHECK(!half.killed);
    // Out of reach, nothing; layer 2 also feels the directional field along the entity's -z,
    // turned a quarter about y to point down -x.
    CHECK(fields.at({11.0F, 0.0F, 0.0F}, 1U, 0U, 0.0).empty());
    const world::FieldEffect blown = fields.at({11.0F, 0.0F, 0.0F}, 2U, 0U, 0.0);
    CHECK(blown.force.x == doctest::Approx(-5.0).epsilon(1e-4));
    CHECK(std::abs(blown.force.z) < 1e-4F);
    // In the box, the strongest strain.
    CHECK(fields.at({1.0F, -1.0F, 21.0F}, 1U, 0U, 0.0).strain == doctest::Approx(40.0));
    CHECK(fields.at({3.0F, 0.0F, 20.0F}, 1U, 0U, 0.0).strain == 0.0F);

    // Noise pushes differ from body to body and move on with time, but repeat exactly.
    scene.create("gust", R"({"PhysicsField3D": {"type": "noise_force", "magnitude": 10,
                                                "shape": "everywhere"}})");
    const world::Tree3D again(scene.world);
    const world::PhysicsFields3D noisy(scene.world, again);
    const Vec3 a = noisy.at({20.0F, 0.0F, 0.0F}, 1U, 1U, 0.5).force;
    CHECK(a == noisy.at({20.0F, 0.0F, 0.0F}, 1U, 1U, 0.5).force);
    CHECK(a != noisy.at({20.0F, 0.0F, 0.0F}, 1U, 2U, 0.5).force);
    CHECK(a != noisy.at({20.0F, 0.0F, 0.0F}, 1U, 1U, 0.9).force);
    CHECK(length(a) > 0.0F);
    CHECK(length(a) <= 10.0F * std::sqrt(3.0F) + 1e-3F);

    // Particles feel only force fields with particles on.
    CHECK(noisy.particle_force({5.0F, 0.0F, 0.0F}, 1U, 0.5).x != 0.0F);
}

TEST_CASE("physics fields: 3D bodies are pushed, held, set moving, put to sleep and removed") {
    Scene scene = still_scene();
    const auto crate = [&](std::string_view id, float x) {
        scene.create(id, R"({"Transform3D": {"position": [)" + std::to_string(x) +
                             R"(, 0, 0]}, "RigidBody3D": {"mass": 2},
            "CollisionShape3D": {"shape": "box", "size": [1, 1, 1]}})");
    };
    crate("pushed", 0.0F);
    crate("held", 20.0F);
    crate("thrown", 40.0F);
    crate("doomed", 60.0F);
    scene.create("push", R"({"Transform3D": {"position": [0, 0, 0]},
        "PhysicsField3D": {"type": "directional_force", "magnitude": 4, "shape": "sphere",
                           "radius": 3, "collision_mask": 1}})");
    scene.create("anchor", R"({"Transform3D": {"position": [20, 0, 0]},
        "PhysicsField3D": {"type": "anchor", "radius": 3}})");
    scene.create("throw", R"({"Transform3D": {"position": [40, 0, 0]},
        "PhysicsField3D": {"type": "radial_velocity", "magnitude": 3, "radius": 3}})");
    scene.create("void", R"({"Transform3D": {"position": [60, 0, 0]},
        "PhysicsField3D": {"type": "kill", "radius": 3}})");
    scene.get<world::RigidBody3D>("held").linear_velocity = {0.0F, 5.0F, 0.0F};

    scene.step(30);
    // 4 newtons on 2 kilograms along -z for half a second: about 1 meter per second.
    CHECK(scene.get<world::RigidBody3D>("pushed").linear_velocity.z ==
          doctest::Approx(-1.0).epsilon(0.05));
    // The anchor held its crate still, whatever its velocity said.
    CHECK(scene.get<world::Transform3D>("held").position.y == doctest::Approx(0.0));
    // The crate at the middle of a radial velocity field gets no way out, so give it one.
    CHECK((scene.world.find("doomed") == entt::null));
    CHECK(scene.emitted("field_killed", "doomed"));

    // A crate off the radial field's middle is set moving outward at its magnitude.
    scene.get<world::Transform3D>("thrown").position = {41.0F, 0.0F, 0.0F};
    scene.step(2);
    CHECK(scene.get<world::RigidBody3D>("thrown").linear_velocity.x ==
          doctest::Approx(3.0).epsilon(0.01));

    // Turning the anchor off lets its crate go.
    scene.get<world::PhysicsField3D>("anchor").enabled = false;
    scene.get<world::RigidBody3D>("held").linear_velocity = {0.0F, 5.0F, 0.0F};
    scene.step(30);
    CHECK(scene.get<world::Transform3D>("held").position.y > 1.0F);

    // A sleep field puts slow crates to sleep.
    scene.create("calm", R"({"Transform3D": {"position": [20, 2.5, 0]},
        "PhysicsField3D": {"type": "sleep", "magnitude": 10, "radius": 5}})");
    scene.step(2);
    CHECK(scene.get<world::RigidBody3D>("held").sleeping);
}

TEST_CASE("physics materials: 3D bodies take friction, bounce, mass and surface from a file") {
    Scene scene = still_scene();
    scene.files["oak.physics_material.json"] =
        R"({"format": 1, "density": 500, "bounce": 0.5, "surface": "wood"})";
    scene.create("plank", R"({"Transform3D": {}, "RigidBody3D": {"mass": 1,
        "physics_material_override": "oak.physics_material.json"},
        "CollisionShape3D": {"shape": "box", "size": [2, 0.5, 1]}})");
    scene.create("field", R"({"PhysicsField3D": {"type": "directional_force",
        "magnitude": 500, "shape": "everywhere"}})");
    scene.step(60);
    // 500 kg a cubic meter times 1 cubic meter weighs 500 kg: 500 newtons for a second.
    CHECK(scene.get<world::RigidBody3D>("plank").linear_velocity.z ==
          doctest::Approx(-1.0).epsilon(0.02));

    const auto hit = physics3d::raycast(scene.world, {0.0F, 0.0F, 10.0F}, {0.0F, 0.0F, -20.0F});
    REQUIRE(hit.has_value());
    CHECK(hit->surface == "wood");

    // A missing file warns and leaves the body's own values.
    scene.create("odd", R"({"Transform3D": {"position": [10, 0, 0]}, "StaticBody3D": {
        "physics_material_override": "missing.physics_material.json"},
        "CollisionShape3D": {"shape": "box", "size": [1, 1, 1]}})");
    scene.step(1);
    const auto plain = physics3d::raycast(scene.world, {10.0F, 5.0F, 0.0F}, {10.0F, -5.0F, 0.0F});
    REQUIRE(plain.has_value());
    CHECK(plain->surface.empty());
}

TEST_CASE("physics fields: 2D bodies are pushed, held and removed, and weigh their material") {
    Scene scene = still_scene();
    scene.files["stone.physics_material.json"] =
        R"({"format": 1, "density": 2, "surface": "stone"})";
    const auto rock = [&](std::string_view id, float x, std::string_view extra) {
        scene.create(id, R"({"Transform2D": {"position": [)" + std::to_string(x) +
                             R"(, 0]}, "RigidBody2D": {"mass": 1)" + std::string(extra) +
                             R"(}, "CollisionShape2D": {"shape": "rectangle", "size": [64, 64]}})");
    };
    rock("pushed", 0.0F, R"(, "physics_material_override": "stone.physics_material.json")");
    rock("held", 1000.0F, "");
    rock("doomed", 2000.0F, "");
    // Along the field's x axis, 2 newtons on a square meter of stone at 2 kg a square meter.
    scene.create("push", R"({"Transform2D": {"position": [0, 0]},
        "PhysicsField2D": {"type": "directional_force", "magnitude": 128, "radius": 200}})");
    scene.create("anchor", R"({"Transform2D": {"position": [1000, 0]},
        "PhysicsField2D": {"type": "anchor", "radius": 200}})");
    scene.create("void", R"({"Transform2D": {"position": [2000, 0]},
        "PhysicsField2D": {"type": "kill", "radius": 200}})");
    scene.get<world::RigidBody2D>("held").linear_velocity = {0.0F, 300.0F};

    scene.step(60);
    // 128 kilogram-pixels per second squared on 2 kilograms for a second: 64 pixels a second.
    CHECK(scene.get<world::RigidBody2D>("pushed").linear_velocity.x ==
          doctest::Approx(64.0).epsilon(0.02));
    CHECK(scene.get<world::Transform2D>("held").position.y == doctest::Approx(0.0));
    CHECK((scene.world.find("doomed") == entt::null));
    CHECK(scene.emitted("field_killed", "doomed"));

    const auto hit = physics2d::raycast(scene.world, {-500.0F, 0.0F}, {500.0F, 0.0F});
    REQUIRE(hit.has_value());
    CHECK(hit->surface == "stone");

    scene.get<world::PhysicsField2D>("anchor").enabled = false;
    scene.get<world::RigidBody2D>("held").linear_velocity = {0.0F, 300.0F};
    scene.step(30);
    CHECK(scene.get<world::Transform2D>("held").position.y > 100.0F);
}

TEST_CASE("physics materials: a rough material's friction wins in 2D, as Godot's") {
    physics2d::Settings settings;
    settings.gravity = 980.0F;
    Scene scene(physics3d::Settings{}, settings);
    scene.files["ice.physics_material.json"] = R"({"format": 1, "friction": 0})";
    scene.files["grit.physics_material.json"] = R"({"format": 1, "friction": 1, "rough": true})";
    scene.create("floor", R"({"Transform2D": {"position": [0, 100]},
        "StaticBody2D": {"physics_material_override": "ice.physics_material.json"},
        "CollisionShape2D": {"shape": "rectangle", "size": [4000, 64]}})");
    const auto slider = [&](std::string_view id, float x, std::string_view material) {
        scene.create(id, R"({"Transform2D": {"position": [)" + std::to_string(x) +
                             R"(, 36]}, "RigidBody2D": {"physics_material_override": ")" +
                             std::string(material) + R"(", "linear_velocity": [200, 0],
            "lock_rotation": true},
            "CollisionShape2D": {"shape": "rectangle", "size": [64, 64]}})");
    };
    slider("skater", -1000.0F, "ice.physics_material.json");
    slider("gritty", 1000.0F, "grit.physics_material.json");
    scene.step(60);
    // On ice the smaller friction wins and the skater slides on; grit's rough friction stops it.
    CHECK(scene.get<world::RigidBody2D>("skater").linear_velocity.x > 150.0F);
    CHECK(std::abs(scene.get<world::RigidBody2D>("gritty").linear_velocity.x) < 5.0F);
}

TEST_CASE("physics fields: strain breaks a Destructible3D's body") {
    Scene scene = still_scene();
    scene.create("glass", R"({"Transform3D": {"position": [0, 1, 0]}, "StaticBody3D": {},
        "CollisionShape3D": {"shape": "box", "size": [2, 2, 0.1]},
        "BoxMesh": {"size": [2, 2, 0.1]}, "MeshInstance3D": {},
        "Destructible3D": {"pieces": 4, "damage_threshold": 50}})");
    scene.create("press", R"({"Transform3D": {"position": [0, 1, 0]},
        "PhysicsField3D": {"type": "strain", "magnitude": 30, "radius": 2}})");
    scene.step(2);
    CHECK((scene.world.find("glass") != entt::null));
    scene.get<world::PhysicsField3D>("press").magnitude = 80.0F;
    scene.step(2);
    CHECK((scene.world.find("glass") == entt::null));
    std::size_t pieces = 0;
    for (const entt::entity piece : scene.world.ecs().view<const world::FracturePiece3D>()) {
        CHECK(scene.world.id_of(piece).starts_with("glass_"));
        ++pieces;
    }
    CHECK(pieces >= 2);
}

TEST_CASE("physics fields: particles with particles on are pushed, in 3D and 2D") {
    Scene scene = still_scene();
    slash::scene3d::add_particles_system(scene.world);
    slash::canvas::add_particles_system(scene.world);
    const std::string emitter3d = R"({"Transform3D": {}, "CPUParticles3D": {"amount": 4,
        "lifetime": 2, "explosiveness": 1, "gravity": [0, 0, 0],
        "initial_velocity_min": 0, "initial_velocity_max": 0, "spread": 0}})";
    scene.create("sparks", emitter3d);
    scene.create("sparks2d", R"({"Transform2D": {}, "CPUParticles2D": {"amount": 4,
        "lifetime": 2, "explosiveness": 1, "gravity": [0, 0],
        "initial_velocity_min": 0, "initial_velocity_max": 0, "spread": 0}})");
    scene.create("up", R"({"Transform3D": {"rotation": [1.5707963, 0, 0]},
        "PhysicsField3D": {"type": "directional_force", "magnitude": 6, "shape": "everywhere"}})");
    scene.create("right", R"({"PhysicsField2D": {"type": "directional_force",
        "magnitude": 60, "shape": "everywhere"}})");
    scene.step(30);
    const auto sparks = slash::scene3d::particles_3d(scene.world, scene.world.find("sparks"));
    REQUIRE(!sparks.empty());
    for (const auto& spark : sparks) {
        // -z turned a quarter about x points up.
        CHECK(spark.velocity.y > 2.0F);
    }
    const auto dots = slash::canvas::particles(scene.world, scene.world.find("sparks2d"));
    REQUIRE(!dots.empty());
    for (const auto& dot : dots) {
        CHECK(dot.velocity.x > 20.0F);
        CHECK(std::abs(dot.velocity.y) < 1e-3F);
    }

    // A field with particles off leaves them alone.
    scene.get<world::PhysicsField2D>("right").particles = false;
    const float before =
        slash::canvas::particles(scene.world, scene.world.find("sparks2d")).front().velocity.x;
    scene.step(10);
    CHECK(slash::canvas::particles(scene.world, scene.world.find("sparks2d")).front().velocity.x ==
          doctest::Approx(before));
}

TEST_CASE("physics settings: substeps") {
    CHECK(physics3d::parse_settings(parse(R"({"substeps": 4})")).substeps == 4);
    CHECK(physics3d::parse_settings(parse("{}")).substeps == 1);
    CHECK(physics2d::parse_settings(parse(R"({"substeps": 8})")).substeps == 8);
    CHECK(physics2d::parse_settings(parse("{}")).substeps == 4);
    CHECK(!json::validate(parse(R"({"substeps": 0})"), physics3d::settings_schema()).empty());
    CHECK(!json::validate(parse(R"({"substeps": 17})"), physics2d::settings_schema()).empty());

    // More substeps settle a fast ball on a floor as well; the result is the same each run.
    for (const int substeps : {1, 4}) {
        physics3d::Settings settings;
        settings.substeps = substeps;
        Scene scene(settings, physics2d::Settings{});
        scene.create("floor", R"({"Transform3D": {"position": [0, -0.5, 0]}, "StaticBody3D": {},
            "CollisionShape3D": {"shape": "box", "size": [20, 1, 20]}})");
        scene.create("ball", R"({"Transform3D": {"position": [0, 3, 0]}, "RigidBody3D": {},
            "CollisionShape3D": {"shape": "sphere", "radius": 0.25}})");
        scene.step(120);
        CHECK(scene.get<world::Transform3D>("ball").position.y ==
              doctest::Approx(0.25).epsilon(0.05));
    }
}
