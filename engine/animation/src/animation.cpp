#include "slash/animation/animation.hpp"

#include "internal.hpp"

#include "slash/core/text.hpp"
#include "slash/scene3d/skeleton.hpp"
#include "slash/world/components.hpp"
#include "slash/world/tree3d.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
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
constexpr std::array<std::string_view, 3> kNodeProperties = {"position", "rotation", "scale"};
constexpr std::array<std::string_view, 6> kTrackTypes = {"value", "bezier",    "event",
                                                         "node",  "animation", "audio"};

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
    key["properties"]["value"]["description"] =
        "A value track's value: one the field takes. A node track's: [x, y, z].";
    key["properties"]["data"]["description"] = "An event track's event data: any JSON.";
    key["properties"]["transition"] =
        property("number", "Easing to the next key, as Godot's: 1 linear, above 1 eases in, "
                           "0 to 1 eases out, below 0 eases in and out, 0 holds the value.");
    key["properties"]["transition"]["default"] = 1;
    json::Value handle = property("array", "");
    handle["items"]["type"] = "number";
    handle["minItems"] = 2;
    handle["maxItems"] = 2;
    key["properties"]["in"] = handle;
    key["properties"]["in"]["description"] =
        "A bezier key's in handle, [seconds, value] from the key: seconds 0 or less.";
    key["properties"]["out"] = std::move(handle);
    key["properties"]["out"]["description"] =
        "A bezier key's out handle, [seconds, value] from the key: seconds 0 or more.";
    key["properties"]["animation"] =
        property("string", "An animation track's: the animation its player plays from here, or "
                           "\"[stop]\" to stop it.");
    key["properties"]["animation"]["minLength"] = 1;
    key["properties"]["stream"] =
        property("string", "An audio track's: the sound or .audio.json file it plays from here, "
                           "on the target's audio player.");
    key["properties"]["stream"]["minLength"] = 1;
    key["properties"]["start_offset"] =
        property("number", "An audio key's seconds of its stream skipped at the start.");
    key["properties"]["start_offset"]["minimum"] = 0;
    key["properties"]["end_offset"] =
        property("number", "An audio key's seconds of its stream cut off at the end.");
    key["properties"]["end_offset"]["minimum"] = 0;

    json::Value track;
    track["type"] = "object";
    track["additionalProperties"] = false;
    track["required"] = json::Array{"keys"};
    track["description"] = "A value track (path), an event track (event), a node track (node), "
                           "or a track whose type says so.";
    track["properties"]["type"] =
        property("string", "What it does: sets a field (value), sets a number along a curve "
                           "(bezier, with path), emits events (event), moves a model's node "
                           "(node), plays the target's AnimationPlayer (animation) or plays "
                           "sounds on its audio player (audio). Left out, from path, event or "
                           "node.");
    track["properties"]["type"]["enum"] = names(kTrackTypes);
    track["properties"]["path"] =
        property("string", "The value it sets: a component, its field and optionally a part, "
                           "as \"Transform2D/position\" or \"Sprite2D/modulate/a\".");
    track["properties"]["event"] =
        property("string", "The event kind it emits when playback passes each key.");
    track["properties"]["node"] =
        property("string", "The node of the entity's model it moves, by name, as a Godot 3D "
                           "track moves a bone.");
    track["properties"]["node"]["minLength"] = 1;
    track["properties"]["property"] =
        property("string", "What a node track moves: its keys' values are [x, y, z], a "
                           "rotation's Euler angles in radians, turning the shorter way round.");
    track["properties"]["property"]["enum"] = names(kNodeProperties);
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
    track["properties"]["use_blend"] =
        property("boolean", "An audio track's sounds play as loud as its animation counts in "
                            "an animation tree's blend.");
    track["properties"]["use_blend"]["default"] = true;
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
    animation["properties"]["markers"] =
        property("object", "Named times in seconds, as Godot's animation markers.");
    animation["properties"]["markers"]["additionalProperties"]["type"] = "number";
    animation["properties"]["markers"]["additionalProperties"]["minimum"] = 0;

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

// Godot's Vector2::bezier_interpolate of one coordinate: the point at t on the cubic curve from
// start to end with controls a and b.
double bezier_point(double start, double a, double b, double end, double t) {
    const double omt = 1.0 - t;
    const double omt2 = omt * omt;
    const double omt3 = omt2 * omt;
    const double t2 = t * t;
    const double t3 = t2 * t;
    return (start * omt3) + (a * omt2 * t * 3.0) + (b * omt * t2 * 3.0) + (end * t3);
}

