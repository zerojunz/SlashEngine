// A stand-in OpenXR runtime, so the OpenXR interface's tests and the openxr-runtime example run
// without a headset. The Khronos loader finds it through the manifest XR_RUNTIME_JSON names, as
// it would any runtime. It is a headless one (XR_MND_headless): a head and two controllers moved
// by a script, the file FAKE_OPENXR_SCRIPT names, one line a command:
//
//   extensions XR_MND_headless XR_EXT_palm_pose    the extensions it offers (these by default)
//   spaces view local stage local_floor            its reference spaces (these by default)
//   bounds 3 2                                     the stage's size in meters
//   local_height 1.6                               how far above the floor the LOCAL space is
//   profile /interaction_profiles/oculus/touch_controller   the controllers it has
//   frame 0 head 0 1.7 0 0 0 0 1                   from frame 0, the head's position and turn
//   frame 2 /user/hand/right/input/aim/pose 0.2 1.2 -0.3 0 0 0 1
//   frame 3 /user/hand/right/input/trigger/value 0.8
//   frame 3 /user/hand/right/input/a/click 1
//   frame 4 /user/hand/right/input/thumbstick 0.5 -0.5
//   frame 5 /user/hand/left/input/grip/pose lost   untracked from then on
//   frame 6 recenter stage                         the stage moved
//   frame 9 exit                                   the runtime asks the session to stop
//
//   view_size 64 64                                each eye's recommended image size
//   probe 37 32                                    a pixel to read back from each eye's image
//
// Offered XR_KHR_vulkan_enable2 (extensions XR_KHR_vulkan_enable2 ...), it also takes sessions
// that draw with Vulkan: it makes the instance and device the application asks for, gives it
// swapchains of Vulkan images, and at each ended frame with a projection layer reads the probed
// pixels back from the images the layer shows.
//
// Frames are counted by xrWaitFrame from 1; a value holds until a later frame changes it. What
// the application did goes to the file FAKE_OPENXR_LOG names, a line a call: instance, session
// begin and end, suggested bindings, haptics, ended frames and the probed pixels.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <vulkan/vulkan_core.h>

#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>
#include <openxr/openxr_platform.h>
#include <set>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

#if defined(_WIN32)
#define FAKE_EXPORT extern "C" __declspec(dllexport)
#else
#define FAKE_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace {

struct V3 {
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
};
struct Q {
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
    float w = 1.0F;
};
struct Pose {
    Q q;
    V3 p;
};

Q mul(const Q& a, const Q& b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
Q conj(const Q& q) {
    return {-q.x, -q.y, -q.z, q.w};
}
V3 rotate(const Q& q, const V3& v) {
    const Q r = mul(mul(q, Q{v.x, v.y, v.z, 0.0F}), conj(q));
    return {r.x, r.y, r.z};
}
Pose compose(const Pose& a, const Pose& b) { // a then b in a's space
    const V3 moved = rotate(a.q, b.p);
    return {mul(a.q, b.q), {a.p.x + moved.x, a.p.y + moved.y, a.p.z + moved.z}};
}
Pose inverse(const Pose& a) {
    const Q q = conj(a.q);
    const V3 p = rotate(q, a.p);
    return {q, {-p.x, -p.y, -p.z}};
}
Pose from_xr(const XrPosef& pose) {
    return {{pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w},
            {pose.position.x, pose.position.y, pose.position.z}};
}
XrPosef to_xr(const Pose& pose) {
    return {{pose.q.x, pose.q.y, pose.q.z, pose.q.w}, {pose.p.x, pose.p.y, pose.p.z}};
}

// A value an input path has: a pose, a number, two numbers or lost tracking.
struct Value {
    enum class Kind : std::uint8_t { Number, Pair, Pose, Lost } kind = Kind::Number;
    float a = 0.0F;
    float b = 0.0F;
    Pose pose;
};

struct Change {
    std::uint64_t frame = 0;
    std::string path; // "head", an input path, "recenter" or "exit"
    Value value;
    std::string word;
};

struct SpaceObject {
    enum class Kind : std::uint8_t { Reference, Action } kind = Kind::Reference;
    XrReferenceSpaceType type = XR_REFERENCE_SPACE_TYPE_STAGE;
    std::uint64_t action = 0;
    XrPath subaction = XR_NULL_PATH;
    Pose offset;
};

struct ActionObject {
    std::uint64_t set = 0;
    XrActionType type = XR_ACTION_TYPE_BOOLEAN_INPUT;
    std::string name;
};

// A swapchain of Vulkan images, handed out in turn.
struct SwapchainObject {
    std::int64_t format = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<VkImage> images;
    std::vector<VkDeviceMemory> memory;
    std::uint32_t next = 0;
    std::deque<std::uint32_t> acquired;
    std::int64_t released = -1; // the image last released, which layers show
};

// The Vulkan objects a drawing session uses, the application's.
struct Vulkan {
    bool requirements = false; // xrGetVulkanGraphicsRequirements2KHR was called
    PFN_vkGetInstanceProcAddr procs = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    std::uint32_t family = 0;
    VkQueue queue = VK_NULL_HANDLE;
};

struct Runtime {
    // The script.
    std::vector<std::string> extensions{"XR_MND_headless", "XR_EXT_palm_pose",
                                        "XR_EXT_local_floor"};
    std::vector<XrReferenceSpaceType> spaces{
        XR_REFERENCE_SPACE_TYPE_VIEW, XR_REFERENCE_SPACE_TYPE_LOCAL, XR_REFERENCE_SPACE_TYPE_STAGE,
        XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR_EXT};
    float bounds_x = 2.0F;
    float bounds_z = 2.0F;
    float local_height = 1.6F;
    std::string profile = "/interaction_profiles/khr/simple_controller";
    std::vector<Change> changes;
    std::string log_path;
    std::uint32_t view_width = 1024;
    std::uint32_t view_height = 1024;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> probes;

    // The state.
    bool instance = false;
    XrVersion api_version = 0;
    std::set<std::string> enabled;
    std::uint64_t next_id = 1;
    std::vector<std::string> paths{""};
    std::map<std::uint64_t, SpaceObject> space_objects;
    std::map<std::uint64_t, ActionObject> actions;
    std::map<std::uint64_t, std::string> action_sets;
    // Suggested bindings: profile path -> (action, binding path).
    std::map<XrPath, std::vector<std::pair<std::uint64_t, XrPath>>> suggested;
    bool attached = false;
    bool session = false;
    XrSessionState state = XR_SESSION_STATE_UNKNOWN;
    bool running = false;
    std::uint64_t frame = 0;
    std::deque<XrEventDataBuffer> events;
    Pose head{{}, {0.0F, 1.6F, 0.0F}};
    Pose stage_shift;
    std::map<std::string, Value> inputs;
    std::size_t applied = 0;
    std::uint64_t session_id = 0;
    Vulkan vulkan;
    std::map<std::uint64_t, SwapchainObject> swapchains;
};

Runtime& rt() {
    static Runtime runtime;
    return runtime;
}

void log_line(const std::string& line) {
    if (!rt().log_path.empty()) {
        std::ofstream(rt().log_path, std::ios::app) << line << "\n";
    }
}

XrReferenceSpaceType space_named(const std::string& name) {
    if (name == "view") {
        return XR_REFERENCE_SPACE_TYPE_VIEW;
    }
    if (name == "local") {
        return XR_REFERENCE_SPACE_TYPE_LOCAL;
    }
    if (name == "local_floor") {
        return XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR_EXT;
    }
    return XR_REFERENCE_SPACE_TYPE_STAGE;
}

void read_script() {
    Runtime& r = rt();
    const char* log = std::getenv("FAKE_OPENXR_LOG");
    r.log_path = log != nullptr ? log : "";
    const char* path = std::getenv("FAKE_OPENXR_SCRIPT");
    if (path == nullptr) {
        return;
    }
    std::ifstream file(path);
    std::string line;
    while (std::getline(file, line)) {
        std::istringstream in(line);
        std::string word;
        if (!(in >> word) || word.starts_with('#')) {
            continue;
        }
        if (word == "extensions") {
            r.extensions.clear();
            while (in >> word) {
                r.extensions.push_back(word);
            }
        } else if (word == "spaces") {
            r.spaces.clear();
            while (in >> word) {
                r.spaces.push_back(space_named(word));
            }
        } else if (word == "bounds") {
            in >> r.bounds_x >> r.bounds_z;
        } else if (word == "local_height") {
            in >> r.local_height;
        } else if (word == "profile") {
            in >> r.profile;
        } else if (word == "view_size") {
            in >> r.view_width >> r.view_height;
        } else if (word == "probe") {
            std::uint32_t x = 0;
            std::uint32_t y = 0;
            in >> x >> y;
            r.probes.emplace_back(x, y);
        } else if (word == "frame") {
            Change change;
            in >> change.frame >> change.path;
            std::vector<std::string> rest;
            while (in >> word) {
                rest.push_back(word);
            }
            if (change.path == "recenter" || change.path == "exit") {
                change.word = rest.empty() ? "" : rest.front();
            } else if (!rest.empty() && rest.front() == "lost") {
                change.value.kind = Value::Kind::Lost;
            } else if (rest.size() == 7) {
                change.value.kind = Value::Kind::Pose;
                change.value.pose = {{std::stof(rest[3]), std::stof(rest[4]), std::stof(rest[5]),
                                      std::stof(rest[6])},
                                     {std::stof(rest[0]), std::stof(rest[1]), std::stof(rest[2])}};
            } else if (rest.size() == 2) {
                change.value.kind = Value::Kind::Pair;
                change.value.a = std::stof(rest[0]);
                change.value.b = std::stof(rest[1]);
            } else if (rest.size() == 1) {
                change.value.a = std::stof(rest[0]);
            }
            r.changes.push_back(change);
        }
    }
}

template <typename H>
H handle(std::uint64_t id) {
    // A handle is a pointer or a number, by platform.
    if constexpr (std::is_pointer_v<H>) {
        return reinterpret_cast<H>(static_cast<std::uintptr_t>(id));
    } else {
        return static_cast<H>(id);
    }
}
template <typename H>
std::uint64_t id_of(H handle) {
    if constexpr (std::is_pointer_v<H>) {
        return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(handle));
    } else {
        return static_cast<std::uint64_t>(handle);
    }
}

