// Fauna: .creature.json recipes, their measures, skeletons, animations, the rigged models and 2D
// cut-outs they build, the fauna system that runs them, and the creature.* commands.
#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/platform/file.hpp"
#include "slash/scene3d/creature.hpp"
#include "slash/scene3d/model.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/creature.hpp"
#include "slash/world/world.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace command = slash::command;
namespace json = slash::json;
namespace scene3d = slash::scene3d;
namespace world = slash::world;

namespace {

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE_MESSAGE(parsed.ok(), text);
    return std::move(parsed).value();
}

// A deer that herds and is wary, a wolf that hunts deer, a dog that can be tamed and ridden, a
// sparrow, a school fish and a beetle.
constexpr std::string_view kDeer = R"({"format": 1, "name": "Deer", "kind": "grazer",
    "body": {"length": 1.6, "height": 1.1, "build": 0.4, "legs": 1.2},
    "look": {"coat": [0.6, 0.4, 0.22], "pattern": "spots", "markings": [0.95, 0.92, 0.85]},
    "parts": {"ears": "long", "horns": "antlers"},
    "behavior": {"temperament": "wary", "flee": 6, "notice": 12, "herd": true,
                 "herd_radius": 5, "roam": 15, "walk_speed": 1.2, "run_speed": 6}})";
constexpr std::string_view kWolf = R"({"format": 1, "name": "Wolf", "kind": "predator",
    "body": {"length": 1.4, "height": 0.8, "build": 0.6},
    "look": {"coat": [0.45, 0.45, 0.45]}, "parts": {"ears": "pointed"},
    "behavior": {"temperament": "calm", "hunts": ["grazer"], "notice": 20, "run_speed": 8,
                 "reach": 1.5, "hunger_rate": 0}})";
constexpr std::string_view kDog = R"({"format": 1, "name": "Dog", "kind": "pet",
    "body": {"length": 0.9, "height": 0.6},
    "behavior": {"temperament": "friendly", "food": ["bone"], "tame": 2, "ride": true,
                 "walk_speed": 1.5, "run_speed": 5}})";
constexpr std::string_view kSparrow = R"({"format": 1, "name": "Sparrow", "kind": "bird",
    "body": {"length": 0.16, "height": 0.12}, "look": {"coat": [0.5, 0.35, 0.2]},
    "behavior": {"temperament": "timid", "herd": true, "herd_radius": 3, "altitude": 4}})";
constexpr std::string_view kFish = R"({"format": 1, "name": "Minnow", "kind": "fish",
    "body": {"length": 0.3, "height": 0.1}, "look": {"pattern": "stripes"},
    "behavior": {"herd": true, "herd_radius": 2, "walk_speed": 0.6, "run_speed": 2}})";
constexpr std::string_view kBeetle = R"({"format": 1, "name": "Beetle", "kind": "insect",
    "body": {"length": 0.05, "height": 0.02}, "parts": {"antennae": true}})";

const std::map<std::string, std::string_view> kFiles{
    {"fauna/deer.creature.json", kDeer},   {"fauna/wolf.creature.json", kWolf},
    {"fauna/dog.creature.json", kDog},     {"fauna/sparrow.creature.json", kSparrow},
    {"fauna/minnow.creature.json", kFish}, {"fauna/beetle.creature.json", kBeetle}};

struct Project {
    std::string root;
    world::ComponentRegistry components;
    command::Registry commands;
    std::unique_ptr<command::Session> session;

