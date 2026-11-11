// Flesh: a character's anatomy (its bones and muscles on its body's frames, in 3D and 2D), the
// muscles' flesh stepped from the bones that move it, skin bound to it, the "::skeleton" and
// "::muscles" model forms, and the flesh_3d and flesh_2d systems behind character.place's flesh
// and flesh.get.
#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/platform/file.hpp"
#include "slash/scene3d/character.hpp"
#include "slash/scene3d/flesh.hpp"
#include "slash/scene3d/model.hpp"
#include "slash/scene3d/scene3d.hpp"
#include "slash/world/character.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/flesh.hpp"
#include "slash/world/tree3d.hpp"
#include "slash/world/world.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
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
using slash::real_t;
using slash::Vec3;

namespace {

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE_MESSAGE(parsed.ok(), text);
    return std::move(parsed).value();
}

constexpr std::string_view kAthlete = R"({"format": 1, "name": "Athlete",
    "body": {"height": 1.8, "build": 0.6, "muscle": 0.9, "fat": 0.1},
    "clothing": [{"kind": "shorts", "color": [0.2, 0.3, 0.6]}]})";
constexpr std::string_view kBaker = R"({"format": 1, "name": "Baker",
    "body": {"height": 1.7, "build": 0.4},
    "clothing": [{"kind": "shirt", "color": [0.9, 0.9, 0.85]},
                 {"kind": "pants", "color": [0.3, 0.25, 0.2]}]})";

world::CharacterRecipe recipe_of(std::string_view text) {
    auto recipe = world::parse_character(parse(text), nullptr);
    REQUIRE_MESSAGE(recipe.ok(), recipe.error().message);
    return std::move(recipe).value();
}

const world::AnatomyMuscle* muscle_named(const world::Anatomy& anatomy, std::string_view name) {
    for (const world::AnatomyMuscle& muscle : anatomy.muscles) {
        if (muscle.name == name) {
            return &muscle;
        }
    }
    return nullptr;
}

const world::MuscleReport* report_named(const std::vector<world::MuscleReport>& reports,
                                        std::string_view name) {
    for (const world::MuscleReport& report : reports) {
        if (report.name == name) {
            return &report;
        }
    }
    return nullptr;
}

real_t dot(Vec3 a, Vec3 b) {
    return (a.x * b.x) + (a.y * b.y) + (a.z * b.z);
}

Vec3 cross(Vec3 a, Vec3 b) {
    return {(a.y * b.z) - (a.z * b.y), (a.z * b.x) - (a.x * b.z), (a.x * b.y) - (a.y * b.x)};
}

Vec3 normalized(Vec3 v) {
    return v * (1.0F / std::sqrt(dot(v, v)));
}

real_t length_of(Vec3 v) {
    return std::sqrt(dot(v, v));
}

// A turn of angle radians about axis (a unit vector) through pivot.
world::Affine3D turn_about(Vec3 axis, real_t angle, Vec3 pivot) {
    const real_t c = std::cos(angle);
    const real_t s = std::sin(angle);
    const auto turned = [&](Vec3 v) {
        return (v * c) + (cross(axis, v) * s) + (axis * (dot(axis, v) * (1.0F - c)));
    };
    world::Affine3D out;
    out.x = turned({1.0F, 0.0F, 0.0F});
    out.y = turned({0.0F, 1.0F, 0.0F});
    out.z = turned({0.0F, 0.0F, 1.0F});
    out.origin = pivot - out.basis(pivot);
    return out;
}

// A body's flesh and a way to step it.
struct Body {
    world::Anatomy anatomy;
    world::FleshBody flesh;
    world::FleshState state;
    std::vector<world::Affine3D> moves;

    explicit Body(const world::CharacterRecipe& recipe, bool flat = false) {
        const world::CharacterMeasures m = world::character_measures(recipe);
        anatomy =
            world::make_anatomy(recipe, flat ? world::body_frames_2d(m) : world::body_frames_3d(m));
        flesh = world::build_flesh(anatomy);
        moves.assign(flesh.bones.size(), world::Affine3D{});
    }

