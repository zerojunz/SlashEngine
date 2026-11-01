#include "slash/command/builtin.hpp"
#include "slash/command/session.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/tree3d.hpp"
#include "slash/world/world.hpp"
#include "slash/xr/xr.hpp"

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

using namespace slash;

namespace {

constexpr double kTick = 1.0 / 60.0;

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE_MESSAGE(parsed.ok(), (parsed.ok() ? std::string() : parsed.error().message));
    return std::move(parsed).value();
}

// An XROrigin3D with a camera, two controllers and an anchor under it.
struct Rig {
    world::ComponentRegistry components;
    std::unique_ptr<world::World> world;
    entt::entity origin = entt::null;
    entt::entity camera = entt::null;
    entt::entity left = entt::null;
    entt::entity right = entt::null;
    std::vector<world::Event> events;

    explicit Rig(xr::Settings settings = {.enabled = true}) {
        world::register_builtin_components(components);
        world = std::make_unique<world::World>(components);
        world->set_event_listener([this](const world::Event& event) { events.push_back(event); });
        xr::install(*world, settings);
        auto& ecs = world->ecs();
        origin = world->create("origin").value();
        camera = world->create("camera").value();
        left = world->create("left").value();
        right = world->create("right").value();
        ecs.emplace<world::Transform3D>(origin).children = {camera, left, right};
        ecs.emplace<world::XROrigin3D>(origin);
        ecs.emplace<world::Transform3D>(camera);
        ecs.emplace<world::Camera3D>(camera);
        ecs.emplace<world::XRCamera3D>(camera);
        ecs.emplace<world::Transform3D>(left);
        ecs.emplace<world::XRController3D>(left).tracker = "left_hand";
        ecs.emplace<world::Transform3D>(right);
        auto& controller = ecs.emplace<world::XRController3D>(right);
        controller.tracker = "right_hand";
        controller.show_when_tracked = true;
        xr::use_test_interface(*world);
    }

    xr::TestInterface& test() const { return *xr::test_interface(*world); }

    std::vector<world::Event> take(std::string_view kind) {
        std::vector<world::Event> found;
        for (const world::Event& event : events) {
            if (event.kind == kind) {
                found.push_back(event);
            }
        }
        return found;
    }
};

void check_near(Vec3 a, Vec3 b) {
    CHECK(a.x == doctest::Approx(b.x).epsilon(1e-5));
    CHECK(a.y == doctest::Approx(b.y).epsilon(1e-5));
    CHECK(a.z == doctest::Approx(b.z).epsilon(1e-5));
}

} // namespace

TEST_CASE("xr: the test headset adds its trackers and becomes the primary interface") {
    Rig rig;
    xr::TestInterface& test = rig.test();
    CHECK(test.is_initialized());
    CHECK(xr::primary_interface(*rig.world) == &test);
    CHECK(xr::find_interface(*rig.world, "Test") == &test);
    // The engine's own setup is not news.
    CHECK(rig.take("interface_added").empty());
    CHECK(rig.take("tracker_added").empty());
    CHECK(
        xr::trackers(*rig.world, static_cast<std::uint32_t>(xr::TrackerType::Controller)).size() ==
        2);
    const xr::Tracker* left = xr::find_tracker(*rig.world, "left_hand");
    REQUIRE(left != nullptr);
    CHECK(left->hand == xr::Hand::Left);
    // Poses come with the first process, as a runtime's do.
    CHECK(left->poses.empty());
    CHECK((test.capabilities() & xr::kStereo) != 0U);
    CHECK(test.play_area_mode() == xr::PlayArea::Stage);

    // Without enabled the headset waits to be started.
    Rig off({});
    CHECK_FALSE(off.test().is_initialized());
    CHECK(xr::primary_interface(*off.world) == nullptr);
    CHECK(xr::trackers(*off.world).empty());
    // Started later, its trackers arrive as events, as Godot's tracker_added.
    REQUIRE(off.test().initialize(*off.world).ok());
    CHECK(xr::primary_interface(*off.world) == &off.test());
    const auto added = off.take("tracker_added");
    REQUIRE(added.size() == 3);
    CHECK(added[0].data.find("tracker")->as_string() == "head");
    CHECK(added[1].data.find("type")->as_string() == "controller");
    off.test().uninitialize(*off.world);
    CHECK(off.take("tracker_removed").size() == 3);
    CHECK(xr::primary_interface(*off.world) == nullptr);
}