    Project(std::string name, const std::map<std::string, std::string_view>& files,
            std::string_view scene = R"({"format": 1, "entities": {
              "camera": {"components": {"Transform3D": {"position": [0, 2, 12]},
                                        "Camera3D": {"current": true}}}}})")
        : root(std::move(name)) {
        std::filesystem::remove_all(root);
        for (const auto& [file, text] : files) {
            const std::string path = root + "/" + file;
            REQUIRE(slash::platform::create_directories(
                        std::filesystem::path(path).parent_path().string())
                        .ok());
            REQUIRE(slash::platform::write_file(path, text).ok());
        }
        REQUIRE(slash::platform::write_file(root + "/main.scene.json", scene).ok());
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

    command::Reply call(std::string_view name, std::string_view params) {
        return session->call(name, parse(params));
    }

    // One creature's state from creature.state.
    json::Value state(std::string_view id) {
        return ok("creature.state", R"({"creature": ")" + std::string(id) + R"("})")["creatures"]
            .as_array()
            .front();
    }

    // The kinds of the game events read since the last read.
    std::vector<std::string> events(std::string_view subscription) {
        std::vector<std::string> out;
        const json::Value read =
            ok("events.read", R"({"subscription": ")" + std::string(subscription) + R"("})");
        for (const json::Value& event : read.find("events")->as_array()) {
            out.push_back(event.find("kind")->as_string());
        }
        return out;
    }
};

// A field that must be there.
const json::Value& field(const json::Value& value, std::string_view key) {
    const json::Value* found = value.find(key);
    REQUIRE_MESSAGE(found != nullptr, key);
    return *found;
}

world::CreatureRecipe recipe_of(std::string_view text) {
    auto recipe = world::parse_creature(parse(text), nullptr);
    REQUIRE_MESSAGE(recipe.ok(), recipe.error().message);
    return std::move(recipe).value();
}

bool has(const std::vector<std::string>& names, std::string_view name) {
    return std::ranges::find(names, name) != names.end();
}

double distance(const json::Value& a, const json::Value& b) {
    double sum = 0.0;
    for (std::size_t i = 0; i < a.as_array().size(); ++i) {
        const double d = a.as_array()[i].as_double() - b.as_array()[i].as_double();
        sum += d * d;
    }
    return std::sqrt(sum);
}

} // namespace

TEST_CASE("fauna: recipes are checked: parts the plan has a place for, riding needs taming") {
    CHECK(world::validate_creature(parse(kDeer)).empty());
    const auto first = [](std::string_view text) {
        const auto errors = world::validate_creature(parse(text));
        REQUIRE_FALSE(errors.empty());
        return errors.front();
    };
    CHECK(first(R"({"format": 1, "kind": "fish", "parts": {"wings": 1}})").path == "/parts/wings");
    CHECK(first(R"({"format": 1, "body": {"plan": "insect"}, "parts": {"horns": "horns"}})").path ==
          "/parts/horns");
    CHECK(first(R"({"format": 1, "parts": {"antennae": true}})").path == "/parts/antennae");
    CHECK(first(R"({"format": 1, "behavior": {"ride": true}})").path == "/behavior/ride");
    CHECK(first(R"({"format": 1, "behavior": {"walk_speed": 3, "run_speed": 2}})").path ==
          "/behavior/run_speed");
    CHECK(first(R"({"format": 1, "kind": "dragon"})").path == "/kind");
    // A file and what it extends are checked together.
    const auto read = [](std::string_view path) -> slash::Result<std::string> {
        if (path == "fish.creature.json") {
            return std::string(R"({"format": 1, "kind": "fish"})");
        }
        return slash::make_error(slash::ErrorCode::NotFound, "no such file");
    };
    auto winged = world::parse_creature(
        parse(R"({"format": 1, "extends": "fish.creature.json", "parts": {"wings": 1}})"), read);
    REQUIRE_FALSE(winged.ok());
    CHECK(winged.error().message.starts_with("/parts/wings"));
}

