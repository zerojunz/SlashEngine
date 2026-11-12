// Visual effects as .vfx.json recipes (slash/world/vfx.hpp): layers on a timeline played as
// entities under a VisualEffect3D or VisualEffect2D, curves, expressions over parameters,
// levels of detail, budgets, camera shakes, and the vfx.* commands.

#include "slash/asset/assets.hpp"
#include "slash/canvas/canvas.hpp"
#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/core/math.hpp"
#include "slash/physics2d/physics2d.hpp"
#include "slash/physics3d/physics3d.hpp"
#include "slash/platform/file.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/tree3d.hpp"
#include "slash/world/vfx.hpp"
#include "slash/world/world.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace asset = slash::asset;
namespace canvas = slash::canvas;
namespace command = slash::command;
namespace json = slash::json;
namespace physics2d = slash::physics2d;
namespace physics3d = slash::physics3d;
namespace world = slash::world;
using slash::Vec2;
using slash::Vec3;

namespace {

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE(parsed.ok());
    return std::move(parsed).value();
}

// An explosion: a flash of light that fades, sparks, smoke only near, a scorch left where it
// went off, and a shake.
constexpr std::string_view kBoom = R"({"format": 1, "name": "Boom", "duration": 1.5,
    "parameters": {"intensity": 1, "color": [1, 0.6, 0.2, 1]},
    "layers": [
        {"name": "flash", "duration": 0.3,
         "components": {"OmniLight3D": {"light_color": "=color", "omni_range": 6}},
         "curves": {"OmniLight3D/light_energy": [[0, "=8 * intensity"], [0.3, 0]]}},
        {"name": "sparks", "duration": 0.5,
         "components": {"CPUParticles3D": {"amount": "=40 * intensity", "lifetime": 0.6,
                                           "explosiveness": 1}}},
        {"name": "smoke", "start": 0.2, "detail": 0.75, "offset": [0, 0.5, 0],
         "components": {"CPUParticles3D": {"amount": 30, "lifetime": 1}}},
        {"name": "scorch", "start": 0.1, "world": true,
         "components": {"Decal": {"size": [2, 1, 2]}}},
        {"name": "shake", "duration": 0.4,
         "components": {"CameraShake": {"strength": 0.2, "frequency": 20}},
         "curves": {"CameraShake/strength": {"keys": [[0, 0.2], [0.4, 0]], "ease": "smooth"}}}],
    "lod": [{"distance": 30, "detail": 0.5}, {"distance": 80, "detail": 0.2}],
    "budget": {"particles": 60, "lights": 1}})";

// A world with files in memory and every canvas system.
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

    Scene() {
        world::register_builtin_components(components);
        canvas::install(world, assets);
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

    [[nodiscard]] bool has(std::string_view id) const { return world.find(id) != entt::null; }

    [[nodiscard]] int count(std::string_view kind) const {
        return static_cast<int>(std::ranges::count_if(
            events, [&](const world::Event& event) { return event.kind == kind; }));
    }
};

std::vector<std::string> messages(const std::vector<json::ValidationError>& errors) {
    std::vector<std::string> out;
    out.reserve(errors.size());
    for (const auto& error : errors) {
        out.push_back(error.path + ": " + error.message);
    }
    return out;
}

} // namespace