TEST_CASE("xr: XR nodes follow their trackers' poses under the origin, as Godot's XRNode3D") {
    Rig rig;
    auto& ecs = rig.world->ecs();
    ecs.get<world::Transform3D>(rig.origin).position = {10.0F, 0.0F, 0.0F};
    ecs.get<world::XROrigin3D>(rig.origin).world_scale = 2.0F;
    world::Transform3D turned;
    turned.position = {0.5F, 1.0F, -0.25F};
    turned.rotation = {0.0F, 1.5F, 0.0F};
    rig.test().trackers()["right_hand"].poses["default"].transform = world::affine(turned);
    rig.world->step(kTick);

    CHECK(xr::world_scale(*rig.world) == 2.0F);
    check_near(xr::world_origin(*rig.world).origin, {10.0F, 0.0F, 0.0F});
    // Godot's XRPose::get_adjusted_transform: the origin times the world scale.
    const auto& right = ecs.get<world::Transform3D>(rig.right);
    check_near(right.position, {1.0F, 2.0F, -0.5F});
    check_near(right.rotation, {0.0F, 1.5F, 0.0F});
    check_near(ecs.get<world::Transform3D>(rig.camera).position, {0.0F, 3.2F, 0.0F});
    const world::Tree3D tree(*rig.world);
    check_near(tree.global(rig.right).origin, {11.0F, 2.0F, -0.5F});
    CHECK(xr::has_tracking_data(*rig.world, rig.right));
    CHECK(xr::has_tracking_data(*rig.world, rig.camera));
    CHECK(xr::tracker_hand(*rig.world, rig.right) == xr::Hand::Right);
    const auto tracking = rig.take("tracking_changed");
    REQUIRE(tracking.size() == 2);
    CHECK(tracking[0].id == "left");
    CHECK(tracking[0].data.find("tracking")->as_bool());

    // A lost pose leaves the node where it was and hides it with show_when_tracked.
    rig.events.clear();
    rig.test().trackers()["right_hand"].poses["default"].tracking = false;
    rig.test().trackers()["right_hand"].poses["default"].transform = {};
    rig.world->step(kTick);
    CHECK_FALSE(xr::has_tracking_data(*rig.world, rig.right));
    check_near(ecs.get<world::Transform3D>(rig.right).position, {1.0F, 2.0F, -0.5F});
    CHECK_FALSE(ecs.get<world::Transform3D>(rig.right).visible);
    CHECK(xr::pose_of(*rig.world, rig.right)->has_tracking_data == false);
    REQUIRE(rig.take("tracking_changed").size() == 1);
    rig.test().trackers()["right_hand"].poses["default"].tracking = true;
    rig.world->step(kTick);
    CHECK(ecs.get<world::Transform3D>(rig.right).visible);

    // A node on a pose its tracker lacks isn't tracked; one on a tracker taken away stops.
    ecs.get<world::XRController3D>(rig.left).pose = "thumb";
    rig.test().trackers().erase("left_hand");
    rig.events.clear();
    rig.world->step(kTick);
    CHECK_FALSE(xr::has_tracking_data(*rig.world, rig.left));
    CHECK(rig.take("tracker_removed").size() == 1);
}

