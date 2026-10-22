#include "slash/animation/animation.hpp"

#include "internal.hpp"

#include "slash/core/text.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <numbers>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <utility>

namespace slash::animation {
namespace {

constexpr std::array<std::string_view, 3> kLoopModes = {"none", "linear", "pingpong"};
constexpr std::array<std::string_view, 5> kInterpolations = {"nearest", "linear", "cubic",
                                                             "linear_angle", "cubic_angle"};
constexpr std::array<std::string_view, 3> kUpdateModes = {"auto", "continuous", "discrete"};

template <std::size_t N>
json::Value names(const std::array<std::string_view, N>& list) {
    json::Array out;
    for (const std::string_view name : list) {
        out.emplace_back(name);
    }
    return out;
}

template <std::size_t N>
std::size_t index_of(const std::array<std::string_view, N>& list, std::string_view name) {
    return static_cast<std::size_t>(std::ranges::find(list, name) - list.begin());
}

json::Value property(std::string_view type, std::string_view description) {
    json::Value out;
    out["type"] = type;
    out["description"] = description;
    return out;
}

json::Value schema() {
    json::Value key;
    key["type"] = "object";
    key["additionalProperties"] = false;
    key["required"] = json::Array{"time"};
    key["properties"]["time"] = property("number", "Seconds from the animation's start.");
    key["properties"]["time"]["minimum"] = 0;
    key["properties"]["value"]["description"] = "A value track's value: one the field takes.";
    key["properties"]["data"]["description"] = "An event track's event data: any JSON.";
    key["properties"]["transition"] =
        property("number", "Easing to the next key, as Godot's: 1 linear, above 1 eases in, "
                           "0 to 1 eases out, below 0 eases in and out, 0 holds the value.");
    key["properties"]["transition"]["default"] = 1;

    json::Value track;
    track["type"] = "object";
    track["additionalProperties"] = false;
    track["required"] = json::Array{"keys"};
    track["description"] = "A value track (path) or an event track (event).";
    track["properties"]["path"] =
        property("string", "The value it sets: a component, its field and optionally a part, "
                           "as \"Transform2D/position\" or \"Sprite2D/modulate/a\".");
    track["properties"]["event"] =
        property("string", "The event kind it emits when playback passes each key.");
    track["properties"]["target"] =
        property("string", "The entity it acts on: a name in the AnimationPlayer's targets, "
                           "as a Godot track's node path. Left out, the player's own entity.");
    track["properties"]["target"]["minLength"] = 1;
    track["properties"]["interpolation"] = property("string", "How values between keys are found.");
    track["properties"]["interpolation"]["enum"] = names(kInterpolations);
    track["properties"]["interpolation"]["default"] = "linear";
    track["properties"]["update"] =
        property("string", "continuous changes smoothly, discrete jumps from key to key; auto "
                           "is continuous for float fields and discrete for others.");
    track["properties"]["update"]["enum"] = names(kUpdateModes);
    track["properties"]["update"]["default"] = "auto";
    track["properties"]["loop_wrap"] =
        property("boolean", "In a looping animation, move from the last key to the first "
                            "across the loop's end.");
    track["properties"]["loop_wrap"]["default"] = true;
    track["properties"]["keys"]["type"] = "array";
    track["properties"]["keys"]["minItems"] = 1;
    track["properties"]["keys"]["items"]["$ref"] = "#/$defs/key";
    track["properties"]["keys"]["description"] = "Keys in time order.";

    json::Value from;
    from["type"] = "object";
    from["additionalProperties"] = false;
    from["required"] = json::Array{"model", "animation"};
    from["description"] = "A model file's animation it plays on the nodes of the entity's model, "
                          "as Godot imports a glTF file's animations.";
    from["properties"]["model"] =
        property("string", "The model file: .gltf, .glb, .fbx or .blend.");
    from["properties"]["model"]["minLength"] = 1;
    from["properties"]["animation"] = property("string", "Its name in the model.");
    from["properties"]["animation"]["minLength"] = 1;

    json::Value animation;
    animation["type"] = "object";
    animation["additionalProperties"] = false;
    animation["description"] = "Needs a length and tracks, unless it comes from a model.";
    animation["properties"]["from"] = std::move(from);
    animation["properties"]["length"] =
        property("number", "Seconds. Left out, the model animation's it comes from.");
    animation["properties"]["length"]["exclusiveMinimum"] = 0;
    animation["properties"]["loop"] =
        property("string", "What happens at the end, as Godot's loop modes. Left out, the model "
                           "animation's it comes from, or none.");
    animation["properties"]["loop"]["enum"] = names(kLoopModes);
    animation["properties"]["loop"]["default"] = "none";
    animation["properties"]["tracks"]["type"] = "array";
    animation["properties"]["tracks"]["items"]["$ref"] = "#/$defs/track";

    json::Value out;
    out["$schema"] = "https://json-schema.org/draft/2020-12/schema";
    out["title"] = "SlashEngine animation library";
    out["description"] = "Keyframe animations of component fields by name, like Godot's "
                         "AnimationLibrary. AnimationPlayer plays them.";
    out["type"] = "object";
    out["additionalProperties"] = false;
    out["required"] = json::Array{"animations", "format"};
    out["properties"]["format"]["const"] = 1;
    out["properties"]["format"]["description"] = "Format version. Must be 1.";
    out["properties"]["animations"]["type"] = "object";
    out["properties"]["animations"]["additionalProperties"]["$ref"] = "#/$defs/animation";
    out["properties"]["animations"]["description"] = "Animations by name.";
    out["$defs"]["animation"] = std::move(animation);
    out["$defs"]["track"] = std::move(track);
    out["$defs"]["key"] = std::move(key);
    return out;
}

bool numeric(const json::Value& value) {
    if (value.is_number()) {
        return true;
    }
    return value.is_array() && value.size() > 0 &&
           std::ranges::all_of(value.as_array(), &json::Value::is_number);
}

std::string number_text(double value) {
    constexpr double kWhole = 1.0e15;
    if (value == std::floor(value) && std::abs(value) < kWhole) {
        return std::to_string(static_cast<std::int64_t>(value));
    }
    return json::write(json::Value(value), json::Style::Compact);
}

// A JSON type name, as errors say it.
std::string kind_of(const json::Value& value) {
    if (value.is_array()) {
        return "an array";
    }
    return "a " + std::string(json::to_string(value.type()));
}

// The definition a {"$ref": "#/$defs/Name"} schema points to, or the schema itself.
const json::Value& dereference(const json::Value& schema, const json::Object& definitions) {
    const json::Value* ref = schema.find("$ref");
    if (ref == nullptr || !ref->is_string()) {
        return schema;
    }
    constexpr std::string_view kPrefix = "#/$defs/";
    const std::string& text = ref->as_string();
    if (!text.starts_with(kPrefix)) {
        return schema;
    }
    const auto it = definitions.find(text.substr(kPrefix.size()));
    return it == definitions.end() ? schema : it->second;
}

std::string schema_type(const json::Value& schema) {
    const json::Value* type = schema.find("type");
    return type != nullptr && type->is_string() ? type->as_string() : std::string();
}

std::optional<std::size_t> component_index(std::string_view token) {
    constexpr std::string_view kXyzw = "xyzw";
    constexpr std::string_view kRgba = "rgba";
    if (token.size() == 1) {
        if (const std::size_t at = kXyzw.find(token.front()); at != std::string_view::npos) {
            return at;
        }
        if (const std::size_t at = kRgba.find(token.front()); at != std::string_view::npos) {
            return at;
        }
    }
    std::size_t index = 0;
    const auto [end, error] = std::from_chars(token.data(), token.data() + token.size(), index);
    if (error != std::errc{} || end != token.data() + token.size()) {
        return std::nullopt;
    }
    return index;
}

double wrap_angle(double angle) {
    constexpr double kTau = 2.0 * std::numbers::pi;
    return std::fmod(std::fmod(angle + std::numbers::pi, kTau) + kTau, kTau) - std::numbers::pi;
}

// Godot's ease curve: the weight x, from 0 to 1, shaped by c.
double ease(double x, double c) {
    x = std::clamp(x, 0.0, 1.0);
    if (c > 0.0) {
        return c < 1.0 ? 1.0 - std::pow(1.0 - x, 1.0 / c) : std::pow(x, c);
    }
    if (c < 0.0) {
        if (x < 0.5) {
            return std::pow(x * 2.0, -c) * 0.5;
        }
        return ((1.0 - std::pow(1.0 - ((x - 0.5) * 2.0), -c)) * 0.5) + 0.5;
    }
    return 0.0;
}

double lerp(double a, double b, double t) {
    return a + ((b - a) * t);
}

// A Catmull-Rom curve through pre, from, to and post at times pre_t, 0, to_t and post_t, found
// with the Barry-Goldman pyramid, so uneven gaps between keys keep the speed smooth.
double cubic(double pre, double from, double to, double post, double weight, double to_t,
             double pre_t, double post_t) {
    const double t = to_t * weight;
    const double a1 = lerp(pre, from, pre_t == 0.0 ? 0.0 : (t - pre_t) / -pre_t);
    const double a2 = lerp(from, to, to_t == 0.0 ? 0.5 : t / to_t);
    const double a3 = lerp(to, post, post_t - to_t == 0.0 ? 1.0 : (t - to_t) / (post_t - to_t));
    const double b1 = lerp(a1, a2, to_t - pre_t == 0.0 ? 0.0 : (t - pre_t) / (to_t - pre_t));
    const double b2 = lerp(a2, a3, post_t == 0.0 ? 1.0 : t / post_t);
    return lerp(b1, b2, to_t == 0.0 ? 0.5 : t / to_t);
}

// The numbers in a numeric value: one, or an array's.
std::vector<double> numbers_of(const json::Value& value) {
    if (value.is_number()) {
        return {value.as_double()};
    }
    std::vector<double> out;
    out.reserve(value.size());
    for (const json::Value& item : value.as_array()) {
        out.push_back(item.as_double());
    }
    return out;
}

json::Value value_of(const std::vector<double>& numbers, bool array) {
    if (!array) {
        return numbers.front();
    }
    json::Array out;
    out.reserve(numbers.size());
    for (const double number : numbers) {
        out.emplace_back(number);
    }
    return out;
}

// Checks a document the schema accepted, collecting every error.
class Checker {
public:
    Checker(const json::Value& doc, const world::ComponentRegistry* components)
        : doc_(doc), components_(components) {}

