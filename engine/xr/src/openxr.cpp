// The OpenXR interface, ported from Godot's modules/openxr (OpenXRAPI and OpenXRInterface): each
// function names the one it follows. Built only with the Khronos loader (SLASH_HAS_OPENXR).
#include "slash/xr/openxr.hpp"

#include "internal.hpp"
#include "openxr_graphics.hpp"

#include "slash/core/log.hpp"
#include "slash/render/device.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <mutex>
#include <openxr/openxr.h>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace slash::xr {
namespace {

constexpr std::string_view kName = "OpenXR";
constexpr XrPosef kIdentityPose{{0.0F, 0.0F, 0.0F, 1.0F}, {0.0F, 0.0F, 0.0F}};
constexpr std::string_view kHeadless = "XR_MND_headless";
constexpr std::string_view kLocalFloor = "XR_EXT_local_floor";
constexpr std::string_view kPalmPose = "XR_EXT_palm_pose";

std::string version_text(XrVersion version) {
    return std::to_string(XR_VERSION_MAJOR(version)) + "." +
           std::to_string(XR_VERSION_MINOR(version)) + "." +
           std::to_string(XR_VERSION_PATCH(version));
}

// An OpenXR structure of the type given, its other fields zero.
template <typename T>
T made(XrStructureType type) {
    T value{};
    value.type = type;
    return value;
}

// OpenXRAPI::get_error_string.
std::string error_string(XrInstance instance, XrResult result) {
    std::array<char, XR_MAX_RESULT_STRING_SIZE> text{};
    if (instance != XR_NULL_HANDLE &&
        XR_SUCCEEDED(xrResultToString(instance, result, text.data()))) {
        return text.data();
    }
    return "XrResult " + std::to_string(static_cast<int>(result));
}

// The runtime manifest set_openxr_runtime names, and the one the loader was last handed.
struct RuntimeOverride {
    std::mutex mutex;
    std::string manifest;
    std::string handed;
};
RuntimeOverride& runtime_override() {
    static RuntimeOverride override;
    return override;
}

// Hands the loader the runtime set_openxr_runtime names as its XR_RUNTIME_JSON property
// (XR_KHR_loader_init and XR_EXT_loader_init_properties), or takes back one handed before, when
// it changed. The loader takes it only while no instance is alive, before it looks for the runtime.
XrResult hand_runtime_to_loader() {
    RuntimeOverride& override = runtime_override();
    const std::scoped_lock lock(override.mutex);
    if (override.manifest == override.handed) {
        return XR_SUCCESS;
    }
    PFN_xrInitializeLoaderKHR initialize = nullptr;
    if (const XrResult result =
            xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR",
                                  reinterpret_cast<PFN_xrVoidFunction*>(&initialize));
        XR_FAILED(result) || initialize == nullptr) {
        return XR_FAILED(result) ? result : XR_ERROR_FUNCTION_UNSUPPORTED;
    }
    const XrLoaderInitPropertyValueEXT value{"XR_RUNTIME_JSON", override.manifest.c_str()};
    auto info = made<XrLoaderInitInfoPropertiesEXT>(XR_TYPE_LOADER_INIT_INFO_PROPERTIES_EXT);
    info.propertyValueCount = override.manifest.empty() ? 0 : 1;
    info.propertyValues = &value;
    const XrResult result =
        initialize(reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR*>(&info));
    if (XR_SUCCEEDED(result)) {
        override.handed = override.manifest;
    }
    return result;
}

// OpenXRAPI::copy_string_to_char_buffer.
template <std::size_t N>
void copy_string(std::string_view from, char (&to)[N]) { // NOLINT(*-avoid-c-arrays)
    const std::size_t count = std::min(from.size(), N - 1);
    std::memcpy(to, from.data(), count);
    to[count] = '\0';
}

// OpenXRAPI::transform_from_pose.
world::Affine3D transform_from_pose(const XrPosef& pose) {
    world::Affine3D out = world::rotation_basis(
        Quat{pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w});
    out.origin = {pose.position.x, pose.position.y, pose.position.z};
    return out;
}