// A bezier track's value at a time, as Godot's Animation::bezier_track_interpolate: the curve
// from the key at or before it toward the next, its out handle and the next key's in handle as
// controls, found at the time by ten halvings of the curve's parameter.
json::Value bezier_value(const std::vector<Key>& keys, double time) {
    const auto value_at = [&](std::size_t i) {
        return keys[i].value.is_number() ? keys[i].value.as_double() : 0.0;
    };
    if (keys.size() == 1) {
        return value_at(0);
    }
    // Godot's Animation::_find: the key at the time, else the last before it.
    std::size_t idx = keys.size();
    for (std::size_t i = 0; i < keys.size(); ++i) {
        if (detail::is_equal_approx(time, keys[i].time)) {
            idx = i;
            break;
        }
        if (keys[i].time < time) {
            idx = i;
        }
    }
    if (idx == keys.size()) {
        return value_at(0);
    }
    if (idx + 1 >= keys.size()) {
        return value_at(keys.size() - 1);
    }
    const double t = time - keys[idx].time;
    const double duration = keys[idx + 1].time - keys[idx].time;
    const std::array<double, 2> start{0.0, value_at(idx)};
    const std::array<double, 2> start_out{start[0] + keys[idx].out[0], start[1] + keys[idx].out[1]};
    const std::array<double, 2> end{duration, value_at(idx + 1)};
    const std::array<double, 2> end_in{end[0] + keys[idx + 1].in[0], end[1] + keys[idx + 1].in[1]};
    const auto point = [&](double at) {
        return std::array<double, 2>{bezier_point(start[0], start_out[0], end_in[0], end[0], at),
                                     bezier_point(start[1], start_out[1], end_in[1], end[1], at)};
    };
    constexpr int kIterations = 10;
    double low = 0.0;
    double high = 1.0;
    for (int i = 0; i < kIterations; ++i) {
        const double middle = (low + high) / 2.0;
        if (point(middle)[0] < t) {
            low = middle;
        } else {
            high = middle;
        }
    }
    const std::array<double, 2> low_pos = point(low);
    const std::array<double, 2> high_pos = point(high);
    const double span = high_pos[0] - low_pos[0];
    const double c = span != 0.0 ? (t - low_pos[0]) / span : 0.0;
    return low_pos[1] + ((high_pos[1] - low_pos[1]) * c);
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
        if (const json::Value* markers = animation.find("markers"); markers != nullptr) {
            for (const auto& [name, time] : markers->as_object()) {
                if (name.empty()) {
                    fail(at + "/markers", "name each marker");
                } else if (time.as_double() > end) {
                    fail(at + "/markers/" + json::pointer_escape(name),
                         number_text(time.as_double()) + " is past the animation's length, " +
                             number_text(end));
                }
            }
        }
        std::set<std::string, std::less<>> paths;
        const json::Array& list = tracks->as_array();
        for (std::size_t i = 0; i < list.size(); ++i) {
            check_track(list[i], at + "/tracks/" + std::to_string(i), end, from != nullptr, paths);
        }
    }

    void check_track(const json::Value& track, const std::string& at, double length,
                     bool from_model, std::set<std::string, std::less<>>& paths) {
        const json::Value* path = track.find("path");
        const json::Value* event = track.find("event");
        const json::Value* node = track.find("node");
        const int kinds = static_cast<int>(path != nullptr) + static_cast<int>(event != nullptr) +
                          static_cast<int>(node != nullptr);
        const json::Value* type_value = track.find("type");
        const std::string type = type_value != nullptr ? type_value->as_string() : std::string();
        if (type == "animation" || type == "audio") {
            if (kinds != 0) {
                fail(at, type == "audio"
                             ? "an audio track plays sounds on its target's audio player, so it "
                               "has no path, event or node"
                             : "an animation track plays the animations of its target's "
                               "AnimationPlayer, so it has no path, event or node");
                return;
            }
        } else if (kinds != 1) {
            fail(at, kinds == 0
                         ? R"(a track has a "path", for a value track, an "event" or a "node", or )"
                           R"("type": "animation" or "audio")"
                         : R"(a track has one of "path", "event" and "node", not more)");
            return;
        } else if (!type.empty()) {
            const bool fits = ((type == "value" || type == "bezier") && path != nullptr) ||
                              (type == "event" && event != nullptr) ||
                              (type == "node" && node != nullptr);
            if (!fits) {
                const char* needs = R"("path")";
                if (type == "event") {
                    needs = R"("event")";
                } else if (type == "node") {
                    needs = R"("node")";
                }
                fail(at + "/type", "a" + std::string(type == "event" ? "n " : " ") + type +
                                       " track has a " + needs);
                return;
            }
        }
        if (node == nullptr && track.find("property") != nullptr) {
            fail(at + "/property", "only node tracks have property");
        }
        if (type != "audio" && track.find("use_blend") != nullptr) {
            fail(at + "/use_blend", "only audio tracks have use_blend");
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
        for (std::size_t j = 0; j < keys.size(); ++j) {
            const std::string key_at = at + "/keys/" + std::to_string(j);
            if (type != "bezier") {
                for (const char* handle : {"in", "out"}) {
                    if (keys[j].find(handle) != nullptr) {
                        fail(key_at + "/" + handle, "only bezier tracks' keys have handles");
                    }
                }
            }
            if (type != "animation" && keys[j].find("animation") != nullptr) {
                fail(key_at + "/animation", "only animation tracks' keys name an animation");
            }
            if (type != "audio") {
                for (const char* field : {"stream", "start_offset", "end_offset"}) {
                    if (keys[j].find(field) != nullptr) {
                        fail(key_at + "/" + field,
                             "only audio tracks' keys have " + std::string(field));
                    }
                }
            }
        }
        if (type == "animation") {
            check_animation_track(track, at);
        } else if (type == "audio") {
            check_audio_track(track, at);
        } else if (type == "bezier") {
            check_bezier_track(track, path->as_string(), at, paths);
        } else if (event != nullptr) {
            check_event_track(track, *event, at);
        } else if (node != nullptr) {
            check_node_track(track, node->as_string(), at, from_model, paths);
        } else {
            check_value_track(track, path->as_string(), at, paths);
        }
    }

    void check_node_track(const json::Value& track, const std::string& node, const std::string& at,
                          bool from_model, std::set<std::string, std::less<>>& moved) {
        if (from_model) {
            fail(at, "an animation from a model moves its nodes by the model's tracks; give "
                     "node tracks an animation of their own");
        }
        if (track.find("target") != nullptr) {
            fail(at + "/target", "node tracks move the nodes of the model the entity draws, so "
                                 "they have no target");
        }
        if (track.find("update") != nullptr) {
            fail(at + "/update", R"(node tracks change smoothly, or jump from key to key with )"
                                 R"("interpolation": "nearest", so they have no update)");
        }
        if (const json::Value* interpolation = track.find("interpolation");
            interpolation != nullptr && interpolation->as_string() != "nearest" &&
            interpolation->as_string() != "linear") {
            fail(at + "/interpolation",
                 R"(node tracks interpolate "nearest" or "linear", as model animations do)");
        }
        const json::Value* what = track.find("property");
        if (what == nullptr) {
            fail(at, R"(a node track needs a "property": "position", "rotation" or "scale")");
        } else if (!moved.insert(node + "\n" + what->as_string()).second) {
            fail(at + "/node",
                 "another track already moves \"" + node + "\"'s " + what->as_string());
        }
        const json::Array& keys = track.find("keys")->as_array();
        for (std::size_t j = 0; j < keys.size(); ++j) {
            const std::string key_at = at + "/keys/" + std::to_string(j);
            if (keys[j].find("data") != nullptr) {
                fail(key_at + "/data", R"(a node track's keys carry "value", not "data")");
            }
            const json::Value* value = keys[j].find("value");
            if (value == nullptr) {
                fail(key_at, R"(a node track's keys need a "value", [x, y, z])");
            } else if (!value->is_array() || value->size() != 3 || !numeric(*value)) {
                std::string got = kind_of(*value);
                if (value->is_number()) {
                    got = "a number";
                } else if (numeric(*value)) {
                    got = std::to_string(value->size()) + " numbers";
                }
                fail(key_at + "/value", "a node track's value is [x, y, z], not " + got);
            }
            if (const json::Value* transition = keys[j].find("transition");
                transition != nullptr && transition->as_double() != 1.0) {
                fail(key_at + "/transition", "node tracks go straight from key to key, so their "
                                             "keys have no transition");
            }
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

    void check_animation_track(const json::Value& track, const std::string& at) {
        if (track.find("target") == nullptr) {
            fail(at, "an animation track needs a \"target\": the entity whose AnimationPlayer "
                     "it plays");
        }
        for (const char* key : {"interpolation", "update", "loop_wrap"}) {
            if (track.find(key) != nullptr) {
                fail(at + "/" + key, "only value tracks have " + std::string(key));
            }
        }
        const json::Array& keys = track.find("keys")->as_array();
        for (std::size_t j = 0; j < keys.size(); ++j) {
            const std::string key_at = at + "/keys/" + std::to_string(j);
            if (keys[j].find("animation") == nullptr) {
                fail(key_at, "an animation track's keys need an \"animation\": the one its "
                             "player plays from there, or \"[stop]\"");
            }
            for (const char* field : {"value", "data", "transition"}) {
                if (keys[j].find(field) != nullptr) {
                    fail(key_at + "/" + field, "an animation track's keys carry \"animation\", "
                                               "not \"" +
                                                   std::string(field) + "\"");
                }
            }
        }
    }

    void check_audio_track(const json::Value& track, const std::string& at) {
        for (const char* key : {"interpolation", "update", "loop_wrap"}) {
            if (track.find(key) != nullptr) {
                fail(at + "/" + key, "only value tracks have " + std::string(key));
            }
        }
        const json::Array& keys = track.find("keys")->as_array();
        for (std::size_t j = 0; j < keys.size(); ++j) {
            const std::string key_at = at + "/keys/" + std::to_string(j);
            if (keys[j].find("stream") == nullptr) {
                fail(key_at, "an audio track's keys need a \"stream\": the sound it plays from "
                             "there");
            }
            for (const char* field : {"value", "data", "transition"}) {
                if (keys[j].find(field) != nullptr) {
                    fail(key_at + "/" + field, R"(an audio track's keys carry "stream", not ")" +
                                                   std::string(field) + R"(")");
                }
            }
        }
    }

    // Whether a path has the form of a value track's, failing it if not.
    bool check_path_form(const std::string& path, const std::string& at) {
        const std::size_t slash = path.find('/');
        if (slash == std::string::npos || slash == 0 || slash + 1 == path.size() ||
            path.find("//") != std::string::npos || path.back() == '/') {
            fail(at + "/path", "a path is a component and one of its fields, as "
                               "\"Transform2D/position\", then optionally a part, as "
                               "\"Transform2D/position/y\"");
            return false;
        }
        return true;
    }

    // Whether no other track sets a path on the same entity, failing it if one does.
    void check_unique_path(const json::Value& track, const std::string& path, const std::string& at,
                           std::set<std::string, std::less<>>& paths) {
        const json::Value* on = track.find("target");
        const std::string target_name = on != nullptr ? on->as_string() : std::string();
        if (!paths.insert(target_name + ":" + path).second) {
            fail(at + "/path", "another track already sets \"" + path + "\"" +
                                   (on != nullptr ? " on \"" + target_name + "\"" : ""));
        }
    }

    void check_bezier_track(const json::Value& track, const std::string& path,
                            const std::string& at, std::set<std::string, std::less<>>& paths) {
        if (!check_path_form(path, at)) {
            return;
        }
        check_unique_path(track, path, at, paths);
        for (const char* key : {"interpolation", "update", "loop_wrap"}) {
            if (track.find(key) != nullptr) {
                fail(at + "/" + key, "a bezier track's curve comes from its keys' handles, so it "
                                     "has no " +
                                         std::string(key));
            }
        }
        const json::Array& keys = track.find("keys")->as_array();
        for (std::size_t j = 0; j < keys.size(); ++j) {
            const std::string key_at = at + "/keys/" + std::to_string(j);
            const json::Value* value = keys[j].find("value");
            if (value == nullptr || !value->is_number()) {
                fail(value == nullptr ? key_at : key_at + "/value",
                     "a bezier track's keys need a \"value\": a number");
            }
            if (keys[j].find("data") != nullptr) {
                fail(key_at + "/data", R"(a bezier track's keys carry "value", not "data")");
            }
            if (keys[j].find("transition") != nullptr) {
                fail(key_at + "/transition", "a bezier key's handles shape the curve, so it has "
                                             "no transition");
            }
            if (const json::Value* in = keys[j].find("in");
                in != nullptr && in->as_array()[0].as_double() > 0.0) {
                fail(key_at + "/in/0", "an in handle points back in time: 0 seconds or less");
            }
            if (const json::Value* out = keys[j].find("out");
                out != nullptr && out->as_array()[0].as_double() < 0.0) {
                fail(key_at + "/out/0", "an out handle points on in time: 0 seconds or more");
            }
        }
        if (components_ == nullptr) {
            return;
        }
        auto resolved = resolve_target(path, *components_);
        if (!resolved) {
            fail(at + "/path", resolved.error().message);
            return;
        }
        const json::Value* type = resolved.value().schema.find("type");
        if (type == nullptr || !type->is_string() || type->as_string() != "number") {
            fail(at + "/path", "a bezier track sets one number, as \"Camera3D/fov\" or "
                               "\"Transform3D/position/y\"; " +
                                   path + " isn't one");
        }
    }

    void check_value_track(const json::Value& track, const std::string& path, const std::string& at,
                           std::set<std::string, std::less<>>& paths) {
        if (!check_path_form(path, at)) {
            return;
        }
        check_unique_path(track, path, at, paths);
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

// A node track as a model animation's track of a node.
scene3d::NodeTrack node_track(const Track& track, std::size_t node) {
    scene3d::NodeTrack out;
    out.node = node;
    out.property = track.property;
    out.step = track.interpolation == Interpolation::Nearest;
    out.loop_wrap = track.loop_wrap;
    out.times.reserve(track.keys.size());
    out.values.reserve(track.keys.size());
    for (const Key& key : track.keys) {
        out.times.push_back(static_cast<float>(key.time));
        Vec3 value;
        if (key.value.is_array() && key.value.size() == 3) {
            const json::Array& xyz = key.value.as_array();
            value =
                Vec3{static_cast<float>(xyz[0].as_double()), static_cast<float>(xyz[1].as_double()),
                     static_cast<float>(xyz[2].as_double())};
        }
        if (track.property == scene3d::NodeProperty::Rotation) {
            const Quat turn = world::quaternion(value);
            out.values.push_back({static_cast<float>(turn.x), static_cast<float>(turn.y),
                                  static_cast<float>(turn.z), static_cast<float>(turn.w)});
        } else {
            out.values.push_back({static_cast<float>(value.x), static_cast<float>(value.y),
                                  static_cast<float>(value.z), 0.0F});
        }
    }
    return out;
}

} // namespace

std::string_view loop_mode_name(LoopMode mode) {
    return kLoopModes.at(static_cast<std::size_t>(mode));
}

std::string_view interpolation_name(Interpolation interpolation) {
    return kInterpolations.at(static_cast<std::size_t>(interpolation));
}

std::string_view node_property_name(scene3d::NodeProperty property) {
    return kNodeProperties.at(static_cast<std::size_t>(property));
}

std::string_view track_type_name(TrackType type) {
    return kTrackTypes.at(static_cast<std::size_t>(type));
}

bool has_node_tracks(const Animation& animation) {
    return std::ranges::any_of(animation.tracks,
                               [](const Track& track) { return !track.node.empty(); });
}

std::shared_ptr<const scene3d::Model> node_animations(const AnimationLibrary& library) {
    auto model = std::make_shared<scene3d::Model>();
    std::map<std::string, std::size_t, std::less<>> nodes;
    for (const auto& [name, animation] : library.animations) {
        if (!has_node_tracks(animation)) {
            continue;
        }
        scene3d::ModelAnimation clip;
        clip.name = name;
        clip.length = animation.length;
        clip.loop = animation.loop != LoopMode::None;
        for (const Track& track : animation.tracks) {
            if (track.node.empty()) {
                continue;
            }
            const auto [it, added] = nodes.try_emplace(track.node, model->nodes.size());
            if (added) {
                scene3d::ModelNode node;
                node.name = track.node;
                model->nodes.push_back(std::move(node));
            }
            clip.tracks.push_back(node_track(track, it->second));
        }
        model->animations.push_back(std::move(clip));
    }
    if (model->animations.empty()) {
        return nullptr;
    }
    return model;
}

json::Value sample_node(const Animation& animation, const Track& track, double time) {
    const std::array<float, 4> value = scene3d::sample_track(
        node_track(track, 0), time, animation.length, animation.loop != LoopMode::None);
    Vec3 out{value[0], value[1], value[2]};
    if (track.property == scene3d::NodeProperty::Rotation) {
        out = world::euler(world::rotation_basis(Quat{value[0], value[1], value[2], value[3]}));
    }
    return json::Array{static_cast<double>(out.x), static_cast<double>(out.y),
                       static_cast<double>(out.z)};
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
        if (const json::Value* markers = json_animation.find("markers"); markers != nullptr) {
            for (const auto& [marker, time] : markers->as_object()) {
                animation.markers.emplace(marker, time.as_double());
            }
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
            if (const json::Value* node = json_track.find("node"); node != nullptr) {
                track.node = node->as_string();
                track.type = TrackType::Node;
            }
            if (!track.event.empty()) {
                track.type = TrackType::Event;
            }
            if (const json::Value* type = json_track.find("type"); type != nullptr) {
                track.type = static_cast<TrackType>(index_of(kTrackTypes, type->as_string()));
            }
            if (const json::Value* property = json_track.find("property"); property != nullptr) {
                track.property = static_cast<scene3d::NodeProperty>(
                    index_of(kNodeProperties, property->as_string()));
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
            if (const json::Value* value = json_track.find("use_blend"); value != nullptr) {
                track.use_blend = value->as_bool();
            }
            const char* carries = "value";
            if (track.type == TrackType::Event) {
                carries = "data";
            } else if (track.type == TrackType::Animation) {
                carries = "animation";
            } else if (track.type == TrackType::Audio) {
                carries = "stream";
            }
            for (const json::Value& json_key : json_track.find("keys")->as_array()) {
                Key key;
                key.time = json_key.find("time")->as_double();
                if (const json::Value* value = json_key.find(carries); value != nullptr) {
                    key.value = *value;
                }
                if (const json::Value* value = json_key.find("transition"); value != nullptr) {
                    key.transition = value->as_double();
                }
                if (const json::Value* value = json_key.find("start_offset"); value != nullptr) {
                    key.start_offset = value->as_double();
                }
                if (const json::Value* value = json_key.find("end_offset"); value != nullptr) {
                    key.end_offset = value->as_double();
                }
                for (const auto& [field, handle] :
                     {std::pair{"in", &key.in}, std::pair{"out", &key.out}}) {
                    if (const json::Value* value = json_key.find(field); value != nullptr) {
                        *handle = {value->as_array()[0].as_double(),
                                   value->as_array()[1].as_double()};
                    }
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
    // Node tracks move the nodes of the model the entity draws.
    const auto check_nodes = [&](const json::Value& components, const AnimationLibrary& library,
                                 const std::string& path, const std::string& at, bool extends) {
        std::set<std::string, std::less<>> moved;
        for (const auto& [name, animation] : library.animations) {
            for (const Track& track : animation.tracks) {
                if (!track.node.empty()) {
                    moved.insert(track.node);
                }
            }
        }
        if (moved.empty()) {
            return;
        }
        const json::Value* instance = components.find("MeshInstance3D");
        const json::Value* mesh = instance != nullptr ? instance->find("mesh") : nullptr;
        if (mesh == nullptr || !mesh->is_string() || !scene3d::is_model_path(mesh->as_string())) {
            if (!extends) {
                errors.push_back({at, path + "'s node tracks move the nodes of the model the "
                                             "entity draws, but it draws none: give it a "
                                             "MeshInstance3D whose mesh is a model file"});
            }
            return;
        }
        const Result<scene3d::Model>& model = files.model(mesh->as_string());
        if (!model) {
            return; // the MeshInstance3D's own check says why
        }
        std::vector<std::string_view> names;
        names.reserve(model.value().nodes.size());
        for (const scene3d::ModelNode& node : model.value().nodes) {
            names.emplace_back(node.name);
        }
        for (const std::string& node : moved) {
            if (std::ranges::find(names, node) != names.end()) {
                continue;
            }
            std::string message = "\"" + node + "\", which ";
            message.append(path)
                .append("'s node tracks move, isn't a node of ")
                .append(mesh->as_string());
            if (const std::string_view hint = closest_match(node, names); !hint.empty()) {
                message += "; did you mean \"" + std::string(hint) + "\"?";
            }
            errors.push_back({at, std::move(message)});
        }
    };
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
        check_nodes(components, library.value(), path, at, extends);
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
        } else if (type == "object" && current.find("additionalProperties") != nullptr &&
                   current.find("additionalProperties")->is_object()) {
            // A map's value by its key, as MeshInstance3D/blend_shapes/smile.
            target.part.push_back(part);
            next = *current.find("additionalProperties");
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
    if (track.type == TrackType::Bezier) {
        return bezier_value(keys, std::clamp(time, 0.0, animation.length));
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
    if (target.component->name() == "Camera3D" && target.field == "current" &&
        target.part.empty() && value.is_bool() && value.as_bool()) {
        make_current_camera(world, entity);
    }
    return {};
}

void make_current_camera(world::World& world, entt::entity camera) {
    for (const auto [entity, other] : world.ecs().view<world::Camera3D>().each()) {
        if (entity != camera) {
            other.current = false;
        }
    }
}

} // namespace slash::animation
