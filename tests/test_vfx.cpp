// Visual effects as .vfx.json recipes (slash/world/vfx.hpp): layers on a timeline played as
// entities under a VisualEffect3D or VisualEffect2D, curves, expressions over parameters,
// levels of detail, budgets, camera shakes, and the vfx.* commands.

#include "slash/asset/assets.hpp"
#include "slash/canvas/canvas.hpp"
#include "slash/canvas/distortion.hpp"
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
#include <array>
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
using slash::Vec2f;
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

TEST_CASE("vfx distortion: each mode moves the picture inside its radius and nothing outside") {
    using canvas::Distortion;
    using canvas::DistortionMode;
    const auto one = [](DistortionMode mode) {
        return std::array<Distortion, 1>{Distortion{.center = {100.0F, 100.0F},
                                                    .radius = 50.0F,
                                                    .mode = mode,
                                                    .strength = 8.0F,
                                                    .progress = 0.5F,
                                                    .width = 0.2F,
                                                    .frequency = 3.0F,
                                                    .speed = 1.0F,
                                                    .seed = 7}};
    };
    for (const DistortionMode mode : {DistortionMode::Heat, DistortionMode::Shockwave,
                                      DistortionMode::Ripple, DistortionMode::Lens}) {
        const auto d = one(mode);
        const Vec2f outside = canvas::distortion_offset(d, {160.0F, 100.0F}, 0.3);
        CHECK(outside.x == 0.0F);
        CHECK(outside.y == 0.0F);
        // The same point and time give the same bits.
        const Vec2f a = canvas::distortion_offset(d, {112.0F, 93.0F}, 1.25);
        const Vec2f b = canvas::distortion_offset(d, {112.0F, 93.0F}, 1.25);
        CHECK(a.x == b.x);
        CHECK(a.y == b.y);
    }
    // A shockwave's ring at half the radius: just outside it the picture comes from nearer the
    // middle (pushed out), just inside from farther (pulled in), and nothing far from the ring.
    const auto shock = one(DistortionMode::Shockwave);
    CHECK(canvas::distortion_offset(shock, {128.0F, 100.0F}, 0.0).x < -1.0F);
    CHECK(canvas::distortion_offset(shock, {122.0F, 100.0F}, 0.0).x > 1.0F);
    CHECK(canvas::distortion_offset(shock, {105.0F, 100.0F}, 0.0).x == 0.0F);
    CHECK(std::abs(canvas::distortion_offset(shock, {125.0F, 100.0F}, 0.0).x) < 1e-4F);
    // At most strength pixels anywhere.
    float most = 0.0F;
    for (int i = 0; i < 500; ++i) {
        const float x = 100.0F + (static_cast<float>(i) * 0.1F);
        most = std::max(most, std::abs(canvas::distortion_offset(shock, {x, 100.0F}, 0.0).x));
    }
    CHECK(most <= 8.0F + 1e-3F);
    CHECK(most > 7.5F);
    // A lens draws each point from nearer its middle, so it bulges; its middle stays put.
    const auto lens = one(DistortionMode::Lens);
    CHECK(canvas::distortion_offset(lens, {120.0F, 100.0F}, 0.0).x < -1.0F);
    CHECK(canvas::distortion_offset(lens, {100.0F, 120.0F}, 0.0).y < -1.0F);
    CHECK(canvas::distortion_offset(lens, {100.0F, 100.0F}, 0.0).x == 0.0F);
    // A ripple moves points along the way out, and changes with time.
    const auto ripple = one(DistortionMode::Ripple);
    const Vec2f r0 = canvas::distortion_offset(ripple, {120.0F, 100.0F}, 0.0);
    const Vec2f r1 = canvas::distortion_offset(ripple, {120.0F, 100.0F}, 0.1);
    CHECK(r0.y == doctest::Approx(0.0).epsilon(1e-6));
    CHECK(r0.x != r1.x);
    // Heat shimmers in both directions and scrolls with time.
    const auto heat = one(DistortionMode::Heat);
    const Vec2f h0 = canvas::distortion_offset(heat, {110.0F, 110.0F}, 0.0);
    const Vec2f h1 = canvas::distortion_offset(heat, {110.0F, 110.0F}, 0.5);
    CHECK((h0.x != h1.x || h0.y != h1.y));
    CHECK(std::abs(h0.x) <= 8.0F);

    // The grid: square cells over the view, six corners a cell, texture coordinates of a point
    // out of reach left as they are.
    const std::vector<slash::render::Vertex2D> mesh =
        canvas::distortion_mesh(shock, {320.0F, 160.0F}, 0.0, 32);
    CHECK(mesh.size() == std::size_t{32} * 16 * 6);
    CHECK(mesh[0].u == 0.0F);
    CHECK(mesh[0].v == 0.0F);
    CHECK(canvas::distortion_mesh({}, {320.0F, 160.0F}, 0.0).empty());
}

