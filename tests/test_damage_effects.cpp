// What damage leaves and does beyond a body's shape: debris, dust, decals and sounds where hits
// land and things break, the living dying and vehicles wrecked, creatures and NPCs feeling it,
// break budgets spreading breaks over ticks, and fracture cut the same on any number of threads.

#include "slash/asset/assets.hpp"
#include "slash/canvas/canvas.hpp"
#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/jobs.hpp"
#include "slash/core/json.hpp"
#include "slash/core/math.hpp"
#include "slash/npc/npc.hpp"
#include "slash/physics2d/physics2d.hpp"
#include "slash/physics3d/physics3d.hpp"
#include "slash/platform/file.hpp"
#include "slash/scene3d/fracture.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/damage.hpp"
#include "slash/world/world.hpp"

#include <doctest/doctest.h>

#include <algorithm>
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
namespace npc = slash::npc;
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

// A field of the first item of a reply's list, or null.
json::Value first(const json::Value& reply, std::string_view list, std::string_view key) {
    const json::Value* items = reply.find(list);
    if (items == nullptr || items->size() == 0) {
        return {};
    }
    const json::Value* found = items->as_array().front().find(key);
    return found == nullptr ? json::Value{} : *found;
}

// A world with files in memory and both physics.
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
        physics3d::install(world);
        physics2d::install(world);
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

// A wooden crate whose hits throw chips, leave a scorch and knock.
constexpr std::string_view kCrateProfile = R"({"format": 1, "material": "wood", "health": 1000,
    "deform": {"dent": 0, "hole": 0, "carve": 0},
    "effects": {"hit_scale": 20,
        "hit": {"debris": {"amount": 20, "lifetime": 1},
                "decal": {"texture": "scorch.png", "size": 0.4, "lifetime": 2},
                "sound": {"stream": "knock.wav", "lifetime": 0.5}}}})";

constexpr std::string_view kCrate3D = R"({
    "Transform3D": {"position": [0, 0.5, 0]},
    "StaticBody3D": {},
    "CollisionShape3D": {"shape": "box", "size": [1, 1, 1]},
    "BoxMesh": {"size": [1, 1, 1]},
    "MeshInstance3D": {},
    "Destructible3D": {"profile": "crate.damage.json"}})";

constexpr std::string_view kCrate2D = R"({
    "Transform2D": {"position": [0, 0]},
    "StaticBody2D": {},
    "CollisionShape2D": {"shape": "rectangle", "size": [64, 64]},
    "Polygon2D": {"polygon": [[-32, -32], [32, -32], [32, 32], [-32, 32]]},
    "Destructible2D": {"profile": "crate.damage.json"}})";

} // namespace

TEST_CASE("damage effects: profiles name what hits and breaks leave, over their material's") {
    auto wood = world::parse_damage(parse(R"({"format": 1, "material": "wood"})"));
    REQUIRE(wood.ok());
    CHECK(wood.value().effects.hit.debris.amount > 0);
    CHECK(wood.value().effects.broken.dust.amount > 0);
    auto crate = world::parse_damage(parse(kCrateProfile));
    REQUIRE(crate.ok());
    CHECK(crate.value().effects.hit.debris.amount == 20);
    CHECK(crate.value().effects.hit.decal.texture == "scorch.png");
    CHECK(crate.value().effects.hit.sound.stream == "knock.wav");
    CHECK(crate.value().effects.hit_scale == doctest::Approx(20.0));
    CHECK_FALSE(
        world::validate_damage(parse(R"({"format": 1, "effects": {"hit": {"debirs": {}}}})"))
            .empty());
}