    std::vector<json::ValidationError> run() {
        for (const auto& [name, animation] : doc_.find("animations")->as_object()) {
            check_animation(animation, "/animations/" + json::pointer_escape(name));
        }
        return std::move(errors_);
    }

private:
    void fail(std::string path, std::string message) {
        errors_.push_back({std::move(path), std::move(message)});
    }

    void check_animation(const json::Value& animation, const std::string& at) {
        const json::Value* from = animation.find("from");
        const json::Value* length = animation.find("length");
        const json::Value* tracks = animation.find("tracks");
        if (from == nullptr && (length == nullptr || tracks == nullptr)) {
            std::string missing = R"("tracks")";
            if (length == nullptr) {
                missing = tracks == nullptr ? R"(a "length" and "tracks")" : R"(a "length")";
            }
            fail(at, "an animation needs " + missing +
                         ", unless it comes \"from\" a model's animation");
        }
        if (from != nullptr) {
            const std::string& model = from->find("model")->as_string();
            if (!scene3d::is_model_path(model) || model.ends_with(".obj")) {
                fail(at + "/from/model", "expected a .gltf, .glb, .fbx or .blend file");
            }
        }
        if (tracks == nullptr) {
            return;
        }
        // Keys past an animation's length are wrong, but a model animation's length isn't known
        // until its file is read.
        const double end =
            length != nullptr ? length->as_double() : std::numeric_limits<double>::infinity();
        std::set<std::string, std::less<>> paths;
        const json::Array& list = tracks->as_array();
        for (std::size_t i = 0; i < list.size(); ++i) {
            check_track(list[i], at + "/tracks/" + std::to_string(i), end, paths);
        }
    }

