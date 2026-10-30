#pragma once

#include "slash/core/json.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/core/math.hpp"
#include "slash/core/result.hpp"
#include "slash/world/tree3d.hpp"
#include "slash/world/world.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// XR, as Godot's XRServer: the interfaces that talk to headsets, the trackers they keep up to date
// (the head, controllers, anchors) and the XR nodes placed by them. XROrigin3D is where the
// player's play space is in the world; XRCamera3D, XRController3D and XRAnchor3D under it follow
// a tracker's pose, as Godot's XRNode3D does. Interfaces are added to a world's server and the
// `xr` system has each initialized one update its trackers once a tick.
//
//   xr::install(world, settings);
//   xr::use_test_interface(world); // sessions: a headset agents move with xr.set
//   if (xr::is_button_pressed(world, right_hand, "trigger_click")) { ... }
namespace slash::xr {

// Godot's XRServer::TrackerType, as bits so a mask can name several.
enum class TrackerType : std::uint8_t {
    Head = 1,
    Controller = 2,
    Basestation = 4,
    Anchor = 8,
    Hand = 16,
    Body = 32,
    Face = 64,
};
inline constexpr std::uint32_t kAnyTracker = 0xFF;
// "head", "controller", "basestation", "anchor", "hand", "body" or "face".
[[nodiscard]] std::string_view tracker_type_name(TrackerType type);
[[nodiscard]] std::optional<TrackerType> tracker_type_named(std::string_view name);

// Godot's XRPositionalTracker::TrackerHand.
enum class Hand : std::uint8_t { Unknown, Left, Right };
[[nodiscard]] std::string_view hand_name(Hand hand); // "unknown", "left" or "right"
[[nodiscard]] std::optional<Hand> hand_named(std::string_view name);

// Godot's XRPose::TrackingConfidence.
enum class Confidence : std::uint8_t { None, Low, High };
[[nodiscard]] std::string_view confidence_name(Confidence confidence); // "none", "low", "high"

// Where a tracked thing is, as Godot's XRPose: in the play space, in meters before the world
// scale.
struct Pose {
    std::string name;
    bool has_tracking_data = false;
    world::Affine3D transform;
    Vec3 linear_velocity;
    Vec3 angular_velocity;
    Confidence confidence = Confidence::None;

    friend bool operator==(const Pose&, const Pose&) = default;
};

// An input's value, as the Variant a Godot tracker keeps: a button, a trigger or a thumbstick.
using Input = std::variant<bool, float, Vec2>;

// Something the interfaces track, as Godot's XRPositionalTracker: its poses and, for a
// controller, its inputs by action name.
struct Tracker {
    TrackerType type = TrackerType::Head;
    std::string name;
    std::string description;
    std::string
        profile; // the interaction profile, such as "/interaction_profiles/khr/simple_controller"
    Hand hand = Hand::Unknown;
    std::map<std::string, Pose, std::less<>> poses;
    std::map<std::string, Input, std::less<>> inputs;
};

// Godot's XRInterface::Capabilities.
enum Capability : std::uint8_t {
    kMono = 1,
    kStereo = 2,
    kQuad = 4,
    kVr = 8,
    kAr = 16,
    kExternal = 32,
};

// Godot's XRInterface::PlayAreaMode.
enum class PlayArea : std::uint8_t { Unknown, ThreeDof, Sitting, Roomscale, Stage };
[[nodiscard]] std::string_view play_area_name(PlayArea mode);
[[nodiscard]] std::optional<PlayArea> play_area_named(std::string_view name);

// Godot's XRInterface::EnvironmentBlendMode.
enum class BlendMode : std::uint8_t { Opaque, Additive, AlphaBlend };
[[nodiscard]] std::string_view blend_mode_name(BlendMode mode); // "opaque", "additive", "alpha"
[[nodiscard]] std::optional<BlendMode> blend_mode_named(std::string_view name);

// Godot's XRInterface::TrackingStatus.
enum class TrackingStatus : std::uint8_t {
    Normal,
    ExcessiveMotion,
    InsufficientFeatures,
    Unknown,
    NotTracking,
};
[[nodiscard]] std::string_view tracking_status_name(TrackingStatus status);

// A view's field of view as the tangents of the angles from its center to its edges, as OpenXR's
// XrFovf: left and down below zero.
struct Fov {
    float left = -1.0F;
    float right = 1.0F;
    float down = -1.0F;
    float up = 1.0F;

