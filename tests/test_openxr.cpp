#include "slash/asset/assets.hpp"
#include "slash/canvas/canvas.hpp"
#include "slash/command/builtin.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/platform/process.hpp"
#include "slash/render/device.hpp"
#include "slash/scene3d/scene3d.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/world.hpp"
#include "slash/xr/openxr.hpp"
#include "slash/xr/xr.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

using namespace slash;

TEST_CASE("openxr: the default action map is Godot's") {
    const xr::ActionMap& map = xr::default_action_map();
    CHECK(map.action_set == "godot");
    CHECK(map.actions.size() == 23);
    CHECK(map.profiles.size() == 11);
    const auto touch = std::ranges::find_if(map.profiles, [](const xr::MapProfile& profile) {
        return profile.path == "/interaction_profiles/oculus/touch_controller";
    });
    REQUIRE(touch != map.profiles.end());
    CHECK(touch->extension.empty());
    // x on the left hand, a on the right, as Godot binds them.
    const auto ax = std::ranges::find_if(touch->bindings, [](const xr::MapBinding& binding) {
        return binding.action == "ax_button";
    });
    REQUIRE(ax != touch->bindings.end());
    CHECK(ax->paths == std::vector<std::string>{"/user/hand/left/input/x/click",
                                                "/user/hand/right/input/a/click"});
    const auto pico = std::ranges::find_if(map.profiles, [](const xr::MapProfile& profile) {
        return profile.path == "/interaction_profiles/bytedance/pico4_controller";
    });
    REQUIRE(pico != map.profiles.end());
    CHECK(pico->extension == "XR_BD_controller_interaction");
}

#if defined(SLASH_FAKE_OPENXR)

namespace {

void set_env(const char* name, const std::string& value) {
#if defined(_WIN32)
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

// A world with an XR origin, a head and two controllers, its OpenXR runtime the stand-in playing
// the script.
struct OpenXRRig {
    std::filesystem::path folder;
    world::ComponentRegistry components;
    std::unique_ptr<world::World> world;
    entt::entity right = entt::null;
    std::vector<world::Event> events;

    OpenXRRig(std::string_view name, const std::string& script, xr::Settings settings) {
        folder = std::filesystem::path("openxr_" + std::string(name));
        std::filesystem::remove_all(folder);
        std::filesystem::create_directories(folder);
        std::ofstream(folder / "script.txt") << script;
        set_env("XR_RUNTIME_JSON", SLASH_FAKE_OPENXR);
        set_env("FAKE_OPENXR_SCRIPT", std::filesystem::absolute(folder / "script.txt").string());
        set_env("FAKE_OPENXR_LOG", std::filesystem::absolute(folder / "log.txt").string());
        world::register_builtin_components(components);
        world = std::make_unique<world::World>(components);
        world->set_event_listener([this](const world::Event& event) { events.push_back(event); });
        xr::install(*world, settings);
        auto& ecs = world->ecs();
        const entt::entity origin = world->create("origin").value();
        const entt::entity head = world->create("head").value();
        right = world->create("right").value();
        ecs.emplace<world::Transform3D>(origin).children = {head, right};
        ecs.emplace<world::XROrigin3D>(origin);
        ecs.emplace<world::Transform3D>(head);
        ecs.emplace<world::Camera3D>(head);
        ecs.emplace<world::XRCamera3D>(head);
        ecs.emplace<world::Transform3D>(right);
        ecs.emplace<world::XRController3D>(right).tracker = "right_hand";
    }
    OpenXRRig(const OpenXRRig&) = delete;
    OpenXRRig& operator=(const OpenXRRig&) = delete;
    OpenXRRig(OpenXRRig&&) = delete;
    OpenXRRig& operator=(OpenXRRig&&) = delete;
    ~OpenXRRig() {
        world.reset();
        std::filesystem::remove_all(folder);
    }

    void step(int ticks) {
        for (int i = 0; i < ticks; ++i) {
            world->step(1.0 / 60.0);
        }
    }
    [[nodiscard]] bool saw(std::string_view kind) const {
        return std::ranges::any_of(events,
                                   [&](const world::Event& event) { return event.kind == kind; });
    }
    [[nodiscard]] std::string log() const {
        std::ifstream file(folder / "log.txt");
        std::stringstream text;
        text << file.rdbuf();
        return text.str();
    }
};

void check_near(Vec3 a, Vec3 b) {
    CHECK(a.x == doctest::Approx(b.x).epsilon(1e-4));
    CHECK(a.y == doctest::Approx(b.y).epsilon(1e-4));
    CHECK(a.z == doctest::Approx(b.z).epsilon(1e-4));
}

constexpr const char* kTouchScript = R"(profile /interaction_profiles/oculus/touch_controller
bounds 3 2
frame 0 head 0 1.7 0 0 0 0 1
frame 0 /user/hand/right/input/aim/pose 0.2 1.2 -0.3 0 0 0 1
frame 0 /user/hand/right/input/grip/pose 0.2 1.1 -0.25 0 0 0 1
frame 3 /user/hand/right/input/trigger/value 0.8
frame 3 /user/hand/right/input/a/click 1
frame 4 /user/hand/right/input/thumbstick 0.5 -0.5
frame 6 /user/hand/right/input/aim/pose lost
frame 9 exit
)";

} // namespace