void queue_state(XrSessionState state) {
    XrEventDataBuffer buffer{};
    auto& event = reinterpret_cast<XrEventDataSessionStateChanged&>(buffer);
    event.type = XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED;
    event.session = handle<XrSession>(rt().session_id);
    event.state = state;
    event.time = static_cast<XrTime>(rt().frame) * 11111111;
    rt().events.push_back(buffer);
}

XrPath path_of(const std::string& text) {
    Runtime& r = rt();
    for (std::size_t i = 1; i < r.paths.size(); ++i) {
        if (r.paths[i] == text) {
            return i;
        }
    }
    r.paths.push_back(text);
    return r.paths.size() - 1;
}

// Applies the script's changes up to the current frame.
void apply_changes() {
    Runtime& r = rt();
    while (r.applied < r.changes.size() && r.changes[r.applied].frame <= r.frame) {
        const Change& change = r.changes[r.applied++];
        if (change.path == "head" && change.value.kind == Value::Kind::Pose) {
            r.head = change.value.pose;
        } else if (change.path == "exit") {
            if (r.running && r.state != XR_SESSION_STATE_STOPPING) {
                r.state = XR_SESSION_STATE_STOPPING;
                queue_state(r.state);
            }
        } else if (change.path == "recenter") {
            XrEventDataBuffer buffer{};
            auto& event = reinterpret_cast<XrEventDataReferenceSpaceChangePending&>(buffer);
            event.type = XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING;
            event.session = handle<XrSession>(r.session_id);
            event.referenceSpaceType = space_named(change.word);
            event.changeTime = static_cast<XrTime>(r.frame) * 11111111;
            r.events.push_back(buffer);
        } else {
            r.inputs[change.path] = change.value;
        }
    }
}

// Where a reference space's origin is in the stage.
Pose reference_origin(XrReferenceSpaceType type) {
    Runtime& r = rt();
    switch (type) {
    case XR_REFERENCE_SPACE_TYPE_VIEW:
        return r.head;
    case XR_REFERENCE_SPACE_TYPE_LOCAL:
        return {{}, {0.0F, r.local_height, 0.0F}};
    default:
        return {};
    }
}

// The binding path an action has on a top-level path in the current profile, if bound.
std::string bound_path(std::uint64_t action, XrPath subaction) {
    Runtime& r = rt();
    const auto found = r.suggested.find(path_of(r.profile));
    if (found == r.suggested.end() || !r.attached) {
        return {};
    }
    const std::string prefix = subaction == XR_NULL_PATH ? "" : r.paths[subaction] + "/";
    for (const auto& [bound_action, binding] : found->second) {
        if (bound_action == action && r.paths[binding].starts_with(prefix)) {
            return r.paths[binding];
        }
    }
    return {};
}

// Where a space is in the stage, or nothing when it isn't tracked.
bool space_pose(std::uint64_t id, Pose& out) {
    Runtime& r = rt();
    const auto found = r.space_objects.find(id);
    if (found == r.space_objects.end()) {
        return false;
    }
    const SpaceObject& space = found->second;
    if (space.kind == SpaceObject::Kind::Reference) {
        out = compose(compose(r.stage_shift, reference_origin(space.type)), space.offset);
        return true;
    }
    const std::string path = bound_path(space.action, space.subaction);
    const auto input = r.inputs.find(path);
    if (path.empty() || input == r.inputs.end() || input->second.kind != Value::Kind::Pose) {
        return false;
    }
    out = compose(compose(r.stage_shift, input->second.pose), space.offset);
    return true;
}

// The current input's value at an action's binding.
const Value* action_value(std::uint64_t action, XrPath subaction) {
    const std::string path = bound_path(action, subaction);
    const auto found = rt().inputs.find(path);
    return path.empty() || found == rt().inputs.end() ? nullptr : &found->second;
}

XrResult XRAPI_CALL enumerate_extensions(const char* /*layer*/, uint32_t capacity, uint32_t* count,
                                         XrExtensionProperties* properties) {
    rt() = Runtime{};
    read_script();
    const std::vector<std::string>& names = rt().extensions;
    *count = static_cast<uint32_t>(names.size());
    if (capacity == 0) {
        return XR_SUCCESS;
    }
    if (capacity < names.size()) {
        return XR_ERROR_SIZE_INSUFFICIENT;
    }
    for (std::size_t i = 0; i < names.size(); ++i) {
        std::strncpy(properties[i].extensionName, names[i].c_str(), XR_MAX_EXTENSION_NAME_SIZE - 1);
        properties[i].extensionVersion = 1;
    }
    return XR_SUCCESS;
}