    friend bool operator==(const Fov&, const Fov&) = default;
};

// Something that talks to a headset, as Godot's XRInterface. Interfaces update the server's
// trackers in process(), which the xr system calls once a tick while they are initialized.
class Interface {
public:
    Interface() = default;
    Interface(const Interface&) = delete;
    Interface& operator=(const Interface&) = delete;
    Interface(Interface&&) = delete;
    Interface& operator=(Interface&&) = delete;
    virtual ~Interface() = default;

    [[nodiscard]] virtual std::string_view name() const = 0;
    [[nodiscard]] virtual std::uint32_t capabilities() const = 0;
    [[nodiscard]] virtual bool is_initialized() const = 0;
    // Starts it; the first interface initialized becomes the primary one, as Godot's do.
    virtual Result<void> initialize(world::World& world) = 0;
    virtual void uninitialize(world::World& world) = 0;
    // Puts an initialized interface's trackers back as they started, for replays; the server's
    // trackers were cleared.
    virtual void reset(world::World& /*world*/) {}
    [[nodiscard]] virtual TrackingStatus tracking_status() const { return TrackingStatus::Unknown; }
    // Updates the trackers. Called by the xr system once a tick.
    virtual void process(world::World& world) = 0;
    // The headset's place in the play space with the world scale and reference frame applied, as
    // Godot's get_camera_transform.
    [[nodiscard]] virtual world::Affine3D camera_transform(const world::World& world) const = 0;
    // The views it draws, as Godot's get_view_count: 2 for a headset's eyes.
    [[nodiscard]] virtual std::uint32_t view_count() const { return 1; }
    // Each view's image size in pixels, as Godot's get_render_target_size.
    [[nodiscard]] virtual Vec2i render_target_size() const { return {}; }
    // Where a view is, as Godot's get_transform_for_view: cam_transform (the world origin) times
    // the reference frame times the view's place in the play space, its position times the
    // world scale.
    [[nodiscard]] virtual world::Affine3D
    transform_for_view(const world::World& world, std::uint32_t view,
                       const world::Affine3D& cam_transform) const {
        (void)view;
        return cam_transform * camera_transform(world);
    }
    // A view's field of view, which Godot's get_projection_for_view makes its projection of.
    [[nodiscard]] virtual Fov fov_for_view(std::uint32_t /*view*/) const { return {}; }
    [[nodiscard]] virtual PlayArea play_area_mode() const { return PlayArea::Unknown; }
    [[nodiscard]] virtual bool supports_play_area_mode(PlayArea mode) const {
        return mode == PlayArea::Unknown;
    }
    virtual bool set_play_area_mode(world::World& /*world*/, PlayArea mode) {
        return mode == PlayArea::Unknown;
    }
    // The play area's corners on the floor, in the world's space around the origin, as Godot's
    // get_play_area; none when the headset doesn't know them.
    [[nodiscard]] virtual std::vector<Vec3> play_area(const world::World& /*world*/) const {
        return {};
    }
    [[nodiscard]] virtual std::vector<BlendMode> supported_environment_blend_modes() const {
        return {BlendMode::Opaque};
    }
    [[nodiscard]] virtual BlendMode environment_blend_mode() const { return BlendMode::Opaque; }
    virtual bool set_environment_blend_mode(BlendMode mode) { return mode == BlendMode::Opaque; }
    // Shakes a controller: the haptic action action_name on tracker_name, at frequency hertz (0
    // for the runtime's own), amplitude 0 to 1, for duration seconds after delay seconds.
    virtual void trigger_haptic_pulse(world::World& /*world*/, std::string_view /*action_name*/,
                                      std::string_view /*tracker_name*/, double /*frequency*/,
                                      double /*amplitude*/, double /*duration*/, double /*delay*/) {
    }
};

// project.json's "xr", as Godot's xr/openxr and xr/shaders settings.
struct Settings {
    bool enabled = false;                      // start the headset's interface when the game does
    std::string form_factor = "head_mounted";  // or "handheld"
    std::string view_configuration = "stereo"; // or "mono"
    std::string reference_space = "stage";     // "local", "stage" or "local_floor"
    BlendMode environment_blend_mode = BlendMode::Opaque;
    bool shaders = false; // xr/shaders/enabled: the renderer draws for headsets
};
// The JSON Schema of project.json's "xr".
[[nodiscard]] json::Value settings_schema();
[[nodiscard]] std::vector<json::ValidationError> validate_settings(const json::Value& settings,
                                                                   std::string_view at);
[[nodiscard]] Settings parse_settings(const json::Value& settings);

// Gives a world its XR server and the `xr` system (priority -850, after input and the gui), which
// sets the world origin and scale from the current XROrigin3D, has each initialized interface
// process, then places the XR nodes by their trackers' poses and sends them their trackers'
// signals as world events. Installing again changes the settings.
void install(world::World& world, const Settings& settings = {});
[[nodiscard]] const Settings& settings(const world::World& world);

// The server, as Godot's XRServer.
[[nodiscard]] float world_scale(const world::World& world);
void set_world_scale(world::World& world, float scale);
// The current XROrigin3D's place in the world, as of the last tick.
[[nodiscard]] world::Affine3D world_origin(const world::World& world);
// What center_on_hmd set: the play space's turn and shift before the world scale's.
[[nodiscard]] world::Affine3D reference_frame(const world::World& world);
void clear_reference_frame(world::World& world);
// Godot's XRServer::RotationMode.
enum class RotationMode : std::uint8_t { ResetFullRotation, ResetButKeepTilt, DontResetRotation };
// Makes where the headset is now the play space's center, as Godot's center_on_hmd; nothing in
// the stage play area, which is the room's own.
void center_on_hmd(world::World& world, RotationMode rotation_mode, bool keep_height);
// The primary interface's camera_transform; the identity with none.
[[nodiscard]] world::Affine3D hmd_transform(const world::World& world);

void add_interface(world::World& world, std::shared_ptr<Interface> interface);
void remove_interface(world::World& world, std::string_view name);
[[nodiscard]] Interface* find_interface(const world::World& world, std::string_view name);
[[nodiscard]] std::vector<Interface*> interfaces(const world::World& world);
[[nodiscard]] Interface* primary_interface(const world::World& world);
void set_primary_interface(world::World& world, Interface* interface);

// Adds a tracker, or replaces the one with its name; emits tracker_added or tracker_updated.
void add_tracker(world::World& world, Tracker tracker);
// Emits tracker_removed.
void remove_tracker(world::World& world, std::string_view name);
[[nodiscard]] const Tracker* find_tracker(const world::World& world, std::string_view name);
// The trackers whose type is in the mask of TrackerType bits, by name.
[[nodiscard]] std::vector<const Tracker*> trackers(const world::World& world,
                                                   std::uint32_t type_mask = kAnyTracker);

// What interfaces call to update a tracker, as XRPositionalTracker's set_pose, invalidate_pose,
// set_input and set_tracker_profile. Each tells the XR nodes following the tracker at the end of
// the xr system's run, as Godot's tracker signals do.
void set_pose(world::World& world, std::string_view tracker, std::string_view name,
              const world::Affine3D& transform, Vec3 linear_velocity, Vec3 angular_velocity,
              Confidence confidence);
void invalidate_pose(world::World& world, std::string_view tracker, std::string_view name);
void set_input(world::World& world, std::string_view tracker, std::string_view name, Input value);
void set_profile(world::World& world, std::string_view tracker, std::string_view profile);

// The pose's transform with the world scale and reference frame applied, as Godot's
// XRPose::get_adjusted_transform: where its XR node goes under the XROrigin3D.
[[nodiscard]] world::Affine3D adjusted_transform(const world::World& world, const Pose& pose);

// What a frame of the primary interface sees: each view's place in the world (from the world
// origin) and field of view, as Godot's viewport asks an XR interface; none without an
// initialized primary interface.
struct View {
    world::Affine3D transform;
    Fov fov;
};
[[nodiscard]] std::vector<View> views(const world::World& world);

// An XR node's tracker and pose: XRCamera3D's are "head" and "default".
struct NodeBinding {
    std::string tracker;
    std::string pose;
};
[[nodiscard]] std::optional<NodeBinding> binding(const world::World& world, entt::entity node);

// What gameplay code asks an XR node (XRCamera3D, XRController3D or XRAnchor3D), as Godot's
// XRNode3D and XRController3D.
[[nodiscard]] bool has_tracking_data(const world::World& world, entt::entity node);
[[nodiscard]] const Pose* pose_of(const world::World& world, entt::entity node);
[[nodiscard]] std::optional<Input> get_input(const world::World& world, entt::entity node,
                                             std::string_view name);
// The input as Godot converts it: a button is pressed when its value isn't zero; a button is 1
// or 0 as a float, and a float is (value, 0) as a Vector2.
[[nodiscard]] bool is_button_pressed(const world::World& world, entt::entity node,
                                     std::string_view name);
[[nodiscard]] float get_float(const world::World& world, entt::entity node, std::string_view name);
[[nodiscard]] Vec2 get_vector2(const world::World& world, entt::entity node, std::string_view name);
[[nodiscard]] Hand tracker_hand(const world::World& world, entt::entity node);
// Shakes the node's controller through the primary interface.
void trigger_haptic_pulse(world::World& world, entt::entity node, std::string_view action_name,
                          double frequency, double amplitude, double duration, double delay);

// What a screen would show as a warning about a world's XR nodes, as Godot's configuration
// warnings: XR nodes outside an XROrigin3D, without a tracker or pose, an XRCamera3D without a
// Camera3D and an XROrigin3D without an XRCamera3D.
struct Problem {
    entt::entity entity = entt::null;
    std::string path; // the field to fix, as a JSON Pointer under the entity
    std::string message;
};
[[nodiscard]] std::vector<Problem> check_nodes(const world::World& world);

// The interface sessions and tests use in place of a headset, named "Test": its trackers are
// what xr.set (or code) says, and process() hands them to the server each tick as a runtime
// would. It starts with a head and two hand controllers standing at the play space's center.
struct TestPose {
    world::Affine3D transform;
    Vec3 linear_velocity;
    Vec3 angular_velocity;
    bool tracking = true;
    Confidence confidence = Confidence::High;
};
struct TestTracker {
    TrackerType type = TrackerType::Controller;
    Hand hand = Hand::Unknown;
    std::string description;
    std::string profile;
    std::map<std::string, TestPose, std::less<>> poses;
    std::map<std::string, Input, std::less<>> inputs;
};
// A pulse trigger_haptic_pulse asked for, kept for agents to check.
struct HapticPulse {
    std::string action;
    std::string tracker;
    double frequency = 0.0;
    double amplitude = 0.0;
    double duration = 0.0;
    double delay = 0.0;
    std::uint64_t tick = 0;
};
class TestInterface final : public Interface {
public:
    static constexpr std::string_view kName = "Test";
    static constexpr std::size_t kMaxPulses = 16;