// _transform_from_location.
Confidence transform_from_location(const XrSpaceLocation& location, world::Affine3D& transform) {
    Confidence confidence = Confidence::None;
    const XrPosef& pose = location.pose;
    if ((location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0) {
        const world::Affine3D basis = world::rotation_basis(
            Quat{pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w});
        transform.x = basis.x;
        transform.y = basis.y;
        transform.z = basis.z;
        confidence = (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT) != 0
                         ? Confidence::High
                         : Confidence::Low;
    } else {
        const world::Affine3D identity;
        transform.x = identity.x;
        transform.y = identity.y;
        transform.z = identity.z;
    }
    if ((location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0) {
        transform.origin = {pose.position.x, pose.position.y, pose.position.z};
        if ((location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT) == 0) {
            confidence = Confidence::Low;
        } else if (confidence == Confidence::None) {
            confidence = Confidence::High;
        }
    } else {
        transform.origin = {};
    }
    return confidence;
}

// OpenXRAPI::parse_velocities.
void parse_velocities(const XrSpaceVelocity& velocity, Vec3& linear, Vec3& angular) {
    linear =
        (velocity.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) != 0
            ? Vec3{velocity.linearVelocity.x, velocity.linearVelocity.y, velocity.linearVelocity.z}
            : Vec3{};
    angular = (velocity.velocityFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT) != 0
                  ? Vec3{velocity.angularVelocity.x, velocity.angularVelocity.y,
                         velocity.angularVelocity.z}
                  : Vec3{};
}

XrReferenceSpaceType reference_space_named(std::string_view name) {
    if (name == "local") {
        return XR_REFERENCE_SPACE_TYPE_LOCAL;
    }
    if (name == "local_floor") {
        return XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR_EXT;
    }
    return XR_REFERENCE_SPACE_TYPE_STAGE;
}

std::string reference_space_name(XrReferenceSpaceType type) {
    switch (type) {
    case XR_REFERENCE_SPACE_TYPE_LOCAL:
        return "local";
    case XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR_EXT:
        return "local_floor";
    case XR_REFERENCE_SPACE_TYPE_STAGE:
        return "stage";
    case XR_REFERENCE_SPACE_TYPE_VIEW:
        return "view";
    default:
        return "unknown";
    }
}

XrEnvironmentBlendMode to_openxr(BlendMode mode) {
    switch (mode) {
    case BlendMode::Additive:
        return XR_ENVIRONMENT_BLEND_MODE_ADDITIVE;
    case BlendMode::AlphaBlend:
        return XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND;
    case BlendMode::Opaque:
        break;
    }
    return XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
}

std::optional<BlendMode> from_openxr(XrEnvironmentBlendMode mode) {
    switch (mode) {
    case XR_ENVIRONMENT_BLEND_MODE_OPAQUE:
        return BlendMode::Opaque;
    case XR_ENVIRONMENT_BLEND_MODE_ADDITIVE:
        return BlendMode::Additive;
    case XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND:
        return BlendMode::AlphaBlend;
    default:
        return std::nullopt;
    }
}

SessionState session_state_of(XrSessionState state) {
    switch (state) {
    case XR_SESSION_STATE_IDLE:
        return SessionState::Idle;
    case XR_SESSION_STATE_READY:
        return SessionState::Ready;
    case XR_SESSION_STATE_SYNCHRONIZED:
        return SessionState::Synchronized;
    case XR_SESSION_STATE_VISIBLE:
        return SessionState::Visible;
    case XR_SESSION_STATE_FOCUSED:
        return SessionState::Focused;
    case XR_SESSION_STATE_STOPPING:
        return SessionState::Stopping;
    case XR_SESSION_STATE_LOSS_PENDING:
        return SessionState::LossPending;
    case XR_SESSION_STATE_EXITING:
        return SessionState::Exiting;
    default:
        return SessionState::Unknown;
    }
}

// OpenXRInterface::create_action's renames: pose actions are named without "_pose" on trackers.
std::string tracker_action_name(const MapAction& action) {
    if (action.type == ActionType::Pose) {
        if (action.name == "default_pose") {
            return "default";
        }
        if (action.name == "aim_pose") {
            return "aim";
        }
        if (action.name == "grip_pose") {
            return "grip";
        }
    }
    return action.name;
}

class OpenXR final : public Interface {
public:
    OpenXR() = default;
    OpenXR(const OpenXR&) = delete;
    OpenXR& operator=(const OpenXR&) = delete;
    OpenXR(OpenXR&&) = delete;
    OpenXR& operator=(OpenXR&&) = delete;
    ~OpenXR() override { finish(); }

    [[nodiscard]] std::string_view name() const override { return kName; }
    // OpenXRInterface::get_capabilities.
    [[nodiscard]] std::uint32_t capabilities() const override { return kVr | kStereo; }
    [[nodiscard]] bool is_initialized() const override { return initialized_; }
    Result<void> initialize(world::World& world) override;
    void uninitialize(world::World& world) override;
    [[nodiscard]] TrackingStatus tracking_status() const override {
        return TrackingStatus::Unknown;
    }
    void process(world::World& world) override;
    [[nodiscard]] world::Affine3D camera_transform(const world::World& world) const override;
    [[nodiscard]] std::uint32_t view_count() const override {
        return static_cast<std::uint32_t>(std::max<std::size_t>(views_.size(), 1));
    }
    [[nodiscard]] Vec2i render_target_size() const override;
    [[nodiscard]] world::Affine3D
    transform_for_view(const world::World& world, std::uint32_t view,
                       const world::Affine3D& cam_transform) const override;
    [[nodiscard]] Fov fov_for_view(std::uint32_t view) const override;
    [[nodiscard]] PlayArea play_area_mode() const override;
    [[nodiscard]] bool supports_play_area_mode(PlayArea mode) const override {
        return mode != PlayArea::ThreeDof && mode != PlayArea::Unknown;
    }
    bool set_play_area_mode(world::World& world, PlayArea mode) override;
    [[nodiscard]] std::vector<Vec3> play_area(const world::World& world) const override;
    [[nodiscard]] std::vector<BlendMode> supported_environment_blend_modes() const override;
    [[nodiscard]] BlendMode environment_blend_mode() const override {
        return from_openxr(blend_mode_).value_or(BlendMode::Opaque);
    }
    bool set_environment_blend_mode(BlendMode mode) override;
    void trigger_haptic_pulse(world::World& world, std::string_view action_name,
                              std::string_view tracker_name, double frequency, double amplitude,
                              double duration, double delay) override;

    [[nodiscard]] const OpenXRInfo& info() const { return info_; }

    Result<OpenXRDevice> prepare_device(world::World& world, std::string_view driver);
    // The device the session draws with, set before initialize.
    void attach_device(render::RenderDevice* device) { device_ = device; }
    Result<OpenXRFrame> begin_frame(world::World& world);
    Result<void> end_frame();

private:
    struct ActionTracker {
        std::size_t tracker = 0;
        XrSpace space = XR_NULL_HANDLE;
    };
    struct Action {
        std::string name;     // as the trackers name it
        std::string map_name; // as the action map names it: "grip" is a float, "grip_pose" a pose
        ActionType type = ActionType::Bool;
        XrAction handle = XR_NULL_HANDLE;
        std::vector<ActionTracker> trackers;
    };
    struct TrackerEntry {
        std::string toplevel; // "/user/hand/left"
        std::string name;     // "left_hand"
        XrPath path = XR_NULL_PATH;
        XrPath profile = XR_NULL_PATH;
        std::vector<std::size_t> actions;
    };

    Result<void> fail(std::string message);
    [[nodiscard]] bool check(XrResult result, std::string_view what) const;
    [[nodiscard]] bool extension_enabled(std::string_view name) const {
        return enabled_.contains(std::string(name));
    }
    Result<void> create_instance(const Settings& settings, std::string_view application);
    Result<void> create_session();
    Result<void> create_swapchains();
    void destroy_swapchains();
    [[nodiscard]] bool drawing() const { return graphics_ != nullptr && device_ != nullptr; }
    bool wait_frame();
    void sync_actions(world::World& world);
    bool setup_play_space();
    bool reset_emulated_floor_height();
    void load_action_map();
    std::size_t find_tracker(world::World& world, const std::string& toplevel);
    void destroy_session();
    void finish();
    void poll_events(world::World& world);
    void on_state(world::World& world, XrSessionState state);
    void emit(world::World& world, std::string kind) const;
    void handle_tracker(world::World& world, TrackerEntry& tracker);
    Confidence locate(XrSpace space, world::Affine3D& transform, Vec3& linear, Vec3& angular);
    void check_profiles(world::World& world);
    void locate_views();
    [[nodiscard]] std::string path_name(XrPath path) const;

    OpenXRInfo info_;
    bool initialized_ = false;
    XrInstance instance_ = XR_NULL_HANDLE;
    XrVersion version_ = XR_API_VERSION_1_0;
    XrSystemId system_ = XR_NULL_SYSTEM_ID;
    XrSession session_ = XR_NULL_HANDLE;
    XrSessionState session_state_ = XR_SESSION_STATE_UNKNOWN;
    bool running_ = false;
    std::set<std::string, std::less<>> available_;
    std::set<std::string, std::less<>> enabled_;
    XrFormFactor form_factor_ = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrViewConfigurationType view_configuration_ = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    std::vector<XrViewConfigurationView> view_configuration_views_;
    std::vector<XrEnvironmentBlendMode> blend_modes_;
    XrEnvironmentBlendMode blend_mode_ = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    std::vector<XrReferenceSpaceType> reference_spaces_;
    XrReferenceSpaceType requested_space_ = XR_REFERENCE_SPACE_TYPE_STAGE;
    XrReferenceSpaceType reference_space_ = XR_REFERENCE_SPACE_TYPE_STAGE;
    bool play_space_dirty_ = true;
    XrSpace play_space_ = XR_NULL_HANDLE;
    XrSpace view_space_ = XR_NULL_HANDLE;
    struct {
        bool enabled = false;
        bool should_reset_floor_height = false;
        XrSpace local_space = XR_NULL_HANDLE;
        XrSpace stage_space = XR_NULL_HANDLE;
    } floor_emulation_;
    XrFrameState frame_state_ = made<XrFrameState>(XR_TYPE_FRAME_STATE);
    std::vector<XrView> views_;
    bool view_pose_valid_ = false;
    std::vector<world::Affine3D> transform_for_view_;
    XrActionSet action_set_ = XR_NULL_HANDLE;
    std::vector<Action> actions_;
    std::vector<TrackerEntry> trackers_;
    bool interaction_profile_changed_ = true;
    world::Affine3D head_transform_;
    Vec3 head_linear_velocity_;
    Vec3 head_angular_velocity_;
    Confidence head_confidence_ = Confidence::None;
    bool reference_stage_changing_ = false;
    // Drawing into the headset: the graphics extension, what it asked of the render device and
    // the device, a swapchain and a render target for each view, and the frame begun.
    struct Swapchain {
        XrSwapchain handle = XR_NULL_HANDLE;
        std::vector<void*> images;
        std::uint32_t acquired = 0;
        bool has_image = false;
        render::TextureId target = render::TextureId::None;
    };
    std::unique_ptr<detail::OpenXRGraphics> graphics_;
    render::XrDeviceHooks hooks_;
    bool prepared_ = false;
    render::RenderDevice* device_ = nullptr;
    std::int64_t swapchain_format_ = 0;
    std::vector<Swapchain> swapchains_;
    bool frame_begun_ = false;
};

Result<void> OpenXR::fail(std::string message) {
    log::error(message, {{"interface", "OpenXR"}});
    finish();
    return make_error(ErrorCode::PlatformFailure, std::move(message));
}

// OpenXRAPI::xr_result.
bool OpenXR::check(XrResult result, std::string_view what) const {
    if (XR_SUCCEEDED(result)) {
        return true;
    }
    log::error("OpenXR: " + std::string(what) + " [" + error_string(instance_, result) + "]");
    return false;
}

// OpenXRAPI::load_supported_extensions, attempt_create_instance, create_instance,
// get_system_info, load_supported_view_configuration_types, _views and
// load_supported_environmental_blend_modes.
Result<void> OpenXR::create_instance(const Settings& settings, std::string_view application) {
    if (const XrResult result = hand_runtime_to_loader(); XR_FAILED(result)) {
        return fail("OpenXR: the loader didn't take the runtime set_openxr_runtime names [" +
                    error_string(XR_NULL_HANDLE, result) + "]");
    }
    std::uint32_t count = 0;
    if (const XrResult result = xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr);
        XR_FAILED(result)) {
        return fail("OpenXR: no runtime found, is a headset's OpenXR runtime installed and "
                    "active? [" +
                    error_string(XR_NULL_HANDLE, result) + "]");
    }
    std::vector<XrExtensionProperties> properties(
        count, made<XrExtensionProperties>(XR_TYPE_EXTENSION_PROPERTIES));
    if (!check(xrEnumerateInstanceExtensionProperties(nullptr, count, &count, properties.data()),
               "failed to enumerate extensions")) {
        return fail("OpenXR: the runtime didn't list its extensions");
    }
    for (const XrExtensionProperties& property : properties) {
        available_.insert(property.extensionName);
    }

    // The extensions Godot's wrappers ask for that this interface uses: the graphics API's when
    // it draws into the headset, else a headless session where the runtime allows one.
    std::vector<std::string> wanted{std::string(kLocalFloor), std::string(kPalmPose)};
    if (graphics_ != nullptr) {
        if (!available_.contains(graphics_->extension())) {
            return fail("OpenXR: the runtime can't draw with " + std::string(graphics_->driver()) +
                        " (it lacks " + std::string(graphics_->extension()) + ")");
        }
        wanted.emplace_back(graphics_->extension());
        for (const std::string_view extension : graphics_->optional_extensions()) {
            wanted.emplace_back(extension);
        }
    } else {
        wanted.emplace_back(kHeadless);
    }
    for (const MapProfile& profile : default_action_map().profiles) {
        if (!profile.extension.empty()) {
            wanted.push_back(profile.extension);
        }
    }
    const auto attempt = [&](XrVersion version) {
        enabled_.clear();
        std::vector<const char*> names;
        for (const std::string& extension : wanted) {
            if (available_.contains(extension) && enabled_.insert(extension).second) {
                names.push_back(available_.find(extension)->c_str());
            }
        }
        auto info = made<XrInstanceCreateInfo>(XR_TYPE_INSTANCE_CREATE_INFO);
        copy_string("SlashEngine", info.applicationInfo.applicationName);
        if (!application.empty()) {
            copy_string(application, info.applicationInfo.applicationName);
        }
        info.applicationInfo.applicationVersion = 1;
        copy_string("SlashEngine", info.applicationInfo.engineName);
        info.applicationInfo.engineVersion = 1;
        info.applicationInfo.apiVersion = version;
        info.enabledExtensionCount = static_cast<std::uint32_t>(names.size());
        info.enabledExtensionNames = names.data();
        return xrCreateInstance(&info, &instance_);
    };
    version_ = XR_API_VERSION_1_1;
    XrResult result = attempt(version_);
    if (result == XR_ERROR_API_VERSION_UNSUPPORTED) {
        version_ = XR_API_VERSION_1_0;
        result = attempt(version_);
    }
    if (XR_FAILED(result)) {
        return fail("OpenXR: failed to create the instance [" +
                    error_string(XR_NULL_HANDLE, result) + "]");
    }
    info_.api_version = version_text(version_);
    info_.extensions.assign(enabled_.begin(), enabled_.end());

    auto instance_properties = made<XrInstanceProperties>(XR_TYPE_INSTANCE_PROPERTIES);
    if (XR_SUCCEEDED(xrGetInstanceProperties(instance_, &instance_properties))) {
        info_.runtime_name = instance_properties.runtimeName;
        info_.runtime_version = version_text(instance_properties.runtimeVersion);
        log::info("OpenXR: running on OpenXR runtime",
                  {{"runtime", info_.runtime_name}, {"version", info_.runtime_version}});
    }

    form_factor_ = settings.form_factor == "handheld" ? XR_FORM_FACTOR_HANDHELD_DISPLAY
                                                      : XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    auto system_info = made<XrSystemGetInfo>(XR_TYPE_SYSTEM_GET_INFO);
    system_info.formFactor = form_factor_;
    if (!check(xrGetSystem(instance_, &system_info, &system_),
               "failed to get system for our form factor")) {
        return fail("OpenXR: no headset is connected for the " + settings.form_factor +
                    " form factor");
    }
    auto system_properties = made<XrSystemProperties>(XR_TYPE_SYSTEM_PROPERTIES);
    if (!check(xrGetSystemProperties(instance_, system_, &system_properties),
               "failed to get system properties")) {
        return fail("OpenXR: the runtime didn't describe the headset");
    }
    info_.system_name = system_properties.systemName;

    view_configuration_ = settings.view_configuration == "mono"
                              ? XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MONO
                              : XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    count = 0;
    if (!check(xrEnumerateViewConfigurations(instance_, system_, 0, &count, nullptr),
               "failed to get view configuration count") ||
        count == 0) {
        return fail("OpenXR: the headset has no view configurations");
    }
    std::vector<XrViewConfigurationType> configurations(count);
    (void)xrEnumerateViewConfigurations(instance_, system_, count, &count, configurations.data());
    if (std::ranges::find(configurations, view_configuration_) == configurations.end()) {
        view_configuration_ = configurations.front();
    }
    count = 0;
    (void)xrEnumerateViewConfigurationViews(instance_, system_, view_configuration_, 0, &count,
                                            nullptr);
    view_configuration_views_.assign(
        count, made<XrViewConfigurationView>(XR_TYPE_VIEW_CONFIGURATION_VIEW));
    if (!check(xrEnumerateViewConfigurationViews(instance_, system_, view_configuration_, count,
                                                 &count, view_configuration_views_.data()),
               "failed to enumerate view configuration views")) {
        return fail("OpenXR: the headset's views couldn't be read");
    }
    views_.assign(count, made<XrView>(XR_TYPE_VIEW));
    transform_for_view_.assign(count, {});
    // OpenXRInterface::_set_default_pos, until the views are located.
    for (std::size_t view = 0; view < transform_for_view_.size(); ++view) {
        transform_for_view_[view].origin.y = 1.5F;
        if (count == 2) {
            transform_for_view_[view].origin.x = view == 0 ? -0.03F : 0.03F;
        }
    }

    count = 0;
    if (!check(xrEnumerateEnvironmentBlendModes(instance_, system_, view_configuration_, 0, &count,
                                                nullptr),
               "failed to get environment blend mode count") ||
        count == 0) {
        return fail("OpenXR: the headset has no environment blend modes");
    }
    blend_modes_.resize(count);
    (void)xrEnumerateEnvironmentBlendModes(instance_, system_, view_configuration_, count, &count,
                                           blend_modes_.data());
    blend_mode_ = to_openxr(settings.environment_blend_mode);
    if (std::ranges::find(blend_modes_, blend_mode_) == blend_modes_.end()) {
        blend_mode_ = blend_modes_.front();
    }
    requested_space_ = reference_space_named(settings.reference_space);
    return {};
}

// OpenXRAPI::create_session, load_supported_reference_spaces and setup_view_space.
Result<void> OpenXR::create_session() {
    auto info = made<XrSessionCreateInfo>(XR_TYPE_SESSION_CREATE_INFO);
    info.systemId = system_;
    if (drawing()) {
        auto binding = graphics_->binding(device_->native_device(), enabled_);
        if (!binding) {
            return fail(binding.error().message);
        }
        info.next = binding.value();
    } else if (!extension_enabled(kHeadless)) {
        return fail("OpenXR: the runtime needs a graphics binding to start a session: a game "
                    "with a window draws into the headset, and sessions without one need "
                    "XR_MND_headless");
    }
    if (const XrResult result = xrCreateSession(instance_, &info, &session_); XR_FAILED(result)) {
        return fail("OpenXR: failed to create session [" + error_string(instance_, result) + "]");
    }
    info_.headless = !drawing();
    std::uint32_t count = 0;
    (void)xrEnumerateReferenceSpaces(session_, 0, &count, nullptr);
    reference_spaces_.resize(count);
    if (!check(xrEnumerateReferenceSpaces(session_, count, &count, reference_spaces_.data()),
               "failed to enumerate reference spaces") ||
        count == 0) {
        return fail("OpenXR: the session has no reference spaces");
    }
    if (std::ranges::find(reference_spaces_, XR_REFERENCE_SPACE_TYPE_VIEW) ==
        reference_spaces_.end()) {
        return fail("OpenXR: reference space XR_REFERENCE_SPACE_TYPE_VIEW is not supported");
    }
    auto view_info = made<XrReferenceSpaceCreateInfo>(XR_TYPE_REFERENCE_SPACE_CREATE_INFO);
    view_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    view_info.poseInReferenceSpace = kIdentityPose;
    if (!check(xrCreateReferenceSpace(session_, &view_info, &view_space_),
               "failed to create view space")) {
        return fail("OpenXR: failed to create the view space");
    }
    play_space_dirty_ = true;
    return {};
}

// OpenXRAPI::create_main_swapchains and OpenXRAPI::obtain_swapchain_formats: a color swapchain
// for each view at its recommended size, in the first format the runtime lists that the device
// copies into, and a render target of that size to draw the view in.
Result<void> OpenXR::create_swapchains() {
    std::uint32_t count = 0;
    (void)xrEnumerateSwapchainFormats(session_, 0, &count, nullptr);
    std::vector<std::int64_t> offered(count);
    if (!check(xrEnumerateSwapchainFormats(session_, count, &count, offered.data()),
               "failed to get swapchain formats")) {
        return fail("OpenXR: the runtime listed no swapchain formats");
    }
    swapchain_format_ = 0;
    const std::vector<std::int64_t> copied = graphics_->formats();
    for (const std::int64_t format : offered) {
        if (std::ranges::find(copied, format) != copied.end()) {
            swapchain_format_ = format;
            break;
        }
    }
    if (swapchain_format_ == 0) {
        return fail("OpenXR: the runtime offers no 8-bit RGBA swapchain format");
    }
    for (const XrViewConfigurationView& view : view_configuration_views_) {
        auto create = made<XrSwapchainCreateInfo>(XR_TYPE_SWAPCHAIN_CREATE_INFO);
        create.usageFlags =
            XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        create.format = swapchain_format_;
        create.sampleCount = 1;
        create.width = view.recommendedImageRectWidth;
        create.height = view.recommendedImageRectHeight;
        create.faceCount = 1;
        create.arraySize = 1;
        create.mipCount = 1;
        Swapchain swapchain;
        if (const XrResult result = xrCreateSwapchain(session_, &create, &swapchain.handle);
            XR_FAILED(result)) {
            destroy_swapchains();
            return fail("OpenXR: failed to create swapchain [" + error_string(instance_, result) +
                        "]");
        }
        auto images = graphics_->images(swapchain.handle);
        auto target = device_->create_texture(
            {.width = create.width, .height = create.height, .render_target = true});
        if (!images || !target) {
            xrDestroySwapchain(swapchain.handle);
            if (target) {
                device_->destroy_texture(target.value());
            }
            destroy_swapchains();
            return fail(!images ? images.error().message
                                : "OpenXR: no render target for a view: " + target.error().message);
        }
        swapchain.images = std::move(images).value();
        swapchain.target = target.value();
        swapchains_.push_back(std::move(swapchain));
    }
    return {};
}

void OpenXR::destroy_swapchains() {
    for (const Swapchain& swapchain : swapchains_) {
        if (swapchain.handle != XR_NULL_HANDLE) {
            xrDestroySwapchain(swapchain.handle);
        }
        if (device_ != nullptr && swapchain.target != render::TextureId::None) {
            device_->destroy_texture(swapchain.target);
        }
    }
    swapchains_.clear();
    frame_begun_ = false;
}

// OpenXRAPI::setup_play_space.
bool OpenXR::setup_play_space() {
    const auto supported = [&](XrReferenceSpaceType type) {
        return std::ranges::find(reference_spaces_, type) != reference_spaces_.end();
    };
    const auto create = [&](XrReferenceSpaceType type, XrPosef pose, XrSpace& space) {
        auto info = made<XrReferenceSpaceCreateInfo>(XR_TYPE_REFERENCE_SPACE_CREATE_INFO);
        info.referenceSpaceType = type;
        info.poseInReferenceSpace = pose;
        return check(xrCreateReferenceSpace(session_, &info, &space),
                     "failed to create " + reference_space_name(type) + " space");
    };
    XrReferenceSpaceType space_type = XR_REFERENCE_SPACE_TYPE_LOCAL;
    bool emulate_floor = false;
    if (supported(requested_space_)) {
        space_type = requested_space_;
    } else if (requested_space_ == XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR_EXT &&
               supported(XR_REFERENCE_SPACE_TYPE_STAGE)) {
        // LOCAL_FLOOR emulated with the STAGE's height under the LOCAL space.
        emulate_floor = true;
        if (floor_emulation_.local_space == XR_NULL_HANDLE &&
            !create(XR_REFERENCE_SPACE_TYPE_LOCAL, kIdentityPose, floor_emulation_.local_space)) {
            emulate_floor = false;
        }
        if (floor_emulation_.stage_space == XR_NULL_HANDLE &&
            !create(XR_REFERENCE_SPACE_TYPE_STAGE, kIdentityPose, floor_emulation_.stage_space)) {
            emulate_floor = false;
        }
        if (!emulate_floor) {
            for (XrSpace* space : {&floor_emulation_.local_space, &floor_emulation_.stage_space}) {
                if (*space != XR_NULL_HANDLE) {
                    xrDestroySpace(*space);
                    *space = XR_NULL_HANDLE;
                }
            }
        }
    }
    XrSpace space = XR_NULL_HANDLE;
    if (!create(space_type, kIdentityPose, space)) {
        return false;
    }
    if (play_space_ != XR_NULL_HANDLE) {
        xrDestroySpace(play_space_);
    }
    play_space_ = space;
    reference_space_ = space_type;
    floor_emulation_.enabled = emulate_floor;
    floor_emulation_.should_reset_floor_height = emulate_floor;
    info_.reference_space = reference_space_name(reference_space_);
    return true;
}

// OpenXRAPI::reset_emulated_floor_height.
bool OpenXR::reset_emulated_floor_height() {
    auto stage = made<XrSpaceLocation>(XR_TYPE_SPACE_LOCATION);
    if (!check(xrLocateSpace(floor_emulation_.stage_space, floor_emulation_.local_space,
                             frame_state_.predictedDisplayTime, &stage),
               "failed to locate STAGE space in LOCAL space, in order to emulate LOCAL_FLOOR")) {
        return false;
    }
    auto info = made<XrReferenceSpaceCreateInfo>(XR_TYPE_REFERENCE_SPACE_CREATE_INFO);
    info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    info.poseInReferenceSpace = {{0.0F, 0.0F, 0.0F, 1.0F}, {0.0F, stage.pose.position.y, 0.0F}};
    XrSpace space = XR_NULL_HANDLE;
    if (!check(xrCreateReferenceSpace(session_, &info, &space),
               "failed to recreate emulated LOCAL_FLOOR play space with latest floor estimate")) {
        return false;
    }
    xrDestroySpace(play_space_);
    play_space_ = space;
    reference_space_ = XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR_EXT;
    info_.reference_space = "local_floor";
    return true;
}

std::string OpenXR::path_name(XrPath path) const {
    if (path == XR_NULL_PATH) {
        return {};
    }
    std::array<char, XR_MAX_PATH_LENGTH> text{};
    std::uint32_t length = 0;
    if (XR_FAILED(xrPathToString(instance_, path, static_cast<std::uint32_t>(text.size()), &length,
                                 text.data()))) {
        return {};
    }
    return text.data();
}

// OpenXRInterface::find_tracker: a controller tracker per top-level path.
std::size_t OpenXR::find_tracker(world::World& world, const std::string& toplevel) {
    for (std::size_t i = 0; i < trackers_.size(); ++i) {
        if (trackers_[i].toplevel == toplevel) {
            return i;
        }
    }
    TrackerEntry entry;
    entry.toplevel = toplevel;
    (void)xrStringToPath(instance_, toplevel.c_str(), &entry.path);
    Tracker tracker{.type = TrackerType::Controller,
                    .name = toplevel,
                    .description = toplevel,
                    .profile = "/interaction_profiles/none",
                    .hand = Hand::Unknown,
                    .poses = {},
                    .inputs = {}};
    if (toplevel == "/user/hand/left") {
        tracker.name = "left_hand";
        tracker.description = "Left hand controller";
        tracker.hand = Hand::Left;
    } else if (toplevel == "/user/hand/right") {
        tracker.name = "right_hand";
        tracker.description = "Right hand controller";
        tracker.hand = Hand::Right;
    }
    entry.name = tracker.name;
    add_tracker(world, std::move(tracker));
    trackers_.push_back(std::move(entry));
    return trackers_.size() - 1;
}

// OpenXRInterface::_load_action_map with OpenXRAPI's action set, action and interaction
// profile calls.
void OpenXR::load_action_map() {
    // Trackers come in initialize(), which has the world.
    const ActionMap& map = default_action_map();
    auto set_info = made<XrActionSetCreateInfo>(XR_TYPE_ACTION_SET_CREATE_INFO);
    copy_string(map.action_set, set_info.actionSetName);
    copy_string(map.localized_name, set_info.localizedActionSetName);
    set_info.priority = 0;
    if (!check(xrCreateActionSet(instance_, &set_info, &action_set_),
               "failed to create action set " + map.action_set)) {
        return;
    }
    for (const MapAction& entry : map.actions) {
        Action action;
        action.name = tracker_action_name(entry);
        action.map_name = entry.name;
        action.type = entry.type;
        std::vector<XrPath> paths;
        for (const std::string& toplevel : entry.toplevel_paths) {
            for (std::size_t i = 0; i < trackers_.size(); ++i) {
                if (trackers_[i].toplevel == toplevel) {
                    action.trackers.push_back({.tracker = i, .space = XR_NULL_HANDLE});
                    paths.push_back(trackers_[i].path);
                }
            }
        }
        if (paths.empty()) {
            continue;
        }
        auto info = made<XrActionCreateInfo>(XR_TYPE_ACTION_CREATE_INFO);
        copy_string(entry.name, info.actionName);
        copy_string(entry.name, info.localizedActionName);
        switch (entry.type) {
        case ActionType::Bool:
            info.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
            break;
        case ActionType::Float:
            info.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
            break;
        case ActionType::Vector2:
            info.actionType = XR_ACTION_TYPE_VECTOR2F_INPUT;
            break;
        case ActionType::Pose:
            info.actionType = XR_ACTION_TYPE_POSE_INPUT;
            break;
        case ActionType::Haptic:
            info.actionType = XR_ACTION_TYPE_VIBRATION_OUTPUT;
            break;
        }
        info.countSubactionPaths = static_cast<std::uint32_t>(paths.size());
        info.subactionPaths = paths.data();
        if (!check(xrCreateAction(action_set_, &info, &action.handle),
                   "failed to create action " + entry.name)) {
            continue;
        }
        for (const ActionTracker& on : action.trackers) {
            trackers_[on.tracker].actions.push_back(actions_.size());
        }
        actions_.push_back(std::move(action));
    }

    // OpenXRAPI::interaction_profile_suggest_bindings, a profile at a time, its paths renamed
    // for OpenXR 1.0 as check_profile_path does.
    const bool palm = version_ >= XR_API_VERSION_1_1 || extension_enabled(kPalmPose);
    for (const MapProfile& profile : map.profiles) {
        if (!profile.extension.empty() && !extension_enabled(profile.extension)) {
            continue;
        }
        XrPath profile_path = XR_NULL_PATH;
        if (XR_FAILED(xrStringToPath(instance_, profile.path.c_str(), &profile_path))) {
            continue;
        }
        std::vector<XrActionSuggestedBinding> bindings;
        for (const MapBinding& binding : profile.bindings) {
            const auto made = std::ranges::find_if(
                actions_, [&](const Action& a) { return a.map_name == binding.action; });
            if (made == actions_.end()) {
                continue;
            }
            for (std::string path : binding.paths) {
                if (path.ends_with("/input/grip_surface/pose")) {
                    if (!palm) {
                        continue;
                    }
                    if (version_ < XR_API_VERSION_1_1) {
                        path.replace(path.size() - std::string_view("grip_surface/pose").size(),
                                     std::string::npos, "palm_ext/pose");
                    }
                }
                XrActionSuggestedBinding suggested{made->handle, XR_NULL_PATH};
                if (XR_SUCCEEDED(xrStringToPath(instance_, path.c_str(), &suggested.binding))) {
                    bindings.push_back(suggested);
                }
            }
        }
        auto suggested = made<XrInteractionProfileSuggestedBinding>(
            XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING);
        suggested.interactionProfile = profile_path;
        suggested.countSuggestedBindings = static_cast<std::uint32_t>(bindings.size());
        suggested.suggestedBindings = bindings.data();
        const XrResult result = xrSuggestInteractionProfileBindings(instance_, &suggested);
        if (result == XR_ERROR_PATH_UNSUPPORTED) {
            log::debug("OpenXR: interaction profile isn't supported on this runtime",
                       {{"profile", profile.path}});
        } else {
            (void)check(result, "failed to suggest bindings for " + profile.path);
        }
    }
}

// OpenXRInterface::initialize, after OpenXRAPI::initialize and initialize_session.
Result<void> OpenXR::initialize(world::World& world) {
    if (initialized_) {
        return {};
    }
    const Settings& current = settings(world);
    if (!prepared_) {
        graphics_.reset();
        if (auto made = create_instance(current, {}); !made) {
            return made;
        }
    }
    prepared_ = false;
    if (graphics_ != nullptr && device_ == nullptr) {
        finish();
        return fail("OpenXR: the render device made for the headset wasn't given");
    }
    if (auto made = create_session(); !made) {
        finish();
        return made;
    }
    if (drawing()) {
        if (auto made = create_swapchains(); !made) {
            finish();
            return made;
        }
    }
    // The trackers the action map's top-level paths name, then the actions on them.
    for (const MapAction& action : default_action_map().actions) {
        for (const std::string& toplevel : action.toplevel_paths) {
            (void)find_tracker(world, toplevel);
        }
    }
    load_action_map();
    add_tracker(world, Tracker{.type = TrackerType::Head,
                               .name = "head",
                               .description = "Players head",
                               .profile = {},
                               .hand = Hand::Unknown,
                               .poses = {},
                               .inputs = {}});
    if (action_set_ != XR_NULL_HANDLE) {
        auto attach = made<XrSessionActionSetsAttachInfo>(XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO);
        attach.countActionSets = 1;
        attach.actionSets = &action_set_;
        (void)check(xrAttachSessionActionSets(session_, &attach), "failed to attach action sets");
    }
    detail::server(world).primary = this;
    initialized_ = true;
    return {};
}

// OpenXRInterface::uninitialize, then OpenXRAPI::finish.
void OpenXR::uninitialize(world::World& world) {
    if (!initialized_) {
        return;
    }
    for (const TrackerEntry& tracker : trackers_) {
        remove_tracker(world, tracker.name);
    }
    remove_tracker(world, "head");
    finish();
    detail::Server& s = detail::server(world);
    if (s.primary == this) {
        s.primary = nullptr;
    }
}

// OpenXRAPI::destroy_session.
void OpenXR::destroy_session() {
    destroy_swapchains();
    if (running_ && session_ != XR_NULL_HANDLE) {
        xrEndSession(session_);
    }
    running_ = false;
    for (Action& action : actions_) {
        for (ActionTracker& on : action.trackers) {
            if (on.space != XR_NULL_HANDLE) {
                xrDestroySpace(on.space);
                on.space = XR_NULL_HANDLE;
            }
        }
    }
    for (XrSpace* space : {&play_space_, &view_space_, &floor_emulation_.local_space,
                           &floor_emulation_.stage_space}) {
        if (*space != XR_NULL_HANDLE) {
            xrDestroySpace(*space);
            *space = XR_NULL_HANDLE;
        }
    }
    floor_emulation_.enabled = false;
    floor_emulation_.should_reset_floor_height = false;
    reference_spaces_.clear();
    if (session_ != XR_NULL_HANDLE) {
        xrDestroySession(session_);
        session_ = XR_NULL_HANDLE;
    }
}

void OpenXR::finish() {
    destroy_session();
    for (const Action& action : actions_) {
        if (action.handle != XR_NULL_HANDLE) {
            xrDestroyAction(action.handle);
        }
    }
    actions_.clear();
    trackers_.clear();
    if (action_set_ != XR_NULL_HANDLE) {
        xrDestroyActionSet(action_set_);
        action_set_ = XR_NULL_HANDLE;
    }
    if (instance_ != XR_NULL_HANDLE) {
        xrDestroyInstance(instance_);
        instance_ = XR_NULL_HANDLE;
    }
    available_.clear();
    enabled_.clear();
    graphics_.reset();
    hooks_ = {};
    prepared_ = false;
    device_ = nullptr;
    initialized_ = false;
    session_state_ = XR_SESSION_STATE_UNKNOWN;
    info_.session_state = SessionState::Unknown;
    info_.running = false;
}

void OpenXR::emit(world::World& world, std::string kind) const {
    json::Value event;
    event["interface"] = kName;
    detail::server(world).emit(world, std::move(kind), std::move(event));
}

// OpenXRAPI's on_state_* and OpenXRInterface's, which emit Godot's signals as world events.
void OpenXR::on_state(world::World& world, XrSessionState state) {
    session_state_ = state;
    info_.session_state = session_state_of(state);
    switch (state) {
    case XR_SESSION_STATE_READY: {
        auto begin = made<XrSessionBeginInfo>(XR_TYPE_SESSION_BEGIN_INFO);
        begin.primaryViewConfigurationType = view_configuration_;
        if (!check(xrBeginSession(session_, &begin), "failed to begin session")) {
            return;
        }
        running_ = true;
        info_.running = true;
        emit(world, "session_begun");
        break;
    }
    case XR_SESSION_STATE_SYNCHRONIZED:
        emit(world, "session_synchronized");
        break;
    case XR_SESSION_STATE_VISIBLE:
        emit(world, "session_visible");
        break;
    case XR_SESSION_STATE_FOCUSED:
        emit(world, "session_focussed");
        break;
    case XR_SESSION_STATE_STOPPING:
        emit(world, "session_stopping");
        if (running_) {
            (void)check(xrEndSession(session_), "failed to end session");
            running_ = false;
            info_.running = false;
        }
        break;
    case XR_SESSION_STATE_LOSS_PENDING:
        emit(world, "session_loss_pending");
        break;
    case XR_SESSION_STATE_EXITING:
        emit(world, "instance_exiting");
        break;
    default:
        break;
    }
}

// OpenXRAPI::poll_events.
void OpenXR::poll_events(world::World& world) {
    auto event = made<XrEventDataBuffer>(XR_TYPE_EVENT_DATA_BUFFER);
    XrResult result = xrPollEvent(instance_, &event);
    while (result == XR_SUCCESS) {
        switch (event.type) {
        case XR_TYPE_EVENT_DATA_EVENTS_LOST:
            log::warn(
                "OpenXR EVENT: event data lost",
                {{"count", reinterpret_cast<const XrEventDataEventsLost&>(event).lostEventCount}});
            break;
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
            on_state(world, reinterpret_cast<const XrEventDataSessionStateChanged&>(event).state);
            break;
        case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING: {
            const auto& pending =
                reinterpret_cast<const XrEventDataReferenceSpaceChangePending&>(event);
            if (floor_emulation_.enabled) {
                floor_emulation_.should_reset_floor_height = true;
            }
            // OpenXRInterface::on_reference_space_change_pending.
            reference_stage_changing_ = true;
            json::Value changed;
            changed["interface"] = kName;
            changed["reference_space"] = reference_space_name(pending.referenceSpaceType);
            detail::server(world).emit(world, "play_area_changed", std::move(changed));
            break;
        }
        case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED:
            if (reinterpret_cast<const XrEventDataInteractionProfileChanged&>(event).session ==
                session_) {
                interaction_profile_changed_ = true;
            }
            break;
        default:
            break;
        }
        event = made<XrEventDataBuffer>(XR_TYPE_EVENT_DATA_BUFFER);
        result = xrPollEvent(instance_, &event);
    }
    if (result != XR_EVENT_UNAVAILABLE) {
        (void)check(result, "failed to poll events");
    }
}

Confidence OpenXR::locate(XrSpace space, world::Affine3D& transform, Vec3& linear, Vec3& angular) {
    auto velocity = made<XrSpaceVelocity>(XR_TYPE_SPACE_VELOCITY);
    auto location = made<XrSpaceLocation>(XR_TYPE_SPACE_LOCATION);
    location.next = &velocity;
    if (!check(xrLocateSpace(space, play_space_, frame_state_.predictedDisplayTime, &location),
               "failed to locate space")) {
        return Confidence::None;
    }
    const Confidence confidence = transform_from_location(location, transform);
    parse_velocities(velocity, linear, angular);
    return confidence;
}

// OpenXRAPI::tracker_check_profile for each tracker, as sync_action_sets does after a change.
void OpenXR::check_profiles(world::World& world) {
    for (TrackerEntry& tracker : trackers_) {
        auto state = made<XrInteractionProfileState>(XR_TYPE_INTERACTION_PROFILE_STATE);
        if (XR_FAILED(xrGetCurrentInteractionProfile(session_, tracker.path, &state))) {
            continue;
        }
        if (state.interactionProfile != tracker.profile) {
            tracker.profile = state.interactionProfile;
            // OpenXRInterface::tracker_profile_changed.
            const std::string name = path_name(tracker.profile);
            set_profile(world, tracker.name, name.empty() ? "/interaction_profiles/none" : name);
        }
    }
}

// OpenXRInterface::handle_tracker.
void OpenXR::handle_tracker(world::World& world, TrackerEntry& tracker) {
    if (tracker.profile == XR_NULL_PATH) {
        return;
    }
    const auto index = static_cast<std::size_t>(&tracker - trackers_.data());
    for (const std::size_t id : tracker.actions) {
        Action& action = actions_[id];
        auto get = made<XrActionStateGetInfo>(XR_TYPE_ACTION_STATE_GET_INFO);
        get.action = action.handle;
        get.subactionPath = tracker.path;
        switch (action.type) {
        case ActionType::Bool: {
            auto state = made<XrActionStateBoolean>(XR_TYPE_ACTION_STATE_BOOLEAN);
            const bool ok = XR_SUCCEEDED(xrGetActionStateBoolean(session_, &get, &state));
            set_input(world, tracker.name, action.name,
                      ok && state.isActive != 0 && state.currentState != 0);
            break;
        }
        case ActionType::Float: {
            auto state = made<XrActionStateFloat>(XR_TYPE_ACTION_STATE_FLOAT);
            const bool ok = XR_SUCCEEDED(xrGetActionStateFloat(session_, &get, &state));
            set_input(world, tracker.name, action.name,
                      ok && state.isActive != 0 ? state.currentState : 0.0F);
            break;
        }
        case ActionType::Vector2: {
            auto state = made<XrActionStateVector2f>(XR_TYPE_ACTION_STATE_VECTOR2F);
            const bool ok = XR_SUCCEEDED(xrGetActionStateVector2f(session_, &get, &state));
            set_input(world, tracker.name, action.name,
                      ok && state.isActive != 0 ? Vec2{state.currentState.x, state.currentState.y}
                                                : Vec2{});
            break;
        }
        case ActionType::Pose: {
            // OpenXRAPI::get_action_pose: a space per action and tracker, made when first used.
            const auto on = std::ranges::find_if(
                action.trackers, [&](const ActionTracker& t) { return t.tracker == index; });
            if (on == action.trackers.end() || frame_state_.predictedDisplayTime == 0) {
                invalidate_pose(world, tracker.name, action.name);
                break;
            }
            if (on->space == XR_NULL_HANDLE) {
                auto space_info = made<XrActionSpaceCreateInfo>(XR_TYPE_ACTION_SPACE_CREATE_INFO);
                space_info.action = action.handle;
                space_info.subactionPath = tracker.path;
                space_info.poseInActionSpace = kIdentityPose;
                if (!check(xrCreateActionSpace(session_, &space_info, &on->space),
                           "couldn't create action space")) {
                    invalidate_pose(world, tracker.name, action.name);
                    break;
                }
            }
            world::Affine3D transform;
            Vec3 linear;
            Vec3 angular;
            const Confidence confidence = locate(on->space, transform, linear, angular);
            if (confidence != Confidence::None) {
                set_pose(world, tracker.name, action.name, transform, linear, angular, confidence);
            } else {
                invalidate_pose(world, tracker.name, action.name);
            }
            break;
        }
        case ActionType::Haptic:
            break;
        }
    }
}

// OpenXRAPI::pre_render's xrLocateViews.
void OpenXR::locate_views() {
    auto info = made<XrViewLocateInfo>(XR_TYPE_VIEW_LOCATE_INFO);
    info.viewConfigurationType = view_configuration_;
    info.displayTime = frame_state_.predictedDisplayTime;
    info.space = play_space_;
    auto state = made<XrViewState>(XR_TYPE_VIEW_STATE);
    std::uint32_t count = 0;
    for (XrView& view : views_) {
        view = made<XrView>(XR_TYPE_VIEW);
    }
    if (!check(xrLocateViews(session_, &info, &state, static_cast<std::uint32_t>(views_.size()),
                             &count, views_.data()),
               "couldn't locate views")) {
        return;
    }
    view_pose_valid_ = (state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0 &&
                       (state.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0;
    if (view_pose_valid_) {
        for (std::size_t view = 0; view < views_.size(); ++view) {
            transform_for_view_[view] = transform_from_pose(views_[view].pose);
        }
    }
}

// OpenXRAPI::process's xrWaitFrame, then the play space set up again when it changed.
bool OpenXR::wait_frame() {
    auto wait = made<XrFrameWaitInfo>(XR_TYPE_FRAME_WAIT_INFO);
    frame_state_ = made<XrFrameState>(XR_TYPE_FRAME_STATE);
    if (!check(xrWaitFrame(session_, &wait, &frame_state_), "xrWaitFrame() was not successful")) {
        return false;
    }
    if (floor_emulation_.should_reset_floor_height && !play_space_dirty_) {
        (void)reset_emulated_floor_height();
        floor_emulation_.should_reset_floor_height = false;
    }
    if (play_space_dirty_) {
        (void)setup_play_space();
        play_space_dirty_ = false;
    }
    return true;
}

// OpenXRAPI::get_head_center and sync_action_sets, at the time the last frame waited for is
// shown.
void OpenXR::sync_actions(world::World& world) {
    world::Affine3D head;
    Vec3 linear;
    Vec3 angular;
    const Confidence confidence = frame_state_.predictedDisplayTime != 0
                                      ? locate(view_space_, head, linear, angular)
                                      : Confidence::None;
    head_confidence_ = confidence;
    if (confidence != Confidence::None) {
        head_transform_ = head;
        head_linear_velocity_ = linear;
        head_angular_velocity_ = angular;
    }
    if (action_set_ != XR_NULL_HANDLE) {
        const XrActiveActionSet active{action_set_, XR_NULL_PATH};
        auto sync = made<XrActionsSyncInfo>(XR_TYPE_ACTIONS_SYNC_INFO);
        sync.countActiveActionSets = 1;
        sync.activeActionSets = &active;
        if (check(xrSyncActions(session_, &sync), "failed to sync active action sets")) {
            if (interaction_profile_changed_) {
                check_profiles(world);
                interaction_profile_changed_ = false;
            }
            for (TrackerEntry& tracker : trackers_) {
                handle_tracker(world, tracker);
            }
        }
    }
}

// OpenXRInterface::process with OpenXRAPI::process. A session that draws waits for its frames as
// it draws them (begin_frame); a headless one waits for one each tick and ends it with nothing
// in it, as Godot's renderer would begin and end it (pre_render, end_frame).
void OpenXR::process(world::World& world) {
    if (instance_ == XR_NULL_HANDLE) {
        return;
    }
    poll_events(world);
    if (running_) {
        if (drawing()) {
            if (frame_state_.predictedDisplayTime != 0 && !play_space_dirty_) {
                sync_actions(world);
            }
        } else if (wait_frame()) {
            sync_actions(world);
            locate_views();
            auto begin = made<XrFrameBeginInfo>(XR_TYPE_FRAME_BEGIN_INFO);
            if (check(xrBeginFrame(session_, &begin), "failed to begin frame")) {
                auto end = made<XrFrameEndInfo>(XR_TYPE_FRAME_END_INFO);
                end.displayTime = frame_state_.predictedDisplayTime;
                end.environmentBlendMode = blend_mode_;
                end.layerCount = 0;
                end.layers = nullptr;
                (void)check(xrEndFrame(session_, &end), "failed to end frame");
                ++info_.frames;
            }
        }
    }
    if (initialized_) {
        set_pose(world, "head", "default", head_transform_, head_linear_velocity_,
                 head_angular_velocity_, head_confidence_);
    }
    if (reference_stage_changing_) {
        emit(world, "pose_recentered");
        reference_stage_changing_ = false;
    }
}

// OpenXRAPI::prepare_for_rendering's choice of graphics extension, then OpenXRAPI::initialize,
// as Godot starts OpenXR before it makes its rendering device: the device is made with what the
// runtime asks of it.
Result<OpenXRDevice> OpenXR::prepare_device(world::World& world, std::string_view driver) {
    if (initialized_ || prepared_) {
        return make_error(ErrorCode::InvalidArgument, "OpenXR has already started");
    }
    const std::string_view wanted =
#if defined(_WIN32)
        driver == "auto" ? std::string_view("d3d12") : driver;
#else
        driver == "auto" ? std::string_view("vulkan") : driver;
#endif
    graphics_ = nullptr;
    if (wanted == "d3d12") {
        graphics_ = detail::make_d3d12_graphics();
    } else if (wanted == "vulkan") {
        graphics_ = detail::make_vulkan_graphics();
    } else if (wanted == "opengl") {
        graphics_ = detail::make_opengl_graphics();
    }
    if (graphics_ == nullptr) {
        std::string drivers = "\"vulkan\"";
        if (detail::make_d3d12_graphics() != nullptr) {
            drivers += ", \"d3d12\"";
        }
        if (detail::make_opengl_graphics() != nullptr) {
            drivers += ", \"opengl\"";
        }
        return make_error(ErrorCode::InvalidArgument,
                          "OpenXR draws with " + drivers + ", not \"" + std::string(driver) + "\"");
    }
    if (auto made = create_instance(settings(world), {}); !made) {
        finish();
        return made.error();
    }
    hooks_ = {};
    if (auto ready = graphics_->prepare(instance_, system_, hooks_); !ready) {
        const std::string message = ready.error().message;
        finish();
        return fail(message).error();
    }
    prepared_ = true;
    info_.graphics = std::string(graphics_->driver());
    return OpenXRDevice{.driver = info_.graphics, .hooks = &hooks_};
}

// OpenXRAPI::pre_render: the frame waited for, the views located, the frame begun and, when the
// runtime shows it, each view's swapchain image acquired.
Result<OpenXRFrame> OpenXR::begin_frame(world::World& /*world*/) {
    OpenXRFrame frame;
    if (!drawing() || !running_ || frame_begun_) {
        return frame;
    }
    if (!wait_frame()) {
        return frame;
    }
    locate_views();
    auto begin = made<XrFrameBeginInfo>(XR_TYPE_FRAME_BEGIN_INFO);
    if (const XrResult result = xrBeginFrame(session_, &begin); XR_FAILED(result)) {
        return fail("OpenXR: failed to begin frame [" + error_string(instance_, result) + "]")
            .error();
    }
    frame_begun_ = true;
    frame.render = frame_state_.shouldRender == XR_TRUE;
    if (!frame.render) {
        return frame;
    }
    for (Swapchain& swapchain : swapchains_) {
        auto acquire = made<XrSwapchainImageAcquireInfo>(XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO);
        if (!check(xrAcquireSwapchainImage(swapchain.handle, &acquire, &swapchain.acquired),
                   "failed to acquire swapchain image")) {
            continue;
        }
        auto wait = made<XrSwapchainImageWaitInfo>(XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO);
        wait.timeout = XR_INFINITE_DURATION;
        swapchain.has_image = check(xrWaitSwapchainImage(swapchain.handle, &wait),
                                    "failed to wait for swapchain image");
        if (!swapchain.has_image) {
            auto release = made<XrSwapchainImageReleaseInfo>(XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO);
            (void)check(xrReleaseSwapchainImage(swapchain.handle, &release),
                        "failed to release swapchain image");
        }
    }
    frame.render = std::ranges::all_of(
        swapchains_, [](const Swapchain& swapchain) { return swapchain.has_image; });
    for (const Swapchain& swapchain : swapchains_) {
        frame.targets.push_back(swapchain.target);
    }
    return frame;
}

// OpenXRAPI::end_frame: each view's render target copied into its swapchain image, the images
// released and the frame ended with a projection layer of the views, or none when the runtime
// doesn't show it.
Result<void> OpenXR::end_frame() {
    if (!frame_begun_) {
        return {};
    }
    frame_begun_ = false;
    Result<void> copied;
    bool complete = !swapchains_.empty();
    std::vector<XrCompositionLayerProjectionView> projection;
    for (std::size_t view = 0; view < swapchains_.size(); ++view) {
        Swapchain& swapchain = swapchains_[view];
        if (!swapchain.has_image) {
            complete = false;
            continue;
        }
        const XrViewConfigurationView& size = view_configuration_views_.at(view);
        if (copied) {
            copied = device_->copy_to_native(
                swapchain.target,
                render::NativeTexture{.image = swapchain.images.at(swapchain.acquired),
                                      .view = nullptr,
                                      .format = static_cast<std::uint32_t>(swapchain_format_),
                                      .width = size.recommendedImageRectWidth,
                                      .height = size.recommendedImageRectHeight,
                                      .pixels = {}});
        }
        auto release = made<XrSwapchainImageReleaseInfo>(XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO);
        (void)check(xrReleaseSwapchainImage(swapchain.handle, &release),
                    "failed to release swapchain image");
        swapchain.has_image = false;
        auto layer_view =
            made<XrCompositionLayerProjectionView>(XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW);
        layer_view.pose = views_.at(view).pose;
        layer_view.fov = views_.at(view).fov;
        layer_view.subImage.swapchain = swapchain.handle;
        layer_view.subImage.imageRect.extent = {
            static_cast<std::int32_t>(size.recommendedImageRectWidth),
            static_cast<std::int32_t>(size.recommendedImageRectHeight)};
        projection.push_back(layer_view);
    }
    auto layer = made<XrCompositionLayerProjection>(XR_TYPE_COMPOSITION_LAYER_PROJECTION);
    layer.space = play_space_;
    layer.viewCount = static_cast<std::uint32_t>(projection.size());
    layer.views = projection.data();
    const auto* layers = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer);
    const bool shown =
        frame_state_.shouldRender == XR_TRUE && complete && copied && view_pose_valid_;
    auto end = made<XrFrameEndInfo>(XR_TYPE_FRAME_END_INFO);
    end.displayTime = frame_state_.predictedDisplayTime;
    end.environmentBlendMode = blend_mode_;
    end.layerCount = shown ? 1 : 0;
    end.layers = shown ? &layers : nullptr;
    if (const XrResult result = xrEndFrame(session_, &end); XR_FAILED(result)) {
        return fail("OpenXR: failed to end frame [" + error_string(instance_, result) + "]");
    }
    ++info_.frames;
    return copied;
}

// OpenXRInterface::get_camera_transform.
world::Affine3D OpenXR::camera_transform(const world::World& world) const {
    world::Affine3D head = head_transform_;
    head.origin = head.origin * world_scale(world);
    return head;
}

// OpenXRAPI::get_recommended_target_size.
Vec2i OpenXR::render_target_size() const {
    if (view_configuration_views_.empty()) {
        return {};
    }
    return {static_cast<int>(view_configuration_views_.front().recommendedImageRectWidth),
            static_cast<int>(view_configuration_views_.front().recommendedImageRectHeight)};
}

// OpenXRInterface::get_transform_for_view.
world::Affine3D OpenXR::transform_for_view(const world::World& world, std::uint32_t view,
                                           const world::Affine3D& cam_transform) const {
    world::Affine3D t;
    if (view < transform_for_view_.size()) {
        t = transform_for_view_[view];
    }
    t.origin = t.origin * world_scale(world);
    return cam_transform * reference_frame(world) * t;
}

Fov OpenXR::fov_for_view(std::uint32_t view) const {
    if (view >= views_.size() || !view_pose_valid_) {
        return {};
    }
    const XrFovf& fov = views_[view].fov;
    // XrFovf holds angles; the projection wants their tangents.
    const auto tangent = [](float angle) {
        const Vec2 turn = cos_sin(angle);
        return turn.y / turn.x;
    };
    return {.left = tangent(fov.angleLeft),
            .right = tangent(fov.angleRight),
            .down = tangent(fov.angleDown),
            .up = tangent(fov.angleUp)};
}

// OpenXRInterface::get_play_area_mode.
PlayArea OpenXR::play_area_mode() const {
    if (!initialized_) {
        return PlayArea::Unknown;
    }
    switch (reference_space_) {
    case XR_REFERENCE_SPACE_TYPE_LOCAL:
        return PlayArea::Sitting;
    case XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR_EXT:
        return PlayArea::Roomscale;
    case XR_REFERENCE_SPACE_TYPE_STAGE:
        return PlayArea::Stage;
    default:
        return PlayArea::Unknown;
    }
}

// OpenXRInterface::set_play_area_mode.
bool OpenXR::set_play_area_mode(world::World& world, PlayArea mode) {
    XrReferenceSpaceType space = XR_REFERENCE_SPACE_TYPE_LOCAL;
    if (mode == PlayArea::Sitting) {
        space = XR_REFERENCE_SPACE_TYPE_LOCAL;
    } else if (mode == PlayArea::Roomscale) {
        space = XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR_EXT;
    } else if (mode == PlayArea::Stage) {
        space = XR_REFERENCE_SPACE_TYPE_STAGE;
    } else {
        return false;
    }
    requested_space_ = space;
    play_space_dirty_ = true;
    clear_reference_frame(world);
    return true;
}

// OpenXRInterface::get_play_area with OpenXRAPI::get_play_space_bounds.
std::vector<Vec3> OpenXR::play_area(const world::World& world) const {
    if (session_ == XR_NULL_HANDLE) {
        return {};
    }
    XrExtent2Df extents{};
    if (XR_FAILED(xrGetReferenceSpaceBoundsRect(session_, reference_space_, &extents)) ||
        extents.width == 0.0F || extents.height == 0.0F) {
        return {};
    }
    constexpr std::array<Vec3, 4> kSides{{
        {-0.5F, 0.0F, -0.5F},
        {0.5F, 0.0F, -0.5F},
        {0.5F, 0.0F, 0.5F},
        {-0.5F, 0.0F, 0.5F},
    }};
    std::vector<Vec3> out;
    const world::Affine3D frame = reference_frame(world);
    for (Vec3 corner : kSides) {
        corner.x *= extents.width;
        corner.z *= extents.height;
        out.push_back(frame.apply(corner));
    }
    return out;
}

std::vector<BlendMode> OpenXR::supported_environment_blend_modes() const {
    std::vector<BlendMode> out;
    for (const XrEnvironmentBlendMode mode : blend_modes_) {
        if (const auto known = from_openxr(mode)) {
            out.push_back(*known);
        }
    }
    return out;
}

// OpenXRAPI::set_environment_blend_mode.
bool OpenXR::set_environment_blend_mode(BlendMode mode) {
    const XrEnvironmentBlendMode wanted = to_openxr(mode);
    if (std::ranges::find(blend_modes_, wanted) == blend_modes_.end()) {
        return false;
    }
    blend_mode_ = wanted;
    return true;
}

// OpenXRInterface::trigger_haptic_pulse with OpenXRAPI's: OpenXR has no delay.
void OpenXR::trigger_haptic_pulse(world::World& /*world*/, std::string_view action_name,
                                  std::string_view tracker_name, double frequency, double amplitude,
                                  double duration, double /*delay*/) {
    if (!running_) {
        return;
    }
    const auto action = std::ranges::find_if(actions_, [&](const Action& a) {
        return a.name == action_name && a.type == ActionType::Haptic;
    });
    const auto tracker = std::ranges::find_if(trackers_, [&](const TrackerEntry& t) {
        return t.name == tracker_name || t.toplevel == tracker_name;
    });
    if (action == actions_.end() || tracker == trackers_.end()) {
        return;
    }
    auto info = made<XrHapticActionInfo>(XR_TYPE_HAPTIC_ACTION_INFO);
    info.action = action->handle;
    info.subactionPath = tracker->path;
    auto vibration = made<XrHapticVibration>(XR_TYPE_HAPTIC_VIBRATION);
    vibration.duration = static_cast<XrDuration>(duration * 1000000000.0);
    vibration.frequency = static_cast<float>(frequency);
    vibration.amplitude = static_cast<float>(amplitude);
    (void)check(xrApplyHapticFeedback(session_, &info,
                                      reinterpret_cast<const XrHapticBaseHeader*>(&vibration)),
                "failed to apply haptic feedback");
}

} // namespace

bool openxr_available() {
    return true;
}

void set_openxr_runtime(std::string_view manifest) {
    RuntimeOverride& override = runtime_override();
    const std::scoped_lock lock(override.mutex);
    override.manifest = std::string(manifest);
}

std::shared_ptr<Interface> make_openxr_interface() {
    return std::make_shared<OpenXR>();
}

const OpenXRInfo* openxr_info(const Interface& interface) {
    if (interface.name() != kName) {
        return nullptr;
    }
    return &static_cast<const OpenXR&>(interface).info();
}

namespace {

OpenXR& openxr_of(world::World& world) {
    Interface* found = find_interface(world, kName);
    if (found == nullptr) {
        add_interface(world, make_openxr_interface());
        found = find_interface(world, kName);
    }
    return static_cast<OpenXR&>(*found);
}

OpenXR* running_openxr(world::World& world) {
    Interface* primary = primary_interface(world);
    if (primary == nullptr || primary->name() != kName) {
        return nullptr;
    }
    return static_cast<OpenXR*>(primary);
}

} // namespace

Result<OpenXRDevice> prepare_openxr_device(world::World& world, std::string_view driver) {
    return openxr_of(world).prepare_device(world, driver);
}

Result<void> use_openxr_interface(world::World& world, render::RenderDevice* device) {
    OpenXR& openxr = openxr_of(world);
    if (!openxr.is_initialized()) {
        openxr.attach_device(device);
    }
    return openxr.initialize(world);
}

Result<OpenXRFrame> begin_openxr_frame(world::World& world) {
    OpenXR* openxr = running_openxr(world);
    if (openxr == nullptr) {
        return OpenXRFrame{};
    }
    return openxr->begin_frame(world);
}

Result<void> end_openxr_frame(world::World& world) {
    OpenXR* openxr = running_openxr(world);
    if (openxr == nullptr) {
        return {};
    }
    return openxr->end_frame();
}

} // namespace slash::xr