TEST_CASE("damage effects: a hit leaves debris, a decal and a sound that go in their time") {
    Scene scene;
    scene.files["crate.damage.json"] = std::string(kCrateProfile);
    const entt::entity crate = scene.create("crate", kCrate3D);
    scene.step(1);

    // Half of hit_scale's damage throws half the chips.
    auto light = physics3d::damage_body(scene.world, crate, 10.0F, "blunt", Vec3{0, 0.5F, 0.5F});
    REQUIRE(light.ok());
    CHECK(light.value().effects ==
          std::vector<std::string>{"crate_debris", "crate_decal", "crate_sound"});
    const auto& ecs = scene.world.ecs();
    const entt::entity chips = scene.world.find("crate_debris");
    REQUIRE((chips != entt::null));
    CHECK(ecs.get<const world::CPUParticles3D>(chips).amount == 10);
    CHECK(ecs.get<const world::CPUParticles3D>(chips).one_shot);
    // The decal faces back the way the hit came, out of the crate's front.
    const entt::entity decal = scene.world.find("crate_decal");
    REQUIRE((decal != entt::null));
    CHECK(ecs.get<const world::Decal>(decal).texture_albedo == "scorch.png");
    const entt::entity sound = scene.world.find("crate_sound");
    REQUIRE((sound != entt::null));
    CHECK(ecs.get<const world::AudioStreamPlayer3D>(sound).stream == "knock.wav");

    // A heavier hit throws them all, under names of their own.
    auto heavy = physics3d::damage_body(scene.world, crate, 60.0F, "blunt", Vec3{0, 1.0F, 0});
    REQUIRE(heavy.ok());
    CHECK(ecs.get<const world::CPUParticles3D>(scene.world.find("crate_debris_2")).amount == 20);
    // Damage with no point leaves nothing.
    auto unseen = physics3d::damage_body(scene.world, crate, 10.0F, "blunt", std::nullopt);
    REQUIRE(unseen.ok());
    CHECK(unseen.value().effects.empty());

    scene.step(40); // two thirds of a second: the sounds are done
    CHECK_FALSE(scene.has("crate_sound"));
    CHECK(scene.has("crate_debris"));
    CHECK(scene.has("crate_decal"));
    scene.step(60); // a second and two thirds: the chips are gone
    CHECK_FALSE(scene.has("crate_debris"));
    CHECK(scene.has("crate_decal"));
    scene.step(30);
    CHECK_FALSE(scene.has("crate_decal"));
    CHECK(scene.has("crate"));
}

TEST_CASE("damage effects: a 2D hit leaves its effects in pixels, and a break its own") {
    Scene scene;
    scene.files["crate.damage.json"] = std::string(kCrateProfile);
    const entt::entity crate = scene.create("crate", kCrate2D);
    scene.step(1);
    auto hit = physics2d::damage_body_2d(scene.world, crate, 20.0F, "blunt", Vec2{0, -32});
    REQUIRE(hit.ok());
    CHECK(hit.value().effects ==
          std::vector<std::string>{"crate_debris", "crate_decal", "crate_sound"});
    const auto& ecs = scene.world.ecs();
    const auto& chips = ecs.get<const world::CPUParticles2D>(scene.world.find("crate_debris"));
    CHECK(chips.amount == 20);
    CHECK(chips.initial_velocity_max == doctest::Approx(2.0 * 64.0));
    CHECK(ecs.get<const world::Sprite2D>(scene.world.find("crate_decal")).texture == "scorch.png");
    CHECK(ecs.all_of<world::AudioStreamPlayer2D>(scene.world.find("crate_sound")));

    // Breaking it leaves wood's break effects: chips and dust.
    auto broke = physics2d::damage_body_2d(scene.world, crate, 5000.0F, "blunt", Vec2{0, 0});
    REQUIRE(broke.ok());
    REQUIRE(broke.value().broke);
    CHECK(std::ranges::count_if(broke.value().effects, [](const std::string& id) {
              return id.starts_with("crate_dust");
          }) == 1);
    scene.step(180);
    CHECK_FALSE(scene.has("crate_debris"));
    CHECK_FALSE(scene.has("crate_dust"));
}