TEST_CASE("vfx: recipes parse, and validation finds expressions, names and keys out of order") {
    const json::Value doc = parse(kBoom);
    CHECK(world::validate_vfx(doc).empty());
    auto recipe = world::parse_vfx(doc);
    REQUIRE(recipe.ok());
    const world::VfxRecipe& boom = recipe.value();
    CHECK(boom.name == "Boom");
    CHECK(boom.dimension == "3d");
    CHECK(boom.duration == doctest::Approx(1.5));
    REQUIRE(boom.layers.size() == 5);
    CHECK(boom.layers[1].name == "sparks");
    CHECK(boom.layers[2].detail == doctest::Approx(0.75));
    CHECK(boom.layers[3].world);
    REQUIRE(boom.layers[4].curves.size() == 1);
    CHECK(boom.layers[4].curves[0].ease == "smooth");
    CHECK(boom.lod.size() == 2);
    CHECK(boom.budget.particles == 60);
    // A layer that lasts to the end lasts from its start.
    CHECK(world::vfx_layer_length(boom, boom.layers[2]) == doctest::Approx(1.3));

    const auto bad = messages(world::validate_vfx(parse(R"({"format": 1, "layers": [
        {"name": "a", "components": {"OmniLight3D": {"light_energy": "=2 *"}}},
        {"name": "a", "components": {},
         "curves": {"OmniLight3D/light_energy": [[0.5, 1], [0.2, 0]]}}],
        "lod": [{"distance": 50, "detail": 0.5}, {"distance": 10, "detail": 0.2}]})")));
    REQUIRE(bad.size() == 4);
    CHECK(bad[0].starts_with("/layers/1/name: another layer"));
    CHECK(bad[1].starts_with("/layers/1/curves/OmniLight3D~1light_energy: keys"));
    CHECK(bad[2].starts_with("/lod/1: levels go nearest first"));
    CHECK(bad[3].starts_with("/layers/0/components/OmniLight3D/light_energy: at character 4"));
    CHECK_FALSE(world::validate_vfx(parse(R"({"format": 1, "layers": [
        {"name": "bad name", "components": {}}]})"))
                    .empty());
}

TEST_CASE("vfx: values work out over parameters, and particles thin with detail") {
    auto recipe = world::parse_vfx(parse(kBoom));
    REQUIRE(recipe.ok());
    const world::VfxRecipe& boom = recipe.value();
    const json::Value params = world::vfx_parameters(boom, parse(R"({"intensity": 2})"));
    CHECK(params.find("intensity")->as_double() == doctest::Approx(2.0));
    CHECK(params.find("color")->as_array().size() == 4);
    // Whole numbers come out as integers, so integer fields take them.
    auto sixteen = world::vfx_value(json::Value("=8 * intensity"), params, 1.0F);
    REQUIRE(sixteen.ok());
    CHECK(sixteen.value().is_integral());
    CHECK(sixteen.value().as_int() == 16);
    auto mixed =
        world::vfx_value(parse(R"([1, "=detail", {"x": "=intensity / 4.0"}])"), params, 0.5F);
    REQUIRE(mixed.ok());
    CHECK(mixed.value().as_array()[1].as_double() == doctest::Approx(0.5));
    CHECK(mixed.value().as_array()[2].find("x")->as_double() == doctest::Approx(0.5));
    CHECK_FALSE(world::vfx_value(json::Value("=nope + 1"), params, 1.0F).ok());

    // Particles' amount scales by detail, never below one.
    auto full = world::vfx_components(boom.layers[1], params, 1.0F);
    REQUIRE(full.ok());
    CHECK(full.value().find("CPUParticles3D")->find("amount")->as_int() == 80);
    auto half = world::vfx_components(boom.layers[1], params, 0.5F);
    REQUIRE(half.ok());
    CHECK(half.value().find("CPUParticles3D")->find("amount")->as_int() == 40);
    auto none = world::vfx_components(boom.layers[1], params, 0.0F);
    REQUIRE(none.ok());
    CHECK(none.value().find("CPUParticles3D")->find("amount")->as_int() == 1);

    // Curves: linear from 16 to 0 over 0.3 s; smooth eases at its ends.
    auto energy = world::vfx_curve_value(boom.layers[0].curves[0], 0.15F, 0.3F, params, 1.0F);
    REQUIRE(energy.ok());
    CHECK(energy.value().as_double() == doctest::Approx(8.0));
    auto shake = world::vfx_curve_value(boom.layers[4].curves[0], 0.1F, 0.4F, params, 1.0F);
    REQUIRE(shake.ok());
    CHECK(shake.value().as_double() > 0.15); // smoothstep(0.25) is 0.156 of the way down
    CHECK(shake.value().as_double() < 0.2);

    // Levels of detail, nearest first.
    CHECK(world::vfx_detail(boom, 0.0F) == doctest::Approx(1.0));
    CHECK(world::vfx_detail(boom, 40.0F) == doctest::Approx(0.5));
    CHECK(world::vfx_detail(boom, 100.0F) == doctest::Approx(0.2));
}