TEST_CASE("vfx distortion: a 2D lens bends the world's picture but not the controls over it") {
    Scene scene;
    // A red half and a blue half meeting at x 300, and a green panel at the top left.
    scene.create("red", R"({"Transform2D": {}, "Polygon2D": {
        "polygon": [[0, 0], [300, 0], [300, 600], [0, 600]], "color": [1, 0, 0, 1]}})");
    scene.create("blue", R"({"Transform2D": {}, "Polygon2D": {
        "polygon": [[300, 0], [1000, 0], [1000, 600], [300, 600]], "color": [0, 0, 1, 1]}})");
    scene.create("panel", R"({"Control": {"offset_right": 40, "offset_bottom": 40},
        "ColorRect": {"color": [0, 1, 0, 1]}})");
    const auto pixel = [](const slash::render::Image& image, int x, int y) {
        const std::size_t i =
            ((static_cast<std::size_t>(y) * image.width) + static_cast<std::size_t>(x)) * 4;
        return std::array<int, 3>{image.pixels[i], image.pixels[i + 1], image.pixels[i + 2]};
    };
    const auto plain = canvas::capture(scene.world, scene.assets, {});
    REQUIRE(plain.ok());
    CHECK(pixel(plain.value().image, 310, 200)[2] > 200);
    CHECK(pixel(plain.value().image, 290, 200)[0] > 200);

    // A lens a little left of the seam draws the blue from nearer its middle out over x 310..330.
    const entt::entity lens = scene.create("lens", R"({"Transform2D": {"position": [290, 200]},
        "ScreenDistortion2D": {"mode": "lens", "radius": 80, "strength": 30}})");
    const auto bent = canvas::capture(scene.world, scene.assets, {});
    REQUIRE(bent.ok());
    const slash::render::Image& image = bent.value().image;
    CHECK(pixel(image, 315, 200)[0] > 200);
    CHECK(pixel(image, 315, 200)[2] < 50);
    // Far from it, and the panel over it, are as they were.
    CHECK(pixel(image, 600, 200) == pixel(plain.value().image, 600, 200));
    CHECK(pixel(image, 20, 20)[1] > 200);
    CHECK(pixel(image, 20, 20) == pixel(plain.value().image, 20, 20));

    // Turned off, nothing bends.
    scene.world.ecs().get<world::ScreenDistortion2D>(lens).enabled = false;
    const auto off = canvas::capture(scene.world, scene.assets, {});
    REQUIRE(off.ok());
    CHECK(pixel(off.value().image, 315, 200) == pixel(plain.value().image, 315, 200));
}

TEST_CASE("vfx distortion: a 3D shockwave is placed and sized through the camera") {
    Scene scene;
    scene.create("camera", R"({"Transform3D": {"position": [0, 0, 10]},
        "Camera3D": {"current": true, "fov": 90}})");
    scene.create("wave", R"({"Transform3D": {"position": [0, 0, 0]},
        "ScreenDistortion3D": {"mode": "shockwave", "radius": 5}})");
    const std::vector<canvas::Distortion> found =
        canvas::distortions(scene.world, {200.0F, 100.0F}, true);
    REQUIRE(found.size() == 1);
    CHECK(found[0].center.x == doctest::Approx(100.0));
    CHECK(found[0].center.y == doctest::Approx(50.0));
    // Ten meters off with a field of view of 90 degrees, half the height is ten meters: five
    // meters is a quarter of the view's 100 pixels.
    CHECK(found[0].radius == doctest::Approx(25.0).epsilon(0.001));
    CHECK(found[0].mode == canvas::DistortionMode::Shockwave);
    // Behind the camera, or with 3D left out, there is none.
    CHECK(canvas::distortions(scene.world, {200.0F, 100.0F}, false).empty());
    scene.world.ecs().get<world::Transform3D>(scene.world.find("wave")).position = {0, 0, 20};
    CHECK(canvas::distortions(scene.world, {200.0F, 100.0F}, true).empty());
}