TEST_CASE("damage effects: a break budget spreads breaks from impacts over ticks") {
    Scene scene;
    physics3d::Settings settings;
    settings.break_budget = 4;
    physics3d::set_settings(scene.world, settings);
    scene.files["vase.damage.json"] = R"({"format": 1, "material": "glass", "health": 1,
        "impact": {"threshold": 1, "damage": 100},
        "fracture": {"pattern": "voronoi", "pieces": 4, "levels": 1}})";
    scene.create("floor", R"({"Transform3D": {}, "StaticBody3D": {},
        "CollisionShape3D": {"shape": "box", "size": [20, 1, 20]}})");
    for (const char* x : {"-3", "0", "3"}) {
        scene.create(std::string("vase") + x, std::string(R"({
            "Transform3D": {"position": [)") + x + R"(, 3, 0]},
            "RigidBody3D": {},
            "CollisionShape3D": {"shape": "box", "size": [0.5, 0.5, 0.5]},
            "BoxMesh": {"size": [0.5, 0.5, 0.5]},
            "MeshInstance3D": {},
            "Destructible3D": {"profile": "vase.damage.json"}})");
    }
    const auto pieces = [&] {
        const auto view = scene.world.ecs().view<const world::FracturePiece3D>();
        return static_cast<int>(std::distance(view.begin(), view.end()));
    };
    // They land together; one breaks that tick, the others wait a tick each.
    int ticks = 0;
    while (pieces() == 0 && ticks < 120) {
        scene.step(1);
        ++ticks;
    }
    REQUIRE(ticks < 120);
    CHECK(pieces() == 4);
    scene.step(1);
    CHECK(pieces() == 8);
    scene.step(1);
    CHECK(pieces() == 12);
    CHECK_FALSE(scene.has("vase0"));
}

TEST_CASE("damage effects: fracture cuts the same pieces on any number of threads") {
    const auto cut = [](slash::jobs::Pool* pool) {
        Scene scene;
        if (pool != nullptr) {
            scene.world.set_pool(*pool);
        }
        scene.files["wall.damage.json"] = R"({"format": 1, "material": "concrete",
            "fracture": {"pattern": "voronoi", "pieces": 12}})";
        const entt::entity wall = scene.create("wall", R"({
            "Transform3D": {"position": [0, 1, 0]},
            "StaticBody3D": {},
            "CollisionShape3D": {"shape": "box", "size": [2, 2, 0.3]},
            "BoxMesh": {"size": [2, 2, 0.3]},
            "MeshInstance3D": {},
            "Destructible3D": {"profile": "wall.damage.json"}})");
        scene.step(1);
        auto broken = physics3d::break_body(scene.world, wall, 2.0F);
        REQUIRE(broken.ok());
        std::vector<std::pair<std::string, json::Value>> out;
        for (const entt::entity piece : broken.value()) {
            out.emplace_back(std::string(scene.world.id_of(piece)),
                             scene.components.find("RigidBody3D")->save(scene.world, piece));
        }
        return out;
    };
    slash::jobs::Pool none(0);
    slash::jobs::Pool four(4);
    const auto alone = cut(&none);
    REQUIRE(alone.size() == 12);
    CHECK(cut(&four) == alone);
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
            {"fauna/deer.creature.json", R"({"format": 1, "name": "Deer", "kind": "grazer",
                "body": {"length": 1.6, "height": 1.1},
                "behavior": {"temperament": "calm", "run_speed": 6}})"},
            {"fauna/boar.creature.json", R"({"format": 1, "name": "Boar", "kind": "grazer",
                "body": {"length": 1.2, "height": 0.8},
                "behavior": {"temperament": "aggressive", "notice": 30, "run_speed": 5}})"},
            {"guard.npc.json", R"({"format": 1, "facts": {"health": 100, "dead": false,
                "safe": false},
                "goals": [{"name": "hide", "when": {"dead": false}, "want": {"safe": true}}],
                "actions": [{"name": "duck", "effects": {"safe": true}, "do": {"wait": 1}}]})"},
            {"flesh.damage.json", R"({"format": 1, "material": "flesh", "health": 100})"},
            {"van.damage.json", R"({"format": 1, "material": "metal", "health": 50})"},
            {"main.scene.json", R"({"format": 1, "entities": {
                "hunter": {"components": {"Transform3D": {"position": [0, 0, 8]},
                                          "CharacterBody3D": {}}},
                "deer": {"components": {"Transform3D": {},
                    "Creature3D": {"recipe": "fauna/deer.creature.json"},
                    "Destructible3D": {"profile": "flesh.damage.json"}}},
                "boar": {"components": {"Transform3D": {"position": [-5, 0, 0]},
                    "Creature3D": {"recipe": "fauna/boar.creature.json"},
                    "Destructible3D": {"profile": "flesh.damage.json"}}},
                "guard": {"components": {"Transform3D": {"position": [5, 0, 0]},
                    "NpcAgent": {"brain": "guard.npc.json"},
                    "Destructible3D": {"profile": "flesh.damage.json"}}},
                "van": {"components": {"Transform3D": {"position": [0, 0, -10]},
                    "Vehicle3D": {"controls": {"throttle": 1}},
                    "Destructible3D": {"profile": "van.damage.json"}}}}})"}};
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

    json::Value ok(std::string_view name, std::string_view params = "{}") {
        command::Reply reply = session->call(name, parse(params));
        REQUIRE_MESSAGE(reply.ok, reply.message);
        return std::move(reply.value);
    }

    void steps(int ticks) {
        for (int i = 0; i < ticks; ++i) {
            session->world().step(1.0 / 60.0);
        }
    }

    [[nodiscard]] const world::CreatureMind& mind(std::string_view id) const {
        const world::World& w = session->world();
        return w.ecs().get<const world::Creature3D>(w.find(id)).mind;
    }

    [[nodiscard]] int fact(std::string_view id, std::string_view name) const {
        world::World& w = session->world();
        const auto brain = npc::brain_of(w, w.find(id));
        REQUIRE(brain != nullptr);
        return npc::facts_of(w, w.find(id)).at(brain->fact(name));
    }
};

} // namespace