TEST_CASE("openxr: a runtime's session runs, tracking the head and hands by Godot's action map") {
    REQUIRE(xr::openxr_available());
    OpenXRRig rig("session", kTouchScript, {.enabled = true});
    const auto started = xr::use_openxr_interface(*rig.world);
    REQUIRE_MESSAGE(started.ok(), (started.ok() ? std::string() : started.error().message));
    xr::Interface* openxr = xr::primary_interface(*rig.world);
    REQUIRE(openxr != nullptr);
    CHECK(openxr->name() == "OpenXR");
    const xr::OpenXRInfo* info = xr::openxr_info(*openxr);
    REQUIRE(info != nullptr);
    CHECK(info->runtime_name == "SlashEngine stand-in");
    CHECK(info->api_version.starts_with("1.1."));
    CHECK(info->headless);
    CHECK(xr::find_tracker(*rig.world, "head") != nullptr);
    CHECK(xr::find_tracker(*rig.world, "left_hand")->hand == xr::Hand::Left);

    // The session goes from ready to focused at the first tick, and frames are waited for.
    rig.step(4);
    CHECK(rig.saw("session_begun"));
    CHECK(rig.saw("session_focussed"));
    CHECK(info->running);
    CHECK(info->session_state == xr::SessionState::Focused);
    CHECK(info->frames == 4);
    CHECK(info->reference_space == "stage");
    CHECK(xr::find_tracker(*rig.world, "right_hand")->profile ==
          "/interaction_profiles/oculus/touch_controller");
    check_near(rig.world->ecs().get<world::Transform3D>(rig.right).position, {0.2F, 1.2F, -0.3F});
    CHECK(xr::get_float(*rig.world, rig.right, "trigger") == doctest::Approx(0.8F));
    CHECK(xr::is_button_pressed(*rig.world, rig.right, "trigger_click"));
    CHECK(xr::is_button_pressed(*rig.world, rig.right, "ax_button"));
    const Vec2 stick = xr::get_vector2(*rig.world, rig.right, "primary");
    CHECK(stick.x == doctest::Approx(0.5F));
    CHECK(stick.y == doctest::Approx(-0.5F));
    const xr::Tracker* right = xr::find_tracker(*rig.world, "right_hand");
    REQUIRE(right->poses.contains("grip"));
    check_near(right->poses.at("grip").transform.origin, {0.2F, 1.1F, -0.25F});

    // The head and each eye, 64 mm apart, each seeing further out than in.
    const std::vector<xr::View> views = xr::views(*rig.world);
    REQUIRE(views.size() == 2);
    check_near(views[0].transform.origin, {-0.032F, 1.7F, 0.0F});
    check_near(views[1].transform.origin, {0.032F, 1.7F, 0.0F});
    CHECK(-views[0].fov.left > views[0].fov.right);
    CHECK(openxr->render_target_size() == Vec2i{1024, 1024});
    check_near(xr::hmd_transform(*rig.world).origin, {0.0F, 1.7F, 0.0F});
    const std::vector<Vec3> area = openxr->play_area(*rig.world);
    REQUIRE(area.size() == 4);
    check_near(area[0], {-1.5F, 0.0F, -1.0F});

    // Haptics reach the runtime by the action's binding on the tracker's hand.
    xr::trigger_haptic_pulse(*rig.world, rig.right, "haptic", 0.0, 0.5, 0.1, 0.0);
    CHECK(rig.log().find("haptic /user/hand/right/output/haptic 100000000 0 0.5") !=
          std::string::npos);

    // A lost pose leaves the node where it was and says so.
    rig.events.clear();
    rig.step(3);
    CHECK(rig.saw("tracking_changed"));
    CHECK_FALSE(xr::has_tracking_data(*rig.world, rig.right));

    // The runtime stops the session: it ends, and the instance is told to exit.
    rig.step(3);
    CHECK(rig.saw("session_stopping"));
    CHECK(rig.saw("instance_exiting"));
    CHECK_FALSE(info->running);
    CHECK(info->session_state == xr::SessionState::Exiting);

    const std::string log = rig.log();
    CHECK(log.find("instance 1.1 XR_EXT_local_floor XR_EXT_palm_pose XR_MND_headless") !=
          std::string::npos);
    CHECK(log.find("suggest /interaction_profiles/oculus/touch_controller") != std::string::npos);
    // Profiles whose extension the runtime lacks are left out.
    CHECK(log.find("pico4") == std::string::npos);
    CHECK(log.find("end frame 4 layers 0") != std::string::npos);
    openxr->uninitialize(*rig.world);
    CHECK(xr::find_tracker(*rig.world, "head") == nullptr);
    CHECK(rig.log().find("destroy instance") != std::string::npos);
}