    void step(int ticks, world::FleshSettings settings = {}, Vec3 push = {}) {
        for (int i = 0; i < ticks; ++i) {
            world::step_flesh(flesh, state,
                              {.moves = moves,
                               .acceleration = push,
                               .seconds = 1.0F / 60.0F,
                               .settings = settings});
        }
    }

    // The farthest any free point is from where its bones alone carry it.
    [[nodiscard]] real_t farthest_from_skinned() const {
        real_t most = 0.0F;
        for (std::size_t i = 0; i < flesh.points.size(); ++i) {
            if (!flesh.points[i].pinned) {
                most = std::max(most, length_of(state.positions[i] - state.skinned[i]));
            }
        }
        return most;
    }

    [[nodiscard]] real_t fastest() const {
        real_t most = 0.0F;
        for (const Vec3 v : state.velocities) {
            most = std::max(most, length_of(v));
        }
        return most;
    }

    // Bends the left elbow by angle radians, the forearm toward the front.
    void bend_left_elbow(real_t angle) {
        const world::PartFrame* lower =
            world::body_frames_3d(world::character_measures(recipe_of(kAthlete)))
                .find(world::BodyPart::LowerArm, 0);
        REQUIRE(lower != nullptr);
        const Vec3 axis = normalized(lower->tail - lower->head);
        const world::Affine3D turn =
            turn_about(normalized(cross(axis, lower->front)), angle, lower->head);
        for (std::size_t i = 0; i < flesh.bones.size(); ++i) {
            const std::string& bone = flesh.bones[i];
            if (bone.starts_with("LeftLowerArm") || bone.starts_with("LeftHand")) {
                moves[i] = turn;
            }
        }
    }
};

struct Project {
    std::string root;
    world::ComponentRegistry components;
    command::Registry commands;
    std::unique_ptr<command::Session> session;

    explicit Project(std::string name) : root(std::move(name)) {
        std::filesystem::remove_all(root);
        REQUIRE(slash::platform::create_directories(root + "/characters").ok());
        REQUIRE(slash::platform::write_file(root + "/characters/athlete.character.json", kAthlete)
                    .ok());
        REQUIRE(
            slash::platform::write_file(root + "/characters/baker.character.json", kBaker).ok());
        REQUIRE(slash::platform::write_file(root + "/main.scene.json",
                                            R"({"format": 1, "entities": {
              "camera": {"components": {"Transform3D": {"position": [0, 1, 4]},
                                        "Camera3D": {"current": true}}}}})")
                    .ok());
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
};

} // namespace

TEST_CASE("flesh: a body's muscle and fat are read, and grow its muscles") {
    const world::CharacterRecipe athlete = recipe_of(kAthlete);
    CHECK(athlete.body.muscle == doctest::Approx(0.9));
    CHECK(athlete.body.fat == doctest::Approx(0.1));
    const world::CharacterRecipe baker = recipe_of(kBaker);
    CHECK(baker.body.muscle == doctest::Approx(0.5));
    CHECK(baker.body.fat == doctest::Approx(0.3));
    CHECK_FALSE(world::parse_character(parse(R"({"format": 1, "name": "X",
        "body": {"muscle": 1.5}})"),
                                       nullptr)
                    .ok());
    const Body strong(athlete);
    const Body soft(recipe_of(R"({"format": 1, "name": "Athlete",
        "body": {"height": 1.8, "build": 0.6, "muscle": 0.1}})"));
    const world::FleshMuscle* a = nullptr;
    const world::FleshMuscle* b = nullptr;
    for (std::size_t i = 0; i < strong.flesh.muscles.size(); ++i) {
        if (strong.flesh.muscles[i].name == "biceps_left") {
            a = &strong.flesh.muscles[i];
            b = &soft.flesh.muscles[i];
        }
    }
    REQUIRE(a != nullptr);
    CHECK(a->rest_radius > b->rest_radius * 1.3F);
}