TEST_CASE("vfx: cost is the busiest moment, and the budget says what goes past it") {
    auto recipe = world::parse_vfx(parse(kBoom));
    REQUIRE(recipe.ok());
    const world::VfxRecipe& boom = recipe.value();
    // At 0.2 s the sparks and the smoke play together, with the flash, the scorch and the shake.
    auto cost = world::vfx_cost(boom, world::vfx_parameters(boom, json::Object{}), 1.0F);
    REQUIRE(cost.ok());
    CHECK(cost.value().particles == 70);
    CHECK(cost.value().lights == 1);
    CHECK(cost.value().decals == 1);
    CHECK(cost.value().layers == 5);
    CHECK(world::vfx_over_budget(boom, cost.value()) ==
          std::vector<std::string>{"particles: 70 of 60"});
    // Far off the smoke drops and the sparks thin.
    auto far = world::vfx_cost(boom, world::vfx_parameters(boom, json::Object{}), 0.5F);
    REQUIRE(far.ok());
    CHECK(far.value().particles == 20);
    CHECK(far.value().layers == 4);
    CHECK(world::vfx_over_budget(boom, far.value()).empty());
}

TEST_CASE("vfx: an effect plays its layers as entities under it on its timeline") {
    Scene scene;
    scene.files["boom.vfx.json"] = std::string(kBoom);
    scene.create("camera", R"({"Transform3D": {"position": [0, 1, 10]},
                               "Camera3D": {"current": true}})");
    const entt::entity boom = scene.create("boom", R"({"Transform3D": {"position": [2, 0, 0]},
                    "VisualEffect3D": {"recipe": "boom.vfx.json",
                                       "parameters": {"intensity": 2}}})");
    const auto& ecs = scene.world.ecs();
    scene.step(1);
    CHECK(scene.count("vfx_started") == 1);
    const entt::entity flash = world::vfx_layer_entity(scene.world, boom, "flash");
    REQUIRE((flash != entt::null));
    CHECK(scene.world.id_of(flash) == "boom_flash");
    CHECK(ecs.get<const world::VfxPart>(flash).effect == "boom");
    // Under the effect, so it moves with it.
    const auto& children = ecs.get<const world::Transform3D>(boom).children;
    CHECK(std::ranges::find(children, flash) != children.end());
    CHECK(world::Tree3D(scene.world).global(flash).origin.x == doctest::Approx(2.0));
    // Its curve sets the light's energy from the parameters: 16 fading to 0 over 0.3 s.
    CHECK(ecs.get<const world::OmniLight3D>(flash).light_energy ==
          doctest::Approx(16.0F * (1.0F - (1.0F / 60.0F / 0.3F))).epsilon(0.01));
    CHECK(ecs.get<const world::OmniLight3D>(flash).light_color.x == doctest::Approx(1.0));
    const entt::entity sparks = world::vfx_layer_entity(scene.world, boom, "sparks");
    REQUIRE((sparks != entt::null));
    CHECK(ecs.get<const world::CPUParticles3D>(sparks).amount == 80);
    CHECK_FALSE(scene.has("boom_smoke"));

    scene.step(12); // 0.22 s: the smoke and the scorch have come
    REQUIRE(scene.has("boom_smoke"));
    CHECK(world::Tree3D(scene.world).global(scene.world.find("boom_smoke")).origin.y ==
          doctest::Approx(0.5));
    // The scorch stays where the effect went off: placed in the world, not under it.
    const entt::entity scorch = scene.world.find("boom_scorch");
    REQUIRE((scorch != entt::null));
    CHECK(ecs.get<const world::Transform3D>(scorch).top_level);
    CHECK(ecs.get<const world::Transform3D>(scorch).position.x == doctest::Approx(2.0));

    scene.step(6); // 0.32 s: the flash has gone
    CHECK_FALSE(scene.has("boom_flash"));
    CHECK(std::ranges::find(ecs.get<const world::Transform3D>(boom).children, flash) ==
          ecs.get<const world::Transform3D>(boom).children.end());
    // The sparks stop emitting at their end, and their particles finish before they go.
    scene.step(12); // 0.52 s
    REQUIRE(scene.has("boom_sparks"));
    CHECK_FALSE(ecs.get<const world::CPUParticles3D>(sparks).emitting);
    CHECK(ecs.get<const world::VfxPart>(sparks).linger > 0.0F);
    CHECK((world::vfx_layer_entity(scene.world, boom, "sparks") == entt::null));
    scene.step(42); // 1.22 s
    CHECK_FALSE(scene.has("boom_sparks"));

    scene.step(18); // 1.52 s: done
    CHECK(scene.count("vfx_finished") == 1);
    CHECK_FALSE(ecs.get<const world::VisualEffect3D>(boom).playing);
    CHECK(ecs.get<const world::VisualEffect3D>(boom).time == doctest::Approx(1.5));
    CHECK_FALSE(scene.has("boom_scorch"));
    CHECK(scene.has("boom_smoke")); // its smoke still drifts
    scene.step(70);
    CHECK_FALSE(scene.has("boom_smoke"));
    CHECK(scene.has("boom")); // not freed: free_on_finish is off
    CHECK(scene.count("vfx_finished") == 1);
}