XrResult XRAPI_CALL create_instance(const XrInstanceCreateInfo* info, XrInstance* instance) {
    Runtime& r = rt();
    r = Runtime{};
    read_script();
    if (XR_VERSION_MAJOR(info->applicationInfo.apiVersion) != 1 ||
        XR_VERSION_MINOR(info->applicationInfo.apiVersion) > 1) {
        return XR_ERROR_API_VERSION_UNSUPPORTED;
    }
    std::string line = "instance " +
                       std::to_string(XR_VERSION_MAJOR(info->applicationInfo.apiVersion)) + "." +
                       std::to_string(XR_VERSION_MINOR(info->applicationInfo.apiVersion));
    for (uint32_t i = 0; i < info->enabledExtensionCount; ++i) {
        const std::string name = info->enabledExtensionNames[i];
        bool offered = false;
        for (const std::string& extension : r.extensions) {
            offered = offered || extension == name;
        }
        if (!offered) {
            return XR_ERROR_EXTENSION_NOT_PRESENT;
        }
        r.enabled.insert(name);
        line += " " + name;
    }
    r.instance = true;
    r.api_version = info->applicationInfo.apiVersion;
    *instance = handle<XrInstance>(r.next_id++);
    log_line(line);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL destroy_instance(XrInstance /*instance*/) {
    rt().instance = false;
    log_line("destroy instance");
    return XR_SUCCESS;
}

XrResult XRAPI_CALL get_instance_properties(XrInstance /*instance*/,
                                            XrInstanceProperties* properties) {
    properties->runtimeVersion = XR_MAKE_VERSION(0, 1, 0);
    std::strncpy(properties->runtimeName, "SlashEngine stand-in", XR_MAX_RUNTIME_NAME_SIZE - 1);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL poll_event(XrInstance /*instance*/, XrEventDataBuffer* event) {
    Runtime& r = rt();
    if (r.events.empty()) {
        return XR_EVENT_UNAVAILABLE;
    }
    *event = r.events.front();
    r.events.pop_front();
    return XR_SUCCESS;
}

XrResult XRAPI_CALL result_to_string(XrInstance /*instance*/, XrResult value,
                                     char buffer[XR_MAX_RESULT_STRING_SIZE]) {
    const std::string text = (value < 0 ? "XR_UNKNOWN_FAILURE_" : "XR_UNKNOWN_SUCCESS_") +
                             std::to_string(static_cast<int>(value));
    std::strncpy(buffer, text.c_str(), XR_MAX_RESULT_STRING_SIZE - 1);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL structure_type_to_string(XrInstance /*instance*/, XrStructureType value,
                                             char buffer[XR_MAX_STRUCTURE_NAME_SIZE]) {
    const std::string text = "XR_UNKNOWN_STRUCTURE_TYPE_" + std::to_string(static_cast<int>(value));
    std::strncpy(buffer, text.c_str(), XR_MAX_STRUCTURE_NAME_SIZE - 1);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL get_system(XrInstance /*instance*/, const XrSystemGetInfo* info,
                               XrSystemId* system) {
    if (info->formFactor != XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY) {
        return XR_ERROR_FORM_FACTOR_UNSUPPORTED;
    }
    *system = 1;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL get_system_properties(XrInstance /*instance*/, XrSystemId system,
                                          XrSystemProperties* properties) {
    properties->systemId = system;
    properties->vendorId = 0;
    std::strncpy(properties->systemName, "Stand-in headset", XR_MAX_SYSTEM_NAME_SIZE - 1);
    properties->graphicsProperties = {4096, 4096, 16};
    properties->trackingProperties = {XR_TRUE, XR_TRUE};
    return XR_SUCCESS;
}

XrResult XRAPI_CALL enumerate_view_configurations(XrInstance /*instance*/, XrSystemId /*system*/,
                                                  uint32_t capacity, uint32_t* count,
                                                  XrViewConfigurationType* types) {
    *count = 1;
    if (capacity >= 1) {
        types[0] = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    }
    return XR_SUCCESS;
}

XrResult XRAPI_CALL get_view_configuration_properties(XrInstance /*instance*/,
                                                      XrSystemId /*system*/,
                                                      XrViewConfigurationType type,
                                                      XrViewConfigurationProperties* properties) {
    properties->viewConfigurationType = type;
    properties->fovMutable = XR_FALSE;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL enumerate_view_configuration_views(XrInstance /*instance*/,
                                                       XrSystemId /*system*/,
                                                       XrViewConfigurationType type,
                                                       uint32_t capacity, uint32_t* count,
                                                       XrViewConfigurationView* views) {
    if (type != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) {
        return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    }
    *count = 2;
    for (uint32_t i = 0; i < capacity && i < 2; ++i) {
        views[i].recommendedImageRectWidth = rt().view_width;
        views[i].maxImageRectWidth = 4096;
        views[i].recommendedImageRectHeight = rt().view_height;
        views[i].maxImageRectHeight = 4096;
        views[i].recommendedSwapchainSampleCount = 1;
        views[i].maxSwapchainSampleCount = 4;
    }
    return XR_SUCCESS;
}

XrResult XRAPI_CALL enumerate_blend_modes(XrInstance /*instance*/, XrSystemId /*system*/,
                                          XrViewConfigurationType /*type*/, uint32_t capacity,
                                          uint32_t* count, XrEnvironmentBlendMode* modes) {
    *count = 1;
    if (capacity >= 1) {
        modes[0] = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    }
    return XR_SUCCESS;
}

XrResult XRAPI_CALL create_session(XrInstance /*instance*/, const XrSessionCreateInfo* info,
                                   XrSession* session) {
    Runtime& r = rt();
    const auto* binding = static_cast<const XrBaseInStructure*>(info->next);
    if (binding == nullptr) {
        if (!r.enabled.contains("XR_MND_headless")) {
            return XR_ERROR_GRAPHICS_DEVICE_INVALID;
        }
    } else if (binding->type == XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR &&
               r.enabled.contains("XR_KHR_vulkan_enable2")) {
        if (!r.vulkan.requirements) {
            return XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING;
        }
        const auto* vulkan = reinterpret_cast<const XrGraphicsBindingVulkan2KHR*>(binding);
        if (r.vulkan.procs == nullptr || vulkan->instance != r.vulkan.instance ||
            vulkan->device == VK_NULL_HANDLE) {
            return XR_ERROR_GRAPHICS_DEVICE_INVALID;
        }
        r.vulkan.physical = vulkan->physicalDevice;
        r.vulkan.device = vulkan->device;
        r.vulkan.family = vulkan->queueFamilyIndex;
        const auto get_queue = reinterpret_cast<PFN_vkGetDeviceQueue>(
            r.vulkan.procs(r.vulkan.instance, "vkGetDeviceQueue"));
        get_queue(r.vulkan.device, r.vulkan.family, vulkan->queueIndex, &r.vulkan.queue);
    } else {
        return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    }
    r.session = true;
    r.session_id = r.next_id++;
    *session = handle<XrSession>(r.session_id);
    r.state = XR_SESSION_STATE_IDLE;
    queue_state(XR_SESSION_STATE_IDLE);
    r.state = XR_SESSION_STATE_READY;
    queue_state(XR_SESSION_STATE_READY);
    log_line(binding == nullptr ? "create session" : "create session vulkan");
    return XR_SUCCESS;
}

XrResult XRAPI_CALL destroy_session(XrSession /*session*/) {
    rt().session = false;
    log_line("destroy session");
    return XR_SUCCESS;
}

XrResult XRAPI_CALL begin_session(XrSession /*session*/, const XrSessionBeginInfo* /*info*/) {
    Runtime& r = rt();
    if (r.state != XR_SESSION_STATE_READY) {
        return XR_ERROR_SESSION_NOT_READY;
    }
    r.running = true;
    for (const XrSessionState state :
         {XR_SESSION_STATE_SYNCHRONIZED, XR_SESSION_STATE_VISIBLE, XR_SESSION_STATE_FOCUSED}) {
        r.state = state;
        queue_state(state);
    }
    log_line("begin session");
    return XR_SUCCESS;
}

XrResult XRAPI_CALL end_session(XrSession /*session*/) {
    Runtime& r = rt();
    if (r.state != XR_SESSION_STATE_STOPPING) {
        return XR_ERROR_SESSION_NOT_STOPPING;
    }
    r.running = false;
    r.state = XR_SESSION_STATE_IDLE;
    queue_state(XR_SESSION_STATE_IDLE);
    r.state = XR_SESSION_STATE_EXITING;
    queue_state(XR_SESSION_STATE_EXITING);
    log_line("end session");
    return XR_SUCCESS;
}

XrResult XRAPI_CALL request_exit_session(XrSession /*session*/) {
    Runtime& r = rt();
    if (!r.running) {
        return XR_ERROR_SESSION_NOT_RUNNING;
    }
    r.state = XR_SESSION_STATE_STOPPING;
    queue_state(r.state);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL enumerate_reference_spaces(XrSession /*session*/, uint32_t capacity,
                                               uint32_t* count, XrReferenceSpaceType* spaces) {
    const auto& offered = rt().spaces;
    *count = static_cast<uint32_t>(offered.size());
    for (uint32_t i = 0; i < capacity && i < offered.size(); ++i) {
        spaces[i] = offered[i];
    }
    return XR_SUCCESS;
}

XrResult XRAPI_CALL create_reference_space(XrSession /*session*/,
                                           const XrReferenceSpaceCreateInfo* info, XrSpace* space) {
    Runtime& r = rt();
    bool offered = false;
    for (const XrReferenceSpaceType type : r.spaces) {
        offered = offered || type == info->referenceSpaceType;
    }
    if (!offered) {
        return XR_ERROR_REFERENCE_SPACE_UNSUPPORTED;
    }
    const std::uint64_t id = r.next_id++;
    r.space_objects[id] = {.kind = SpaceObject::Kind::Reference,
                           .type = info->referenceSpaceType,
                           .action = 0,
                           .subaction = XR_NULL_PATH,
                           .offset = from_xr(info->poseInReferenceSpace)};
    *space = handle<XrSpace>(id);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL get_bounds(XrSession /*session*/, XrReferenceSpaceType type,
                               XrExtent2Df* bounds) {
    if (type != XR_REFERENCE_SPACE_TYPE_STAGE) {
        *bounds = {0.0F, 0.0F};
        return XR_SPACE_BOUNDS_UNAVAILABLE;
    }
    *bounds = {rt().bounds_x, rt().bounds_z};
    return XR_SUCCESS;
}

XrResult XRAPI_CALL create_action_space(XrSession /*session*/, const XrActionSpaceCreateInfo* info,
                                        XrSpace* space) {
    Runtime& r = rt();
    const std::uint64_t id = r.next_id++;
    r.space_objects[id] = {.kind = SpaceObject::Kind::Action,
                           .type = XR_REFERENCE_SPACE_TYPE_STAGE,
                           .action = id_of(info->action),
                           .subaction = info->subactionPath,
                           .offset = from_xr(info->poseInActionSpace)};
    *space = handle<XrSpace>(id);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL locate_space(XrSpace space, XrSpace base, XrTime /*time*/,
                                 XrSpaceLocation* location) {
    Pose pose;
    Pose base_pose;
    location->locationFlags = 0;
    location->pose = to_xr({});
    if (!space_pose(id_of(base), base_pose) || !space_pose(id_of(space), pose)) {
        return XR_SUCCESS;
    }
    location->pose = to_xr(compose(inverse(base_pose), pose));
    location->locationFlags =
        XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT |
        XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
    if (location->next != nullptr) {
        auto* velocity = static_cast<XrSpaceVelocity*>(location->next);
        if (velocity->type == XR_TYPE_SPACE_VELOCITY) {
            velocity->velocityFlags = 0;
        }
    }
    return XR_SUCCESS;
}

XrResult XRAPI_CALL destroy_space(XrSpace space) {
    rt().space_objects.erase(id_of(space));
    return XR_SUCCESS;
}

XrResult XRAPI_CALL wait_frame(XrSession /*session*/, const XrFrameWaitInfo* /*info*/,
                               XrFrameState* state) {
    Runtime& r = rt();
    if (!r.running) {
        return XR_ERROR_SESSION_NOT_RUNNING;
    }
    ++r.frame;
    apply_changes();
    state->predictedDisplayTime = static_cast<XrTime>(r.frame) * 11111111;
    state->predictedDisplayPeriod = 11111111;
    state->shouldRender = r.state == XR_SESSION_STATE_VISIBLE || r.state == XR_SESSION_STATE_FOCUSED
                              ? XR_TRUE
                              : XR_FALSE;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL begin_frame(XrSession /*session*/, const XrFrameBeginInfo* /*info*/) {
    return rt().running ? XR_SUCCESS : XR_ERROR_SESSION_NOT_RUNNING;
}

// A Vulkan function of the application's instance or device.
template <typename F>
F vk(const char* name) {
    return reinterpret_cast<F>(rt().vulkan.procs(rt().vulkan.instance, name));
}

XrResult XRAPI_CALL vulkan_requirements(XrInstance /*instance*/, XrSystemId /*system*/,
                                        XrGraphicsRequirementsVulkan2KHR* requirements) {
    requirements->minApiVersionSupported = XR_MAKE_VERSION(1, 0, 0);
    requirements->maxApiVersionSupported = XR_MAKE_VERSION(1, 3, 0);
    rt().vulkan.requirements = true;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL create_vulkan_instance(XrInstance /*instance*/,
                                           const XrVulkanInstanceCreateInfoKHR* info,
                                           VkInstance* instance, VkResult* result) {
    Runtime& r = rt();
    if (!r.vulkan.requirements) {
        return XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING;
    }
    const auto create = reinterpret_cast<PFN_vkCreateInstance>(
        info->pfnGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance"));
    *result = create(info->vulkanCreateInfo, info->vulkanAllocator, instance);
    if (*result == VK_SUCCESS) {
        r.vulkan.procs = info->pfnGetInstanceProcAddr;
        r.vulkan.instance = *instance;
        log_line("vulkan instance");
    }
    return XR_SUCCESS;
}

// The first GPU, as a runtime names the one its headset is on.
XrResult XRAPI_CALL vulkan_graphics_device(XrInstance /*instance*/,
                                           const XrVulkanGraphicsDeviceGetInfoKHR* info,
                                           VkPhysicalDevice* physical) {
    Runtime& r = rt();
    if (r.vulkan.procs == nullptr || info->vulkanInstance != r.vulkan.instance) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    const auto enumerate = vk<PFN_vkEnumeratePhysicalDevices>("vkEnumeratePhysicalDevices");
    std::uint32_t count = 1;
    VkPhysicalDevice first = VK_NULL_HANDLE;
    const VkResult listed = enumerate(r.vulkan.instance, &count, &first);
    if ((listed != VK_SUCCESS && listed != VK_INCOMPLETE) || count == 0) {
        return XR_ERROR_RUNTIME_FAILURE;
    }
    *physical = first;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL create_vulkan_device(XrInstance /*instance*/,
                                         const XrVulkanDeviceCreateInfoKHR* info, VkDevice* device,
                                         VkResult* result) {
    Runtime& r = rt();
    if (r.vulkan.procs == nullptr) {
        return XR_ERROR_VALIDATION_FAILURE;
    }
    const auto create = vk<PFN_vkCreateDevice>("vkCreateDevice");
    *result =
        create(info->vulkanPhysicalDevice, info->vulkanCreateInfo, info->vulkanAllocator, device);
    if (*result == VK_SUCCESS) {
        log_line("vulkan device");
    }
    return XR_SUCCESS;
}

// A memory type of bits with the properties, or the first of bits.
std::uint32_t memory_type(std::uint32_t bits, VkMemoryPropertyFlags wanted) {
    const auto properties =
        vk<PFN_vkGetPhysicalDeviceMemoryProperties>("vkGetPhysicalDeviceMemoryProperties");
    VkPhysicalDeviceMemoryProperties memory{};
    properties(rt().vulkan.physical, &memory);
    std::uint32_t fallback = UINT32_MAX;
    for (std::uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
        if ((bits & (1U << i)) == 0) {
            continue;
        }
        if ((memory.memoryTypes[i].propertyFlags & wanted) == wanted) {
            return i;
        }
        fallback = std::min(fallback, i);
    }
    return fallback;
}

// Records commands into a command buffer of its own and waits until the queue has run them.
template <typename Record>
bool run_commands(const Record& record) {
    const Vulkan& v = rt().vulkan;
    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.queueFamilyIndex = v.family;
    VkCommandPool pool = VK_NULL_HANDLE;
    if (vk<PFN_vkCreateCommandPool>("vkCreateCommandPool")(v.device, &pool_info, nullptr, &pool) !=
        VK_SUCCESS) {
        return false;
    }
    VkCommandBufferAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocate.commandPool = pool;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    bool ran = vk<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(
                   v.device, &allocate, &commands) == VK_SUCCESS;
    if (ran) {
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vk<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(commands, &begin);
        record(commands);
        vk<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(commands);
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &commands;
        ran = vk<PFN_vkQueueSubmit>("vkQueueSubmit")(v.queue, 1, &submit, VK_NULL_HANDLE) ==
                  VK_SUCCESS &&
              vk<PFN_vkQueueWaitIdle>("vkQueueWaitIdle")(v.queue) == VK_SUCCESS;
    }
    vk<PFN_vkDestroyCommandPool>("vkDestroyCommandPool")(v.device, pool, nullptr);
    return ran;
}

void move_image(VkCommandBuffer commands, VkImage image, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vk<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(
        commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
        nullptr, 0, nullptr, 1, &barrier);
}

// B8G8R8A8_SRGB first, which the application can't copy into, then the RGBA8 formats.
constexpr std::array<std::int64_t, 3> kSwapchainFormats{
    VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM};

XrResult XRAPI_CALL enumerate_swapchain_formats(XrSession /*session*/, uint32_t capacity,
                                                uint32_t* count, int64_t* formats) {
    if (rt().vulkan.device == VK_NULL_HANDLE) {
        *count = 0;
        return XR_SUCCESS;
    }
    *count = static_cast<uint32_t>(kSwapchainFormats.size());
    if (capacity == 0) {
        return XR_SUCCESS;
    }
    if (capacity < kSwapchainFormats.size()) {
        return XR_ERROR_SIZE_INSUFFICIENT;
    }
    std::copy(kSwapchainFormats.begin(), kSwapchainFormats.end(), formats);
    return XR_SUCCESS;
}

void free_swapchain(SwapchainObject& swapchain) {
    const VkDevice device = rt().vulkan.device;
    vk<PFN_vkDeviceWaitIdle>("vkDeviceWaitIdle")(device);
    for (std::size_t i = 0; i < swapchain.images.size(); ++i) {
        vk<PFN_vkDestroyImage>("vkDestroyImage")(device, swapchain.images[i], nullptr);
        vk<PFN_vkFreeMemory>("vkFreeMemory")(device, swapchain.memory[i], nullptr);
    }
    swapchain.images.clear();
    swapchain.memory.clear();
}

// Three images in the layout the application draws to, as runtimes hand them out.
XrResult XRAPI_CALL create_swapchain(XrSession /*session*/, const XrSwapchainCreateInfo* info,
                                     XrSwapchain* made) {
    Runtime& r = rt();
    if (r.vulkan.device == VK_NULL_HANDLE) {
        return XR_ERROR_FUNCTION_UNSUPPORTED;
    }
    if (std::find(kSwapchainFormats.begin(), kSwapchainFormats.end(), info->format) ==
        kSwapchainFormats.end()) {
        return XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
    }
    SwapchainObject swapchain;
    swapchain.format = info->format;
    swapchain.width = info->width;
    swapchain.height = info->height;
    const VkDevice device = r.vulkan.device;
    for (int i = 0; i < 3; ++i) {
        VkImageCreateInfo image_info{};
        image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image_info.imageType = VK_IMAGE_TYPE_2D;
        image_info.format = static_cast<VkFormat>(info->format);
        image_info.extent = {info->width, info->height, 1};
        image_info.mipLevels = 1;
        image_info.arrayLayers = 1;
        image_info.samples = VK_SAMPLE_COUNT_1_BIT;
        image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                           VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImage image = VK_NULL_HANDLE;
        if (vk<PFN_vkCreateImage>("vkCreateImage")(device, &image_info, nullptr, &image) !=
            VK_SUCCESS) {
            free_swapchain(swapchain);
            return XR_ERROR_RUNTIME_FAILURE;
        }
        VkMemoryRequirements needs{};
        vk<PFN_vkGetImageMemoryRequirements>("vkGetImageMemoryRequirements")(device, image, &needs);
        VkMemoryAllocateInfo allocate{};
        allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocate.allocationSize = needs.size;
        allocate.memoryTypeIndex =
            memory_type(needs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkDeviceMemory memory = VK_NULL_HANDLE;
        if (vk<PFN_vkAllocateMemory>("vkAllocateMemory")(device, &allocate, nullptr, &memory) !=
                VK_SUCCESS ||
            vk<PFN_vkBindImageMemory>("vkBindImageMemory")(device, image, memory, 0) !=
                VK_SUCCESS) {
            vk<PFN_vkDestroyImage>("vkDestroyImage")(device, image, nullptr);
            free_swapchain(swapchain);
            return XR_ERROR_RUNTIME_FAILURE;
        }
        swapchain.images.push_back(image);
        swapchain.memory.push_back(memory);
    }
    if (!run_commands([&](VkCommandBuffer commands) {
            for (VkImage image : swapchain.images) {
                move_image(commands, image, VK_IMAGE_LAYOUT_UNDEFINED,
                           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            }
        })) {
        free_swapchain(swapchain);
        return XR_ERROR_RUNTIME_FAILURE;
    }
    const std::uint64_t id = r.next_id++;
    r.swapchains[id] = std::move(swapchain);
    *made = handle<XrSwapchain>(id);
    log_line("swapchain " + std::to_string(info->width) + "x" + std::to_string(info->height) +
             " format " + std::to_string(info->format));
    return XR_SUCCESS;
}

XrResult XRAPI_CALL destroy_swapchain(XrSwapchain made) {
    Runtime& r = rt();
    const auto found = r.swapchains.find(id_of(made));
    if (found == r.swapchains.end()) {
        return XR_ERROR_HANDLE_INVALID;
    }
    free_swapchain(found->second);
    r.swapchains.erase(found);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL enumerate_swapchain_images(XrSwapchain made, uint32_t capacity, uint32_t* count,
                                               XrSwapchainImageBaseHeader* images) {
    const auto found = rt().swapchains.find(id_of(made));
    if (found == rt().swapchains.end()) {
        return XR_ERROR_HANDLE_INVALID;
    }
    const std::vector<VkImage>& own = found->second.images;
    *count = static_cast<uint32_t>(own.size());
    if (capacity == 0) {
        return XR_SUCCESS;
    }
    if (capacity < own.size() || images->type != XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR) {
        return capacity < own.size() ? XR_ERROR_SIZE_INSUFFICIENT : XR_ERROR_VALIDATION_FAILURE;
    }
    auto* vulkan = reinterpret_cast<XrSwapchainImageVulkan2KHR*>(images);
    for (std::size_t i = 0; i < own.size(); ++i) {
        vulkan[i].image = own[i];
    }
    return XR_SUCCESS;
}

XrResult XRAPI_CALL acquire_swapchain_image(XrSwapchain made,
                                            const XrSwapchainImageAcquireInfo* /*info*/,
                                            uint32_t* index) {
    const auto found = rt().swapchains.find(id_of(made));
    if (found == rt().swapchains.end()) {
        return XR_ERROR_HANDLE_INVALID;
    }
    SwapchainObject& swapchain = found->second;
    *index = swapchain.next;
    swapchain.acquired.push_back(swapchain.next);
    swapchain.next = (swapchain.next + 1) % static_cast<std::uint32_t>(swapchain.images.size());
    return XR_SUCCESS;
}

XrResult XRAPI_CALL wait_swapchain_image(XrSwapchain made,
                                         const XrSwapchainImageWaitInfo* /*info*/) {
    const auto found = rt().swapchains.find(id_of(made));
    if (found == rt().swapchains.end()) {
        return XR_ERROR_HANDLE_INVALID;
    }
    return found->second.acquired.empty() ? XR_ERROR_CALL_ORDER_INVALID : XR_SUCCESS;
}

XrResult XRAPI_CALL release_swapchain_image(XrSwapchain made,
                                            const XrSwapchainImageReleaseInfo* /*info*/) {
    const auto found = rt().swapchains.find(id_of(made));
    if (found == rt().swapchains.end()) {
        return XR_ERROR_HANDLE_INVALID;
    }
    SwapchainObject& swapchain = found->second;
    if (swapchain.acquired.empty()) {
        return XR_ERROR_CALL_ORDER_INVALID;
    }
    swapchain.released = swapchain.acquired.front();
    swapchain.acquired.pop_front();
    return XR_SUCCESS;
}

// Reads the probed pixels of a swapchain's last released image and logs them as "eye N x y r g
// b a".
void log_probes(std::uint32_t eye, const SwapchainObject& swapchain) {
    const Vulkan& v = rt().vulkan;
    if (swapchain.released < 0 || rt().probes.empty()) {
        return;
    }
    const VkImage image = swapchain.images.at(static_cast<std::size_t>(swapchain.released));
    const VkDeviceSize bytes = VkDeviceSize{swapchain.width} * swapchain.height * 4;
    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = bytes;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer buffer = VK_NULL_HANDLE;
    if (vk<PFN_vkCreateBuffer>("vkCreateBuffer")(v.device, &buffer_info, nullptr, &buffer) !=
        VK_SUCCESS) {
        log_line("eye " + std::to_string(eye) + " unreadable");
        return;
    }
    VkMemoryRequirements needs{};
    vk<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(v.device, buffer,
                                                                           &needs);
    VkMemoryAllocateInfo allocate{};
    allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate.allocationSize = needs.size;
    allocate.memoryTypeIndex =
        memory_type(needs.memoryTypeBits,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    const bool ready =
        vk<PFN_vkAllocateMemory>("vkAllocateMemory")(v.device, &allocate, nullptr, &memory) ==
            VK_SUCCESS &&
        vk<PFN_vkBindBufferMemory>("vkBindBufferMemory")(v.device, buffer, memory, 0) ==
            VK_SUCCESS &&
        run_commands([&](VkCommandBuffer commands) {
            move_image(commands, image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {swapchain.width, swapchain.height, 1};
            vk<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(
                commands, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
            move_image(commands, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        }) &&
        vk<PFN_vkMapMemory>("vkMapMemory")(v.device, memory, 0, bytes, 0, &mapped) == VK_SUCCESS;
    if (ready) {
        const auto* pixels = static_cast<const std::uint8_t*>(mapped);
        const bool bgra = swapchain.format == VK_FORMAT_B8G8R8A8_SRGB;
        for (const auto& [x, y] : rt().probes) {
            if (x >= swapchain.width || y >= swapchain.height) {
                continue;
            }
            const std::uint8_t* p = pixels + ((std::size_t{y} * swapchain.width) + x) * 4;
            const int red = bgra ? p[2] : p[0];
            const int blue = bgra ? p[0] : p[2];
            log_line("eye " + std::to_string(eye) + " " + std::to_string(x) + " " +
                     std::to_string(y) + " " + std::to_string(red) + " " + std::to_string(p[1]) +
                     " " + std::to_string(blue) + " " + std::to_string(p[3]));
        }
        vk<PFN_vkUnmapMemory>("vkUnmapMemory")(v.device, memory);
    } else {
        log_line("eye " + std::to_string(eye) + " unreadable");
    }
    vk<PFN_vkDestroyBuffer>("vkDestroyBuffer")(v.device, buffer, nullptr);
    if (memory != VK_NULL_HANDLE) {
        vk<PFN_vkFreeMemory>("vkFreeMemory")(v.device, memory, nullptr);
    }
}

XrResult XRAPI_CALL end_frame(XrSession /*session*/, const XrFrameEndInfo* info) {
    log_line("end frame " + std::to_string(rt().frame) + " layers " +
             std::to_string(info->layerCount));
    for (uint32_t i = 0; i < info->layerCount; ++i) {
        if (info->layers[i]->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
            continue;
        }
        const auto* layer = reinterpret_cast<const XrCompositionLayerProjection*>(info->layers[i]);
        for (uint32_t eye = 0; eye < layer->viewCount; ++eye) {
            const XrSwapchainSubImage& image = layer->views[eye].subImage;
            const auto found = rt().swapchains.find(id_of(image.swapchain));
            if (found == rt().swapchains.end() || image.imageRect.extent.width <= 0 ||
                !found->second.acquired.empty()) {
                return XR_ERROR_LAYER_INVALID;
            }
            log_probes(eye, found->second);
        }
    }
    return XR_SUCCESS;
}

XrResult XRAPI_CALL locate_views(XrSession /*session*/, const XrViewLocateInfo* info,
                                 XrViewState* state, uint32_t capacity, uint32_t* count,
                                 XrView* views) {
    *count = 2;
    if (capacity < 2) {
        return capacity == 0 ? XR_SUCCESS : XR_ERROR_SIZE_INSUFFICIENT;
    }
    Pose base;
    if (!space_pose(id_of(info->space), base)) {
        state->viewStateFlags = 0;
        return XR_SUCCESS;
    }
    // Eyes 64 mm apart, each seeing 50 degrees out, 40 in and 45 up and down.
    const Pose head = compose(rt().stage_shift, rt().head);
    for (uint32_t eye = 0; eye < 2; ++eye) {
        const Pose offset{{}, {eye == 0 ? -0.032F : 0.032F, 0.0F, 0.0F}};
        views[eye].pose = to_xr(compose(inverse(base), compose(head, offset)));
        const float outer = 50.0F * 3.14159265F / 180.0F;
        const float inner = 40.0F * 3.14159265F / 180.0F;
        const float vertical = 45.0F * 3.14159265F / 180.0F;
        views[eye].fov = eye == 0 ? XrFovf{-outer, inner, vertical, -vertical}
                                  : XrFovf{-inner, outer, vertical, -vertical};
    }
    state->viewStateFlags = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT |
                            XR_VIEW_STATE_ORIENTATION_TRACKED_BIT |
                            XR_VIEW_STATE_POSITION_TRACKED_BIT;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL string_to_path(XrInstance /*instance*/, const char* text, XrPath* path) {
    *path = path_of(text);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL path_to_string(XrInstance /*instance*/, XrPath path, uint32_t capacity,
                                   uint32_t* count, char* buffer) {
    if (path == XR_NULL_PATH || path >= rt().paths.size()) {
        return XR_ERROR_PATH_INVALID;
    }
    const std::string& text = rt().paths[path];
    *count = static_cast<uint32_t>(text.size() + 1);
    if (capacity == 0) {
        return XR_SUCCESS;
    }
    if (capacity < text.size() + 1) {
        return XR_ERROR_SIZE_INSUFFICIENT;
    }
    std::memcpy(buffer, text.c_str(), text.size() + 1);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL create_action_set(XrInstance /*instance*/, const XrActionSetCreateInfo* info,
                                      XrActionSet* set) {
    const std::uint64_t id = rt().next_id++;
    rt().action_sets[id] = info->actionSetName;
    *set = handle<XrActionSet>(id);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL destroy_action_set(XrActionSet set) {
    rt().action_sets.erase(id_of(set));
    return XR_SUCCESS;
}

XrResult XRAPI_CALL create_action(XrActionSet set, const XrActionCreateInfo* info,
                                  XrAction* action) {
    const std::uint64_t id = rt().next_id++;
    rt().actions[id] = {.set = id_of(set), .type = info->actionType, .name = info->actionName};
    *action = handle<XrAction>(id);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL destroy_action(XrAction action) {
    rt().actions.erase(id_of(action));
    return XR_SUCCESS;
}

XrResult XRAPI_CALL suggest_bindings(XrInstance /*instance*/,
                                     const XrInteractionProfileSuggestedBinding* suggested) {
    Runtime& r = rt();
    const std::string& profile = r.paths[suggested->interactionProfile];
    auto& bindings = r.suggested[suggested->interactionProfile];
    bindings.clear();
    for (uint32_t i = 0; i < suggested->countSuggestedBindings; ++i) {
        bindings.emplace_back(id_of(suggested->suggestedBindings[i].action),
                              suggested->suggestedBindings[i].binding);
    }
    log_line("suggest " + profile + " " + std::to_string(bindings.size()));
    return XR_SUCCESS;
}

XrResult XRAPI_CALL attach_action_sets(XrSession /*session*/,
                                       const XrSessionActionSetsAttachInfo* /*info*/) {
    Runtime& r = rt();
    r.attached = true;
    XrEventDataBuffer buffer{};
    auto& event = reinterpret_cast<XrEventDataInteractionProfileChanged&>(buffer);
    event.type = XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED;
    event.session = handle<XrSession>(r.session_id);
    r.events.push_back(buffer);
    log_line("attach");
    return XR_SUCCESS;
}

XrResult XRAPI_CALL get_current_profile(XrSession /*session*/, XrPath /*toplevel*/,
                                        XrInteractionProfileState* state) {
    Runtime& r = rt();
    const XrPath profile = path_of(r.profile);
    state->interactionProfile =
        r.attached && r.suggested.contains(profile) ? profile : XR_NULL_PATH;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL sync_actions(XrSession /*session*/, const XrActionsSyncInfo* /*info*/) {
    return rt().state == XR_SESSION_STATE_FOCUSED ? XR_SUCCESS : XR_SESSION_NOT_FOCUSED;
}

XrResult XRAPI_CALL get_boolean(XrSession /*session*/, const XrActionStateGetInfo* info,
                                XrActionStateBoolean* state) {
    const Value* value = action_value(id_of(info->action), info->subactionPath);
    state->isActive = value != nullptr ? XR_TRUE : XR_FALSE;
    // A number bound to a button is pressed past half way, as runtimes convert them.
    state->currentState = value != nullptr && value->a > 0.5F ? XR_TRUE : XR_FALSE;
    state->changedSinceLastSync = XR_FALSE;
    state->lastChangeTime = 0;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL get_float(XrSession /*session*/, const XrActionStateGetInfo* info,
                              XrActionStateFloat* state) {
    const Value* value = action_value(id_of(info->action), info->subactionPath);
    state->isActive = value != nullptr ? XR_TRUE : XR_FALSE;
    state->currentState = value != nullptr ? value->a : 0.0F;
    state->changedSinceLastSync = XR_FALSE;
    state->lastChangeTime = 0;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL get_vector2(XrSession /*session*/, const XrActionStateGetInfo* info,
                                XrActionStateVector2f* state) {
    const Value* value = action_value(id_of(info->action), info->subactionPath);
    state->isActive = value != nullptr ? XR_TRUE : XR_FALSE;
    state->currentState = value != nullptr ? XrVector2f{value->a, value->b} : XrVector2f{0, 0};
    state->changedSinceLastSync = XR_FALSE;
    state->lastChangeTime = 0;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL get_pose(XrSession /*session*/, const XrActionStateGetInfo* info,
                             XrActionStatePose* state) {
    state->isActive =
        action_value(id_of(info->action), info->subactionPath) != nullptr ? XR_TRUE : XR_FALSE;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL apply_haptic(XrSession /*session*/, const XrHapticActionInfo* info,
                                 const XrHapticBaseHeader* haptic) {
    const auto* vibration = reinterpret_cast<const XrHapticVibration*>(haptic);
    std::ostringstream line;
    line << "haptic " << bound_path(id_of(info->action), info->subactionPath) << " "
         << vibration->duration << " " << vibration->frequency << " " << vibration->amplitude;
    log_line(line.str());
    return XR_SUCCESS;
}

XrResult XRAPI_CALL stop_haptic(XrSession /*session*/, const XrHapticActionInfo* /*info*/) {
    return XR_SUCCESS;
}

XrResult XRAPI_CALL get_instance_proc_addr(XrInstance instance, const char* name,
                                           PFN_xrVoidFunction* function) {
    static const std::map<std::string, PFN_xrVoidFunction> kFunctions{
        {"xrGetInstanceProcAddr", reinterpret_cast<PFN_xrVoidFunction>(&get_instance_proc_addr)},
        {"xrEnumerateInstanceExtensionProperties",
         reinterpret_cast<PFN_xrVoidFunction>(&enumerate_extensions)},
        {"xrCreateInstance", reinterpret_cast<PFN_xrVoidFunction>(&create_instance)},
        {"xrDestroyInstance", reinterpret_cast<PFN_xrVoidFunction>(&destroy_instance)},
        {"xrGetInstanceProperties", reinterpret_cast<PFN_xrVoidFunction>(&get_instance_properties)},
        {"xrPollEvent", reinterpret_cast<PFN_xrVoidFunction>(&poll_event)},
        {"xrResultToString", reinterpret_cast<PFN_xrVoidFunction>(&result_to_string)},
        {"xrStructureTypeToString",
         reinterpret_cast<PFN_xrVoidFunction>(&structure_type_to_string)},
        {"xrGetSystem", reinterpret_cast<PFN_xrVoidFunction>(&get_system)},
        {"xrGetSystemProperties", reinterpret_cast<PFN_xrVoidFunction>(&get_system_properties)},
        {"xrEnumerateViewConfigurations",
         reinterpret_cast<PFN_xrVoidFunction>(&enumerate_view_configurations)},
        {"xrGetViewConfigurationProperties",
         reinterpret_cast<PFN_xrVoidFunction>(&get_view_configuration_properties)},
        {"xrEnumerateViewConfigurationViews",
         reinterpret_cast<PFN_xrVoidFunction>(&enumerate_view_configuration_views)},
        {"xrEnumerateEnvironmentBlendModes",
         reinterpret_cast<PFN_xrVoidFunction>(&enumerate_blend_modes)},
        {"xrCreateSession", reinterpret_cast<PFN_xrVoidFunction>(&create_session)},
        {"xrDestroySession", reinterpret_cast<PFN_xrVoidFunction>(&destroy_session)},
        {"xrBeginSession", reinterpret_cast<PFN_xrVoidFunction>(&begin_session)},
        {"xrEndSession", reinterpret_cast<PFN_xrVoidFunction>(&end_session)},
        {"xrRequestExitSession", reinterpret_cast<PFN_xrVoidFunction>(&request_exit_session)},
        {"xrEnumerateReferenceSpaces",
         reinterpret_cast<PFN_xrVoidFunction>(&enumerate_reference_spaces)},
        {"xrCreateReferenceSpace", reinterpret_cast<PFN_xrVoidFunction>(&create_reference_space)},
        {"xrGetReferenceSpaceBoundsRect", reinterpret_cast<PFN_xrVoidFunction>(&get_bounds)},
        {"xrCreateActionSpace", reinterpret_cast<PFN_xrVoidFunction>(&create_action_space)},
        {"xrLocateSpace", reinterpret_cast<PFN_xrVoidFunction>(&locate_space)},
        {"xrDestroySpace", reinterpret_cast<PFN_xrVoidFunction>(&destroy_space)},
        {"xrWaitFrame", reinterpret_cast<PFN_xrVoidFunction>(&wait_frame)},
        {"xrBeginFrame", reinterpret_cast<PFN_xrVoidFunction>(&begin_frame)},
        {"xrEndFrame", reinterpret_cast<PFN_xrVoidFunction>(&end_frame)},
        {"xrLocateViews", reinterpret_cast<PFN_xrVoidFunction>(&locate_views)},
        {"xrStringToPath", reinterpret_cast<PFN_xrVoidFunction>(&string_to_path)},
        {"xrPathToString", reinterpret_cast<PFN_xrVoidFunction>(&path_to_string)},
        {"xrCreateActionSet", reinterpret_cast<PFN_xrVoidFunction>(&create_action_set)},
        {"xrDestroyActionSet", reinterpret_cast<PFN_xrVoidFunction>(&destroy_action_set)},
        {"xrCreateAction", reinterpret_cast<PFN_xrVoidFunction>(&create_action)},
        {"xrDestroyAction", reinterpret_cast<PFN_xrVoidFunction>(&destroy_action)},
        {"xrSuggestInteractionProfileBindings",
         reinterpret_cast<PFN_xrVoidFunction>(&suggest_bindings)},
        {"xrAttachSessionActionSets", reinterpret_cast<PFN_xrVoidFunction>(&attach_action_sets)},
        {"xrGetCurrentInteractionProfile",
         reinterpret_cast<PFN_xrVoidFunction>(&get_current_profile)},
        {"xrSyncActions", reinterpret_cast<PFN_xrVoidFunction>(&sync_actions)},
        {"xrGetActionStateBoolean", reinterpret_cast<PFN_xrVoidFunction>(&get_boolean)},
        {"xrGetActionStateFloat", reinterpret_cast<PFN_xrVoidFunction>(&get_float)},
        {"xrGetActionStateVector2f", reinterpret_cast<PFN_xrVoidFunction>(&get_vector2)},
        {"xrGetActionStatePose", reinterpret_cast<PFN_xrVoidFunction>(&get_pose)},
        {"xrApplyHapticFeedback", reinterpret_cast<PFN_xrVoidFunction>(&apply_haptic)},
        {"xrStopHapticFeedback", reinterpret_cast<PFN_xrVoidFunction>(&stop_haptic)},
        {"xrGetVulkanGraphicsRequirements2KHR",
         reinterpret_cast<PFN_xrVoidFunction>(&vulkan_requirements)},
        {"xrCreateVulkanInstanceKHR",
         reinterpret_cast<PFN_xrVoidFunction>(&create_vulkan_instance)},
        {"xrGetVulkanGraphicsDevice2KHR",
         reinterpret_cast<PFN_xrVoidFunction>(&vulkan_graphics_device)},
        {"xrCreateVulkanDeviceKHR", reinterpret_cast<PFN_xrVoidFunction>(&create_vulkan_device)},
        {"xrEnumerateSwapchainFormats",
         reinterpret_cast<PFN_xrVoidFunction>(&enumerate_swapchain_formats)},
        {"xrCreateSwapchain", reinterpret_cast<PFN_xrVoidFunction>(&create_swapchain)},
        {"xrDestroySwapchain", reinterpret_cast<PFN_xrVoidFunction>(&destroy_swapchain)},
        {"xrEnumerateSwapchainImages",
         reinterpret_cast<PFN_xrVoidFunction>(&enumerate_swapchain_images)},
        {"xrAcquireSwapchainImage", reinterpret_cast<PFN_xrVoidFunction>(&acquire_swapchain_image)},
        {"xrWaitSwapchainImage", reinterpret_cast<PFN_xrVoidFunction>(&wait_swapchain_image)},
        {"xrReleaseSwapchainImage", reinterpret_cast<PFN_xrVoidFunction>(&release_swapchain_image)},
    };
    const auto found = kFunctions.find(name);
    // Before an instance only these three may be asked for.
    const bool before =
        found != kFunctions.end() &&
        (found->first == "xrEnumerateInstanceExtensionProperties" ||
         found->first == "xrCreateInstance" || found->first == "xrGetInstanceProcAddr");
    if (found == kFunctions.end() || (instance == XR_NULL_HANDLE && !before)) {
        *function = nullptr;
        return found == kFunctions.end() ? XR_ERROR_FUNCTION_UNSUPPORTED : XR_ERROR_HANDLE_INVALID;
    }
    *function = found->second;
    return XR_SUCCESS;
}

} // namespace

FAKE_EXPORT XrResult XRAPI_CALL xrNegotiateLoaderRuntimeInterface(
    const XrNegotiateLoaderInfo* loader, XrNegotiateRuntimeRequest* request) {
    if (loader == nullptr || request == nullptr ||
        loader->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
        request->structType != XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST ||
        loader->minInterfaceVersion > XR_CURRENT_LOADER_RUNTIME_VERSION ||
        loader->maxInterfaceVersion < XR_CURRENT_LOADER_RUNTIME_VERSION) {
        return XR_ERROR_INITIALIZATION_FAILED;
    }
    request->runtimeInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
    request->runtimeApiVersion = XR_API_VERSION_1_1;
    request->getInstanceProcAddr = &get_instance_proc_addr;
    return XR_SUCCESS;
}