    void check_track(const json::Value& track, const std::string& at, double length,
                     std::set<std::string, std::less<>>& paths) {
        const json::Value* path = track.find("path");
        const json::Value* event = track.find("event");
        if ((path == nullptr) == (event == nullptr)) {
            fail(at, path == nullptr ? R"(a track has a "path", for a value track, or an "event")"
                                     : R"(a track has a "path" or an "event", not both)");
            return;
        }
        const json::Array& keys = track.find("keys")->as_array();
        for (std::size_t j = 0; j < keys.size(); ++j) {
            const std::string key_at = at + "/keys/" + std::to_string(j);
            const double time = keys[j].find("time")->as_double();
            if (time > length) {
                fail(key_at + "/time",
                     number_text(time) + " is past the animation's length, " + number_text(length));
            }
            if (j > 0 && time <= keys[j - 1].find("time")->as_double()) {
                fail(key_at + "/time",
                     "keys go in time order, each later than the one before: " + number_text(time) +
                         " comes after " + number_text(keys[j - 1].find("time")->as_double()));
            }
        }
        if (event != nullptr) {
            check_event_track(track, *event, at);
        } else {
            check_value_track(track, path->as_string(), at, paths);
        }
    }

    void check_event_track(const json::Value& track, const json::Value& event,
                           const std::string& at) {
        const std::string& kind = event.as_string();
        if (kind.empty()) {
            fail(at + "/event", "name the event");
        } else if (kind.starts_with("entity.") || kind == "command") {
            fail(at + "/event", "\"" + kind +
                                    "\" is one of the engine's own events; name it "
                                    "something else");
        }
        for (const char* key : {"interpolation", "update", "loop_wrap"}) {
            if (track.find(key) != nullptr) {
                fail(at + "/" + key, "only value tracks have " + std::string(key));
            }
        }
        const json::Array& keys = track.find("keys")->as_array();
        for (std::size_t j = 0; j < keys.size(); ++j) {
            if (keys[j].find("value") != nullptr) {
                fail(at + "/keys/" + std::to_string(j) + "/value",
                     R"(an event track's keys carry "data", not "value")");
            }
        }
    }