TEST_CASE("damage effects: creatures run from or turn on whoever hurt them, and die") {
    Project project("damage_creatures_project");
    // The living die rather than break, so they need no body to be valid.
    CHECK(slash::scene3d::check_destructibles(project.session->world()).empty());
    project.steps(1);
    const json::Value shot = project.ok(
        "damage.apply",
        R"({"id": "deer", "amount": 30, "kind": "pierce", "at": [0, 1, 0], "by": "hunter"})");
    CHECK(first(shot, "hurt", "health") == 70.0);
    project.ok("damage.apply", R"({"id": "boar", "amount": 10, "by": "hunter"})");
    project.steps(1);
    const world::World& w = project.session->world();
    CHECK(project.mind("deer").mode == "fleeing");
    CHECK((project.mind("deer").target == w.find("hunter")));
    CHECK(project.mind("boar").mode == "attacking");
    CHECK((project.mind("boar").target == w.find("hunter")));

    const json::Value killed =
        project.ok("damage.apply", R"({"id": "deer", "amount": 500, "by": "hunter"})");
    CHECK(first(killed, "hurt", "killed") == true);
    CHECK(first(killed, "hurt", "broke").is_null());
    CHECK(project.mind("deer").mode == "dead");
    CHECK_FALSE(w.ecs().get<const world::Creature3D>(w.find("deer")).enabled);
    // The dead take no more damage, and stay dead.
    const json::Value again = project.ok("damage.apply", R"({"id": "deer", "amount": 5})");
    CHECK(first(again, "hurt", "dealt") == 0.0);
    const json::Value state = project.ok("damage.state", R"({"ids": ["deer"]})");
    CHECK(first(state, "bodies", "dead") == true);
}

TEST_CASE("damage effects: an NPC's brain feels its health, and a vehicle is wrecked") {
    Project project("damage_npc_project");
    project.steps(1);
    CHECK(project.fact("guard", "health") == 100);
    project.ok("damage.apply", R"({"id": "guard", "amount": 40})");
    project.steps(1);
    CHECK(project.fact("guard", "health") == 60);
    CHECK(project.fact("guard", "dead") == 0);
    const json::Value killed = project.ok("damage.apply", R"({"id": "guard", "amount": 100})");
    CHECK(first(killed, "hurt", "killed") == true);
    project.steps(1);
    CHECK(project.fact("guard", "health") == 0);
    CHECK(project.fact("guard", "dead") == 1);

    const json::Value wreck =
        project.ok("damage.apply", R"({"id": "van", "amount": 1000, "by": "hunter"})");
    CHECK(first(wreck, "hurt", "wrecked") == true);
    const world::World& w = project.session->world();
    const auto& van = w.ecs().get<const world::Vehicle3D>(w.find("van"));
    CHECK_FALSE(van.enabled);
    CHECK(van.controls.throttle == 0.0F);
    const json::Value state = project.ok("damage.state", R"({"ids": ["van"]})");
    CHECK(first(state, "bodies", "wrecked") == true);
}