TEST_CASE("vfx: far from the camera layers drop and particles thin; one-shots free themselves") {
    Scene scene;
    scene.files["boom.vfx.json"] = std::string(kBoom);
    scene.create("camera", R"({"Transform3D": {"position": [0, 0, 40]},
                               "Camera3D": {"current": true}})");
    const entt::entity boom = scene.create("boom", R"({"Transform3D": {},
                    "VisualEffect3D": {"recipe": "boom.vfx.json", "free_on_finish": true}})");
    scene.step(20);
    const auto& ecs = scene.world.ecs();
    CHECK_FALSE(scene.has("boom_smoke")); // needs 0.75 detail, and 40 m gives 0.5
    const entt::entity sparks = world::vfx_layer_entity(scene.world, boom, "sparks");
    REQUIRE((sparks != entt::null));
    CHECK(ecs.get<const world::CPUParticles3D>(sparks).amount == 20);
    // lod_bias reaches further: at 4, 40 m counts as 10.
    scene.world.ecs().get<world::VisualEffect3D>(boom).lod_bias = 4.0F;
    scene.step(1);
    CHECK(scene.has("boom_smoke"));

    scene.step(100); // past its end: its smoke still drifts, so it waits
    CHECK(scene.count("vfx_finished") == 1);
    CHECK(scene.has("boom"));
    scene.step(70); // past its particles' life
    CHECK_FALSE(scene.has("boom_smoke"));
    CHECK_FALSE(scene.has("boom"));
}

TEST_CASE("vfx: a looping effect starts its layers again each cycle with new seeds") {
    Scene scene;
    scene.files["torch.vfx.json"] = R"({"format": 1, "duration": 0.5, "loop": true,
        "layers": [{"name": "embers", "duration": 0.25,
                    "components": {"CPUParticles3D": {"amount": 4, "lifetime": 0.1, "seed": 3}}}]})";
    const entt::entity torch = scene.create("torch", R"({"Transform3D": {},
                     "VisualEffect3D": {"recipe": "torch.vfx.json", "seed": 10}})");
    const auto& ecs = scene.world.ecs();
    scene.step(1);
    const entt::entity first = world::vfx_layer_entity(scene.world, torch, "embers");
    REQUIRE((first != entt::null));
    CHECK(ecs.get<const world::CPUParticles3D>(first).seed == 13);
    scene.step(30); // into the second cycle
    const entt::entity second = world::vfx_layer_entity(scene.world, torch, "embers");
    REQUIRE((second != entt::null));
    CHECK((second != first));
    CHECK(ecs.get<const world::VfxPart>(second).cycle == 1);
    CHECK(ecs.get<const world::CPUParticles3D>(second).seed == 13 + 7919);
    CHECK(scene.count("vfx_finished") == 0);
    CHECK(ecs.get<const world::VisualEffect3D>(torch).playing);
    // Stopped, it plays nothing more; its embers finish.
    scene.world.ecs().get<world::VisualEffect3D>(torch).playing = false;
    scene.step(30);
    CHECK((world::vfx_layer_entity(scene.world, torch, "embers") == entt::null));
}

