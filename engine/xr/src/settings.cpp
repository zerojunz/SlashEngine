#include "slash/xr/xr.hpp"

namespace slash::xr {
namespace {

json::Value choice(std::initializer_list<std::string_view> values, std::string_view fallback,
                   std::string_view description) {
    json::Value schema;
    json::Array names;
    for (const std::string_view value : values) {
        names.emplace_back(std::string(value));
    }
    schema["enum"] = std::move(names);
    schema["default"] = fallback;
    schema["description"] = description;
    return schema;
}

json::Value boolean(bool value, std::string_view description) {
    json::Value schema;
    schema["type"] = "boolean";
    schema["default"] = value;
    schema["description"] = description;
    return schema;
}

json::Value object(std::string_view description) {
    json::Value schema;
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["description"] = description;
    return schema;
}

const json::Value* member(const json::Value* object, std::string_view key) {
    return object != nullptr && object->is_object() ? object->find(key) : nullptr;
}

void read_string(const json::Value* object, std::string_view key, std::string& out) {
    if (const json::Value* value = member(object, key); value != nullptr && value->is_string()) {
        out = value->as_string();
    }
}

void read_bool(const json::Value* object, std::string_view key, bool& out) {
    if (const json::Value* value = member(object, key); value != nullptr && value->is_bool()) {
        out = value->as_bool();
    }
}

} // namespace

json::Value settings_schema() {
    json::Value openxr = object("The headset's runtime, as Godot's xr/openxr settings.");
    openxr["properties"]["enabled"] =
        boolean(false, "Start the headset when the game starts; sessions start the test "
                       "headset instead.");
    openxr["properties"]["form_factor"] =
        choice({"head_mounted", "handheld"}, "head_mounted",
               "A headset, or a phone or tablet held up to the room.");
    openxr["properties"]["view_configuration"] =
        choice({"mono", "stereo"}, "stereo", "One view, or one for each eye.");
    openxr["properties"]["reference_space"] = choice(
        {"local", "stage", "local_floor"}, "stage",
        "Where the play space's origin is: where the head started (local), the room's center "
        "on the floor (stage) or the floor under where the head started (local_floor).");
    openxr["properties"]["environment_blend_mode"] =
        choice({"opaque", "additive", "alpha"}, "opaque",
               "How the game's picture blends with the room: not at all (virtual reality), "
               "added to it, or by its alpha (augmented reality).");
    json::Value shaders = object("As Godot's xr/shaders.");
    shaders["properties"]["enabled"] =
        boolean(false, "Draw for headsets: each eye's view in one pass.");
    json::Value schema = object("Virtual and augmented reality, as Godot's xr settings.");
    schema["properties"]["openxr"] = std::move(openxr);
    schema["properties"]["shaders"] = std::move(shaders);
    return schema;
}

std::vector<json::ValidationError> validate_settings(const json::Value& settings,
                                                     std::string_view at) {
    std::vector<json::ValidationError> errors = json::validate(settings, settings_schema());
    for (json::ValidationError& error : errors) {
        error.path = std::string(at) + error.path;
    }
    return errors;
}

Settings parse_settings(const json::Value& settings) {
    Settings out;
    const json::Value* openxr = member(&settings, "openxr");
    read_bool(openxr, "enabled", out.enabled);
    read_string(openxr, "form_factor", out.form_factor);
    read_string(openxr, "view_configuration", out.view_configuration);
    read_string(openxr, "reference_space", out.reference_space);
    std::string blend;
    read_string(openxr, "environment_blend_mode", blend);
    if (const auto mode = blend_mode_named(blend)) {
        out.environment_blend_mode = *mode;
    }
    read_bool(member(&settings, "shaders"), "enabled", out.shaders);
    return out;
}

} // namespace slash::xr