TEST_CASE("xr: controllers send their inputs as Godot's XRController3D signals") {
    Rig rig;
    auto& inputs = rig.test().trackers()["right_hand"].inputs;
    inputs["trigger"] = 0.5F;
    inputs["ax_button"] = true;
    inputs["primary"] = Vec2{0.0F, 1.0F};
    inputs["menu_button"] = false;
    rig.world->step(kTick);
    const auto pressed = rig.take("button_pressed");
    REQUIRE(pressed.size() == 1);
    CHECK(pressed[0].id == "right");
    CHECK(pressed[0].data.find("name")->as_string() == "ax_button");
    // A new input that is false is a release, as Godot's set_input.
    REQUIRE(rig.take("button_released").size() == 1);
    const auto floats = rig.take("input_float_changed");
    REQUIRE(floats.size() == 1);
    CHECK(floats[0].data.find("value")->as_double() == 0.5);
    const auto axes = rig.take("input_vector2_changed");
    REQUIRE(axes.size() == 1);
    CHECK(axes[0].data.find("value")->as_array()[1].as_double() == 1.0);

    // Godot's conversions between buttons, triggers and thumbsticks.
    CHECK(xr::is_button_pressed(*rig.world, rig.right, "ax_button"));
    CHECK(xr::is_button_pressed(*rig.world, rig.right, "trigger"));
    CHECK_FALSE(xr::is_button_pressed(*rig.world, rig.right, "menu_button"));
    CHECK(xr::get_float(*rig.world, rig.right, "ax_button") == 1.0F);
    CHECK(xr::get_float(*rig.world, rig.right, "primary") == 0.0F);
    CHECK(xr::get_vector2(*rig.world, rig.right, "trigger") == Vec2{0.5F, 0.0F});
    CHECK(xr::get_vector2(*rig.world, rig.right, "primary") == Vec2{0.0F, 1.0F});
    CHECK_FALSE(xr::get_input(*rig.world, rig.right, "grip").has_value());
    // The other controller heard nothing.
    CHECK_FALSE(xr::is_button_pressed(*rig.world, rig.left, "ax_button"));

    // Unchanged inputs say nothing; changed ones say it once.
    rig.events.clear();
    rig.world->step(kTick);
    CHECK(rig.take("input_float_changed").empty());
    inputs["ax_button"] = false;
    rig.test().trackers()["right_hand"].profile = "/interaction_profiles/khr/simple_controller";
    rig.world->step(kTick);
    CHECK(rig.take("button_released").size() == 1);
    const auto profile = rig.take("profile_changed");
    REQUIRE(profile.size() == 1);
    CHECK(profile[0].data.find("role")->as_string() ==
          "/interaction_profiles/khr/simple_controller");

    xr::trigger_haptic_pulse(*rig.world, rig.right, "haptic", 0.0, 0.8, 0.1, 0.0);
    REQUIRE(rig.test().pulses().size() == 1);
    CHECK(rig.test().pulses()[0].tracker == "right_hand");
    CHECK(rig.test().pulses()[0].amplitude == 0.8);
}

TEST_CASE("xr: center_on_hmd makes the head the play space's center, as Godot's") {
    Rig rig({.enabled = true, .reference_space = "local_floor"});
    CHECK(rig.test().play_area_mode() == xr::PlayArea::Roomscale);
    world::Transform3D head;
    head.position = {1.0F, 1.6F, 2.0F};
    head.rotation = {0.2F, 1.0F, 0.0F};
    rig.test().trackers()["head"].poses["default"].transform = world::affine(head);
    rig.events.clear();
    xr::center_on_hmd(*rig.world, xr::RotationMode::ResetButKeepTilt, true);
    CHECK(rig.take("reference_frame_changed").size() == 1);
    rig.world->step(kTick);
    const auto& camera = rig.world->ecs().get<world::Transform3D>(rig.camera);
    // The head stands over the center, its height kept, facing the way the space now faces:
    // only its tilt is left.
    check_near(camera.position, {0.0F, 1.6F, 0.0F});
    CHECK(camera.rotation.x == doctest::Approx(0.2F).epsilon(1e-4));
    CHECK(camera.rotation.y == doctest::Approx(0.0F).epsilon(1e-4));

    xr::center_on_hmd(*rig.world, xr::RotationMode::DontResetRotation, false);
    rig.world->step(kTick);
    check_near(rig.world->ecs().get<world::Transform3D>(rig.camera).position, {});
    CHECK(rig.world->ecs().get<world::Transform3D>(rig.camera).rotation.y ==
          doctest::Approx(1.0F).epsilon(1e-4));
    xr::clear_reference_frame(*rig.world);
    CHECK(xr::reference_frame(*rig.world) == world::Affine3D{});

    // Not in the stage play area, which is the room's own.
    Rig stage;
    stage.test().trackers()["head"].poses["default"].transform = world::affine(head);
    xr::center_on_hmd(*stage.world, xr::RotationMode::ResetFullRotation, true);
    CHECK(xr::reference_frame(*stage.world) == world::Affine3D{});
    // Its play area is the size given, around the origin.
    const std::vector<Vec3> area = stage.test().play_area(*stage.world);
    REQUIRE(area.size() == 4);
    check_near(area[0], {-1.0F, 0.0F, -1.0F});
    check_near(area[2], {1.0F, 0.0F, 1.0F});
}

