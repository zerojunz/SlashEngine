// A stand-in OpenXR runtime, so the OpenXR interface's tests and the openxr-runtime example run
// without a headset. The Khronos loader finds it through the manifest xr::set_openxr_runtime (or
// XR_RUNTIME_JSON) names, as it would any runtime. It is a headless one (XR_MND_headless): a head
// and two controllers moved by a script, the file FAKE_OPENXR_SCRIPT names, one line a command:
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
// Frames are counted by xrWaitFrame from 1; a value holds until a later frame changes it. What
// the application did goes to the file FAKE_OPENXR_LOG names, a line a call: instance, session
// begin and end, suggested bindings, haptics and ended frames.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>
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

// An environment variable, or "" when it isn't set.
std::string environment(const char* name) {
#if defined(_MSC_VER)
    // MSVC deprecates getenv.
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) {
        return {};
    }
    std::string out(value);
    std::free(value);
    return out;
#else
    const char* value = std::getenv(name); // NOLINT(concurrency-mt-unsafe): read at load
    return value == nullptr ? std::string() : std::string(value);
#endif
}

// Copies text into a fixed buffer of size chars, cut short if it must be, always ended by a
// null (MSVC deprecates strncpy).
void copy_text(char* out, std::size_t size, const std::string& text) {
    const std::size_t length = std::min(text.size(), size - 1);
    std::memcpy(out, text.data(), length);
    out[length] = '\0';
}

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
    r.log_path = environment("FAKE_OPENXR_LOG");
    const std::string path = environment("FAKE_OPENXR_SCRIPT");
    if (path.empty()) {
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
        copy_text(properties[i].extensionName, XR_MAX_EXTENSION_NAME_SIZE, names[i]);
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
    copy_text(properties->runtimeName, XR_MAX_RUNTIME_NAME_SIZE, "SlashEngine stand-in");
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
    copy_text(buffer, XR_MAX_RESULT_STRING_SIZE, text);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL structure_type_to_string(XrInstance /*instance*/, XrStructureType value,
                                             char buffer[XR_MAX_STRUCTURE_NAME_SIZE]) {
    const std::string text = "XR_UNKNOWN_STRUCTURE_TYPE_" + std::to_string(static_cast<int>(value));
    copy_text(buffer, XR_MAX_STRUCTURE_NAME_SIZE, text);
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
    copy_text(properties->systemName, XR_MAX_SYSTEM_NAME_SIZE, "Stand-in headset");
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
        views[i].recommendedImageRectWidth = 1024;
        views[i].maxImageRectWidth = 4096;
        views[i].recommendedImageRectHeight = 1024;
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
    if (info->next != nullptr || !r.enabled.contains("XR_MND_headless")) {
        return XR_ERROR_GRAPHICS_DEVICE_INVALID; // a stand-in draws nothing
    }
    r.session = true;
    r.session_id = r.next_id++;
    *session = handle<XrSession>(r.session_id);
    r.state = XR_SESSION_STATE_IDLE;
    queue_state(XR_SESSION_STATE_IDLE);
    r.state = XR_SESSION_STATE_READY;
    queue_state(XR_SESSION_STATE_READY);
    log_line("create session");
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

XrResult XRAPI_CALL end_frame(XrSession /*session*/, const XrFrameEndInfo* info) {
    log_line("end frame " + std::to_string(rt().frame) + " layers " +
             std::to_string(info->layerCount));
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