TEST_CASE("vfx: a 2D effect plays in pixels, flashes the screen and shakes the camera") {
    Scene scene;
    scene.files["hit.vfx.json"] = R"({"format": 1, "dimension": "2d", "duration": 0.5,
        "layers": [
            {"name": "sparks", "duration": 0.2, "offset": [10, 0],
             "components": {"CPUParticles2D": {"amount": 12, "lifetime": 0.3}}},
            {"name": "glow", "duration": 0.3,
             "components": {"PointLight2D": {"energy": 2}},
             "curves": {"PointLight2D/energy": [[0, 2], [0.3, 0]]}},
            {"name": "flash", "duration": 0.2,
             "components": {"Control": {"anchor_right": 1, "anchor_bottom": 1},
                            "ColorRect": {"color": [1, 1, 1, 0.8]}},
             "curves": {"ColorRect/color/a": [[0, 0.8], [0.2, 0]]}},
            {"name": "shake", "duration": 0.3,
             "components": {"CameraShake": {"strength": 6, "frequency": 25}}}]})";
    const entt::entity camera =
        scene.create("camera", R"({"Transform2D": {}, "Camera2D": {"offset": [5, 0]}})");
    const entt::entity hit = scene.create("hit", R"({"Transform2D": {"position": [100, 50]},
                   "VisualEffect2D": {"recipe": "hit.vfx.json"}})");
    const auto& ecs = scene.world.ecs();
    scene.step(6);
    const entt::entity sparks = world::vfx_layer_entity(scene.world, hit, "sparks");
    REQUIRE((sparks != entt::null));
    CHECK(ecs.get<const world::Transform2D>(sparks).position.x == doctest::Approx(10.0));
    CHECK(ecs.get<const world::CPUParticles2D>(sparks).amount == 12);
    const entt::entity glow = world::vfx_layer_entity(scene.world, hit, "glow");
    REQUIRE((glow != entt::null));
    CHECK(ecs.get<const world::PointLight2D>(glow).energy == doctest::Approx(4.0 / 3.0));
    // The flash is a control on the screen, not placed in the world.
    const entt::entity flash = world::vfx_layer_entity(scene.world, hit, "flash");
    REQUIRE((flash != entt::null));
    CHECK_FALSE(ecs.all_of<world::Transform2D>(flash));
    CHECK(ecs.get<const world::ColorRect>(flash).color.w == doctest::Approx(0.4));
    // The shake moves the camera off its own offset, and puts it back after.
    const Vec2 shaken = ecs.get<const world::Camera2D>(camera).offset;
    CHECK((shaken.x != 5.0F || shaken.y != 0.0F));
    const world::CameraShakeOffsets offsets = world::camera_shake_offsets(scene.world);
    CHECK(shaken.x - offsets.camera_2d.x == doctest::Approx(5.0));
    CHECK(std::abs(offsets.camera_2d.x) <= 6.0F);
    scene.step(20);
    CHECK(ecs.get<const world::Camera2D>(camera).offset.x == doctest::Approx(5.0));
    CHECK(ecs.get<const world::Camera2D>(camera).offset.y == doctest::Approx(0.0));
    CHECK_FALSE(scene.has("hit_flash"));
}