TEST_CASE("vfx hooks: creatures, characters and vehicles play their recipes' effects") {
    Scene scene;
    scene.files["fx/puff.vfx.json"] = R"({"format": 1, "dimension": "2d", "duration": 0.5,
        "layers": [{"name": "dust", "components": {"CPUParticles2D": {"amount": 8}}}]})";
    scene.files["fx/exhaust.vfx.json"] = R"({"format": 1, "dimension": "2d", "loop": true,
        "duration": 0.5,
        "layers": [{"name": "smoke", "components": {"CPUParticles2D": {"amount": 4}}}]})";
    scene.files["deer.creature.json"] = R"({"format": 1, "vfx": {"fled": "fx/puff.vfx.json"}})";
    scene.files["van.vehicle.json"] = R"({"format": 1, "vfx": {"running": "fx/exhaust.vfx.json"}})";
    const entt::entity deer = scene.create("deer", R"({"Transform2D": {"position": [40, 30]},
        "Creature2D": {"recipe": "deer.creature.json", "enabled": false}})");
    REQUIRE(world::vfx_hooks_of(scene.world, deer).size() == 1);

    // An event with a place plays there; one without plays where the creature is.
    scene.world.emit("fled", "deer", slash::json::parse(R"({"position": [5, 6]})").value());
    scene.world.emit("fled", "deer", json::Value());
    scene.world.emit("grazed", "deer", json::Value());
    scene.step(2);
    const auto& ecs = scene.world.ecs();
    const entt::entity first = scene.world.find("deer_fled");
    const entt::entity second = scene.world.find("deer_fled_2");
    REQUIRE((first != entt::null));
    REQUIRE((second != entt::null));
    CHECK(ecs.get<const world::VisualEffect2D>(first).recipe == "fx/puff.vfx.json");
    CHECK(ecs.get<const world::VisualEffect2D>(first).free_on_finish);
    CHECK(ecs.get<const world::Transform2D>(first).position.x == doctest::Approx(5.0));
    CHECK(ecs.get<const world::Transform2D>(second).position.x == doctest::Approx(40.0));
    CHECK_FALSE(scene.has("deer_grazed"));
    scene.step(120);
    CHECK_FALSE(scene.has("deer_fled"));

    // A van's exhaust runs while its throttle is open and follows it, then winds down.
    const entt::entity van = scene.create("van", R"({"Transform2D": {"position": [200, 100]},
        "Vehicle2D": {"recipe": "van.vehicle.json", "controls": {"throttle": 1}}})");
    scene.step(3);
    const entt::entity exhaust = scene.world.find("van_running");
    REQUIRE((exhaust != entt::null));
    CHECK(ecs.get<const world::VisualEffect2D>(exhaust).playing);
    scene.world.ecs().get<world::Transform2D>(van).position.x = 260.0F;
    scene.step(2);
    CHECK(ecs.get<const world::Transform2D>(exhaust).position.x == doctest::Approx(260.0));
    scene.world.ecs().get<world::Vehicle2D>(van).controls.throttle = 0.0F;
    scene.step(2);
    REQUIRE(scene.has("van_running"));
    CHECK_FALSE(ecs.get<const world::VisualEffect2D>(scene.world.find("van_running")).playing);
    scene.step(90);
    CHECK_FALSE(scene.has("van_running"));
}

TEST_CASE("vfx hooks: foliage plays its type's effects where it parts and is harvested") {
    Scene scene;
    scene.files["fx/rustle.vfx.json"] = R"({"format": 1, "dimension": "2d", "duration": 0.3,
        "layers": [{"name": "leaves", "components": {"CPUParticles2D": {"amount": 3}}}]})";
    scene.files["tuft.foliage.json"] = R"({"format": 1, "space": "2d", "kind": "grass",
        "texture": "tuft.png", "size": [8, 16], "interaction": {"radius": 20, "bend": 0.5},
        "vfx": {"foliage_parted": "fx/rustle.vfx.json",
                "foliage_harvested": "fx/rustle.vfx.json"}})";
    CHECK(world::foliage_vfx_hooks(scene.world, "tuft.foliage.json").size() == 2);
    CHECK(world::foliage_vfx_hooks(scene.world, "nothing.json").empty());
    scene.create("meadow", R"({"Transform2D": {}, "Foliage2D": {"groups": [
        {"type": "tuft.foliage.json", "instances": [100, 100, 0, 1, 300, 100, 0, 1]}]}})");
    scene.create("kid", R"({"Transform2D": {"position": [90, 95]}, "CharacterBody2D": {}})");
    scene.step(3);
    // Only the tuft beside the kid parts, once, and its rustle plays at its foot.
    CHECK(scene.count("foliage_parted") == 1);
    const entt::entity rustle = scene.world.find("meadow_foliage_parted");
    REQUIRE((rustle != entt::null));
    CHECK(scene.world.ecs().get<const world::Transform2D>(rustle).position.x ==
          doctest::Approx(100.0));
    scene.step(5);
    CHECK(scene.count("foliage_parted") == 1);
    CHECK_FALSE(scene.has("meadow_foliage_parted_2"));
    // Harvesting (gameplay code's event here) plays at the place it names.
    scene.world.emit(
        "foliage_harvested", "meadow",
        slash::json::parse(R"({"type": "tuft.foliage.json", "position": [300, 100]})").value());
    scene.step(2);
    const entt::entity cut = scene.world.find("meadow_foliage_harvested");
    REQUIRE((cut != entt::null));
    CHECK(scene.world.ecs().get<const world::Transform2D>(cut).position.x ==
          doctest::Approx(300.0));
}