TEST_CASE("xr: each eye's view is the head's moved half the IPD, with its own field of view") {
    Rig rig;
    rig.world->ecs().get<world::Transform3D>(rig.origin).position = {2.0F, 0.0F, 0.0F};
    rig.test().set_ipd(0.06F);
    rig.world->step(kTick); // the origin reaches the server
    const std::vector<xr::View> views = xr::views(*rig.world);
    REQUIRE(views.size() == 2);
    // The stage's head stands 1.6 m up, under the origin.
    check_near(views[0].transform.origin, {1.97F, 1.6F, 0.0F});
    check_near(views[1].transform.origin, {2.03F, 1.6F, 0.0F});
    // Each eye sees further out than in, as headsets' canted views do.
    CHECK(-views[0].fov.left > views[0].fov.right);
    CHECK(views[1].fov.right > -views[1].fov.left);
    CHECK(views[0].fov.up == views[1].fov.up);
    CHECK(rig.test().render_target_size() == Vec2i{1024, 1024});

    // A turned head turns the eyes about it.
    world::Transform3D head;
    head.position = {0.0F, 1.6F, 0.0F};
    head.rotation = {0.0F, 1.5707963F, 0.0F};
    rig.test().trackers()["head"].poses["default"].transform = world::affine(head);
    const std::vector<xr::View> turned = xr::views(*rig.world);
    check_near(turned[0].transform.origin, {2.0F, 1.6F, 0.03F});

    // Mono has one view, at the head; a headset not started has none.
    Rig mono({.enabled = true, .view_configuration = "mono"});
    REQUIRE(xr::views(*mono.world).size() == 1);
    check_near(xr::views(*mono.world)[0].transform.origin, {0.0F, 1.6F, 0.0F});
    CHECK(xr::views(*mono.world)[0].fov.left == -xr::views(*mono.world)[0].fov.right);
    mono.test().uninitialize(*mono.world);
    CHECK(xr::views(*mono.world).empty());
}

TEST_CASE("xr: problems name XR nodes outside an origin and origins without a camera") {
    world::ComponentRegistry components;
    world::register_builtin_components(components);
    world::World world(components);
    auto& ecs = world.ecs();
    const entt::entity origin = world.create("origin").value();
    const entt::entity stray = world.create("stray").value();
    ecs.emplace<world::Transform3D>(origin);
    ecs.emplace<world::XROrigin3D>(origin);
    ecs.emplace<world::Transform3D>(stray);
    ecs.emplace<world::XRController3D>(stray);
    const std::vector<xr::Problem> problems = xr::check_nodes(world);
    REQUIRE(problems.size() == 3);
    CHECK(problems[0].message == "an XROrigin3D needs an XRCamera3D among its children");
    CHECK(problems[1].entity == stray);
    CHECK(problems[2].path == "/components/XRController3D/tracker");
}

TEST_CASE("xr: settings come from project.json's xr, as Godot's xr/openxr") {
    const json::Value settings = parse(R"({"openxr": {"enabled": true,
        "reference_space": "local", "environment_blend_mode": "alpha",
        "view_configuration": "mono"}, "shaders": {"enabled": true}})");
    CHECK(xr::validate_settings(settings, "/xr").empty());
    const xr::Settings parsed = xr::parse_settings(settings);
    CHECK(parsed.enabled);
    CHECK(parsed.shaders);
    CHECK(parsed.reference_space == "local");
    CHECK(parsed.environment_blend_mode == xr::BlendMode::AlphaBlend);
    const auto errors =
        xr::validate_settings(parse(R"({"openxr": {"reference_space": "floor"}})"), "/xr");
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].path == "/xr/openxr/reference_space");

    Rig mono(parsed);
    CHECK((mono.test().capabilities() & xr::kMono) != 0U);
    CHECK(mono.test().environment_blend_mode() == xr::BlendMode::AlphaBlend);
    // A local space starts at the head.
    mono.world->step(kTick);
    check_near(mono.world->ecs().get<world::Transform3D>(mono.camera).position, {});
}