TEST_CASE("vfx: damage profiles play an effect where hits land, as hard as they hit") {
    Scene scene;
    physics3d::install(scene.world);
    physics2d::install(scene.world);
    scene.files["sparks.vfx.json"] = R"vfx({"format": 1, "duration": 0.3,
        "parameters": {"strength": 1},
        "layers": [{"name": "burst", "components": {"CPUParticles3D": {
            "amount": "=round(30 * strength)", "lifetime": 0.2, "one_shot": true}}}]})vfx";
    scene.files["chips.vfx.json"] = R"({"format": 1, "dimension": "2d", "duration": 0.2,
        "layers": [{"name": "burst", "components": {"CPUParticles2D": {"amount": 6}}}]})";
    scene.files["metal.damage.json"] = R"({"format": 1, "material": "metal", "health": 1000,
        "deform": {"dent": 0, "hole": 0, "carve": 0},
        "effects": {"hit_scale": 20, "hit": {"debris": {"amount": 0}, "dust": {"amount": 0},
                                             "vfx": "sparks.vfx.json"}}})";
    scene.files["wood.damage.json"] = R"({"format": 1, "material": "wood", "health": 1000,
        "deform": {"dent": 0, "hole": 0, "carve": 0},
        "effects": {"hit": {"debris": {"amount": 0}, "dust": {"amount": 0},
                            "vfx": "chips.vfx.json"}}})";
    const entt::entity plate = scene.create("plate", R"({
        "Transform3D": {"position": [0, 0.5, 0]}, "StaticBody3D": {},
        "CollisionShape3D": {"shape": "box", "size": [1, 1, 1]},
        "BoxMesh": {"size": [1, 1, 1]}, "MeshInstance3D": {},
        "Destructible3D": {"profile": "metal.damage.json"}})");
    const entt::entity crate = scene.create("crate", R"({
        "Transform2D": {"position": [300, 0]}, "StaticBody2D": {},
        "CollisionShape2D": {"shape": "rectangle", "size": [64, 64]},
        "Polygon2D": {"polygon": [[-32, -32], [32, -32], [32, 32], [-32, 32]]},
        "Destructible2D": {"profile": "wood.damage.json"}})");
    scene.step(1);
    auto hit = physics3d::damage_body(scene.world, plate, 10.0F, "blunt", Vec3{0, 0.5F, 0.5F});
    REQUIRE(hit.ok());
    REQUIRE(std::ranges::find(hit.value().effects, "plate_vfx") != hit.value().effects.end());
    const auto& ecs = scene.world.ecs();
    const entt::entity effect = scene.world.find("plate_vfx");
    REQUIRE((effect != entt::null));
    // As hard as it hit, after the metal's resistance: under hit_scale, less than full.
    const double strength =
        ecs.get<const world::VisualEffect3D>(effect).parameters.find("strength")->as_double();
    CHECK(strength > 0.0);
    CHECK(strength < 1.0);
    CHECK(ecs.get<const world::Transform3D>(effect).position.z == doctest::Approx(0.5));
    scene.step(1);
    const entt::entity burst = world::vfx_layer_entity(scene.world, effect, "burst");
    REQUIRE((burst != entt::null));
    CHECK(ecs.get<const world::CPUParticles3D>(burst).amount == std::lround(30.0 * strength));
    auto chipped =
        physics2d::damage_body_2d(scene.world, crate, 10.0F, "blunt", Vec2{332.0F, 0.0F});
    REQUIRE(chipped.ok());
    CHECK(scene.has("crate_vfx"));
    CHECK(ecs.all_of<world::VisualEffect2D>(scene.world.find("crate_vfx")));
    // Both free themselves once played and their particles have gone.
    scene.step(90);
    CHECK_FALSE(scene.has("plate_vfx"));
    CHECK_FALSE(scene.has("crate_vfx"));
    CHECK(scene.count("vfx_failed") == 0);
}