TEST_CASE("flesh: a 3D anatomy has every bone and muscle on both sides, on the rig's bones") {
    const Body body(recipe_of(kAthlete));
    const world::Anatomy& anatomy = body.anatomy;
    CHECK(anatomy.dimensions == 3);
    CHECK(world::muscle_kinds().size() == 28);
    CHECK(anatomy.muscles.size() == world::muscle_kinds().size() * 2);
    for (const std::string_view kind : world::anatomy_bone_kinds()) {
        const bool found = std::ranges::any_of(
            anatomy.bones, [kind](const world::AnatomyBone& bone) { return bone.kind == kind; });
        CHECK_MESSAGE(found, kind);
    }
    for (const world::AnatomyBone& bone : anatomy.bones) {
        CHECK_MESSAGE(!bone.pieces.empty(), bone.name);
        CHECK_MESSAGE(!bone.bone.empty(), bone.name);
    }
    for (const world::AnatomyMuscle& muscle : anatomy.muscles) {
        CHECK_MESSAGE(muscle.rings.size() == 7, muscle.name);
        CHECK_MESSAGE(muscle.rings[3].ru > 0.0F, muscle.name);
    }
    // The biceps runs down the front of the upper arm, from the shoulder to the forearm, and its
    // left one is on the body's left (+x, as Godot's humanoid faces +z).
    const world::AnatomyMuscle* biceps = muscle_named(anatomy, "biceps_left");
    REQUIRE(biceps != nullptr);
    CHECK(biceps->origin_bone == "LeftUpperArm");
    CHECK(biceps->insertion_bone == "LeftLowerArm");
    CHECK(biceps->group == "arm");
    CHECK(biceps->rings.front().center.y > biceps->rings.back().center.y);
    CHECK(biceps->rings[3].center.x > 0.0F);
    const world::AnatomyMuscle* right = muscle_named(anatomy, "biceps_right");
    REQUIRE(right != nullptr);
    CHECK(right->rings[3].center.x == doctest::Approx(-biceps->rings[3].center.x));
    // A muscle stays outside the bone it rests on.
    for (const world::AnatomyMuscle& muscle : anatomy.muscles) {
        const Vec3 span = muscle.bone_b - muscle.bone_a;
        const real_t span2 = std::max<real_t>(dot(span, span), 1e-9F);
        const Vec3 middle = muscle.rings[3].center;
        const real_t t = std::clamp<real_t>(dot(middle - muscle.bone_a, span) / span2, 0.0F, 1.0F);
        const real_t away = length_of(middle - (muscle.bone_a + (span * t)));
        CHECK_MESSAGE(away >= muscle.bone_radius * 0.99F, muscle.name);
    }
}

TEST_CASE("flesh: a 2D anatomy is the side view: one of each middle pair, on the cut-out") {
    const Body flat(recipe_of(kAthlete), true);
    CHECK(flat.anatomy.dimensions == 2);
    CHECK(flat.flesh.dimensions == 2);
    CHECK(flat.anatomy.sides[0] == "back");
    CHECK(flat.anatomy.sides[1] == "front");
    CHECK(muscle_named(flat.anatomy, "biceps_back") != nullptr);
    CHECK(muscle_named(flat.anatomy, "biceps_front") != nullptr);
    CHECK(muscle_named(flat.anatomy, "rectus_abdominis_front") != nullptr);
    CHECK(muscle_named(flat.anatomy, "rectus_abdominis_back") == nullptr);
    CHECK(flat.anatomy.muscles.size() < world::muscle_kinds().size() * 2);
    const world::AnatomyMuscle* biceps = muscle_named(flat.anatomy, "biceps_front");
    REQUIRE(biceps != nullptr);
    CHECK(biceps->origin_bone == "upper_arm_front");
    for (const world::FleshElement& element : flat.flesh.elements) {
        CHECK(element.volume > 0.0F);
    }
}

TEST_CASE("flesh: at rest flesh stays where its bones put it, the same every run") {
    Body body(recipe_of(kAthlete));
    body.step(90);
    CHECK(body.farthest_from_skinned() < 0.01F);
    CHECK(body.fastest() < 0.05F);
    for (const world::MuscleReport& report : world::muscle_reports(body.flesh, body.state)) {
        CHECK_MESSAGE(report.activation == doctest::Approx(0.0), report.name);
        CHECK_MESSAGE(report.length == doctest::Approx(1.0).epsilon(0.01), report.name);
        CHECK_MESSAGE(std::abs(report.bulge) < 0.05F, report.name);
    }
    Body again(recipe_of(kAthlete));
    again.step(90);
    CHECK(again.state.positions == body.state.positions);
}