TEST_CASE("fauna: a recipe extends another, and its plan follows its kind") {
    const auto read = [](std::string_view path) -> slash::Result<std::string> {
        if (path == "deer.creature.json") {
            return std::string(kDeer);
        }
        return slash::make_error(slash::ErrorCode::NotFound, "no such file");
    };
    auto fawn = world::parse_creature(
        parse(R"({"format": 1, "extends": "deer.creature.json", "name": "Fawn",
                  "body": {"length": 0.8, "height": 0.6}, "parts": {"horns": "none"}})"),
        read);
    REQUIRE_MESSAGE(fawn.ok(), fawn.error().message);
    CHECK(fawn.value().name == "Fawn");
    CHECK(fawn.value().species == "Fawn");
    CHECK(fawn.value().body.length == doctest::Approx(0.8));
    CHECK(fawn.value().look.pattern == "spots");
    CHECK(fawn.value().parts.horns == "none");
    CHECK(fawn.value().parts.ears == "long");
    CHECK(fawn.value().behavior.herd);
    const world::CreatureRecipe sparrow = recipe_of(kSparrow);
    CHECK(sparrow.body.plan == "bird");
    CHECK(sparrow.parts.wings == doctest::Approx(0.32)); // twice its length
    CHECK(world::creature_flies(sparrow));
    CHECK(sparrow.behavior.altitude == doctest::Approx(4.0));
    const world::CreatureRecipe beetle = recipe_of(kBeetle);
    CHECK_FALSE(world::creature_flies(beetle)); // no wings
    CHECK(world::creature_swims(recipe_of(kFish)));
}

TEST_CASE("fauna: skeletons by plan, with the back at the recipe's height") {
    const world::CreatureRecipe deer = recipe_of(kDeer);
    const world::CreatureMeasures m = world::creature_measures(deer);
    CHECK(m.back + m.body_depth == doctest::Approx(1.1));
    const auto names = [](const world::CreatureRecipe& recipe) {
        std::vector<std::string> out;
        for (const world::CreatureBone& bone : world::creature_skeleton(recipe)) {
            out.push_back(bone.name);
        }
        return out;
    };
    CHECK(names(deer) == std::vector<std::string>{"body", "neck", "head", "tail", "leg_front_near",
                                                  "leg_front_far", "leg_back_near",
                                                  "leg_back_far"});
    CHECK(names(recipe_of(kSparrow)) == std::vector<std::string>{"body", "neck", "head", "tail",
                                                                 "leg_near", "leg_far", "wing_near",
                                                                 "wing_far"});
    CHECK(names(recipe_of(kFish)) == std::vector<std::string>{"body", "head", "tail"});
    CHECK(names(recipe_of(kBeetle)).size() == 9); // body, head, tail and six legs
    // Every bone's parent comes before it, and the nose is ahead of the tail.
    for (const world::CreatureRecipe& recipe :
         {deer, recipe_of(kSparrow), recipe_of(kFish), recipe_of(kBeetle)}) {
        const auto bones = world::creature_skeleton(recipe);
        for (std::size_t i = 0; i < bones.size(); ++i) {
            CHECK(bones[i].parent < static_cast<int>(i));
        }
        const auto at = [&](std::string_view name) {
            return std::ranges::find_if(bones, [&](const auto& b) { return b.name == name; })->head;
        };
        CHECK(at("head").z > at("tail").z);
    }
}

TEST_CASE("fauna: animations pose legs, wings and tails, and loop") {
    const world::CreatureRecipe deer = recipe_of(kDeer);
    std::vector<std::string> names;
    for (const world::CreatureAnimation& a : world::creature_animations(deer)) {
        names.push_back(a.name);
    }
    CHECK(names == std::vector<std::string>{"idle", "walk", "run", "eat", "attack"});
    const auto walk = world::creature_animations(deer)[1];
    CHECK(walk.loop);
    const auto turn_of = [&](std::string_view animation, double time, std::string_view bone) {
        float angle = 0.0F;
        for (const world::CreatureTurn& turn : world::creature_pose(deer, animation, time)) {
            if (turn.bone == bone) {
                angle += turn.angle;
            }
        }
        return angle;
    };
    // A walk swings the legs a quarter cycle apart, and comes round again.
    CHECK(turn_of("walk", walk.length * 0.25, "leg_back_near") == doctest::Approx(0.45));
    CHECK(turn_of("walk", walk.length * 0.25, "leg_back_far") == doctest::Approx(-0.45));
    CHECK(turn_of("walk", walk.length * 1.25, "leg_back_near") ==
          doctest::Approx(turn_of("walk", walk.length * 0.25, "leg_back_near")));
    CHECK(turn_of("eat", 0.0, "neck") > 0.8F);
    // Birds fly with their wings, opposite each other, and fold them on the ground.
    const world::CreatureRecipe sparrow = recipe_of(kSparrow);
    const auto wings = [&](std::string_view animation, double time) {
        std::map<std::string, float> out;
        for (const world::CreatureTurn& turn : world::creature_pose(sparrow, animation, time)) {
            if (turn.bone.starts_with("wing")) {
                out[turn.bone + (turn.axis.z != 0.0F ? "/flap" : "/fold")] += turn.angle;
            }
        }
        return out;
    };
    const auto flying = wings("fly", 0.1);
    CHECK(flying.at("wing_near/flap") == doctest::Approx(-flying.at("wing_far/flap")));
    CHECK_FALSE(flying.contains("wing_near/fold"));
    CHECK(wings("idle", 0.0).at("wing_near/fold") == doctest::Approx(1.4));
}