TEST_CASE("openxr: local_floor is the stage's height under local where the runtime lacks it") {
    OpenXRRig rig("floor", "spaces view local stage\nframe 0 head 0 1.7 0 0 0 0 1\n",
                  {.enabled = true, .reference_space = "local_floor"});
    REQUIRE(xr::use_openxr_interface(*rig.world).ok());
    rig.step(3);
    const xr::Interface* openxr = xr::primary_interface(*rig.world);
    CHECK(openxr->play_area_mode() == xr::PlayArea::Roomscale);
    CHECK(xr::openxr_info(*openxr)->reference_space == "local_floor");
    check_near(xr::hmd_transform(*rig.world).origin, {0.0F, 1.7F, 0.0F});
}

TEST_CASE("openxr: runtimes that can't start a session say why") {
    {
        // Without headless sessions, a session needs a render device to draw with.
        OpenXRRig rig("graphics", "extensions XR_EXT_palm_pose\n", {.enabled = true});
        const auto started = xr::use_openxr_interface(*rig.world);
        REQUIRE_FALSE(started.ok());
        CHECK(started.error().message.find("graphics binding") != std::string::npos);
        CHECK_FALSE(xr::find_interface(*rig.world, "OpenXR")->is_initialized());
        CHECK(xr::primary_interface(*rig.world) == nullptr);
    }
    {
        OpenXRRig rig("missing", "", {.enabled = true});
        set_env("XR_RUNTIME_JSON", (rig.folder / "no_runtime.json").string());
        const auto started = xr::use_openxr_interface(*rig.world);
        REQUIRE_FALSE(started.ok());
        CHECK(started.error().message.find("no runtime") != std::string::npos);
    }
}