    [[nodiscard]] std::string_view name() const override { return kName; }
    [[nodiscard]] std::uint32_t capabilities() const override;
    [[nodiscard]] bool is_initialized() const override { return state_.initialized; }
    Result<void> initialize(world::World& world) override;
    void uninitialize(world::World& world) override;
    void reset(world::World& world) override;
    [[nodiscard]] TrackingStatus tracking_status() const override { return TrackingStatus::Normal; }
    void process(world::World& world) override;
    [[nodiscard]] world::Affine3D camera_transform(const world::World& world) const override;
    [[nodiscard]] PlayArea play_area_mode() const override { return state_.play_area_mode; }
    [[nodiscard]] bool supports_play_area_mode(PlayArea mode) const override;
    bool set_play_area_mode(world::World& world, PlayArea mode) override;
    [[nodiscard]] std::vector<Vec3> play_area(const world::World& world) const override;
    [[nodiscard]] std::vector<BlendMode> supported_environment_blend_modes() const override;
    [[nodiscard]] BlendMode environment_blend_mode() const override { return state_.blend_mode; }
    [[nodiscard]] std::uint32_t view_count() const override { return state_.stereo ? 2U : 1U; }
    [[nodiscard]] Vec2i render_target_size() const override { return state_.render_target_size; }
    [[nodiscard]] world::Affine3D
    transform_for_view(const world::World& world, std::uint32_t view,
                       const world::Affine3D& cam_transform) const override;
    [[nodiscard]] Fov fov_for_view(std::uint32_t view) const override;
    // The distance between the eyes in meters; 0.064 at first.
    void set_ipd(float meters) { state_.ipd = meters; }
    void set_render_target_size(Vec2i size) { state_.render_target_size = size; }
    bool set_environment_blend_mode(BlendMode mode) override;
    void trigger_haptic_pulse(world::World& world, std::string_view action_name,
                              std::string_view tracker_name, double frequency, double amplitude,
                              double duration, double delay) override;