TEST_CASE("fauna: a recipe builds a rigged, animated model and loads as any model") {
    const world::CreatureRecipe deer = recipe_of(kDeer);
    const scene3d::Model model = scene3d::build_creature(deer);
    REQUIRE(model.skins.size() == 1);
    CHECK(model.skins[0].joints.size() == world::creature_skeleton(deer).size());
    REQUIRE(model.meshes.size() == 1);
    std::vector<std::string> materials;
    for (const scene3d::ModelMaterial& material : model.materials) {
        materials.push_back(material.name);
    }
    for (const char* name : {"coat", "belly", "markings", "eyes", "antler"}) {
        CHECK_MESSAGE(has(materials, name), name);
    }
    // It stands on the ground, its antlers above its back.
    float low = 1e9F;
    float high = -1e9F;
    for (const scene3d::ModelSurface& surface : model.meshes[0].surfaces) {
        for (const auto& vertex : surface.vertices) {
            low = std::min(low, vertex.position[1]);
            high = std::max(high, vertex.position[1]);
        }
    }
    CHECK(low == doctest::Approx(0.0).epsilon(0.02));
    CHECK(high > 1.1F);
    std::vector<std::string> animations;
    for (const scene3d::ModelAnimation& animation : model.animations) {
        animations.push_back(animation.name);
        CHECK_FALSE(animation.tracks.empty());
    }
    CHECK(animations == std::vector<std::string>{"idle", "walk", "run", "eat", "attack"});
    std::vector<std::string> nodes;
    for (const scene3d::ModelNode& node : model.nodes) {
        nodes.push_back(node.name);
    }
    CHECK(has(nodes, "saddle"));
    CHECK(has(nodes, "mouth"));
    // A bird's model flies; an insect's wings are see-through.
    const scene3d::Model sparrow = scene3d::build_creature(recipe_of(kSparrow));
    CHECK(std::ranges::any_of(sparrow.animations, [](const auto& a) { return a.name == "fly"; }));
    const scene3d::Model fly = scene3d::build_creature(recipe_of(
        R"({"format": 1, "kind": "insect", "body": {"length": 0.02, "height": 0.01},
            "parts": {"wings": 0.03}})"));
    const auto glass = std::ranges::find_if(
        fly.materials, [](const scene3d::ModelMaterial& m) { return m.name == "wings"; });
    REQUIRE(glass != fly.materials.end());
    CHECK(glass->standard.transparency == "alpha");
    // Through load_model and check_model, by its path.
    auto loaded = scene3d::load_model(kDeer, "deer.creature.json", nullptr);
    REQUIRE_MESSAGE(loaded.ok(), loaded.error().message);
    CHECK(loaded.value().animations.size() == 5);
    const auto errors =
        scene3d::check_model(R"({"format": 1, "kind": "fish", "parts": {"horns": "horns"}})",
                             "bad.creature.json", nullptr);
    REQUIRE_FALSE(errors.empty());
    CHECK(errors.front().path == "/parts/horns");
}

TEST_CASE("fauna: a 2D cut-out has a bone for each of the skeleton's, the far ones behind") {
    const world::CreatureRecipe deer = recipe_of(kDeer);
    const std::vector<world::CutoutBone> bones = world::creature_cutout(deer);
    CHECK(bones.size() == world::creature_skeleton(deer).size());
    for (const world::CutoutBone& bone : bones) {
        CHECK_MESSAGE(!bone.shapes.empty(), bone.name);
        if (bone.name.ends_with("_far")) {
            CHECK(bone.z_index < 0);
        }
    }
    CHECK(bones.front().name == "body");
    CHECK(bones.front().position.y == doctest::Approx(world::creature_measures(deer).back));
}