TEST_CASE("vfx: an effect that can't play says so once") {
    Scene scene;
    scene.files["flat.vfx.json"] = R"({"format": 1, "dimension": "2d",
        "layers": [{"name": "a", "components": {"PointLight2D": {}}}]})";
    scene.create("lost", R"({"Transform3D": {}, "VisualEffect3D": {"recipe": "nope.vfx.json"}})");
    scene.create("wrong", R"({"Transform3D": {}, "VisualEffect3D": {"recipe": "flat.vfx.json"}})");
    scene.create("odd", R"({"Transform2D": {}, "VisualEffect2D": {"recipe": "flat.vfx.json"}})");
    scene.files["odd.vfx.json"] = "";
    scene.step(5);
    CHECK(scene.count("vfx_failed") == 2);
    const auto wrong = std::ranges::find_if(scene.events, [](const world::Event& event) {
        return event.kind == "vfx_failed" && event.id == "wrong";
    });
    REQUIRE(wrong != scene.events.end());
    CHECK(wrong->data.find("error")->as_string().find("2d effect") != std::string::npos);
    CHECK(scene.has("odd_a"));
}

namespace {

struct Project {
    std::string root;
    world::ComponentRegistry components;
    command::Registry commands;
    std::unique_ptr<command::Session> session;

    explicit Project(std::string name) : root(std::move(name)) {
        std::filesystem::remove_all(root);
        const std::map<std::string, std::string_view> files{
            {"fx/boom.vfx.json", kBoom},
            {"fx/broken.vfx.json", R"({"format": 1, "duration": 1, "layers": [
                {"name": "late", "start": 2, "components": {"OmniLigth3D": {}}},
                {"name": "fine", "detail": 1, "components": {"OmniLight3D": {}},
                 "curves": {"SpotLight3D/light_energy": [[0, 1]]}}],
                "lod": [{"distance": 0, "detail": 0.5}]})"},
            {"main.scene.json", R"({"format": 1, "entities": {
                "camera": {"components": {"Transform3D": {"position": [0, 1, 10]},
                                          "Camera3D": {"current": true}}},
                "hero": {"components": {"Transform3D": {"position": [4, 0, 0]}}},
                "torch": {"components": {"Transform3D": {},
                    "VisualEffect3D": {"recipe": "fx/missing.vfx.json"}}}}})"}};
        for (const auto& [file, text] : files) {
            const std::string path = root + "/" + file;
            REQUIRE(slash::platform::create_directories(
                        std::filesystem::path(path).parent_path().string())
                        .ok());
            REQUIRE(slash::platform::write_file(path, text).ok());
        }
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

    json::Value ok(std::string_view name, std::string_view params = "{}") const {
        command::Reply reply = session->call(name, parse(params));
        REQUIRE_MESSAGE(reply.ok, reply.message);
        return std::move(reply.value);
    }

    command::Reply call(std::string_view name, std::string_view params) const {
        return session->call(name, parse(params));
    }
};

} // namespace