TEST_CASE("openxr: a game draws each eye into the runtime's swapchains") {
    // Eyes of 64x64 pixels; a red post 2 m ahead shows left of the left eye's center and right
    // of the right eye's, over a blue background. The assets outlive the world that uses them.
    asset::Assets assets{asset::folder_source("openxr_drawing")};
    OpenXRRig rig("drawing", R"(extensions XR_KHR_vulkan_enable2 XR_EXT_palm_pose
view_size 64 64
probe 38 32
probe 26 32
probe 2 2
frame 0 head 0 1.7 0 0 0 0 1
)",
                  {.enabled = true});
    canvas::install(*rig.world, assets);
    const auto create = [&](std::string_view id, std::string_view components) {
        const entt::entity entity = rig.world->create(id).value();
        const json::Value values = json::parse(components).value();
        for (const auto& [name, fields] : values.as_object()) {
            std::string path;
            REQUIRE(rig.components.find(name)->assign(*rig.world, entity, fields, path).ok());
        }
    };
    create("env", R"({"WorldEnvironment": {"background_mode": "color",
                                           "background_color": [0, 0, 1, 1]}})");
    create("post", R"({"Transform3D": {"position": [0, 1.7, -2]}, "MeshInstance3D": {},
                       "BoxMesh": {"size": [0.5, 3, 0.5]},
                       "StandardMaterial3D": {"shading_mode": "unshaded",
                                              "albedo_color": [1, 0, 0, 1]}})");

    // The runtime has its say in the device, then the session draws with it.
    const auto prepared = xr::prepare_openxr_device(*rig.world, "vulkan");
    REQUIRE_MESSAGE(prepared.ok(), (prepared.ok() ? std::string() : prepared.error().message));
    CHECK(prepared.value().driver == "vulkan");
    auto device = render::create_device(
        {.window = nullptr, .driver = "vulkan", .debug = false, .xr = prepared.value().hooks});
    if (!device) {
#if defined(__linux__)
        if (!platform::environment_variable("SLASH_EXPECT_GPU").empty()) {
            FAIL("SLASH_EXPECT_GPU is set, but Vulkan doesn't work: " << device.error().message);
        }
#endif
        MESSAGE("no Vulkan here, so drawing into a headset is skipped: " << device.error().message);
        xr::find_interface(*rig.world, "OpenXR")->uninitialize(*rig.world);
        return;
    }
    const auto started = xr::use_openxr_interface(*rig.world, device.value().get());
    REQUIRE_MESSAGE(started.ok(), (started.ok() ? std::string() : started.error().message));
    xr::Interface* openxr = xr::primary_interface(*rig.world);
    const xr::OpenXRInfo* info = xr::openxr_info(*openxr);
    CHECK_FALSE(info->headless);
    CHECK(info->graphics == "vulkan");
    CHECK(openxr->render_target_size() == Vec2i{64, 64});
    {
        canvas::CanvasRenderer renderer(*device.value(), assets);
        int drawn = 0;
        for (int tick = 0; tick < 3; ++tick) {
            rig.step(1);
            const auto frame = xr::begin_openxr_frame(*rig.world);
            REQUIRE_MESSAGE(frame.ok(), (frame.ok() ? std::string() : frame.error().message));
            if (frame.value().render) {
                ++drawn;
                const std::vector<xr::View> views = xr::views(*rig.world);
                REQUIRE(views.size() == frame.value().targets.size());
                for (std::size_t i = 0; i < views.size(); ++i) {
                    canvas::CanvasOptions eye;
                    eye.target = frame.value().targets[i];
                    eye.width = 64.0F;
                    eye.height = 64.0F;
                    eye.view3d =
                        scene3d::eye_view(*rig.world, views[i].transform, views[i].fov.left,
                                          views[i].fov.right, views[i].fov.down, views[i].fov.up);
                    REQUIRE(renderer.draw(*rig.world, eye).ok());
                }
            }
            const auto ended = xr::end_openxr_frame(*rig.world);
            REQUIRE_MESSAGE(ended.ok(), (ended.ok() ? std::string() : ended.error().message));
        }
        CHECK(drawn > 0);
    }
    openxr->uninitialize(*rig.world);

    const std::string log = rig.log();
    CHECK(log.find("instance 1.1 XR_EXT_palm_pose XR_KHR_vulkan_enable2") != std::string::npos);
    CHECK(log.find("vulkan device") != std::string::npos);
    CHECK(log.find("create session vulkan") != std::string::npos);
    // The runtime's first format is BGRA, which the device doesn't copy into; then sRGB RGBA.
    CHECK(log.find("swapchain 64x64 format 43") != std::string::npos);
    CHECK(log.find("layers 1") != std::string::npos);
    // The post is in each eye where that eye sees it, and the background around it.
    CHECK(log.find("eye 0 38 32 255 0 0 255") != std::string::npos);
    CHECK(log.find("eye 1 38 32 0 0 255 255") != std::string::npos);
    CHECK(log.find("eye 0 26 32 0 0 255 255") != std::string::npos);
    CHECK(log.find("eye 1 26 32 255 0 0 255") != std::string::npos);
    CHECK(log.find("eye 1 2 2 0 0 255 255") != std::string::npos);
    CHECK(log.find("unreadable") == std::string::npos);
}