TEST_CASE("fauna: a wary herd roams, keeps together and flees a person") {
    Project project("fauna_herd_project", kFiles);
    json::Value herd = project.ok(
        "creature.populate",
        R"({"recipe": "fauna/deer.creature.json", "at": [0, 0, 0], "count": 4, "radius": 4})");
    REQUIRE(herd["placed"].as_array().size() == 4);
    CHECK(herd["placed"].as_array().front().as_string() == "deer_1");
    project.ok("events.subscribe", R"({"kinds": ["creature_fled", "creature_noticed"]})");
    project.ok("step", R"({"ticks": 600})");
    // Ten seconds on, they are near home and near each other.
    for (const json::Value& id : herd["placed"].as_array()) {
        const json::Value state = project.state(id.as_string());
        CHECK(distance(field(state, "position"), parse("[0, 0, 0]")) < 15.0 + 1.0);
        CHECK(field(state, "mode").as_string() != "fleeing");
    }
    CHECK(project.events("events_1").empty());
    // A person walks into the herd: they run.
    project.ok("entity.create", R"({"id": "hunter", "components": {
        "Transform3D": {"position": [0, 0, 0]}, "CharacterBody3D": {}}})");
    project.ok("step", R"({"ticks": 30})");
    const std::vector<std::string> events = project.events("events_1");
    CHECK(has(events, "creature_fled"));
    CHECK(has(events, "creature_noticed"));
    const json::Value fleeing = project.state("deer_1");
    CHECK(field(fleeing, "mode").as_string() == "fleeing");
    CHECK(field(fleeing, "target").as_string() == "hunter");
    CHECK(field(fleeing, "speed").as_double() > 3.0);
    // They face where they run.
    const auto& transform = project.session->world().ecs().get<world::Transform3D>(
        project.session->world().find("deer_1"));
    const world::CreatureMind& mind =
        project.session->world()
            .ecs()
            .get<world::Creature3D>(project.session->world().find("deer_1"))
            .mind;
    CHECK(std::abs(std::sin(transform.rotation.y) -
                   (mind.velocity.x / std::hypot(mind.velocity.x, mind.velocity.z))) < 0.05);
    // Their AnimationPlayers run.
    const auto& player = project.session->world().ecs().get<world::AnimationPlayer>(
        project.session->world().find("deer_1"));
    CHECK(player.animation == "run");
}

TEST_CASE("fauna: a hungry predator hunts and catches its prey") {
    Project project("fauna_hunt_project", kFiles);
    project.ok("creature.place",
               R"({"recipe": "fauna/deer.creature.json", "id": "deer", "at": [6, 0, 0]})");
    project.ok("creature.place",
               R"({"recipe": "fauna/wolf.creature.json", "id": "wolf", "at": [-6, 0, 0]})");
    project.ok("component.set",
               R"({"id": "wolf", "components": {"Creature3D": {"mind": {"hunger": 0.9}}}})");
    project.ok("events.subscribe", R"({"kinds": ["creature_caught"]})");
    project.ok("step", R"({"ticks": 20})");
    CHECK(project.state("wolf")["mode"].as_string() == "hunting");
    CHECK(project.state("deer")["mode"].as_string() == "fleeing");
    project.ok("step", R"({"ticks": 900})");
    CHECK(project.events("events_1") == std::vector<std::string>{"creature_caught"});
    CHECK(project.state("deer")["mode"].as_string() == "caught");
    CHECK(project.state("deer")["speed"].as_double() == 0.0);
    CHECK(project.state("wolf")["hunger"].as_double() == 0.0);
}