    void check_value_track(const json::Value& track, const std::string& path, const std::string& at,
                           std::set<std::string, std::less<>>& paths) {
        const std::size_t slash = path.find('/');
        if (slash == std::string::npos || slash == 0 || slash + 1 == path.size() ||
            path.find("//") != std::string::npos || path.back() == '/') {
            fail(at + "/path", "a path is a component and one of its fields, as "
                               "\"Transform2D/position\", then optionally a part, as "
                               "\"Transform2D/position/y\"");
            return;
        }
        const json::Value* on = track.find("target");
        const std::string target_name = on != nullptr ? on->as_string() : std::string();
        if (!paths.insert(target_name + ":" + path).second) {
            fail(at + "/path", "another track already sets \"" + path + "\"" +
                                   (on != nullptr ? " on \"" + target_name + "\"" : ""));
        }
        const json::Array& keys = track.find("keys")->as_array();
        bool values_ok = true;
        for (std::size_t j = 0; j < keys.size(); ++j) {
            const std::string key_at = at + "/keys/" + std::to_string(j);
            if (keys[j].find("data") != nullptr) {
                fail(key_at + "/data", R"(a value track's keys carry "value", not "data")");
            }
            if (keys[j].find("value") == nullptr) {
                fail(key_at, "a value track's keys need a \"value\"");
                values_ok = false;
            }
        }
        Target target;
        const bool resolving = components_ != nullptr;
        if (resolving) {
            auto resolved = resolve_target(path, *components_);
            if (!resolved) {
                fail(at + "/path", resolved.error().message);
                return;
            }
            target = std::move(resolved).value();
            const std::size_t before = errors_.size();
            for (std::size_t j = 0; j < keys.size() && values_ok; ++j) {
                const std::string key_at = at + "/keys/" + std::to_string(j) + "/value";
                for (json::ValidationError& error :
                     json::validate(*keys[j].find("value"), target.schema)) {
                    fail(key_at + error.path, std::move(error.message));
                }
            }
            // A value the field doesn't take says enough.
            values_ok = values_ok && errors_.size() == before;
        }
        if (!values_ok) {
            return;
        }
        // Values that change smoothly must be numbers, or arrays of numbers of one length.
        const json::Value* update = track.find("update");
        const std::string mode = update != nullptr ? update->as_string() : "auto";
        const json::Value* interpolation = track.find("interpolation");
        const bool nearest = interpolation != nullptr && interpolation->as_string() == "nearest";
        bool continuous = mode == "continuous";
        if (mode == "auto") {
            continuous = resolving ? !target.discrete : numeric(*keys.front().find("value"));
        }
        if (!continuous || nearest) {
            return;
        }
        const json::Value& first = *keys.front().find("value");
        for (std::size_t j = 0; j < keys.size(); ++j) {
            const json::Value& value = *keys[j].find("value");
            const std::string key_at = at + "/keys/" + std::to_string(j) + "/value";
            if (!numeric(value)) {
                fail(key_at, "a continuous track's values are numbers or arrays of numbers, "
                             "not " +
                                 kind_of(value) + R"(; use "update": "discrete")");
                return;
            }
            if (value.is_array() != first.is_array() ||
                (value.is_array() && value.size() != first.size())) {
                fail(key_at, "each key's value has the same size as the first key's, so values "
                             "can change smoothly between them");
                return;
            }
        }
    }