TEST_CASE("xr commands: xr.state reports and xr.set moves the test headset") {
    const std::filesystem::path root = "xr_commands_project";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    std::ofstream(root / "project.json")
        << R"({"name": "XR", "main_scene": "main.scene.json", "xr": {"openxr": {"enabled": true}}})";
    std::ofstream(root / "main.scene.json") << R"({
  "entities": {
    "origin": {"components": {"Transform3D": {"children": ["head", "right"]}, "XROrigin3D": {}}},
    "head": {"components": {"Camera3D": {}, "Transform3D": {}, "XRCamera3D": {}}},
    "right": {"components": {"Transform3D": {}, "XRController3D": {"tracker": "right_hand"}}}
  },
  "format": 1
})";
    world::ComponentRegistry components;
    world::register_builtin_components(components);
    command::Registry commands;
    command::register_builtin_commands(commands);
    command::register_run_commands(commands);
    command::SessionOptions options;
    options.project_root = root.string();
    options.capture_logs = false;
    command::Session session(components, commands, std::move(options));
    const auto ok = [&](std::string_view name, std::string_view params) {
        command::Reply reply = session.call(name, parse(params));
        REQUIRE_MESSAGE(reply.ok, reply.message);
        return std::move(reply.value);
    };
    ok("scene.open", R"({"path": "main.scene.json"})");

    json::Value set = ok("xr.set", R"({"trackers": {"right_hand": {"position": [0.3, 1.1, -0.5],
        "inputs": {"trigger": 1.0, "ax_button": true}}}})");
    CHECK(set.find("tick")->as_int() == 0);
    ok("step", R"({"ticks": 1})");
    json::Value state = ok("xr.state", R"({"trackers": ["right_hand"]})");
    const json::Value* right = state.find("trackers")->find("right_hand");
    REQUIRE(right != nullptr);
    CHECK(right->find("hand")->as_string() == "right");
    CHECK(right->find("poses")->find("aim")->find("position")->as_array()[0].as_double() ==
          doctest::Approx(0.3));
    CHECK(right->find("inputs")->find("ax_button")->as_bool());
    CHECK(state.find("nodes")->find("right")->find("tracking")->as_bool());
    CHECK(state.find("nodes")->find("head") == nullptr);
    CHECK(state.find("interfaces")->as_array()[0].find("primary")->as_bool());
    json::Value entity = ok("entity.get", R"({"id": "right", "components": ["Transform3D"]})");
    CHECK(json::write(entity).find("0.3") != std::string::npos);

    // Unknown trackers need a type, with a hint.
    const command::Reply unknown =
        session.call("xr.set", parse(R"({"trackers": {"right_hnd": {"position": [0, 0, 0]}}})"));
    REQUIRE_FALSE(unknown.ok);
    CHECK(unknown.message.find("did you mean \"right_hand\"") != std::string::npos);
    CHECK(unknown.message.starts_with("/trackers/right_hnd: "));

    // An anchor added and a batch that fails leaves the headset as it was.
    ok("xr.set", R"({"trackers": {"table": {"type": "anchor", "position": [1, 0.8, -1]}}})");
    const command::Reply failed = session.call("batch", parse(R"({"commands": [
        {"command": "xr.set", "params": {"trackers": {"table": {"remove": true}}}},
        {"command": "xr.set", "params": {"play_area_mode": "3dof"}}]})"));
    CHECK_FALSE(failed.ok);
    ok("step", R"({"ticks": 1})");
    state = ok("xr.state", R"({"trackers": ["table"]})");
    CHECK(state.find("trackers")->find("table")->find("type")->as_string() == "anchor");

    // Stopping the headset takes its trackers away.
    ok("xr.set", R"({"initialize": false})");
    state = ok("xr.state", "{}");
    CHECK(state.find("trackers")->as_object().empty());
    CHECK_FALSE(state.find("interfaces")->as_array()[0].find("initialized")->as_bool());
    std::filesystem::remove_all(root);
}