TEST_CASE("fauna: fed enough, a friendly dog is tamed, follows and can be ridden") {
    Project project("fauna_dog_project", kFiles);
    project.ok("entity.create", R"({"id": "kid", "components": {
        "Transform3D": {"position": [0, 0, 0]}, "CharacterBody3D": {}}})");
    project.ok("creature.place",
               R"({"recipe": "fauna/dog.creature.json", "id": "dog", "at": [6, 0, 0]})");
    project.ok("step", R"({"ticks": 240})");
    // Friendly: it comes to the kid.
    CHECK(distance(project.state("dog")["position"], parse("[0, 0, 0]")) < 2.5);
    command::Reply wrong =
        project.call("creature.interact",
                     R"({"creature": "dog", "action": "feed", "by": "kid", "food": "rock"})");
    CHECK_FALSE(wrong.ok);
    CHECK(wrong.message == "/food: it doesn't eat \"rock\"; it eats bone");
    command::Reply early =
        project.call("creature.interact", R"({"creature": "dog", "action": "ride", "by": "kid"})");
    CHECK(early.message.starts_with("/action: only its owner can ride it"));
    json::Value fed =
        project.ok("creature.interact",
                   R"({"creature": "dog", "action": "feed", "by": "kid", "food": "bone"})");
    CHECK(fed == parse(R"({"tamed": false, "mode": "eating", "fed": 1})"));
    fed = project.ok("creature.interact",
                     R"({"creature": "dog", "action": "feed", "by": "kid", "food": "bone"})");
    CHECK(fed["tamed"].as_bool());
    CHECK(project.state("dog")["owner"].as_string() == "kid");
    // Tame, it follows its owner.
    project.ok("component.set",
               R"({"id": "kid", "components": {"Transform3D": {"position": [10, 0, 0]}}})");
    project.ok("step", R"({"ticks": 300})");
    CHECK(distance(project.state("dog")["position"], parse("[10, 0, 0]")) < 5.0);
    // Ridden, it carries the kid where it is steered.
    project.ok("creature.interact", R"({"creature": "dog", "action": "ride", "by": "kid"})");
    project.ok("creature.interact",
               R"({"creature": "dog", "action": "steer", "direction": [0, 0, 1]})");
    const json::Value before = project.state("dog")["position"];
    project.ok("step", R"({"ticks": 60})");
    const json::Value after = project.state("dog")["position"];
    CHECK(after.as_array()[2].as_double() - before.as_array()[2].as_double() > 2.0);
    CHECK(project.state("dog")["mode"].as_string() == "ridden");
    const auto& kid = project.session->world().ecs().get<world::Transform3D>(
        project.session->world().find("kid"));
    CHECK(kid.position.z == doctest::Approx(after.as_array()[2].as_double()).epsilon(0.05));
    project.ok("creature.interact", R"({"creature": "dog", "action": "dismount"})");
    CHECK(project.state("dog")["mode"].as_string() == "idle");
    project.ok("creature.interact", R"({"creature": "dog", "action": "release"})");
    CHECK(project.state("dog").find("owner") == nullptr);
}