    const json::Value& doc_;
    const world::ComponentRegistry* components_;
    std::vector<json::ValidationError> errors_;
};

Interpolation parse_interpolation(std::string_view name) {
    return static_cast<Interpolation>(index_of(kInterpolations, name));
}

} // namespace

std::string_view loop_mode_name(LoopMode mode) {
    return kLoopModes.at(static_cast<std::size_t>(mode));
}

std::string_view interpolation_name(Interpolation interpolation) {
    return kInterpolations.at(static_cast<std::size_t>(interpolation));
}

json::Value animation_schema() {
    return schema();
}

std::vector<json::ValidationError>
validate_animation_library(const json::Value& doc, const world::ComponentRegistry* components) {
    std::vector<json::ValidationError> errors = json::validate(doc, schema());
    if (!errors.empty()) {
        return errors;
    }
    return Checker(doc, components).run();
}

bool is_model_library(std::string_view path) {
    return scene3d::is_model_path(path);
}

AnimationLibrary model_library(const scene3d::Model& model, std::string_view path) {
    AnimationLibrary library;
    for (const scene3d::ModelAnimation& clip : model.animations) {
        Animation animation;
        animation.length = clip.length;
        animation.loop = clip.loop ? LoopMode::Linear : LoopMode::None;
        animation.from = {std::string(path), clip.name};
        library.animations.emplace(clip.name, std::move(animation));
    }
    return library;
}

Result<void> resolve_clip(Animation& animation, const scene3d::Model& model) {
    const auto found = std::ranges::find(model.animations, animation.from.animation,
                                         &scene3d::ModelAnimation::name);
    if (found == model.animations.end()) {
        std::vector<std::string_view> names;
        names.reserve(model.animations.size());
        for (const scene3d::ModelAnimation& clip : model.animations) {
            names.emplace_back(clip.name);
        }
        std::string message =
            animation.from.model + " has no animation \"" + animation.from.animation + "\"";
        if (const std::string_view hint = closest_match(animation.from.animation, names);
            !hint.empty()) {
            message += "; did you mean \"" + std::string(hint) + "\"?";
        } else if (names.empty()) {
            message += "; it has none";
        }
        return make_error(ErrorCode::NotFound, std::move(message));
    }
    if (!animation.own_length) {
        animation.length = found->length;
    }
    if (!animation.own_loop) {
        animation.loop = found->loop ? LoopMode::Linear : LoopMode::None;
    }
    return {};
}

Result<AnimationLibrary> parse_animation_library(const json::Value& doc) {
    const std::vector<json::ValidationError> errors = validate_animation_library(doc);
    if (!errors.empty()) {
        const json::ValidationError& first = errors.front();
        return make_error(ErrorCode::InvalidArgument,
                          (first.path.empty() ? std::string("root") : first.path) + ": " +
                              first.message);
    }
    AnimationLibrary library;
    for (const auto& [name, json_animation] : doc.find("animations")->as_object()) {
        Animation animation;
        if (const json::Value* length = json_animation.find("length"); length != nullptr) {
            animation.length = length->as_double();
        } else {
            animation.own_length = false;
        }
        if (const json::Value* loop = json_animation.find("loop"); loop != nullptr) {
            animation.loop = static_cast<LoopMode>(index_of(kLoopModes, loop->as_string()));
        } else {
            animation.own_loop = false;
        }
        if (const json::Value* from = json_animation.find("from"); from != nullptr) {
            animation.from.model = from->find("model")->as_string();
            animation.from.animation = from->find("animation")->as_string();
        }
        static const json::Array kNoTracks;
        const json::Value* json_tracks = json_animation.find("tracks");
        for (const json::Value& json_track :
             json_tracks != nullptr ? json_tracks->as_array() : kNoTracks) {
            Track track;
            if (const json::Value* path = json_track.find("path"); path != nullptr) {
                track.path = path->as_string();
            }
            if (const json::Value* event = json_track.find("event"); event != nullptr) {
                track.event = event->as_string();
            }
            if (const json::Value* target = json_track.find("target"); target != nullptr) {
                track.target = target->as_string();
            }
            if (const json::Value* value = json_track.find("interpolation"); value != nullptr) {
                track.interpolation = parse_interpolation(value->as_string());
            }
            if (const json::Value* value = json_track.find("update"); value != nullptr) {
                track.update = static_cast<UpdateMode>(index_of(kUpdateModes, value->as_string()));
            }
            if (const json::Value* value = json_track.find("loop_wrap"); value != nullptr) {
                track.loop_wrap = value->as_bool();
            }
            for (const json::Value& json_key : json_track.find("keys")->as_array()) {
                Key key;
                key.time = json_key.find("time")->as_double();
                if (const json::Value* value = json_key.find(track.path.empty() ? "data" : "value");
                    value != nullptr) {
                    key.value = *value;
                }
                if (const json::Value* value = json_key.find("transition"); value != nullptr) {
                    key.transition = value->as_double();
                }
                track.keys.push_back(std::move(key));
            }
            animation.tracks.push_back(std::move(track));
        }
        library.animations.emplace(name, std::move(animation));
    }
    return library;
}

std::vector<json::ValidationError> check_players(const json::Value& doc,
                                                 const world::FileReader& reader) {
    std::vector<json::ValidationError> errors;
    if (!reader) {
        return errors;
    }
    detail::CheckedFiles files(reader);
    const auto check = [&](const json::Value& components, const std::string& at, bool extends) {
        const json::Value* player = components.find("AnimationPlayer");
        if (player == nullptr || !player->is_object()) {
            return;
        }
        // A player whose library comes from a prefab or an instanced scene is checked there.
        const json::Value* library_path = player->find("library");
        if (library_path == nullptr || !library_path->is_string() ||
            library_path->as_string().empty()) {
            return;
        }
        const std::string& path = library_path->as_string();
        if (!path.ends_with(kAnimationSuffix) && !is_model_library(path)) {
            errors.push_back({at + "/library", "animation libraries are .anim.json files or "
                                               "models, as \"anim/hero.anim.json\" or "
                                               "\"models/hero.glb\""});
            return;
        }
        const Result<AnimationLibrary>& library = files.library(path);
        if (!library) {
            errors.push_back({at + "/library", path + ": " + library.error().message});
            return;
        }
        const auto& animations = library.value().animations;
        if (const json::Value* name = player->find("animation");
            name != nullptr && name->is_string() && !name->as_string().empty() &&
            !animations.contains(name->as_string())) {
            std::vector<std::string_view> names;
            names.reserve(animations.size());
            for (const auto& [each, animation] : animations) {
                names.emplace_back(each);
            }
            std::string message = path + " has no animation \"" + name->as_string() + "\"";
            if (const std::string_view hint = closest_match(name->as_string(), names);
                !hint.empty()) {
                message += "; did you mean \"" + std::string(hint) + "\"?";
            }
            errors.push_back({at + "/animation", std::move(message)});
        }
        const json::Value* targets = player->find("targets");
        if (extends && targets == nullptr) {
            return;
        }
        std::vector<std::string_view> bound;
        if (targets != nullptr && targets->is_object()) {
            for (const auto& [name, entity] : targets->as_object()) {
                bound.emplace_back(name);
            }
        }
        std::set<std::string, std::less<>> missing;
        for (const auto& [name, animation] : animations) {
            for (const Track& track : animation.tracks) {
                if (!track.target.empty() &&
                    std::ranges::find(bound, track.target) == bound.end()) {
                    missing.insert(track.target);
                }
            }
        }
        for (const std::string& name : missing) {
            std::string message = "no entity for target \"" + name + "\", which ";
            message.append(path).append("'s tracks name; add it to targets");
            if (const std::string_view hint = closest_match(name, bound); !hint.empty()) {
                message += "; did you mean \"" + std::string(hint) + "\"?";
            }
            errors.push_back({targets != nullptr ? at + "/targets" : at, std::move(message)});
        }
    };
    if (const json::Value* entities = doc.find("entities"); entities != nullptr) {
        for (const auto& [id, entity] : entities->as_object()) {
            if (const json::Value* components = entity.find("components"); components != nullptr) {
                check(*components,
                      "/entities/" + json::pointer_escape(id) + "/components/AnimationPlayer",
                      entity.find("extends") != nullptr || id.find('/') != std::string::npos);
            }
        }
    } else if (const json::Value* components = doc.find("components"); components != nullptr) {
        check(*components, "/components/AnimationPlayer", doc.find("extends") != nullptr);
    }
    return errors;
}

Result<Target> resolve_target(std::string_view path, const world::ComponentRegistry& components) {
    std::vector<std::string> parts;
    for (std::size_t start = 0; start <= path.size();) {
        const std::size_t end = std::min(path.find('/', start), path.size());
        parts.emplace_back(path.substr(start, end - start));
        start = end + 1;
    }
    if (parts.size() < 2 || std::ranges::any_of(parts, &std::string::empty)) {
        return make_error(ErrorCode::InvalidArgument,
                          "a path is a component and one of its fields, as "
                          "\"Transform2D/position\"");
    }
    Target target;
    target.component = components.find(parts[0]);
    if (target.component == nullptr) {
        std::vector<std::string_view> names;
        names.reserve(components.types().size());
        for (const auto& type : components.types()) {
            names.emplace_back(type->name());
        }
        std::string message = "no component \"" + parts[0] + "\"";
        if (const std::string_view hint = closest_match(parts[0], names); !hint.empty()) {
            message += "; did you mean \"" + std::string(hint) + "\"?";
        }
        return make_error(ErrorCode::NotFound, std::move(message));
    }
    const auto fields = target.component->fields();
    if (std::ranges::find(fields, parts[1]) == fields.end()) {
        std::string message = parts[0] + " has no field \"" + parts[1] + "\"";
        if (const std::string_view hint = closest_match(parts[1], fields); !hint.empty()) {
            message += "; did you mean \"" + std::string(hint) + "\"?";
        }
        return make_error(ErrorCode::NotFound, std::move(message));
    }
    target.field = parts[1];

    world::SchemaBuilder builder;
    (void)target.component->schema(builder);
    const json::Object& definitions = builder.definitions();
    json::Value schema = *builder.definition(parts[0]).find("properties")->find(parts[1]);
    std::string walked = parts[0] + "/" + parts[1];
    for (std::size_t i = 2; i < parts.size(); ++i) {
        const json::Value& current = dereference(schema, definitions);
        const std::string type = schema_type(current);
        const std::string& part = parts[i];
        json::Value next;
        if (type == "array" && current.find("items") != nullptr) {
            const std::optional<std::size_t> index = component_index(part);
            const json::Value* most = current.find("maxItems");
            if (!index || (most != nullptr && *index >= static_cast<std::size_t>(most->as_int()))) {
                std::string message = walked;
                message += R"( has no part ")" + part + R"("; pick x, y, z or w, r, g, b or a, )";
                message += "or an index";
                return make_error(ErrorCode::InvalidArgument, std::move(message));
            }
            target.part.push_back(std::to_string(*index));
            next = *current.find("items");
        } else if (type == "object" && current.find("properties") != nullptr) {
            const json::Object& properties = current.find("properties")->as_object();
            if (!properties.contains(part)) {
                std::vector<std::string_view> keys;
                keys.reserve(properties.size());
                for (const auto& [name, property] : properties) {
                    keys.emplace_back(name);
                }
                std::string message = walked;
                message += R"( has no field ")" + part + "\"";
                if (const std::string_view hint = closest_match(part, keys); !hint.empty()) {
                    message += "; did you mean \"" + std::string(hint) + "\"?";
                }
                return make_error(ErrorCode::NotFound, std::move(message));
            }
            target.part.push_back(part);
            next = properties.find(part)->second;
        } else {
            return make_error(ErrorCode::InvalidArgument,
                              walked + " is " + (type.empty() ? "a value" : "a " + type) +
                                  "; it has no parts");
        }
        schema = std::move(next);
        walked += "/" + part;
    }
    const json::Value& final_schema = dereference(schema, definitions);
    const std::string type = schema_type(final_schema);
    const json::Value* items = final_schema.find("items");
    target.discrete = !(type == "number" ||
                        (type == "array" && items != nullptr && schema_type(*items) == "number"));
    target.schema = schema;
    if (!definitions.empty()) {
        target.schema["$defs"] = definitions;
    }
    return target;
}