TEST_CASE("flesh: bending the elbow shortens the biceps, which bulges, and works it") {
    Body body(recipe_of(kAthlete));
    body.step(10);
    body.bend_left_elbow(2.0F);
    body.step(60);
    const std::vector<world::MuscleReport> reports = world::muscle_reports(body.flesh, body.state);
    const world::MuscleReport* left = report_named(reports, "biceps_left");
    const world::MuscleReport* right = report_named(reports, "biceps_right");
    REQUIRE(left != nullptr);
    REQUIRE(right != nullptr);
    CHECK(left->length < 0.9F);
    CHECK(left->bulge > 0.05F);
    CHECK(left->activation > 0.2F);
    CHECK(right->length == doctest::Approx(1.0).epsilon(0.01));
    CHECK(left->bulge > right->bulge + 0.04F);
    // The flesh followed the forearm: no free point is torn far from the shape its muscle takes
    // between its moved ends.
    real_t torn = 0.0F;
    for (std::size_t i = 0; i < body.flesh.points.size(); ++i) {
        torn = std::max(torn, length_of(body.state.positions[i] - body.state.targets[i]));
    }
    CHECK(torn < 0.06F);
}

TEST_CASE("flesh: a muscle flexed by hand bulges without moving a bone") {
    Body body(recipe_of(kAthlete));
    body.step(30);
    const real_t before =
        report_named(world::muscle_reports(body.flesh, body.state), "biceps_left")->bulge;
    const std::map<std::string, float> flexed{{"biceps", 1.0F}};
    body.step(60, {.muscles = &flexed});
    const std::vector<world::MuscleReport> reports = world::muscle_reports(body.flesh, body.state);
    CHECK(report_named(reports, "biceps_left")->activation == doctest::Approx(1.0));
    CHECK(report_named(reports, "biceps_left")->bulge > before + 0.05F);
    CHECK(report_named(reports, "triceps_left")->activation == doctest::Approx(0.0));
    // A group flexes every muscle in it.
    const std::map<std::string, float> core{{"core", 0.5F}};
    body.step(1, {.muscles = &core});
    CHECK(report_named(world::muscle_reports(body.flesh, body.state), "external_oblique_right")
              ->activation == doctest::Approx(0.5));
}

TEST_CASE("flesh: a jolt makes flesh jiggle, then it settles, the stiffer the sooner") {
    const auto ring_out = [](float jiggle) {
        Body body(recipe_of(kAthlete));
        body.step(30);
        body.step(3, {.jiggle = jiggle}, {0.0F, 0.0F, 40.0F});
        const real_t moved = body.farthest_from_skinned();
        body.step(20, {.jiggle = jiggle});
        return std::pair{moved, body.fastest()};
    };
    const auto [loose_moved, loose_later] = ring_out(1.0F);
    const auto [firm_moved, firm_later] = ring_out(0.0F);
    CHECK(loose_moved > 0.002F);
    CHECK(loose_moved > firm_moved);
    CHECK(loose_later >= firm_later);
    Body body(recipe_of(kAthlete));
    body.step(30);
    body.step(3, {}, {0.0F, 0.0F, 40.0F});
    body.step(240);
    CHECK(body.farthest_from_skinned() < 0.01F);
}

