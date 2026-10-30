#pragma once

#include "slash/core/result.hpp"
#include "slash/world/world.hpp"
#include "slash/xr/xr.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// OpenXR, as Godot's OpenXR module: an XR interface over the Khronos loader, which finds the
// headset's runtime (SteamVR, Meta's, Monado, Windows Mixed Reality, ...). It makes an instance
// and a session for project.json's xr.openxr settings, follows the session's states, keeps the
// head and controller trackers up to date from Godot's default action map and locates each eye's
// view. Games start it when xr.openxr.enabled is set.
//
//   if (auto started = xr::use_openxr_interface(world); !started) { log the error }
//
// The loader is built into games on Windows, Linux and macOS; elsewhere openxr_available() is
// false and use_openxr_interface fails.
namespace slash::xr {

// Whether this build has the OpenXR loader.
[[nodiscard]] bool openxr_available();

// Godot's OpenXRAction::ActionType.
enum class ActionType : std::uint8_t { Bool, Float, Vector2, Pose, Haptic };

// One of an action map's actions, as Godot's OpenXRAction: a name the trackers' inputs and poses
// take (pose actions drop "_pose": "aim_pose" is the pose "aim", "default_pose" "default") and
// the top-level paths it is on, such as "/user/hand/left".
struct MapAction {
    std::string name;
    ActionType type = ActionType::Bool;
    std::vector<std::string> toplevel_paths;
};
// An action's input or output paths on a controller, as Godot's OpenXRIPBinding.
struct MapBinding {
    std::string action;
    std::vector<std::string> paths;
};
// A controller the action map suggests bindings for, as Godot's OpenXRInteractionProfile. A
// profile that needs an extension is left out when the runtime lacks it.
struct MapProfile {
    std::string path; // e.g. "/interaction_profiles/oculus/touch_controller"
    std::string extension;
    std::vector<MapBinding> bindings;
};
// Godot's OpenXRActionMap: one action set and the profiles it suggests.
struct ActionMap {
    std::string action_set = "godot";
    std::string localized_name = "Godot action set";
    std::vector<MapAction> actions;
    std::vector<MapProfile> profiles;
};
// Godot's default action map (OpenXRActionMap::create_default_action_sets): triggers, grips,
// thumbsticks, buttons, the default, aim, grip and palm poses and haptics on both hands, bound
// for the controllers of OpenXR 1.0 and the vendor extensions Godot binds.
[[nodiscard]] const ActionMap& default_action_map();

// Godot's OpenXRInterface::SessionState.
enum class SessionState : std::uint8_t {
    Unknown,
    Idle,
    Ready,
    Synchronized,
    Visible,
    Focused,
    Stopping,
    LossPending,
    Exiting,
};
[[nodiscard]] std::string_view session_state_name(SessionState state); // "idle", "focused", ...

// What a started OpenXR interface found, for xr.state and logs.
struct OpenXRInfo {
    std::string runtime_name;
    std::string runtime_version; // "1.0.42"
    std::string api_version;     // the OpenXR version the instance was made for, "1.1.0"
    std::string system_name;
    SessionState session_state = SessionState::Unknown;
    bool running = false;                // between xrBeginSession and xrEndSession
    bool headless = false;               // a session without graphics (XR_MND_headless)
    std::string reference_space;         // "local", "stage" or "local_floor"
    std::vector<std::string> extensions; // the instance's enabled extensions
    std::uint64_t frames = 0;            // frames waited for and ended
};

// Makes the interface, named "OpenXR"; null where this build has no loader.
[[nodiscard]] std::shared_ptr<Interface> make_openxr_interface();
// What an interface made by make_openxr_interface found; null for any other interface.
[[nodiscard]] const OpenXRInfo* openxr_info(const Interface& interface);
// Adds the OpenXR interface to the world's server and initializes it with the world's settings,
// as Godot does at start when xr/openxr/enabled is set. Fails, leaving it added but not
// initialized, when there is no loader, no runtime or the runtime refuses the settings.
Result<void> use_openxr_interface(world::World& world);

} // namespace slash::xr