json::Value sample(const Animation& animation, const Track& track, double time, bool discrete) {
    const std::vector<Key>& keys = track.keys;
    if (keys.empty()) {
        return {};
    }
    const double length = animation.length;
    time = std::clamp(time, 0.0, length);
    const bool wrap = animation.loop != LoopMode::None && track.loop_wrap && keys.size() > 1;
    const std::size_t count = keys.size();
    // The keys at or before the time.
    const auto before =
        static_cast<std::size_t>(std::ranges::upper_bound(keys, time, std::less<>{},
                                                          [](const Key& key) { return key.time; }) -
                                 keys.begin());
    if (discrete || track.interpolation == Interpolation::Nearest) {
        if (before == 0) {
            return wrap ? keys.back().value : keys.front().value;
        }
        return keys[before - 1].value;
    }
    // The keys either side of the time, a and b, and their times on the same clock: before the
    // first key of a wrapping track, a is the last key a loop earlier; after its last key, b is
    // the first key a loop later.
    std::size_t a = before - 1;
    std::size_t b = before;
    double a_time = 0.0;
    double b_time = 0.0;
    if (before == 0) {
        if (!wrap) {
            return keys.front().value;
        }
        a = count - 1;
        b = 0;
        a_time = keys.back().time - length;
        b_time = keys.front().time;
    } else if (before == count) {
        if (!wrap || time == keys.back().time) {
            return keys.back().value;
        }
        b = 0;
        a_time = keys.back().time;
        b_time = keys.front().time + length;
    } else {
        a_time = keys[a].time;
        b_time = keys[b].time;
    }
    const Key& from = keys[a];
    const Key& to = keys[b];
    const auto matches = [&](const Key& key) {
        return numeric(key.value) && key.value.is_array() == from.value.is_array() &&
               key.value.size() == from.value.size();
    };
    if (!matches(from) || !matches(to)) {
        return from.value;
    }
    const double span = b_time - a_time;
    const double weight = ease(span > 0.0 ? (time - a_time) / span : 0.0, from.transition);
    const bool angle = track.interpolation == Interpolation::LinearAngle ||
                       track.interpolation == Interpolation::CubicAngle;
    const bool smooth = track.interpolation == Interpolation::Cubic ||
                        track.interpolation == Interpolation::CubicAngle;
    const std::vector<double> start = numbers_of(from.value);
    std::vector<double> end = numbers_of(to.value);
    if (angle) {
        for (std::size_t i = 0; i < start.size(); ++i) {
            end[i] = start[i] + wrap_angle(end[i] - start[i]);
        }
    }
    std::vector<double> out(start.size());
    if (!smooth) {
        for (std::size_t i = 0; i < out.size(); ++i) {
            out[i] = lerp(start[i], end[i], weight);
        }
        return value_of(out, from.value.is_array());
    }
    // The keys before a and after b, round the loop for a wrapping track. At the ends of one
    // that doesn't wrap, a and b stand in for them, so the curve flattens there.
    const double a_shift = a_time - keys[a].time;
    const double b_shift = b_time - keys[b].time;
    std::vector<double> pre = start;
    std::vector<double> post = end;
    double pre_time = a_time;
    double post_time = b_time;
    const auto neighbour = [&](std::size_t index, double shift, std::vector<double>& numbers,
                               double& at) {
        const Key& key = keys[index];
        if (matches(key)) {
            numbers = numbers_of(key.value);
            at = key.time + shift;
        }
    };
    if (a > 0) {
        neighbour(a - 1, a_shift, pre, pre_time);
    } else if (wrap) {
        neighbour(count - 1, a_shift - length, pre, pre_time);
    }
    if (b + 1 < count) {
        neighbour(b + 1, b_shift, post, post_time);
    } else if (wrap) {
        neighbour(0, b_shift + length, post, post_time);
    }
    if (angle) {
        for (std::size_t i = 0; i < start.size(); ++i) {
            pre[i] = start[i] + wrap_angle(pre[i] - start[i]);
            post[i] = end[i] + wrap_angle(post[i] - end[i]);
        }
    }
    for (std::size_t i = 0; i < out.size(); ++i) {
        out[i] = cubic(pre[i], start[i], end[i], post[i], weight, span, pre_time - a_time,
                       post_time - a_time);
    }
    return value_of(out, from.value.is_array());
}