TEST_CASE("flesh: skin follows the muscles under its part, less under fat") {
    Body body(recipe_of(kAthlete));
    const world::AnatomyMuscle* biceps = muscle_named(body.anatomy, "biceps_left");
    REQUIRE(biceps != nullptr);
    const world::MuscleRing& middle = biceps->rings[3];
    // A point of skin over the biceps' belly, one on the other arm and one on the head.
    const Vec3 over = middle.center + (middle.u * (middle.ru * 1.5F));
    const std::vector<world::SkinPoint> skin{
        {.rest = over, .part = {.part = world::BodyPart::UpperArm, .side = 0}},
        {.rest = {-over.x, over.y, over.z}, .part = {.part = world::BodyPart::UpperArm, .side = 1}},
        {.rest = {0.0F, 1.7F, 0.0F}, .part = {.part = world::BodyPart::Head, .side = -1}}};
    const world::FleshBinding lean = world::bind_skin(body.flesh, skin, 0.0F);
    const world::FleshBinding fat = world::bind_skin(body.flesh, skin, 0.9F);
    body.step(30);
    std::vector<Vec3> at_rest;
    world::skin_offsets(body.state, lean, at_rest);
    REQUIRE(at_rest.size() == 3);
    CHECK(length_of(at_rest[0]) < 0.005F);
    const std::map<std::string, float> flexed{{"biceps_left", 1.0F}};
    body.step(60, {.muscles = &flexed});
    std::vector<Vec3> moved;
    world::skin_offsets(body.state, lean, moved);
    std::vector<Vec3> padded;
    world::skin_offsets(body.state, fat, padded);
    CHECK(length_of(moved[0]) > 0.003F);
    CHECK(dot(moved[0], middle.u) > 0.0F);
    CHECK(length_of(moved[1]) < 0.002F);
    CHECK(length_of(moved[2]) < 0.002F);
    CHECK(length_of(padded[0]) < length_of(moved[0]));
}

TEST_CASE("flesh: a character's model forms show its skeleton and its muscles on its rig") {
    const auto read = [](std::string_view) -> slash::Result<std::string> {
        return slash::make_error(slash::ErrorCode::NotFound, "no such file");
    };
    const auto materials = [](const scene3d::Model& model) {
        std::vector<std::string> names;
        for (const scene3d::ModelMaterial& material : model.materials) {
            names.push_back(material.name);
        }
        return names;
    };
    auto skeleton = scene3d::build_character(recipe_of(kAthlete), read, "skeleton");
    REQUIRE_MESSAGE(skeleton.ok(), skeleton.error().message);
    CHECK(materials(skeleton.value()) == std::vector<std::string>{"bone"});
    CHECK_FALSE(skeleton.value().skins.empty());
    auto muscles = scene3d::build_character(recipe_of(kAthlete), read, "muscles");
    REQUIRE_MESSAGE(muscles.ok(), muscles.error().message);
    CHECK(materials(muscles.value()) == std::vector<std::string>{"bone", "muscle", "tendon"});
    auto whole = scene3d::build_character(recipe_of(kAthlete), read);
    REQUIRE(whole.ok());
    CHECK(muscles.value().nodes.size() == whole.value().nodes.size());
    auto wrong = scene3d::build_character(recipe_of(kAthlete), read, "organs");
    REQUIRE_FALSE(wrong.ok());
    CHECK(wrong.error().message.find("skeleton and muscles") != std::string::npos);

    Project project("flesh_forms_project");
    project.ok("entity.create", R"({"id": "body", "components": {"Transform3D": {},
        "MeshInstance3D": {"mesh": "characters/athlete.character.json::muscles"}}})");
    CHECK(scene3d::check_meshes(project.session->world()).empty());
    project.ok("entity.create", R"({"id": "bad", "components": {
        "Transform3D": {}, "MeshInstance3D": {"mesh": "characters/athlete.character.json::x"}}})");
    CHECK_FALSE(scene3d::check_meshes(project.session->world()).empty());
}

