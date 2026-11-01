#include "slash/command/xr.hpp"

#include "slash/command/session.hpp"
#include "slash/core/text.hpp"
#include "slash/world/tree3d.hpp"
#include "slash/xr/xr.hpp"

#include <array>
#include <string>
#include <utility>
#include <vector>

namespace slash::command {
namespace {

json::Value property(std::string_view type, std::string_view description) {
    json::Value schema;
    schema["type"] = type;
    schema["description"] = description;
    return schema;
}

json::Value numbers(std::size_t count, std::string_view description) {
    json::Value schema = property("array", description);
    schema["items"]["type"] = "number";
    schema["minItems"] = count;
    schema["maxItems"] = count;
    return schema;
}

json::Value choice(json::Array values, std::string_view description) {
    json::Value schema;
    schema["enum"] = std::move(values);
    schema["description"] = description;
    return schema;
}

json::Value object_schema(json::Object properties) {
    json::Value schema;
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["properties"] = std::move(properties);
    return schema;
}

json::Value pose_properties_schema(std::string_view what) {
    json::Object properties{
        {"position", numbers(3, "Meters in the play space.")},
        {"rotation", numbers(3, "Radians, as Transform3D's.")},
        {"linear_velocity", numbers(3, "Meters a second.")},
        {"angular_velocity", numbers(3, "Radians a second.")},
        {"tracking", property("boolean", "false loses its tracking.")},
        {"confidence", choice({"low", "high"}, "Default: high.")},
    };
    json::Value schema = object_schema(std::move(properties));
    schema["description"] = what;
    return schema;
}

json::Value tracker_schema() {
    json::Value poses;
    poses["type"] = "object";
    poses["description"] = "Poses by name.";
    poses["additionalProperties"] = pose_properties_schema("");
    json::Value input;
    input["description"] = "A button (boolean), trigger (number) or thumbstick ([x, y]).";
    input["type"] = json::Array{"boolean", "number", "array"};
    input["items"]["type"] = "number";
    input["minItems"] = 2;
    input["maxItems"] = 2;
    json::Value inputs;
    inputs["type"] = "object";
    inputs["description"] = "Inputs by action name, as trigger, grip, primary or ax_button.";
    inputs["additionalProperties"] = std::move(input);
    json::Object properties{
        {"type", choice({"head", "controller", "basestation", "anchor", "hand", "body", "face"},
                        "Adds a tracker of this type, or changes one's.")},
        {"hand", choice({"unknown", "left", "right"}, "")},
        {"profile", property("string", "The interaction profile.")},
        {"position", numbers(3, "Moves every pose, in meters in the play space.")},
        {"rotation", numbers(3, "Turns every pose, in radians as Transform3D's.")},
        {"tracking", property("boolean", "Every pose's.")},
        {"poses", std::move(poses)},
        {"inputs", std::move(inputs)},
        {"remove", property("boolean", "Takes the tracker away.")},
    };
    return object_schema(std::move(properties));
}

Reply param_error(int error_code, std::string_view path, const std::string& message) {
    json::Value data;
    data["path"] = path;
    return Reply::failure(error_code, std::string(path) + ": " + message, std::move(data));
}

std::string quoted(std::string_view name) {
    return "\"" + std::string(name) + "\"";
}

Vec3 vec3_of(const json::Value& value) {
    const json::Array& items = value.as_array();
    return {static_cast<float>(items[0].as_double()), static_cast<float>(items[1].as_double()),
            static_cast<float>(items[2].as_double())};
}

// Sets what params give of a pose: a position or rotation keeps the other.
void apply_pose(const json::Value& params, xr::TestPose& pose) {
    world::Transform3D transform;
    transform.position = pose.transform.origin;
    transform.rotation = world::euler(world::orthonormalized(pose.transform));
    bool moved = false;
    if (const json::Value* position = params.find("position")) {
        transform.position = vec3_of(*position);
        moved = true;
    }
    if (const json::Value* rotation = params.find("rotation")) {
        transform.rotation = vec3_of(*rotation);
        moved = true;
    }
    if (moved) {
        pose.transform = world::affine(transform);
    }
    if (const json::Value* velocity = params.find("linear_velocity")) {
        pose.linear_velocity = vec3_of(*velocity);
    }
    if (const json::Value* velocity = params.find("angular_velocity")) {
        pose.angular_velocity = vec3_of(*velocity);
    }
    if (const json::Value* tracking = params.find("tracking")) {
        pose.tracking = tracking->as_bool();
    }
    if (const json::Value* confidence = params.find("confidence")) {
        pose.confidence =
            confidence->as_string() == "low" ? xr::Confidence::Low : xr::Confidence::High;
    }
}

std::vector<std::string> names_of(const std::map<std::string, xr::TestTracker, std::less<>>& map) {
    std::vector<std::string> names;
    names.reserve(map.size());
    for (const auto& [name, tracker] : map) {
        names.push_back(name);
    }
    return names;
}

Reply set_tracker(xr::TestInterface& test, const std::string& name, const json::Value& params,
                  const std::string& at) {
    auto& trackers = test.trackers();
    auto found = trackers.find(name);
    if (const json::Value* remove = params.find("remove"); remove != nullptr && remove->as_bool()) {
        if (found != trackers.end()) {
            trackers.erase(found);
        }
        return Reply::success({});
    }
    if (found == trackers.end()) {
        const json::Value* type = params.find("type");
        if (type == nullptr) {
            std::string message =
                "the test headset has no tracker " + quoted(name) + "; give its type to add one";
            const std::vector<std::string> names = names_of(trackers);
            if (const std::string_view hint = closest_match(name, names); !hint.empty()) {
                message += ", or did you mean " + quoted(hint) + "?";
            }
            return param_error(code::kNotFound, at, message);
        }
        found = trackers.emplace(name, xr::TestTracker{}).first;
    }
    xr::TestTracker& tracker = found->second;
    // The schema allows only names these know.
    if (const json::Value* type = params.find("type")) {
        tracker.type = xr::tracker_type_named(type->as_string()).value_or(tracker.type);
    }
    if (const json::Value* hand = params.find("hand")) {
        tracker.hand = xr::hand_named(hand->as_string()).value_or(tracker.hand);
    }
    if (const json::Value* profile = params.find("profile")) {
        tracker.profile = profile->as_string();
    }
    if (tracker.poses.empty() &&
        (params.find("position") != nullptr || params.find("rotation") != nullptr)) {
        tracker.poses["default"];
    }
    for (auto& [pose_name, pose] : tracker.poses) {
        apply_pose(params, pose);
    }
    if (const json::Value* poses = params.find("poses")) {
        for (const auto& [pose_name, pose_params] : poses->as_object()) {
            apply_pose(pose_params, tracker.poses[pose_name]);
        }
    }
    if (const json::Value* inputs = params.find("inputs")) {
        for (const auto& [input_name, value] : inputs->as_object()) {
            if (value.is_bool()) {
                tracker.inputs.insert_or_assign(input_name, value.as_bool());
            } else if (value.is_number()) {
                tracker.inputs.insert_or_assign(input_name, static_cast<float>(value.as_double()));
            } else {
                const json::Array& axis = value.as_array();
                tracker.inputs.insert_or_assign(input_name,
                                                Vec2{static_cast<float>(axis[0].as_double()),
                                                     static_cast<float>(axis[1].as_double())});
            }
        }
    }
    return Reply::success({});
}

Reply xr_set(Session& session, const json::Value& params) {
    world::World& world = session.world();
    xr::TestInterface* test = xr::test_interface(world);
    if (test == nullptr) {
        return Reply::failure(code::kNotFound, "this session has no test headset");
    }
    if (const json::Value* initialize = params.find("initialize")) {
        if (initialize->as_bool()) {
            if (auto started = test->initialize(world); !started) {
                return Reply::failure(started.error());
            }
        } else {
            test->uninitialize(world);
        }
    }
    if (const json::Value* trackers = params.find("trackers")) {
        for (const auto& [name, tracker] : trackers->as_object()) {
            Reply set =
                set_tracker(*test, name, tracker, "/trackers/" + json::pointer_escape(name));
            if (!set.ok) {
                return set;
            }
        }
    }
    if (const json::Value* mode = params.find("play_area_mode")) {
        if (!test->set_play_area_mode(
                world, xr::play_area_named(mode->as_string()).value_or(xr::PlayArea::Unknown))) {
            return param_error(code::kInvalidArgument, "/play_area_mode",
                               "the test headset has no " + quoted(mode->as_string()) +
                                   " play area");
        }
    }
    if (const json::Value* size = params.find("play_area_size")) {
        const json::Array& items = size->as_array();
        test->set_play_area_size(
            {static_cast<float>(items[0].as_double()), static_cast<float>(items[1].as_double())});
    }
    if (const json::Value* ipd = params.find("ipd")) {
        test->set_ipd(static_cast<float>(ipd->as_double()));
    }
    if (const json::Value* size = params.find("render_target_size")) {
        const json::Array& items = size->as_array();
        test->set_render_target_size({static_cast<std::int32_t>(items[0].as_int()),
                                      static_cast<std::int32_t>(items[1].as_int())});
    }
    if (const json::Value* blend = params.find("environment_blend_mode")) {
        (void)test->set_environment_blend_mode(
            xr::blend_mode_named(blend->as_string()).value_or(xr::BlendMode::Opaque));
    }
    if (const json::Value* center = params.find("center_on_hmd")) {
        const std::string& mode = center->as_string();
        xr::RotationMode rotation = xr::RotationMode::ResetFullRotation;
        if (mode == "reset_but_keep_tilt") {
            rotation = xr::RotationMode::ResetButKeepTilt;
        } else if (mode == "dont_reset_rotation") {
            rotation = xr::RotationMode::DontResetRotation;
        }
        const json::Value* keep = params.find("keep_height");
        xr::center_on_hmd(world, rotation, keep == nullptr || keep->as_bool());
    }
    json::Value out;
    out["tick"] = world.tick();
    return Reply::success(std::move(out));
}

Reply xr_state(Session& session, const json::Value& params) {
    std::vector<std::string> names;
    if (const json::Value* trackers = params.find("trackers")) {
        for (const json::Value& name : trackers->as_array()) {
            names.push_back(name.as_string());
        }
    }
    return Reply::success(xr::state_json(session.world(), names));
}

} // namespace

void register_xr_commands(Registry& registry) {
    using enum Capability;
    json::Value names = property("array", "Tracker names to return; default all.");
    names["items"]["type"] = "string";
    registry.add({
        .name = "xr.state",
        .description = "Returns XR: the interfaces, the trackers' poses and inputs, the XR "
                       "nodes and whether they are tracked, haptic pulses and problems.",
        .capabilities = {Read},
        .effect = Effect::Read,
        .params = object_schema({{"trackers", std::move(names)}}),
        .handler = xr_state,
    });
    json::Value trackers;
    trackers["type"] = "object";
    trackers["description"] = "Changes by tracker name: head, left_hand, right_hand or new ones.";
    trackers["additionalProperties"] = tracker_schema();
    json::Value ipd = property("number", "Meters between the eyes. Default: 0.064.");
    ipd["minimum"] = 0;
    ipd["maximum"] = 0.2;
    json::Value target = property("array", "Each view's image, in pixels. Default: [1024, 1024].");
    target["items"]["type"] = "integer";
    target["items"]["minimum"] = 1;
    target["items"]["maximum"] = 4096;
    target["minItems"] = 2;
    target["maxItems"] = 2;
    registry.add({
        .name = "xr.set",
        .description = "Moves the test headset's trackers, presses their inputs and sets its "
                       "play area, as a player would; the server has them at the next tick, "
                       "which it returns.",
        .capabilities = {Run},
        .effect = Effect::Read,
        .params = object_schema({
            {"initialize", property("boolean", "Starts or stops the test headset.")},
            {"trackers", std::move(trackers)},
            {"play_area_mode", choice({"sitting", "roomscale", "stage"}, "")},
            {"play_area_size", numbers(2, "Meters, x by z.")},
            {"environment_blend_mode", choice({"opaque", "additive", "alpha"}, "")},
            {"ipd", std::move(ipd)},
            {"render_target_size", std::move(target)},
            {"center_on_hmd",
             choice({"reset_full_rotation", "reset_but_keep_tilt", "dont_reset_rotation"},
                    "Makes where the head is the play space's center, as Godot's center_on_hmd; "
                    "not in the stage play area.")},
            {"keep_height", property("boolean", "With center_on_hmd. Default: true.")},
        }),
        .handler = xr_set,
    });
}

} // namespace slash::command