TEST_CASE("fauna: 2D creatures walk along the ground, swim and fly, turning their cut-outs") {
    Project project("fauna_2d_project", kFiles,
                    R"({"format": 1, "entities": {"camera": {"components": {
                        "Transform2D": {"position": [400, 300]}, "Camera2D": {}}}}})");
    project.ok("creature.place",
               R"({"recipe": "fauna/deer.creature.json", "id": "doe", "at": [100, 400]})");
    json::Value school = project.ok(
        "creature.populate",
        R"({"recipe": "fauna/minnow.creature.json", "at": [500, 200], "count": 5, "radius": 250,
            "pixels_per_meter": 200})");
    CHECK(school["placed"].as_array().size() == 5);
    project.ok("step", R"({"ticks": 2})");
    world::World& world = project.session->world();
    REQUIRE((world.find("doe_body") != entt::null));
    REQUIRE((world.find("doe_leg_front_near") != entt::null));
    CHECK(world.ecs().all_of<world::Polygon2D>(world.find("doe_head")));
    // A person comes close from the left: the doe runs right along the ground.
    project.ok("entity.create", R"({"id": "walker", "components": {
        "Transform2D": {"position": [-150, 400]}, "CharacterBody2D": {}}})");
    project.ok("step", R"({"ticks": 45})");
    const json::Value doe = project.state("doe");
    CHECK(field(doe, "mode").as_string() == "fleeing");
    CHECK(field(doe, "position").as_array()[0].as_double() > 150.0);
    CHECK(field(doe, "position").as_array()[1].as_double() == doctest::Approx(400.0));
    CHECK_FALSE(world.ecs().get<world::Creature2D>(world.find("doe")).flip_h);
    // Its legs swing as it runs.
    const float swing =
        world.ecs().get<world::Transform2D>(world.find("doe_leg_front_near")).rotation;
    project.ok("step", R"({"ticks": 7})");
    CHECK(world.ecs().get<world::Transform2D>(world.find("doe_leg_front_near")).rotation != swing);
    // Fish wander in x and y; a fish swimming left is mirrored.
    project.ok("step", R"({"ticks": 600})");
    bool vertical = false;
    for (const json::Value& id : school["placed"].as_array()) {
        const json::Value state = project.state(id.as_string());
        vertical =
            vertical || std::abs(field(state, "position").as_array()[1].as_double() - 200.0) > 5.0;
        const auto& fish = world.ecs().get<world::Creature2D>(world.find(id.as_string()));
        const auto& root = world.ecs().get<world::Transform2D>(world.find(id.as_string()));
        CHECK((root.scale.x < 0.0F) == fish.flip_h);
    }
    CHECK(vertical);
    // A ragdoll fits the cut-out: a body per bone, a capsule along each leg's polygon.
    const json::Value ragdoll =
        project.ok("ragdoll.generate", R"({"id": "doe", "out": "fauna/doe.ragdoll.json"})");
    CHECK(field(ragdoll, "bones").as_array().size() == 8);
    auto profile = slash::platform::read_file("fauna_2d_project/fauna/doe.ragdoll.json");
    REQUIRE(profile.ok());
    const json::Value written = parse(profile.value());
    const json::Value& leg = field(field(written, "bones"), "doe_leg_front_near");
    CHECK(field(leg, "shape").as_string() == "capsule");
    CHECK(field(leg, "height").as_double() > 20.0);
}

TEST_CASE("fauna: runs repeat, and creature commands report their errors by path") {
    const auto run = [] {
        Project project("fauna_repeat_project", kFiles);
        project.ok("creature.populate", R"({"recipe": "fauna/deer.creature.json",
            "at": [0, 0, 0], "count": 3, "seed": 7})");
        project.ok("creature.populate", R"({"recipe": "fauna/sparrow.creature.json",
            "at": [0, 0, 0], "count": 3})");
        project.ok("step", R"({"ticks": 400})");
        return project.ok("creature.state");
    };
    const json::Value first = run();
    CHECK(field(first, "creatures").as_array().size() == 6);
    CHECK(first == run());
    // Sparrows fly at their altitude.
    for (const json::Value& state : field(first, "creatures").as_array()) {
        if (field(state, "id").as_string().starts_with("sparrow")) {
            CHECK(field(state, "position").as_array()[1].as_double() > 2.0);
        }
    }
    Project project("fauna_errors_project", kFiles);
    CHECK(project.call("creature.place", R"({"recipe": "deer.glb", "id": "x", "at": [0, 0]})")
              .message.starts_with("/recipe"));
    CHECK(project.call("creature.interact", R"({"creature": "camera", "action": "release"})")
              .message == "/creature: \"camera\" has no Creature3D or Creature2D");
    CHECK(project.call("creature.interact", R"({"creature": "nobody", "action": "release"})")
              .message.starts_with("/creature: no entity has the id"));
    project.ok("creature.place",
               R"({"recipe": "fauna/deer.creature.json", "id": "deer", "at": [0, 0, 0]})");
    CHECK(project.call("creature.interact", R"({"creature": "deer", "action": "feed"})").message ==
          "/by: feed needs who does it");
    CHECK(project
              .call("creature.interact",
                    R"({"creature": "deer", "action": "steer", "direction": [1, 0]})")
              .message == "/direction: a 3D creature steers by [x, y, z]");
}