TEST_CASE("flesh: character.place adds flesh, which flesh.get reads as it steps") {
    Project project("flesh_place_project");
    const json::Value placed = project.ok(
        "character.place",
        R"({"recipe": "characters/athlete.character.json", "id": "athlete", "at": [0, 0, 0],
            "flesh": true})");
    CHECK(placed.find("components")->as_array().back().as_string() == "Flesh3D");
    const command::Reply early = project.call("flesh.get", R"({"id": "athlete"})");
    CHECK_FALSE(early.ok);
    CHECK(early.message.find("no flesh yet") != std::string::npos);
    project.ok("step", R"({"ticks": 3})");
    json::Value got = project.ok("flesh.get", R"({"id": "athlete", "muscles": ["biceps"]})");
    CHECK(got.find("dimensions")->as_int() == 3);
    REQUIRE(got.find("muscles")->as_array().size() == 2);
    CHECK(got.find("muscles")->as_array()[0].find("name")->as_string() == "biceps_left");
    CHECK(got.find("bones") == nullptr);
    got = project.ok("flesh.get", R"({"id": "athlete", "muscles": ["arm"], "bones": true})");
    CHECK(got.find("muscles")->as_array().size() > 2);
    const json::Array& bones = got.find("bones")->as_array();
    CHECK(std::ranges::any_of(bones, [](const json::Value& bone) {
        return bone.find("name")->as_string() == "humerus_left" &&
               bone.find("bone")->as_string() == "LeftUpperArm";
    }));
    // Flexing by hand shows in what it reads.
    project.ok("component.set", R"({"id": "athlete", "components": {"Flesh3D": {
        "muscles": {"biceps": 1}}}})");
    project.ok("step", R"({"ticks": 30})");
    got = project.ok("flesh.get", R"({"id": "athlete", "muscles": ["biceps_left"]})");
    CHECK(got.find("muscles")->as_array()[0].find("activation")->as_double() ==
          doctest::Approx(1.0));
    CHECK(got.find("muscles")->as_array()[0].find("bulge")->as_double() > 0.05);
    // Its skin moves by it as it is drawn.
    CHECK(scene3d::flesh_pose(project.session->world(), project.session->world().find("athlete")) !=
          nullptr);
    json::Value frame = project.ok("frame.capture", R"({"points": [[0, 0]]})");
    CHECK(frame["drawn"]["meshes"].as_int() >= 1);
    const command::Reply none = project.call("flesh.get", R"({"id": "camera"})");
    CHECK_FALSE(none.ok);
    CHECK(none.message.find("no Flesh3D or Flesh2D") != std::string::npos);
}

TEST_CASE("flesh: a Character2D's flesh moves its parts and shows its anatomy") {
    Project project("flesh_2d_project");
    project.ok("character.place",
               R"({"recipe": "characters/baker.character.json", "id": "baker", "at": [200, 300],
                   "flesh": true})");
    project.ok("step", R"({"ticks": 2})");
    world::World& world = project.session->world();
    json::Value got = project.ok("flesh.get", R"({"id": "baker", "muscles": ["biceps"]})");
    CHECK(got.find("dimensions")->as_int() == 2);
    CHECK(got.find("muscles")->as_array().size() == 2);
    const entt::entity arm = world.find("baker_upper_arm_front");
    REQUIRE((arm != entt::null));
    REQUIRE(world::flesh_offsets_2d(world, arm) != nullptr);
    CHECK(world::flesh_offsets_2d(world, arm)->size() ==
          world.ecs().get<world::Polygon2D>(arm).polygon.size());
    CHECK((world.find("baker_anatomy") == entt::null));
    project.ok("component.set", R"({"id": "baker", "components": {"Flesh2D": {
        "view": "muscles", "muscles": {"biceps_front": 1}}}})");
    project.ok("step", R"({"ticks": 20})");
    const entt::entity view = world.find("baker_anatomy");
    REQUIRE((view != entt::null));
    REQUIRE(world.ecs().all_of<world::Polygon2D>(view));
    CHECK_FALSE(world.ecs().get<world::Polygon2D>(view).polygon.empty());
    bool moved = false;
    for (const slash::Vec2 offset : *world::flesh_offsets_2d(world, arm)) {
        moved = moved || std::abs(offset.x) + std::abs(offset.y) > 0.05F;
    }
    CHECK(moved);
    project.ok("component.set", R"({"id": "baker", "components": {"Flesh2D": {
        "view": "skeleton"}}})");
    project.ok("step", R"({"ticks": 1})");
    CHECK_FALSE(world.ecs().get<world::Polygon2D>(world.find("baker_anatomy")).polygon.empty());
    project.ok("component.set", R"({"id": "baker", "components": {"Flesh2D": {
        "enabled": false}}})");
    project.ok("step", R"({"ticks": 1})");
    CHECK(world::flesh_offsets_2d(world, arm) == nullptr);
}