Result<void> apply(world::World& world, entt::entity entity, const Target& target,
                   const json::Value& value) {
    json::Value set = value;
    const json::Value* type = target.schema.find("type");
    if (type != nullptr && type->is_string() && type->as_string() == "integer" &&
        value.is_number()) {
        set = static_cast<std::int64_t>(std::llround(value.as_double()));
    }
    json::Value fields;
    if (target.part.empty()) {
        fields[target.field] = std::move(set);
    } else {
        const json::Value current = target.component->has(world, entity)
                                        ? target.component->get(world, entity)
                                        : target.component->defaults();
        json::Value field =
            current.find(target.field) != nullptr ? *current.find(target.field) : json::Value();
        json::Value* node = &field;
        for (const std::string& part : target.part) {
            if (node->is_array()) {
                std::size_t index = 0;
                std::from_chars(part.data(), part.data() + part.size(), index);
                if (index >= node->size()) {
                    return make_error(ErrorCode::InvalidArgument, "the value has no part " + part);
                }
                node = &node->as_array()[index];
            } else if (node->is_object() || node->is_null()) {
                node = &(*node)[part];
            } else {
                return make_error(ErrorCode::InvalidArgument, "the value has no part " + part);
            }
        }
        *node = std::move(set);
        fields[target.field] = std::move(field);
    }
    std::string path;
    if (auto assigned = target.component->assign(world, entity, fields, path); !assigned) {
        return make_error(assigned.error().code,
                          (path.empty() ? std::string() : path + ": ") + assigned.error().message);
    }
    return {};
}

} // namespace slash::animation