TEST_CASE("vfx commands: play an effect, read its state, check recipes and preview one") {
    Project project("command_vfx_project");
    // Played at a point, named after its file.
    const json::Value played =
        project.ok("vfx.play", R"({"recipe": "fx/boom.vfx.json", "at": [0, 0, -2],
                                   "parameters": {"intensity": 0.5}})");
    CHECK(played.find("id")->as_string() == "boom");
    CHECK(played.find("layers")->as_int() == 5);
    // Again under the hero, named apart.
    CHECK(project
              .ok("vfx.play", R"({"recipe": "fx/boom.vfx.json", "parent": "hero",
                                     "free_on_finish": false})")
              .find("id")
              ->as_string() == "boom_2");
    const auto bad_param = project.call("vfx.play", R"({"recipe": "fx/boom.vfx.json",
                                                        "parameters": {"intensty": 1}})");
    CHECK_FALSE(bad_param.ok);
    CHECK(bad_param.message.find("did you mean \"intensity\"") != std::string::npos);
    CHECK_FALSE(project.call("vfx.play", R"({"recipe": "fx/boom.vfx.json", "at": [1, 2]})").ok);
    CHECK_FALSE(project.call("vfx.play", R"({"recipe": "fx/boom.vfx.json", "bone": "hand"})").ok);
    CHECK_FALSE(project.call("vfx.play", R"({"recipe": "fx/boom.vfx.json", "parent": "heor"})").ok);

    // One that frees itself under the hero leaves the hero's children as they were.
    project.ok("vfx.play", R"({"recipe": "fx/boom.vfx.json", "parent": "hero", "id": "burst"})");
    project.ok("step", R"({"ticks": 3})");
    const json::Value state = project.ok("vfx.state", R"({"ids": ["boom"]})");
    const json::Value* boom = state.find("effects")->find("boom");
    REQUIRE(boom != nullptr);
    CHECK(boom->find("playing")->as_bool());
    CHECK(boom->find("detail")->as_double() == doctest::Approx(1.0));
    CHECK(boom->find("layers")->find("flash")->as_string() == "boom_flash");
    CHECK(boom->find("layers")->find("smoke") == nullptr);
    CHECK(state.find("shake_3d") != nullptr);
    CHECK(project.ok("vfx.state").find("effects")->find("torch")->find("error") != nullptr);

    // The one-shot played by the command frees itself; the one under the hero stays.
    project.ok("step", R"({"ticks": 200})");
    CHECK(project.ok("vfx.state").find("effects")->find("boom") == nullptr);
    CHECK(project.ok("vfx.state").find("effects")->find("burst") == nullptr);
    const world::World& w = project.session->world();
    CHECK(w.ecs().get<const world::Transform3D>(w.find("hero")).children.size() == 1);
    CHECK(project.ok("vfx.state").find("effects")->find("boom_2")->find("finished")->as_bool());

    // Checks: the world's broken effect, the cost against the budget, and the broken recipe.
    const json::Value checked = project.ok("vfx.check");
    const json::Value* recipe = checked.find("recipes")->find("fx/boom.vfx.json");
    REQUIRE(recipe != nullptr);
    CHECK(recipe->find("cost")->find("particles")->as_int() == 70);
    CHECK(recipe->find("over_budget")->as_array().front().as_string() == "particles: 70 of 60");
    CHECK(recipe->find("lod")->as_array().size() == 2);
    REQUIRE(checked.find("effects") != nullptr);
    CHECK(checked.find("effects")->as_array().front().find("id")->as_string() == "torch");
    const json::Value broken = project.ok("vfx.check", R"({"recipe": "fx/broken.vfx.json"})");
    const json::Value& warnings =
        *broken.find("recipes")->find("fx/broken.vfx.json")->find("warnings");
    REQUIRE(warnings.as_array().size() == 4);
    CHECK(warnings.as_array()[0].as_string().find("never plays") != std::string::npos);
    CHECK(warnings.as_array()[1].as_string().find("did you mean \"OmniLight3D\"") !=
          std::string::npos);
    CHECK(warnings.as_array()[2].as_string().find("more detail") != std::string::npos);
    CHECK(warnings.as_array()[3].as_string().find("SpotLight3D/light_energy") != std::string::npos);

    // A preview plays it alone: its layers, particles, light and shake at each layer's start.
    const json::Value preview =
        project.ok("vfx.preview", R"({"recipe": "fx/boom.vfx.json", "times": [0.1, 0.35, 1.0],
                           "capture": {"at": 0.1, "size": [64, 48]}})");
    const json::Array& samples = preview.find("samples")->as_array();
    REQUIRE(samples.size() == 3);
    CHECK(samples[0].find("layers")->as_array().size() == 4);
    CHECK(samples[0].find("light")->as_double() > 0.0);
    CHECK(samples[0].find("particles")->as_int() > 0);
    CHECK(samples[0].find("shake") != nullptr);
    CHECK(samples[1].find("light") == nullptr);
    CHECK(samples[2].find("layers")->as_array().size() == 2);
    CHECK(preview.find("events")->as_array().front().find("kind")->as_string() == "vfx_started");
    CHECK(preview.find("capture")->find("out")->as_string() == "previews/boom.png");
    CHECK(std::filesystem::exists(project.root + "/previews/boom.png"));
    // Far off, the smoke never comes.
    const json::Value far = project.ok(
        "vfx.preview", R"({"recipe": "fx/boom.vfx.json", "times": [0.3], "distance": 50})");
    const json::Array& layers = far.find("samples")->as_array()[0].find("layers")->as_array();
    CHECK(std::ranges::find(layers, json::Value("smoke")) == layers.end());
}