    // What it tracks, which process() hands to the server: changes show at the next tick.
    std::map<std::string, TestTracker, std::less<>>& trackers() { return state_.trackers; }
    [[nodiscard]] const std::map<std::string, TestTracker, std::less<>>& trackers() const {
        return state_.trackers;
    }
    // The play area's size in meters, x by z; 2 by 2 at first.
    [[nodiscard]] Vec2 play_area_size() const { return state_.play_area_size; }
    void set_play_area_size(Vec2 size) { state_.play_area_size = size; }
    // The last kMaxPulses pulses, oldest first.
    [[nodiscard]] const std::vector<HapticPulse>& pulses() const { return state_.pulses; }
    // Back to the head and hands it starts with.
    void reset_trackers(const world::World& world);

    struct State {
        bool initialized = false;
        bool stereo = true;
        PlayArea play_area_mode = PlayArea::Stage;
        BlendMode blend_mode = BlendMode::Opaque;
        Vec2 play_area_size{2.0F, 2.0F};
        float ipd = 0.064F;
        Vec2i render_target_size{1024, 1024};
        std::map<std::string, TestTracker, std::less<>> trackers;
        std::vector<std::string> published; // the trackers it added to the server
        std::vector<HapticPulse> pulses;
    };
    [[nodiscard]] const State& state() const { return state_; }
    void set_state(State state) { state_ = std::move(state); }

private:
    State state_;
};
// Adds the test interface to a world's server, initialized when the settings enable XR; returns
// it. As the engine's own setup, it emits no interface_added or tracker_added events.
TestInterface& use_test_interface(world::World& world);
[[nodiscard]] TestInterface* test_interface(const world::World& world);

// What xr.state returns: the interfaces, the server's scale, origin and reference frame, the
// trackers (those named, or all), the XR nodes and their tracking, the test interface's pulses,
// and the XR nodes' problems.
[[nodiscard]] json::Value state_json(const world::World& world,
                                     const std::vector<std::string>& names = {});

// A copy of the server's state and the test interface's, so a session can put them back when a
// command fails or was a dry run. Other interfaces are not copied.
struct Snapshot {
    std::shared_ptr<const void> data;
};
[[nodiscard]] Snapshot save(const world::World& world);
void restore(world::World& world, const Snapshot& snapshot);

} // namespace slash::xr