TEST_CASE("openxr: xr.set switches a session between the test headset and the runtime") {
    const std::filesystem::path root = "openxr_session";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    std::ofstream(root / "script.txt") << kTouchScript;
    set_env("XR_RUNTIME_JSON", SLASH_FAKE_OPENXR);
    set_env("FAKE_OPENXR_SCRIPT", std::filesystem::absolute(root / "script.txt").string());
    set_env("FAKE_OPENXR_LOG", std::filesystem::absolute(root / "log.txt").string());
    std::ofstream(root / "project.json")
        << R"({"name": "XR", "main_scene": "main.scene.json", "xr": {"openxr": {"enabled": true}}})";
    std::ofstream(root / "main.scene.json") << R"({
  "entities": {
    "origin": {"components": {"Transform3D": {"children": ["right"]}, "XROrigin3D": {}}},
    "right": {"components": {"Transform3D": {}, "XRController3D": {"tracker": "right_hand"}}}
  },
  "format": 1
})";
    {
        world::ComponentRegistry components;
        world::register_builtin_components(components);
        command::Registry commands;
        command::register_builtin_commands(commands);
        command::register_run_commands(commands);
        command::SessionOptions options;
        options.project_root = root.string();
        options.capture_logs = false;
        command::Session session(components, commands, std::move(options));
        const auto call = [&](std::string_view name, std::string_view params) {
            return session.call(name, json::parse(params).value());
        };
        REQUIRE(call("scene.open", R"({"path": "main.scene.json"})").ok);

        // The switch is a command of its own.
        const command::Reply mixed = call("xr.set", R"({"interface": "openxr", "ipd": 0.06})");
        REQUIRE_FALSE(mixed.ok);
        CHECK(mixed.data.find("path")->as_string() == "/interface");

        REQUIRE(call("xr.set", R"({"interface": "openxr"})").ok);
        REQUIRE(call("step", R"({"ticks": 4})").ok);
        const command::Reply state = call("xr.state", R"({"trackers": ["right_hand"]})");
        REQUIRE(state.ok);
        const json::Value* openxr = nullptr;
        for (const json::Value& entry : state.value.find("interfaces")->as_array()) {
            if (entry.find("name")->as_string() == "OpenXR") {
                openxr = &entry;
            }
        }
        REQUIRE(openxr != nullptr);
        CHECK(openxr->find("primary") != nullptr);
        CHECK(openxr->find("openxr")->find("session_state")->as_string() == "focused");
        CHECK(state.value.find("trackers")->find("right_hand")->find("profile")->as_string() ==
              "/interaction_profiles/oculus/touch_controller");

        // Back to the test headset: the runtime's session and instance end.
        REQUIRE(call("xr.set", R"({"interface": "test"})").ok);
        const command::Reply back = call("xr.state", "{}");
        for (const json::Value& entry : back.value.find("interfaces")->as_array()) {
            CHECK((entry.find("primary") != nullptr) ==
                  (entry.find("name")->as_string() == "Test"));
        }
    }
    std::ifstream file(root / "log.txt");
    std::stringstream log;
    log << file.rdbuf();
    CHECK(log.str().find("destroy instance") != std::string::npos);
    std::filesystem::remove_all(root);
}

#endif
