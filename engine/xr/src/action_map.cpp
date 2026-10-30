#include "slash/xr/openxr.hpp"

#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

namespace slash::xr {
namespace {

// A path on both hands' controllers, as Godot writes
// "/user/hand/left<path>,/user/hand/right<path>".
std::vector<std::string> both(std::string_view path) {
    return {"/user/hand/left" + std::string(path), "/user/hand/right" + std::string(path)};
}

// A path that differs between the hands.
std::vector<std::string> each(std::string_view left, std::string_view right) {
    return {"/user/hand/left" + std::string(left), "/user/hand/right" + std::string(right)};
}

std::vector<std::string> left(std::string_view path) {
    return {"/user/hand/left" + std::string(path)};
}

std::vector<std::string> right(std::string_view path) {
    return {"/user/hand/right" + std::string(path)};
}

// The poses and haptics every controller profile binds the same way.
void add_poses(MapProfile& profile) {
    profile.bindings.push_back({"default_pose", both("/input/aim/pose")});
    profile.bindings.push_back({"aim_pose", both("/input/aim/pose")});
    profile.bindings.push_back({"grip_pose", both("/input/grip/pose")});
    profile.bindings.push_back({"palm_pose", both("/input/grip_surface/pose")});
}

ActionMap make_default() {
    // OpenXRActionMap::create_default_action_sets, without the Vive trackers' and eye gaze's top
    // level paths and profiles.
    ActionMap map;
    const std::vector<std::string> hands{"/user/hand/left", "/user/hand/right"};
    for (const auto& [name, type] : std::initializer_list<std::pair<const char*, ActionType>>{
             {"trigger", ActionType::Float},        {"trigger_click", ActionType::Bool},
             {"trigger_touch", ActionType::Bool},   {"grip", ActionType::Float},
             {"grip_click", ActionType::Bool},      {"grip_force", ActionType::Float},
             {"primary", ActionType::Vector2},      {"primary_click", ActionType::Bool},
             {"primary_touch", ActionType::Bool},   {"secondary", ActionType::Vector2},
             {"secondary_click", ActionType::Bool}, {"secondary_touch", ActionType::Bool},
             {"menu_button", ActionType::Bool},     {"select_button", ActionType::Bool},
             {"ax_button", ActionType::Bool},       {"ax_touch", ActionType::Bool},
             {"by_button", ActionType::Bool},       {"by_touch", ActionType::Bool},
             {"default_pose", ActionType::Pose},    {"aim_pose", ActionType::Pose},
             {"grip_pose", ActionType::Pose},       {"palm_pose", ActionType::Pose},
             {"haptic", ActionType::Haptic},
         }) {
        map.actions.push_back({.name = name, .type = type, .toplevel_paths = hands});
    }

    MapProfile simple{
        .path = "/interaction_profiles/khr/simple_controller", .extension = "", .bindings = {}};
    add_poses(simple);
    simple.bindings.push_back({"menu_button", both("/input/menu/click")});
    simple.bindings.push_back({"select_button", both("/input/select/click")});
    simple.bindings.push_back({"haptic", both("/output/haptic")});
    map.profiles.push_back(std::move(simple));

    MapProfile vive{
        .path = "/interaction_profiles/htc/vive_controller", .extension = "", .bindings = {}};
    add_poses(vive);
    vive.bindings.push_back({"menu_button", both("/input/menu/click")});
    vive.bindings.push_back({"select_button", both("/input/system/click")});
    vive.bindings.push_back({"trigger", both("/input/trigger/value")});
    vive.bindings.push_back({"trigger_click", both("/input/trigger/click")});
    vive.bindings.push_back({"grip", both("/input/squeeze/click")});
    vive.bindings.push_back({"grip_click", both("/input/squeeze/click")});
    vive.bindings.push_back({"primary", both("/input/trackpad")});
    vive.bindings.push_back({"primary_click", both("/input/trackpad/click")});
    vive.bindings.push_back({"primary_touch", both("/input/trackpad/touch")});
    vive.bindings.push_back({"haptic", both("/output/haptic")});
    map.profiles.push_back(std::move(vive));

    MapProfile wmr{.path = "/interaction_profiles/microsoft/motion_controller",
                   .extension = "",
                   .bindings = {}};
    add_poses(wmr);
    wmr.bindings.push_back({"menu_button", both("/input/menu/click")});
    wmr.bindings.push_back({"trigger", both("/input/trigger/value")});
    wmr.bindings.push_back({"trigger_click", both("/input/trigger/value")});
    wmr.bindings.push_back({"grip", both("/input/squeeze/click")});
    wmr.bindings.push_back({"grip_click", both("/input/squeeze/click")});
    wmr.bindings.push_back({"primary", both("/input/thumbstick")});
    wmr.bindings.push_back({"primary_click", both("/input/thumbstick/click")});
    wmr.bindings.push_back({"secondary", both("/input/trackpad")});
    wmr.bindings.push_back({"secondary_click", both("/input/trackpad/click")});
    wmr.bindings.push_back({"secondary_touch", both("/input/trackpad/touch")});
    wmr.bindings.push_back({"haptic", both("/output/haptic")});
    map.profiles.push_back(std::move(wmr));

    MapProfile touch{
        .path = "/interaction_profiles/oculus/touch_controller", .extension = "", .bindings = {}};
    add_poses(touch);
    touch.bindings.push_back({"menu_button", each("/input/menu/click", "/input/system/click")});
    touch.bindings.push_back({"ax_button", each("/input/x/click", "/input/a/click")});
    touch.bindings.push_back({"ax_touch", each("/input/x/touch", "/input/a/touch")});
    touch.bindings.push_back({"by_button", each("/input/y/click", "/input/b/click")});
    touch.bindings.push_back({"by_touch", each("/input/y/touch", "/input/b/touch")});
    touch.bindings.push_back({"trigger", both("/input/trigger/value")});
    touch.bindings.push_back({"trigger_click", both("/input/trigger/value")});
    touch.bindings.push_back({"trigger_touch", both("/input/trigger/touch")});
    touch.bindings.push_back({"grip", both("/input/squeeze/value")});
    touch.bindings.push_back({"grip_click", both("/input/squeeze/value")});
    touch.bindings.push_back({"primary", both("/input/thumbstick")});
    touch.bindings.push_back({"primary_click", both("/input/thumbstick/click")});
    touch.bindings.push_back({"primary_touch", both("/input/thumbstick/touch")});
    touch.bindings.push_back({"haptic", both("/output/haptic")});
    map.profiles.push_back(std::move(touch));

    MapProfile pico{.path = "/interaction_profiles/bytedance/pico4_controller",
                    .extension = "XR_BD_controller_interaction",
                    .bindings = {}};
    add_poses(pico);
    pico.bindings.push_back({"select_button", both("/input/system/click")});
    pico.bindings.push_back({"menu_button", left("/input/menu/click")});
    pico.bindings.push_back({"ax_button", each("/input/x/click", "/input/a/click")});
    pico.bindings.push_back({"ax_touch", each("/input/x/touch", "/input/a/touch")});
    pico.bindings.push_back({"by_button", each("/input/y/click", "/input/b/click")});
    pico.bindings.push_back({"by_touch", each("/input/y/touch", "/input/b/touch")});
    pico.bindings.push_back({"trigger", both("/input/trigger/value")});
    pico.bindings.push_back({"trigger_click", both("/input/trigger/value")});
    pico.bindings.push_back({"trigger_touch", both("/input/trigger/touch")});
    pico.bindings.push_back({"grip", both("/input/squeeze/value")});
    pico.bindings.push_back({"grip_click", both("/input/squeeze/value")});
    pico.bindings.push_back({"primary", both("/input/thumbstick")});
    pico.bindings.push_back({"primary_click", both("/input/thumbstick/click")});
    pico.bindings.push_back({"primary_touch", both("/input/thumbstick/touch")});
    pico.bindings.push_back({"haptic", both("/output/haptic")});
    map.profiles.push_back(std::move(pico));

    MapProfile index{
        .path = "/interaction_profiles/valve/index_controller", .extension = "", .bindings = {}};
    add_poses(index);
    index.bindings.push_back({"menu_button", both("/input/system/click")});
    index.bindings.push_back({"ax_button", both("/input/a/click")});
    index.bindings.push_back({"ax_touch", both("/input/a/touch")});
    index.bindings.push_back({"by_button", both("/input/b/click")});
    index.bindings.push_back({"by_touch", both("/input/b/touch")});
    index.bindings.push_back({"trigger", both("/input/trigger/value")});
    index.bindings.push_back({"trigger_click", both("/input/trigger/click")});
    index.bindings.push_back({"trigger_touch", both("/input/trigger/touch")});
    index.bindings.push_back({"grip", both("/input/squeeze/value")});
    index.bindings.push_back({"grip_click", both("/input/squeeze/value")});
    index.bindings.push_back({"grip_force", both("/input/squeeze/force")});
    index.bindings.push_back({"primary", both("/input/thumbstick")});
    index.bindings.push_back({"primary_click", both("/input/thumbstick/click")});
    index.bindings.push_back({"primary_touch", both("/input/thumbstick/touch")});
    index.bindings.push_back({"secondary", both("/input/trackpad")});
    index.bindings.push_back({"secondary_click", both("/input/trackpad/force")});
    index.bindings.push_back({"secondary_touch", both("/input/trackpad/touch")});
    index.bindings.push_back({"haptic", both("/output/haptic")});
    map.profiles.push_back(std::move(index));

    MapProfile hp{.path = "/interaction_profiles/hp/mixed_reality_controller",
                  .extension = "XR_EXT_hp_mixed_reality_controller",
                  .bindings = {}};
    add_poses(hp);
    hp.bindings.push_back({"menu_button", both("/input/menu/click")});
    hp.bindings.push_back({"ax_button", each("/input/x/click", "/input/a/click")});
    hp.bindings.push_back({"by_button", each("/input/y/click", "/input/b/click")});
    hp.bindings.push_back({"trigger", both("/input/trigger/value")});
    hp.bindings.push_back({"trigger_click", both("/input/trigger/value")});
    hp.bindings.push_back({"grip", both("/input/squeeze/value")});
    hp.bindings.push_back({"grip_click", both("/input/squeeze/value")});
    hp.bindings.push_back({"primary", both("/input/thumbstick")});
    hp.bindings.push_back({"primary_click", both("/input/thumbstick/click")});
    hp.bindings.push_back({"haptic", both("/output/haptic")});
    map.profiles.push_back(std::move(hp));

    MapProfile odyssey{.path = "/interaction_profiles/samsung/odyssey_controller",
                       .extension = "XR_EXT_samsung_odyssey_controller",
                       .bindings = {}};
    add_poses(odyssey);
    odyssey.bindings.push_back({"menu_button", both("/input/menu/click")});
    odyssey.bindings.push_back({"trigger", both("/input/trigger/value")});
    odyssey.bindings.push_back({"trigger_click", both("/input/trigger/value")});
    odyssey.bindings.push_back({"grip", both("/input/squeeze/click")});
    odyssey.bindings.push_back({"grip_click", both("/input/squeeze/click")});
    odyssey.bindings.push_back({"primary", both("/input/thumbstick")});
    odyssey.bindings.push_back({"primary_click", both("/input/thumbstick/click")});
    odyssey.bindings.push_back({"secondary", both("/input/trackpad")});
    odyssey.bindings.push_back({"secondary_click", both("/input/trackpad/click")});
    odyssey.bindings.push_back({"secondary_touch", both("/input/trackpad/touch")});
    odyssey.bindings.push_back({"haptic", both("/output/haptic")});
    map.profiles.push_back(std::move(odyssey));

    MapProfile cosmos{.path = "/interaction_profiles/htc/vive_cosmos_controller",
                      .extension = "XR_HTC_vive_cosmos_controller_interaction",
                      .bindings = {}};
    add_poses(cosmos);
    cosmos.bindings.push_back({"menu_button", left("/input/menu/click")});
    cosmos.bindings.push_back({"select_button", right("/input/system/click")});
    cosmos.bindings.push_back({"ax_button", each("/input/x/click", "/input/a/click")});
    cosmos.bindings.push_back({"by_button", each("/input/y/click", "/input/b/click")});
    cosmos.bindings.push_back({"trigger", both("/input/trigger/value")});
    cosmos.bindings.push_back({"trigger_click", both("/input/trigger/click")});
    cosmos.bindings.push_back({"grip", both("/input/squeeze/click")});
    cosmos.bindings.push_back({"grip_click", both("/input/squeeze/click")});
    cosmos.bindings.push_back({"primary", both("/input/thumbstick")});
    cosmos.bindings.push_back({"primary_click", both("/input/thumbstick/click")});
    cosmos.bindings.push_back({"primary_touch", both("/input/thumbstick/touch")});
    cosmos.bindings.push_back({"haptic", both("/output/haptic")});
    map.profiles.push_back(std::move(cosmos));

    MapProfile focus3{.path = "/interaction_profiles/htc/vive_focus3_controller",
                      .extension = "XR_HTC_vive_focus3_controller_interaction",
                      .bindings = {}};
    add_poses(focus3);
    focus3.bindings.push_back({"menu_button", left("/input/menu/click")});
    focus3.bindings.push_back({"select_button", right("/input/system/click")});
    focus3.bindings.push_back({"ax_button", each("/input/x/click", "/input/a/click")});
    focus3.bindings.push_back({"by_button", each("/input/y/click", "/input/b/click")});
    focus3.bindings.push_back({"trigger", both("/input/trigger/value")});
    focus3.bindings.push_back({"trigger_click", both("/input/trigger/click")});
    focus3.bindings.push_back({"trigger_touch", both("/input/trigger/touch")});
    focus3.bindings.push_back({"grip", both("/input/squeeze/value")});
    focus3.bindings.push_back({"grip_click", both("/input/squeeze/click")});
    focus3.bindings.push_back({"primary", both("/input/thumbstick")});
    focus3.bindings.push_back({"primary_click", both("/input/thumbstick/click")});
    focus3.bindings.push_back({"primary_touch", both("/input/thumbstick/touch")});
    focus3.bindings.push_back({"secondary_touch", both("/input/thumbrest/touch")});
    focus3.bindings.push_back({"haptic", both("/output/haptic")});
    map.profiles.push_back(std::move(focus3));

    MapProfile huawei{.path = "/interaction_profiles/huawei/controller",
                      .extension = "XR_HUAWEI_controller_interaction",
                      .bindings = {}};
    add_poses(huawei);
    huawei.bindings.push_back({"menu_button", both("/input/home/click")});
    huawei.bindings.push_back({"trigger", both("/input/trigger/value")});
    huawei.bindings.push_back({"trigger_click", both("/input/trigger/click")});
    huawei.bindings.push_back({"primary", both("/input/trackpad")});
    huawei.bindings.push_back({"primary_click", both("/input/trackpad/click")});
    huawei.bindings.push_back({"primary_touch", both("/input/trackpad/touch")});
    huawei.bindings.push_back({"haptic", both("/output/haptic")});
    map.profiles.push_back(std::move(huawei));
    return map;
}

} // namespace

const ActionMap& default_action_map() {
    static const ActionMap map = make_default();
    return map;
}

std::string_view session_state_name(SessionState state) {
    switch (state) {
    case SessionState::Idle:
        return "idle";
    case SessionState::Ready:
        return "ready";
    case SessionState::Synchronized:
        return "synchronized";
    case SessionState::Visible:
        return "visible";
    case SessionState::Focused:
        return "focused";
    case SessionState::Stopping:
        return "stopping";
    case SessionState::LossPending:
        return "loss_pending";
    case SessionState::Exiting:
        return "exiting";
    case SessionState::Unknown:
        break;
    }
    return "unknown";
}

} // namespace slash::xr
