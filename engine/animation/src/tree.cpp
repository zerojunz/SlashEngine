#include "slash/animation/tree.hpp"

#include "internal.hpp"

#include "slash/core/text.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>

namespace slash::animation {
namespace {

constexpr std::array<std::string_view, 14> kTypes = {
    "Animation", "BlendTree", "BlendSpace1D", "BlendSpace2D", "OneShot",
    "Add2",      "Add3",      "Blend2",       "Blend3",       "Sub2",
    "TimeScale", "TimeSeek",  "Transition",   "StateMachine"};
constexpr std::array<std::string_view, 5> kRootTypes = {"Animation", "BlendTree", "BlendSpace1D",
                                                        "BlendSpace2D", "StateMachine"};
constexpr std::array<std::string_view, 3> kLoopModes = {"none", "linear", "pingpong"};
constexpr std::array<std::string_view, 2> kPlayModes = {"forward", "backward"};
constexpr std::array<std::string_view, 3> kBlendModes = {"interpolated", "discrete",
                                                         "discrete_carry"};
constexpr std::array<std::string_view, 2> kMixModes = {"blend", "add"};
constexpr std::array<std::string_view, 4> kRequests = {"none", "fire", "abort", "fade_out"};
constexpr std::array<std::string_view, 3> kMachineTypes = {"root", "nested", "grouped"};
constexpr std::array<std::string_view, 3> kSwitchModes = {"immediate", "sync", "at_end"};
constexpr std::array<std::string_view, 3> kAdvanceModes = {"disabled", "enabled", "auto"};
constexpr std::string_view kStart = "Start";
constexpr std::string_view kEnd = "End";
// As Godot's AnimationNodeBlendSpace1D and 2D.
constexpr std::size_t kMaxBlendPoints = 64;

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

json::Value number(std::string_view description, double fallback) {
    json::Value out = property("number", description);
    out["default"] = fallback;
    return out;
}

json::Value boolean(std::string_view description, bool fallback) {
    json::Value out = property("boolean", description);
    out["default"] = fallback;
    return out;
}

template <std::size_t N>
json::Value choice(std::string_view description, const std::array<std::string_view, N>& list) {
    json::Value out = property("string", description);
    out["enum"] = names(list);
    out["default"] = list.front();
    return out;
}

json::Value pair(std::string_view description, double x, double y) {
    json::Value out = property("array", description);
    out["items"]["type"] = "number";
    out["minItems"] = 2;
    out["maxItems"] = 2;
    out["default"] = json::Array{x, y};
    return out;
}

json::Value curve(std::string_view description) {
    json::Value point;
    point["type"] = "array";
    point["items"]["type"] = "number";
    point["minItems"] = 2;
    point["maxItems"] = 2;
    json::Value out = property("array", description);
    out["items"] = std::move(point);
    return out;
}

bool is_root_type(std::string_view type) {
    return std::ranges::find(kRootTypes, type) != kRootTypes.end();
}

// The inputs of a node type with fixed inputs, as Godot names them.
std::vector<std::string> fixed_inputs(TreeNodeType type) {
    switch (type) {
    case TreeNodeType::OneShot:
        return {"in", "shot"};
    case TreeNodeType::Add2:
        return {"in", "add"};
    case TreeNodeType::Add3:
        return {"-add", "in", "+add"};
    case TreeNodeType::Blend2:
        return {"in", "blend"};
    case TreeNodeType::Blend3:
        return {"-blend", "in", "+blend"};
    case TreeNodeType::Sub2:
        return {"in", "sub"};
    case TreeNodeType::TimeScale:
    case TreeNodeType::TimeSeek:
        return {"in"};
    case TreeNodeType::Output:
        return {"output"};
    default:
        return {};
    }
}

bool has_sync(TreeNodeType type) {
    switch (type) {
    case TreeNodeType::OneShot:
    case TreeNodeType::Add2:
    case TreeNodeType::Add3:
    case TreeNodeType::Blend2:
    case TreeNodeType::Blend3:
    case TreeNodeType::Sub2:
    case TreeNodeType::Transition:
    case TreeNodeType::BlendSpace1D:
    case TreeNodeType::BlendSpace2D:
        return true;
    default:
        return false;
    }
}

TreeNodeType type_of(std::string_view name) {
    return static_cast<TreeNodeType>(index_of(kTypes, name));
}

// The schema of one node type; children refer to "#/$defs/node" or "#/$defs/root_node".
json::Value node_schema(TreeNodeType type) {
    json::Value out;
    out["type"] = "object";
    out["additionalProperties"] = false;
    out["required"] = json::Array{"type"};
    json::Value& properties = out["properties"];
    properties["type"]["const"] = tree_node_type_name(type);
    if (tree_node_has_filter(type)) {
        properties["filter_enabled"] =
            boolean("Blend only the tracks filters names; the rest pass through or stop, as "
                    "Godot's filters.",
                    false);
        properties["filters"]["type"] = "array";
        properties["filters"]["items"]["type"] = "string";
        properties["filters"]["description"] =
            "Track names: a model node's, a value track's path or \"@\" and an event track's "
            "event, after a target and \":\" for a track with one.";
    }
    if (has_sync(type)) {
        properties["sync"] =
            boolean("Keep inputs that count for nothing playing, as Godot's sync.", false);
    }
    const std::vector<std::string> inputs = fixed_inputs(type);
    if (!inputs.empty()) {
        json::Value& connections = properties["inputs"];
        connections["type"] = "object";
        connections["additionalProperties"] = false;
        connections["description"] = "The blend tree node connected to each input, by name.";
        json::Array required;
        for (const std::string& input : inputs) {
            connections["properties"][input]["type"] = "string";
            required.emplace_back(input);
        }
        connections["required"] = std::move(required);
        out["required"] = json::Array{"type", "inputs"};
    }
    switch (type) {
    case TreeNodeType::Animation:
        out["required"] = json::Array{"type", "animation"};
        properties["animation"] =
            property("string", "The animation it plays: its name in the default library, or "
                               "\"library/name\".");
        properties["animation"]["minLength"] = 1;
        properties["play_mode"] = choice("Play it forward or backward.", kPlayModes);
        properties["advance_on_start"] =
            boolean("Move on by a tick's time on the tick it starts, as Godot's.", false);
        properties["use_custom_timeline"] =
            boolean("Use timeline_length, start_offset, loop_mode and stretch_time_scale in "
                    "place of the animation's length and loop.",
                    false);
        properties["timeline_length"] = number("Seconds, with use_custom_timeline.", 1.0);
        properties["timeline_length"]["exclusiveMinimum"] = 0;
        properties["stretch_time_scale"] =
            boolean("Stretch the animation over timeline_length, rather than cut or pad it.", true);
        properties["start_offset"] = number("Seconds into the animation it starts at.", 0.0);
        properties["loop_mode"] = choice("How the custom timeline loops.", kLoopModes);
        break;
    case TreeNodeType::BlendTree:
        out["required"] = json::Array{"type", "output"};
        properties["nodes"]["type"] = "object";
        properties["nodes"]["additionalProperties"]["$ref"] = "#/$defs/node";
        properties["nodes"]["description"] = "Its nodes by name.";
        properties["output"] = property("string", "The node whose result it gives.");
        properties["output"]["minLength"] = 1;
        break;
    case TreeNodeType::BlendSpace1D:
    case TreeNodeType::BlendSpace2D: {
        const bool two = type == TreeNodeType::BlendSpace2D;
        json::Value point;
        point["type"] = "object";
        point["additionalProperties"] = false;
        point["required"] = json::Array{"node", "position"};
        point["properties"]["node"]["$ref"] = "#/$defs/root_node";
        point["properties"]["position"] =
            two ? pair("Where it is, [x, y].", 0, 0) : number("Where it is.", 0.0);
        point["properties"]["position"].as_object().erase("default");
        properties["points"]["type"] = "array";
        properties["points"]["maxItems"] = kMaxBlendPoints;
        properties["points"]["items"] = std::move(point);
        properties["points"]["description"] =
            "Nodes at positions; their parameters are named by their index.";
        properties["blend_mode"] = choice("Blend the points around the position, or play only the "
                                          "closest, starting it over or carrying on where the last "
                                          "one was.",
                                          kBlendModes);
        if (two) {
            properties["min_space"] = pair("The space's lowest corner, for editors.", -1, -1);
            properties["max_space"] = pair("The space's highest corner.", 1, 1);
            properties["snap"] = pair("The step positions snap to in editors.", 0.1, 0.1);
            properties["x_label"] = property("string", "What x stands for.");
            properties["x_label"]["default"] = "x";
            properties["y_label"] = property("string", "What y stands for.");
            properties["y_label"]["default"] = "y";
            properties["auto_triangles"] = boolean(
                "Join the points in triangles by Delaunay triangulation, as Godot's.", true);
            json::Value triangle;
            triangle["type"] = "array";
            triangle["items"]["type"] = "integer";
            triangle["items"]["minimum"] = 0;
            triangle["minItems"] = 3;
            triangle["maxItems"] = 3;
            properties["triangles"]["type"] = "array";
            properties["triangles"]["items"] = std::move(triangle);
            properties["triangles"]["description"] =
                "Triangles of three points' indexes, with auto_triangles false.";
        } else {
            properties["min_space"] = number("The space's lowest position, for editors.", -1.0);
            properties["max_space"] = number("The space's highest position.", 1.0);
            properties["snap"] = number("The step positions snap to in editors.", 0.1);
            properties["value_label"] = property("string", "What the position stands for.");
            properties["value_label"]["default"] = "value";
        }
        break;
    }
    case TreeNodeType::OneShot:
        properties["fadein_time"] = number("Seconds the shot fades in over.", 0.0);
        properties["fadein_time"]["minimum"] = 0;
        properties["fadein_curve"] = curve("The fade in's shape, [offset, value] points.");
        properties["fadeout_time"] = number("Seconds the shot fades out over.", 0.0);
        properties["fadeout_time"]["minimum"] = 0;
        properties["fadeout_curve"] = curve("The fade out's shape, [offset, value] points.");
        properties["break_loop_at_end"] =
            boolean("End a looping shot at the end of its loop.", false);
        properties["autorestart"] = boolean("Fire again after the shot ends.", false);
        properties["autorestart_delay"] = number("Seconds before it fires again.", 1.0);
        properties["autorestart_delay"]["minimum"] = 0;
        properties["autorestart_random_delay"] =
            number("Up to this many seconds more, at random.", 0.0);
        properties["autorestart_random_delay"]["minimum"] = 0;
        properties["mix_mode"] = choice("Blend the shot over the input, or add it.", kMixModes);
        break;
    case TreeNodeType::TimeSeek:
        properties["explicit_elapse"] =
            boolean("A seek counts the time it skips, as Godot's, so events between fire.", true);
        break;
    case TreeNodeType::Transition: {
        out["required"] = json::Array{"type", "inputs"};
        json::Value input;
        input["type"] = "object";
        input["additionalProperties"] = false;
        input["required"] = json::Array{"name", "node"};
        input["properties"]["name"] = property("string", "Its name, which requests name.");
        input["properties"]["name"]["minLength"] = 1;
        input["properties"]["node"] = property("string", "The blend tree node connected to it.");
        input["properties"]["auto_advance"] =
            boolean("Go on to the next input when this one ends.", false);
        input["properties"]["break_loop_at_end"] =
            boolean("With auto_advance, end a looping input at the end of its loop.", false);
        input["properties"]["reset"] = boolean("Start it over when it is switched to.", true);
        properties["inputs"]["type"] = "array";
        properties["inputs"]["minItems"] = 1;
        properties["inputs"]["items"] = std::move(input);
        properties["xfade_time"] = number("Seconds a switch cross-fades over.", 0.0);
        properties["xfade_time"]["minimum"] = 0;
        properties["xfade_curve"] = curve("The cross-fade's shape, [offset, value] points.");
        properties["allow_transition_to_self"] =
            boolean("A request for the current input starts it over.", false);
        break;
    }
    case TreeNodeType::StateMachine: {
        properties["state_machine_type"] =
            choice("A root state machine starts over when the tree does; a nested one carries on "
                   "until it ends; a grouped one is a state of another, which plays it as part of "
                   "itself.",
                   kMachineTypes);
        properties["allow_transition_to_self"] =
            boolean("A travel to the current state starts it over.", false);
        properties["reset_ends"] =
            boolean("Cross-fades from Start and to End blend with nothing playing; false leaves "
                    "them to the parent's own cross-fade.",
                    false);
        json::Value state;
        state["type"] = "object";
        state["additionalProperties"] = false;
        state["properties"]["node"]["$ref"] = "#/$defs/root_node";
        state["properties"]["position"] =
            pair("Where it is, [x, y]; travel goes the shortest way between positions.", 0, 0);
        properties["states"]["type"] = "object";
        properties["states"]["additionalProperties"] = std::move(state);
        properties["states"]["description"] =
            "Its states by name, each with a node. \"Start\" and \"End\" are its own and play "
            "nothing; they take only a position.";
        json::Value transition;
        transition["type"] = "object";
        transition["additionalProperties"] = false;
        transition["required"] = json::Array{"from", "to"};
        json::Value& fields = transition["properties"];
        fields["from"] = property("string", "The state it leaves, or \"Start\".");
        fields["to"] = property("string", "The state it goes to, or \"End\".");
        fields["switch_mode"] =
            choice("When it happens: at once, at once with the next state going on from where "
                   "the last one was, or when the current state ends.",
                   kSwitchModes);
        fields["advance_mode"] = choice(
            "Never taken, taken by travel, or also taken on its own when it can.", kAdvanceModes);
        fields["advance_mode"]["default"] = "enabled";
        fields["advance_condition"] =
            property("string", "With auto, the parameter \"conditions/<it>\" must be true for it "
                               "to be taken on its own.");
        fields["advance_condition"]["default"] = "";
        fields["xfade_time"] = number("Seconds the two states cross-fade over.", 0.0);
        fields["xfade_time"]["minimum"] = 0;
        fields["xfade_curve"] = curve("The cross-fade's shape, [offset, value] points.");
        fields["break_loop_at_end"] =
            boolean("With at_end, end a looping state at the end of its loop.", false);
        fields["reset"] = boolean("Start the next state over.", true);
        fields["priority"] = property("integer", "Lower goes first when several can be taken on "
                                                 "their own; travel counts it as cost.");
        fields["priority"]["minimum"] = 0;
        fields["priority"]["default"] = 1;
        properties["transitions"]["type"] = "array";
        properties["transitions"]["items"] = std::move(transition);
        properties["transitions"]["description"] = "The ways from one state to another.";
        break;
    }
    default:
        break;
    }
    return out;
}

json::Value schema() {
    json::Value node;
    json::Value root_node;
    for (std::size_t i = 0; i < kTypes.size(); ++i) {
        json::Value option = node_schema(static_cast<TreeNodeType>(i));
        if (is_root_type(kTypes.at(i))) {
            root_node["anyOf"].push_back(option);
        }
        node["anyOf"].push_back(std::move(option));
    }
    node["description"] = "A node of a blend tree.";
    root_node["description"] =
        "A tree's root, a blend space's point or a state machine's state: an "
        "Animation, BlendTree, blend space or StateMachine.";

    json::Value out;
    out["$schema"] = "https://json-schema.org/draft/2020-12/schema";
    out["title"] = "SlashEngine animation tree";
    out["description"] = "A tree of animation nodes that blends animations, as Godot's "
                         "AnimationTree. The AnimationTree component plays it.";
    out["type"] = "object";
    out["additionalProperties"] = false;
    out["required"] = json::Array{"format", "root"};
    out["properties"]["format"]["const"] = 1;
    out["properties"]["format"]["description"] = "Format version. Must be 1.";
    out["properties"]["root"]["$ref"] = "#/$defs/root_node";
    out["$defs"]["node"] = std::move(node);
    out["$defs"]["root_node"] = std::move(root_node);
    return out;
}

double number_of(const json::Value& node, std::string_view key, double fallback) {
    const json::Value* value = node.find(key);
    return value != nullptr ? value->as_double() : fallback;
}

bool bool_of(const json::Value& node, std::string_view key, bool fallback) {
    const json::Value* value = node.find(key);
    return value != nullptr ? value->as_bool() : fallback;
}

std::string string_of(const json::Value& node, std::string_view key, std::string_view fallback) {
    const json::Value* value = node.find(key);
    return value != nullptr ? value->as_string() : std::string(fallback);
}

Vec2 pair_of(const json::Value& node, std::string_view key, Vec2 fallback) {
    const json::Value* value = node.find(key);
    if (value == nullptr) {
        return fallback;
    }
    return {static_cast<float>(value->as_array()[0].as_double()),
            static_cast<float>(value->as_array()[1].as_double())};
}

TreeCurve curve_of(const json::Value& node, std::string_view key) {
    TreeCurve out;
    if (const json::Value* points = node.find(key); points != nullptr) {
        for (const json::Value& point : points->as_array()) {
            out.push_back({point.as_array()[0].as_double(), point.as_array()[1].as_double()});
        }
    }
    return out;
}

// Checks a document the schema accepted, collecting every error.
class Checker {
public:
    explicit Checker(const json::Value& doc) : doc_(doc) {}

    std::vector<json::ValidationError> run() {
        check_node(*doc_.find("root"), "/root", false);
        return std::move(errors_);
    }

private:
    void fail(std::string path, std::string message) {
        errors_.push_back({std::move(path), std::move(message)});
    }

    void check_curve(const json::Value& node, std::string_view key, const std::string& at) {
        const json::Value* points = node.find(key);
        if (points == nullptr) {
            return;
        }
        const json::Array& list = points->as_array();
        for (std::size_t i = 0; i < list.size(); ++i) {
            const double offset = list[i].as_array()[0].as_double();
            const std::string point_at = at + "/" + std::string(key) + "/" + std::to_string(i);
            if (offset < 0.0 || offset > 1.0) {
                fail(point_at + "/0", "a curve's offsets go from 0 to 1");
            } else if (i > 0 && offset < list[i - 1].as_array()[0].as_double()) {
                fail(point_at + "/0", "a curve's points go in order of offset");
            }
        }
    }

    // in_machine: the node is a state machine's state.
    void check_node(const json::Value& node, const std::string& at, bool in_machine) {
        const TreeNodeType type = type_of(node.find("type")->as_string());
        check_curve(node, "fadein_curve", at);
        check_curve(node, "fadeout_curve", at);
        check_curve(node, "xfade_curve", at);
        switch (type) {
        case TreeNodeType::BlendTree:
            check_blend_tree(node, at);
            break;
        case TreeNodeType::BlendSpace1D:
        case TreeNodeType::BlendSpace2D:
            check_blend_space(node, at, type == TreeNodeType::BlendSpace2D);
            break;
        case TreeNodeType::StateMachine:
            check_state_machine(node, at, in_machine);
            break;
        case TreeNodeType::Animation:
            if (!bool_of(node, "use_custom_timeline", false)) {
                for (const char* key :
                     {"timeline_length", "start_offset", "loop_mode", "stretch_time_scale"}) {
                    if (node.find(key) != nullptr) {
                        fail(at + "/" + key,
                             std::string(key) + " is only read with \"use_custom_timeline\": true");
                    }
                }
            }
            break;
        default:
            break;
        }
    }

    void check_blend_tree(const json::Value& tree, const std::string& at) {
        static const json::Object kEmpty;
        const json::Value* found = tree.find("nodes");
        const json::Object& nodes = found != nullptr ? found->as_object() : kEmpty;
        std::vector<std::string_view> names;
        names.reserve(nodes.size());
        for (const auto& [name, node] : nodes) {
            names.emplace_back(name);
        }
        const auto unknown = [&](const std::string& name) {
            std::string message = "the blend tree has no node \"" + name + "\"";
            if (const std::string_view hint = closest_match(name, names); !hint.empty()) {
                message += "; did you mean \"" + std::string(hint) + "\"?";
            }
            return message;
        };
        // Who each node's result goes to, as Godot's connect_node allows one.
        std::map<std::string, std::string, std::less<>> fed;
        const auto connect = [&](const std::string& from, const std::string& to,
                                 const std::string& to_at) {
            if (!nodes.contains(from)) {
                fail(to_at, unknown(from));
                return;
            }
            if (from == to) {
                fail(to_at, "a node can't be connected to itself");
                return;
            }
            if (const auto [it, added] = fed.emplace(from, to); !added) {
                fail(to_at, "\"" + from + "\" is already connected to " +
                                (it->second.empty() ? std::string("the output")
                                                    : "\"" + it->second + "\"") +
                                "; a node's result goes to one input");
            }
        };
        for (const auto& [name, node] : nodes) {
            const std::string node_at = at + "/nodes/" + json::pointer_escape(name);
            if (name.find('/') != std::string::npos) {
                fail(node_at, "node names can't have \"/\", which parameter names put between "
                              "node names");
            }
            if (name == "output") {
                fail(node_at, "\"output\" is the blend tree's own output; name the node "
                              "something else and give it as \"output\"");
            }
            const json::Value* inputs = node.find("inputs");
            if (inputs != nullptr && inputs->is_object()) {
                for (const auto& [input, from] : inputs->as_object()) {
                    connect(from.as_string(), name,
                            node_at + "/inputs/" + json::pointer_escape(input));
                }
            } else if (inputs != nullptr) {
                const json::Array& list = inputs->as_array();
                std::set<std::string, std::less<>> taken;
                for (std::size_t i = 0; i < list.size(); ++i) {
                    const std::string input_at = node_at + "/inputs/" + std::to_string(i);
                    const std::string& input = list[i].find("name")->as_string();
                    if (input.find_first_of("./") != std::string::npos) {
                        fail(input_at + "/name", R"(input names can't have "." or "/")");
                    } else if (!taken.insert(input).second) {
                        fail(input_at + "/name", "another input is named \"" + input + "\"");
                    }
                    connect(list[i].find("node")->as_string(), name, input_at + "/node");
                }
            }
            check_node(node, node_at, false);
        }
        const std::string& output = tree.find("output")->as_string();
        if (!nodes.contains(output)) {
            fail(at + "/output", unknown(output));
        } else {
            connect(output, "", at + "/output");
        }
        // A loop would never end.
        std::map<std::string, int, std::less<>> marks; // 1 visiting, 2 done
        std::function<bool(const std::string&)> visit = [&](const std::string& name) {
            int& mark = marks[name];
            if (mark == 1) {
                return true;
            }
            if (mark == 2) {
                return false;
            }
            mark = 1;
            const json::Value& node = nodes.find(name)->second;
            if (const json::Value* inputs = node.find("inputs"); inputs != nullptr) {
                const auto next = [&](const std::string& from) {
                    return nodes.contains(from) && from != name && visit(from);
                };
                if (inputs->is_object()) {
                    for (const auto& [input, from] : inputs->as_object()) {
                        if (next(from.as_string())) {
                            return true;
                        }
                    }
                } else {
                    for (const json::Value& input : inputs->as_array()) {
                        if (next(input.find("node")->as_string())) {
                            return true;
                        }
                    }
                }
            }
            marks[name] = 2;
            return false;
        };
        for (const auto& [name, node] : nodes) {
            if (marks[name] == 0 && visit(name)) {
                fail(at + "/nodes/" + json::pointer_escape(name),
                     "\"" + name + "\" is connected in a loop back to itself");
                break;
            }
        }
    }

    void check_blend_space(const json::Value& space, const std::string& at, bool two) {
        const auto axis_check = [&](int axis, const char* name) {
            const json::Value* low = space.find("min_space");
            const json::Value* high = space.find("max_space");
            const auto get = [&](const json::Value* value, double fallback) {
                if (value == nullptr) {
                    return fallback;
                }
                return two ? value->as_array()[static_cast<std::size_t>(axis)].as_double()
                           : value->as_double();
            };
            if (get(low, -1.0) >= get(high, 1.0)) {
                fail(at + (high != nullptr ? "/max_space" : "/min_space") +
                         (two ? "/" + std::to_string(axis) : std::string()),
                     std::string("max_space") + name + " must be more than min_space" + name);
            }
        };
        if (two) {
            axis_check(0, " x");
            axis_check(1, " y");
        } else {
            axis_check(0, "");
        }
        const json::Value* points = space.find("points");
        const std::size_t count = points != nullptr ? points->size() : 0;
        if (points != nullptr) {
            const json::Array& list = points->as_array();
            for (std::size_t i = 0; i < list.size(); ++i) {
                check_node(*list[i].find("node"), at + "/points/" + std::to_string(i) + "/node",
                           false);
            }
        }
        const json::Value* triangles = space.find("triangles");
        if (triangles == nullptr) {
            return;
        }
        if (bool_of(space, "auto_triangles", true)) {
            fail(at + "/triangles", "triangles are only read with \"auto_triangles\": false");
            return;
        }
        std::set<std::array<std::int64_t, 3>> seen;
        const json::Array& list = triangles->as_array();
        for (std::size_t i = 0; i < list.size(); ++i) {
            const std::string triangle_at = at + "/triangles/" + std::to_string(i);
            std::array<std::int64_t, 3> corners{};
            bool ok = true;
            for (std::size_t j = 0; j < 3; ++j) {
                corners.at(j) = list[i].as_array()[j].as_int();
                if (corners.at(j) >= static_cast<std::int64_t>(count)) {
                    fail(triangle_at + "/" + std::to_string(j),
                         "there is no point " + std::to_string(corners.at(j)) + "; the space has " +
                             std::to_string(count));
                    ok = false;
                }
            }
            std::ranges::sort(corners);
            if (ok && (corners[0] == corners[1] || corners[1] == corners[2])) {
                fail(triangle_at, "a triangle's three points are different points");
            } else if (ok && !seen.insert(corners).second) {
                fail(triangle_at, "another triangle has the same points");
            }
        }
    }

    void check_state_machine(const json::Value& machine, const std::string& at, bool in_machine) {
        if (string_of(machine, "state_machine_type", "root") == "grouped" && !in_machine) {
            fail(at + "/state_machine_type", "a grouped state machine is a state of another state "
                                             "machine, which plays it as part of itself");
        }
        static const json::Object kEmpty;
        const json::Value* found = machine.find("states");
        const json::Object& states = found != nullptr ? found->as_object() : kEmpty;
        std::vector<std::string_view> names = {kStart, kEnd};
        for (const auto& [name, state] : states) {
            if (name != kStart && name != kEnd) {
                names.emplace_back(name);
            }
        }
        const auto has = [&](const std::string& name) {
            return std::ranges::find(names, name) != names.end();
        };
        const auto unknown = [&](const std::string& name) {
            std::string message = "the state machine has no state \"" + name + "\"";
            if (const std::string_view hint = closest_match(name, names); !hint.empty()) {
                message += "; did you mean \"" + std::string(hint) + "\"?";
            }
            return message;
        };
        for (const auto& [name, state] : states) {
            const std::string state_at = at + "/states/" + json::pointer_escape(name);
            const json::Value* node = state.find("node");
            if (name == kStart || name == kEnd) {
                if (node != nullptr) {
                    fail(state_at + "/node", "\"" + name +
                                                 "\" is the state machine's own and "
                                                 "plays nothing; it takes only a position");
                }
                continue;
            }
            if (name.find('/') != std::string::npos) {
                fail(state_at, "state names can't have \"/\", which goes between a grouped state "
                               "machine's name and its states'");
            }
            if (node == nullptr) {
                fail(state_at, "a state needs a node, as {\"node\": {\"type\": \"Animation\", "
                               "\"animation\": \"Idle\"}}");
                continue;
            }
            check_node(*node, state_at + "/node", true);
        }
        // How many transitions go into and out of each state.
        std::map<std::string, int, std::less<>> into;
        std::map<std::string, int, std::less<>> out_of;
        std::set<std::pair<std::string, std::string>> seen;
        if (const json::Value* transitions = machine.find("transitions"); transitions != nullptr) {
            const json::Array& list = transitions->as_array();
            for (std::size_t i = 0; i < list.size(); ++i) {
                const std::string transition_at = at + "/transitions/" + std::to_string(i);
                const std::string& from = list[i].find("from")->as_string();
                const std::string& to = list[i].find("to")->as_string();
                bool ok = true;
                if (from == kEnd) {
                    fail(transition_at + "/from", "nothing leaves End");
                    ok = false;
                } else if (!has(from)) {
                    fail(transition_at + "/from", unknown(from));
                    ok = false;
                }
                if (to == kStart) {
                    fail(transition_at + "/to", "nothing goes to Start; the state machine's "
                                                "\"start\" parameter starts it over");
                    ok = false;
                } else if (!has(to)) {
                    fail(transition_at + "/to", unknown(to));
                    ok = false;
                }
                if (ok && from == to) {
                    fail(transition_at, "a transition goes from one state to another; with "
                                        "allow_transition_to_self, a travel to the current state "
                                        "starts it over");
                } else if (ok && !seen.emplace(from, to).second) {
                    fail(transition_at,
                         "another transition goes from \"" + from + "\" to \"" + to + "\"");
                }
                if (string_of(list[i], "advance_condition", "").find_first_of("/:") !=
                    std::string::npos) {
                    fail(transition_at + "/advance_condition",
                         "condition names can't have \"/\" or \":\"");
                }
                check_curve(list[i], "xfade_curve", transition_at);
                ++into[to];
                ++out_of[from];
            }
        }
        // A grouped state machine's transitions from its Start and to its End stand for those
        // into and out of it, one for one, as Godot's.
        for (const auto& [name, state] : states) {
            const json::Value* node = state.find("node");
            if (node == nullptr || node->find("type")->as_string() != "StateMachine" ||
                string_of(*node, "state_machine_type", "root") != "grouped") {
                continue;
            }
            int from_start = 0;
            int to_end = 0;
            if (const json::Value* inner = node->find("transitions"); inner != nullptr) {
                for (const json::Value& transition : inner->as_array()) {
                    from_start += transition.find("from")->as_string() == kStart ? 1 : 0;
                    to_end += transition.find("to")->as_string() == kEnd ? 1 : 0;
                }
            }
            const std::string state_at = at + "/states/" + json::pointer_escape(name);
            if (from_start != into[name]) {
                fail(state_at, "the transitions into a grouped state machine go on through those "
                               "from its Start, so there are as many of each; \"" +
                                   name + "\" has " + std::to_string(into[name]) + " into it and " +
                                   std::to_string(from_start) + " from its Start");
            }
            if (to_end != out_of[name]) {
                fail(state_at, "the transitions to a grouped state machine's End go on through "
                               "those out of it, so there are as many of each; \"" +
                                   name + "\" has " + std::to_string(to_end) + " to its End and " +
                                   std::to_string(out_of[name]) + " out of it");
            }
        }
    }

    const json::Value& doc_;
    std::vector<json::ValidationError> errors_;
};

// Builds the flat list of nodes from a checked document.
class Builder {
public:
    explicit Builder(AnimationTreeResource& out) : out_(out) {}

    int add(const json::Value& node, std::string name, std::string path, int parent) {
        const int index = static_cast<int>(out_.nodes.size());
        out_.nodes.emplace_back();
        {
            TreeNode& made = out_.nodes.back();
            made.type = type_of(node.find("type")->as_string());
            made.name = std::move(name);
            made.path = std::move(path);
            made.parent = parent;
            fill(made, node);
        }
        const TreeNodeType type = out_.nodes[static_cast<std::size_t>(index)].type;
        if (type == TreeNodeType::BlendTree) {
            add_blend_tree(index, node);
        } else if (type == TreeNodeType::BlendSpace1D || type == TreeNodeType::BlendSpace2D) {
            add_blend_space(index, node, type == TreeNodeType::BlendSpace2D);
        } else if (type == TreeNodeType::StateMachine) {
            add_state_machine(index, node);
        }
        return index;
    }

private:
    static void fill(TreeNode& made, const json::Value& node) {
        made.animation = string_of(node, "animation", "");
        made.play_mode = static_cast<PlayMode>(
            index_of(kPlayModes, string_of(node, "play_mode", kPlayModes[0])));
        made.advance_on_start = bool_of(node, "advance_on_start", false);
        made.use_custom_timeline = bool_of(node, "use_custom_timeline", false);
        made.timeline_length = number_of(node, "timeline_length", 1.0);
        made.stretch_time_scale = bool_of(node, "stretch_time_scale", true);
        made.start_offset = number_of(node, "start_offset", 0.0);
        made.loop_mode = static_cast<LoopMode>(
            index_of(kLoopModes, string_of(node, "loop_mode", kLoopModes[0])));
        made.filter_enabled = bool_of(node, "filter_enabled", false);
        if (const json::Value* filters = node.find("filters"); filters != nullptr) {
            for (const json::Value& filter : filters->as_array()) {
                made.filters.push_back(filter.as_string());
            }
        }
        made.sync = bool_of(node, "sync", false);
        made.fadein_time = number_of(node, "fadein_time", 0.0);
        made.fadeout_time = number_of(node, "fadeout_time", 0.0);
        made.fadein_curve = curve_of(node, "fadein_curve");
        made.fadeout_curve = curve_of(node, "fadeout_curve");
        made.break_loop_at_end = bool_of(node, "break_loop_at_end", false);
        made.autorestart = bool_of(node, "autorestart", false);
        made.autorestart_delay = number_of(node, "autorestart_delay", 1.0);
        made.autorestart_random_delay = number_of(node, "autorestart_random_delay", 0.0);
        made.mix_mode =
            static_cast<MixMode>(index_of(kMixModes, string_of(node, "mix_mode", kMixModes[0])));
        made.explicit_elapse = bool_of(node, "explicit_elapse", true);
        made.xfade_time = number_of(node, "xfade_time", 0.0);
        made.xfade_curve = curve_of(node, "xfade_curve");
        made.allow_transition_to_self = bool_of(node, "allow_transition_to_self", false);
        if (made.type == TreeNodeType::Transition) {
            for (const json::Value& input : node.find("inputs")->as_array()) {
                made.transitions.push_back(
                    {.name = input.find("name")->as_string(),
                     .auto_advance = bool_of(input, "auto_advance", false),
                     .break_loop_at_end = bool_of(input, "break_loop_at_end", false),
                     .reset = bool_of(input, "reset", true)});
            }
        }
        made.blend_mode = static_cast<BlendMode>(
            index_of(kBlendModes, string_of(node, "blend_mode", kBlendModes[0])));
        if (made.type == TreeNodeType::BlendSpace2D) {
            made.min_space = pair_of(node, "min_space", made.min_space);
            made.max_space = pair_of(node, "max_space", made.max_space);
            made.snap = pair_of(node, "snap", made.snap);
            made.x_label = string_of(node, "x_label", "x");
            made.y_label = string_of(node, "y_label", "y");
            made.auto_triangles = bool_of(node, "auto_triangles", true);
        } else if (made.type == TreeNodeType::BlendSpace1D) {
            made.min_space.x = static_cast<float>(number_of(node, "min_space", -1.0));
            made.max_space.x = static_cast<float>(number_of(node, "max_space", 1.0));
            made.snap.x = static_cast<float>(number_of(node, "snap", 0.1));
            made.x_label = string_of(node, "value_label", "value");
        }
        made.state_machine_type = static_cast<StateMachineType>(
            index_of(kMachineTypes, string_of(node, "state_machine_type", kMachineTypes[0])));
        made.reset_ends = bool_of(node, "reset_ends", false);
        if (const json::Value* transitions = node.find("transitions");
            transitions != nullptr && made.type == TreeNodeType::StateMachine) {
            for (const json::Value& t : transitions->as_array()) {
                made.machine_transitions.push_back(
                    {.from = t.find("from")->as_string(),
                     .to = t.find("to")->as_string(),
                     .advance_condition = string_of(t, "advance_condition", ""),
                     .xfade_time = number_of(t, "xfade_time", 0.0),
                     .xfade_curve = curve_of(t, "xfade_curve"),
                     .priority = static_cast<int>(number_of(t, "priority", 1.0)),
                     .switch_mode = static_cast<SwitchMode>(
                         index_of(kSwitchModes, string_of(t, "switch_mode", kSwitchModes[0]))),
                     .advance_mode = static_cast<AdvanceMode>(
                         index_of(kAdvanceModes, string_of(t, "advance_mode", "enabled"))),
                     .break_loop_at_end = bool_of(t, "break_loop_at_end", false),
                     .reset = bool_of(t, "reset", true)});
            }
        }
    }

    void add_blend_tree(int index, const json::Value& node) {
        const std::string base = out_.nodes[static_cast<std::size_t>(index)].path;
        std::map<std::string, int, std::less<>> by_name;
        std::vector<std::pair<int, const json::Value*>> made;
        if (const json::Value* nodes = node.find("nodes"); nodes != nullptr) {
            for (const auto& [name, child] : nodes->as_object()) {
                const int child_index = add(child, name, base + name + "/", index);
                by_name.emplace(name, child_index);
                made.emplace_back(child_index, &child);
                out_.nodes[static_cast<std::size_t>(index)].children.push_back(child_index);
            }
        }
        const int output = static_cast<int>(out_.nodes.size());
        out_.nodes.emplace_back();
        {
            TreeNode& made_output = out_.nodes.back();
            made_output.type = TreeNodeType::Output;
            made_output.name = "output";
            made_output.path = base + "output/";
            made_output.parent = index;
            made_output.inputs.push_back(by_name.at(node.find("output")->as_string()));
        }
        out_.nodes[static_cast<std::size_t>(index)].children.push_back(output);
        // Connections, now every node has its index.
        for (const auto& [child_index, child] : made) {
            TreeNode& to = out_.nodes[static_cast<std::size_t>(child_index)];
            const json::Value* inputs = child->find("inputs");
            if (inputs == nullptr) {
                continue;
            }
            if (inputs->is_object()) {
                for (const std::string& input : fixed_inputs(to.type)) {
                    to.inputs.push_back(by_name.at(inputs->find(input)->as_string()));
                }
            } else {
                for (const json::Value& input : inputs->as_array()) {
                    to.inputs.push_back(by_name.at(input.find("node")->as_string()));
                }
            }
        }
    }

    void add_blend_space(int index, const json::Value& node, bool two) {
        const std::string base = out_.nodes[static_cast<std::size_t>(index)].path;
        if (const json::Value* points = node.find("points"); points != nullptr) {
            const json::Array& list = points->as_array();
            for (std::size_t i = 0; i < list.size(); ++i) {
                const std::string name = std::to_string(i);
                const int child = add(*list[i].find("node"), name, base + name + "/", index);
                TreeNode& space = out_.nodes[static_cast<std::size_t>(index)];
                space.children.push_back(child);
                const json::Value& position = *list[i].find("position");
                space.positions.push_back(
                    two ? Vec2{static_cast<float>(position.as_array()[0].as_double()),
                               static_cast<float>(position.as_array()[1].as_double())}
                        : Vec2{static_cast<float>(position.as_double()), 0.0F});
            }
        }
        TreeNode& space = out_.nodes[static_cast<std::size_t>(index)];
        if (two && space.auto_triangles) {
            space.triangles = detail::delaunay(space.positions);
        } else if (const json::Value* triangles = node.find("triangles"); triangles != nullptr) {
            for (const json::Value& triangle : triangles->as_array()) {
                std::array<int, 3> corners{};
                for (std::size_t j = 0; j < 3; ++j) {
                    corners.at(j) = static_cast<int>(triangle.as_array()[j].as_int());
                }
                std::ranges::sort(corners);
                space.triangles.push_back(corners);
            }
        }
    }

    // Its states by name, Start and End among them at Godot's positions, and their nodes.
    void add_state_machine(int index, const json::Value& node) {
        const std::string base = out_.nodes[static_cast<std::size_t>(index)].path;
        std::map<std::string, MachineState, std::less<>> states;
        states.emplace(kStart, MachineState{.name = std::string(kStart), .position = {200, 100}});
        states.emplace(kEnd, MachineState{.name = std::string(kEnd), .position = {900, 100}});
        if (const json::Value* list = node.find("states"); list != nullptr) {
            for (const auto& [name, state] : list->as_object()) {
                MachineState& made = states[name];
                made.name = name;
                made.position = pair_of(state, "position", made.position);
                if (const json::Value* child = state.find("node"); child != nullptr) {
                    made.node = add(*child, name, base + name + "/", index);
                }
            }
        }
        TreeNode& machine = out_.nodes[static_cast<std::size_t>(index)];
        for (auto& [name, state] : states) {
            if (state.node >= 0) {
                machine.children.push_back(state.node);
            }
            machine.states.push_back(std::move(state));
        }
    }

    AnimationTreeResource& out_;
};

json::Value zero_pair() {
    return json::Array{0.0, 0.0};
}

void add_parameters(const AnimationTreeResource& tree, int index, std::vector<TreeParameter>& out) {
    const TreeNode& node = tree.nodes[static_cast<std::size_t>(index)];
    const auto add = [&](std::string_view name, json::Value fallback, bool input,
                         std::string_view doc) {
        out.push_back({.key = node.path + std::string(name),
                       .name = std::string(name),
                       .node = index,
                       .default_value = std::move(fallback),
                       .input = input,
                       .doc = std::string(doc)});
    };
    add("current_length", 0.0, false, "Seconds the node's result lasts.");
    add("current_position", 0.0, false, "Seconds into the node's result.");
    add("current_delta", 0.0, false, "Seconds the node moved on in the last tick.");
    switch (node.type) {
    case TreeNodeType::Animation:
        add("backward", false, false, "Whether a pingpong loop is on its way back.");
        break;
    case TreeNodeType::OneShot:
        add("active", false, false, "Whether the shot plays.");
        add("internal_active", false, false, "Whether the shot plays and isn't fading out.");
        add("request", "none", true,
            "\"fire\" plays the shot, \"abort\" stops it, \"fade_out\" fades it out; the tree "
            "sets it back to \"none\".");
        add("fade_in_remaining", 0.0, false, "Seconds of fade in left.");
        add("fade_out_remaining", 0.0, false, "Seconds of fade out left.");
        add("time_to_restart", -1.0, false, "Seconds before autorestart fires, or -1.");
        break;
    case TreeNodeType::Add2:
    case TreeNodeType::Add3:
        add("add_amount", 0.0, true, "How much of the added input to add.");
        break;
    case TreeNodeType::Blend2:
    case TreeNodeType::Blend3:
        add("blend_amount", 0.0, true, "How far to blend toward the other input.");
        break;
    case TreeNodeType::Sub2:
        add("sub_amount", 0.0, true, "How much of the subtracted input to take away.");
        break;
    case TreeNodeType::TimeScale:
        add("scale", 1.0, true, "How fast its input plays; negative plays backward.");
        break;
    case TreeNodeType::TimeSeek:
        add("seek_request", -1.0, true,
            "Seconds to jump its input to; the tree sets it back to -1.");
        break;
    case TreeNodeType::Transition:
        add("current_state", "", false, "The input playing.");
        add("transition_request", "", true,
            "An input's name to switch to; the tree sets it back to \"\".");
        add("current_index", -1, false, "The index of the input playing.");
        add("prev_index", -1, false, "The input fading out, or -1.");
        add("prev_xfading", 0.0, false, "Seconds of cross-fade left.");
        break;
    case TreeNodeType::BlendSpace1D:
        add("blend_position", 0.0, true, "Where in the space to blend.");
        add("closest", -1, false, "The point playing in a discrete blend mode.");
        break;
    case TreeNodeType::BlendSpace2D:
        add("blend_position", zero_pair(), true, "Where in the space to blend, [x, y].");
        add("closest", -1, false, "The point playing in a discrete blend mode.");
        break;
    case TreeNodeType::StateMachine: {
        add("playback", json::Object{}, false,
            "The state playing, the one fading out and the travel path, as Godot's playback.");
        if (node.state_machine_type != StateMachineType::Grouped) {
            add("travel", "", true,
                "A state to go to by the cheapest way through transitions, or to jump to if "
                "there is none; \"group/state\" goes into a grouped state machine. The tree sets "
                "it back to \"\".");
            add("start", "", true,
                "A state to jump to and start over; the tree sets it back to \"\".");
            add("next", false, true,
                "true takes the next transition now, without waiting for at_end; the tree sets "
                "it back to false.");
            add("stop", false, true,
                "true plays nothing for a tick and leaves the state machine stopped, so the next "
                "travel sets out from Start; the tree sets it back to false.");
        }
        std::set<std::string, std::less<>> conditions;
        for (const MachineTransition& transition : node.machine_transitions) {
            if (!transition.advance_condition.empty()) {
                conditions.insert(transition.advance_condition);
            }
        }
        for (const std::string& condition : conditions) {
            add("conditions/" + condition, false, true,
                "Whether the auto transitions with advance_condition \"" + condition +
                    "\" can be taken.");
        }
        break;
    }
    default:
        break;
    }
    for (const int child : node.children) {
        add_parameters(tree, child, out);
    }
}

// The error in a path of states from a state machine, or "": its states, then those of the grouped
// state machines it goes into.
std::string check_state_path(const AnimationTreeResource& tree, int machine,
                             const std::string& path) {
    std::string walked;
    std::size_t begin = 0;
    while (true) {
        const std::size_t slash = path.find('/', begin);
        const std::string name =
            path.substr(begin, slash == std::string::npos ? std::string::npos : slash - begin);
        const TreeNode& node = tree.nodes[static_cast<std::size_t>(machine)];
        std::vector<std::string_view> names;
        const MachineState* found = nullptr;
        for (const MachineState& state : node.states) {
            names.emplace_back(state.name);
            if (state.name == name) {
                found = &state;
            }
        }
        if (found == nullptr) {
            std::string message =
                (walked.empty() ? std::string("the state machine") : "\"" + walked + "\"") +
                " has no state \"" + name + "\"";
            if (const std::string_view hint = closest_match(name, names); !hint.empty()) {
                message += "; did you mean \"" + std::string(hint) + "\"?";
            }
            return message;
        }
        if (!walked.empty() && (name == kStart || name == kEnd)) {
            return "a grouped state machine's " + name +
                   " isn't played on its own; play the state before or after the group";
        }
        if (slash == std::string::npos) {
            return {};
        }
        walked += (walked.empty() ? "" : "/") + name;
        const TreeNode* child =
            found->node >= 0 ? &tree.nodes[static_cast<std::size_t>(found->node)] : nullptr;
        if (child == nullptr || child->type != TreeNodeType::StateMachine ||
            child->state_machine_type != StateMachineType::Grouped) {
            return "\"" + walked + "\" isn't a grouped state machine, so a path can't go into it";
        }
        machine = found->node;
        begin = slash + 1;
    }
}

} // namespace

std::string_view tree_node_type_name(TreeNodeType type) {
    if (type == TreeNodeType::Output) {
        return "Output";
    }
    return kTypes.at(static_cast<std::size_t>(type));
}

std::vector<std::string> tree_node_input_names(const TreeNode& node) {
    if (node.type == TreeNodeType::Transition) {
        std::vector<std::string> out;
        out.reserve(node.transitions.size());
        for (const TransitionInput& input : node.transitions) {
            out.push_back(input.name);
        }
        return out;
    }
    return fixed_inputs(node.type);
}

bool tree_node_has_filter(TreeNodeType type) {
    switch (type) {
    case TreeNodeType::OneShot:
    case TreeNodeType::Add2:
    case TreeNodeType::Add3:
    case TreeNodeType::Blend2:
    case TreeNodeType::Sub2:
        return true;
    default:
        return false;
    }
}

json::Value tree_schema() {
    return schema();
}

std::vector<json::ValidationError> validate_tree(const json::Value& doc) {
    std::vector<json::ValidationError> errors = json::validate(doc, schema());
    if (!errors.empty()) {
        return errors;
    }
    return Checker(doc).run();
}

Result<AnimationTreeResource> parse_tree(const json::Value& doc) {
    const std::vector<json::ValidationError> errors = validate_tree(doc);
    if (!errors.empty()) {
        const json::ValidationError& first = errors.front();
        return make_error(ErrorCode::InvalidArgument,
                          (first.path.empty() ? std::string("root") : first.path) + ": " +
                              first.message);
    }
    AnimationTreeResource tree;
    Builder(tree).add(*doc.find("root"), "", "", -1);
    return tree;
}

std::vector<TreeParameter> tree_parameters(const AnimationTreeResource& tree) {
    std::vector<TreeParameter> out;
    if (!tree.nodes.empty()) {
        add_parameters(tree, 0, out);
    }
    return out;
}

std::string check_parameter(const AnimationTreeResource& tree, const TreeParameter& parameter,
                            const json::Value& value) {
    const TreeNode& node = tree.nodes[static_cast<std::size_t>(parameter.node)];
    if (parameter.name == "request") {
        if (!value.is_string() ||
            std::ranges::find(kRequests, value.as_string()) == kRequests.end()) {
            std::string message = R"(expected "none", "fire", "abort" or "fade_out")";
            if (value.is_string()) {
                if (const std::string_view hint = closest_match(value.as_string(), kRequests);
                    !hint.empty()) {
                    message += "; did you mean \"" + std::string(hint) + "\"?";
                }
            }
            return message;
        }
        return {};
    }
    if (node.type == TreeNodeType::StateMachine &&
        (parameter.name == "travel" || parameter.name == "start")) {
        if (!value.is_string()) {
            return "expected the name of one of the state machine's states";
        }
        return value.as_string().empty()
                   ? std::string()
                   : check_state_path(tree, parameter.node, value.as_string());
    }
    if (parameter.name == "transition_request") {
        if (!value.is_string()) {
            return "expected the name of one of the Transition's inputs";
        }
        const std::vector<std::string> inputs = tree_node_input_names(node);
        if (!value.as_string().empty() &&
            std::ranges::find(inputs, value.as_string()) == inputs.end()) {
            std::string message = "the Transition has no input \"" + value.as_string() + "\"";
            if (const std::string_view hint = closest_match(value.as_string(), inputs);
                !hint.empty()) {
                message += "; did you mean \"" + std::string(hint) + "\"?";
            }
            return message;
        }
        return {};
    }
    if (parameter.default_value.is_array()) {
        if (!value.is_array() || value.size() != 2 || !value.as_array()[0].is_number() ||
            !value.as_array()[1].is_number()) {
            return "expected [x, y]";
        }
        return {};
    }
    if (parameter.default_value.is_bool()) {
        return value.is_bool() ? std::string() : "expected true or false";
    }
    if (parameter.default_value.is_string()) {
        return value.is_string() ? std::string() : "expected a string";
    }
    return value.is_number() ? std::string() : "expected a number";
}

std::string value_track_name(const Track& track) {
    return track.target.empty() ? track.path : track.target + ":" + track.path;
}

std::string event_track_name(const Track& track) {
    return track.target.empty() ? "@" + track.event : track.target + ":@" + track.event;
}

std::vector<json::ValidationError> check_trees(const json::Value& doc,
                                               const world::FileReader& reader) {
    std::vector<json::ValidationError> errors;
    if (!reader) {
        return errors;
    }
    detail::CheckedFiles files(reader);
    std::map<std::string, Result<AnimationTreeResource>, std::less<>> trees;
    const auto load_tree = [&](const std::string& path) -> const Result<AnimationTreeResource>& {
        if (const auto it = trees.find(path); it != trees.end()) {
            return it->second;
        }
        const auto parse = [&]() -> Result<AnimationTreeResource> {
            auto text = reader(path);
            if (!text) {
                return text.error();
            }
            auto parsed = json::parse(text.value());
            if (!parsed) {
                return parsed.error();
            }
            return parse_tree(parsed.value());
        };
        return trees.emplace(path, parse()).first->second;
    };
    const auto check = [&](const json::Value& components, const std::string& at, bool extends) {
        const json::Value* component = components.find("AnimationTree");
        if (component == nullptr || !component->is_object()) {
            return;
        }
        const json::Value* root = component->find("tree_root");
        if (root == nullptr || !root->is_string() || root->as_string().empty()) {
            return; // checked in the prefab or scene it comes from
        }
        const std::string& path = root->as_string();
        if (!path.ends_with(kTreeSuffix)) {
            errors.push_back({at + "/tree_root", "animation trees are .animtree.json files, as "
                                                 "\"anim/hero.animtree.json\""});
            return;
        }
        const Result<AnimationTreeResource>& tree = load_tree(path);
        if (!tree) {
            errors.push_back({at + "/tree_root", path + ": " + tree.error().message});
            return;
        }
        // The animations of its libraries, by their names in the tree.
        std::map<std::string, const Animation*, std::less<>> animations;
        std::set<std::string, std::less<>> tracks;
        std::set<std::string, std::less<>> nodes_moved;
        std::set<std::string, std::less<>> targeted;
        const json::Value* libraries = component->find("libraries");
        if (libraries != nullptr && libraries->is_object()) {
            for (const auto& [name, file] : libraries->as_object()) {
                const std::string library_at = at + "/libraries/" + json::pointer_escape(name);
                if (!file.is_string()) {
                    continue;
                }
                const std::string& library_path = file.as_string();
                if (!library_path.ends_with(kAnimationSuffix) && !is_model_library(library_path)) {
                    errors.push_back({library_at, "animation libraries are .anim.json files or "
                                                  "models, as \"anim/hero.anim.json\" or "
                                                  "\"models/hero.glb\""});
                    continue;
                }
                if (name.find('/') != std::string::npos) {
                    errors.push_back({library_at, "library names can't have \"/\", which goes "
                                                  "between a library's name and an animation's"});
                    continue;
                }
                const Result<AnimationLibrary>& library = files.library(library_path);
                if (!library) {
                    errors.push_back({library_at, library_path + ": " + library.error().message});
                    continue;
                }
                for (const auto& [animation_name, animation] : library.value().animations) {
                    std::string key = name;
                    if (!key.empty()) {
                        key += '/';
                    }
                    key += animation_name;
                    animations.emplace(std::move(key), &animation);
                    for (const Track& track : animation.tracks) {
                        tracks.insert(track.path.empty() ? event_track_name(track)
                                                         : value_track_name(track));
                        if (!track.target.empty()) {
                            targeted.insert(track.target);
                        }
                    }
                    if (const scene3d::ModelAnimation* clip =
                            files.clip(animation.from.model, animation.from.animation);
                        clip != nullptr) {
                        const Result<scene3d::Model>& model = files.model(animation.from.model);
                        for (const scene3d::NodeTrack& track : clip->tracks) {
                            if (track.node < model.value().nodes.size()) {
                                const std::string& node = model.value().nodes[track.node].name;
                                tracks.insert(node);
                                nodes_moved.insert(node);
                            }
                        }
                    }
                }
            }
        }
        // A deterministic tree sets value tracks no animation plays to their RESET values, or to
        // zero without one, as Godot's does.
        const json::Value* deterministic = component->find("deterministic");
        if (deterministic == nullptr || !deterministic->is_bool() || deterministic->as_bool()) {
            const auto reset = animations.find("RESET");
            std::set<std::string, std::less<>> keyed;
            if (reset != animations.end()) {
                for (const Track& track : reset->second->tracks) {
                    if (!track.path.empty() && !track.keys.empty()) {
                        keyed.insert(value_track_name(track));
                    }
                }
            }
            std::map<std::string, json::Value, std::less<>> unkeyed;
            for (const auto& [name, animation] : animations) {
                for (const Track& track : animation->tracks) {
                    if (track.path.empty() || track.keys.empty()) {
                        continue;
                    }
                    const std::string track_name = value_track_name(track);
                    if (const json::Value zero = detail::zero_of(track.keys[0].value);
                        !zero.is_null() && !keyed.contains(track_name)) {
                        unkeyed.try_emplace(track_name, zero);
                    }
                }
            }
            for (const auto& [track_name, zero] : unkeyed) {
                errors.push_back(
                    {at + "/libraries",
                     track_name +
                         " has no key in a RESET animation in the library \"\"; a "
                         "deterministic tree sets it to " +
                         json::write(zero, json::Style::Compact) +
                         " while no animation plays it, as Godot's does: add a RESET animation "
                         "with its rest value, or set deterministic to false"});
            }
        }
        std::vector<std::string_view> animation_names;
        animation_names.reserve(animations.size());
        for (const auto& [name, animation] : animations) {
            animation_names.emplace_back(name);
        }
        const std::vector<std::string_view> track_names(tracks.begin(), tracks.end());
        // The nodes' animations and filters, by the JSON Pointer of the node in the tree file.
        const AnimationTreeResource& resource = tree.value();
        const std::function<std::string(int)> pointer_of = [&](int index) -> std::string {
            const TreeNode& node = resource.nodes[static_cast<std::size_t>(index)];
            if (node.parent < 0) {
                return "/root";
            }
            const TreeNode& parent = resource.nodes[static_cast<std::size_t>(node.parent)];
            if (parent.type == TreeNodeType::BlendTree) {
                return pointer_of(node.parent) + "/nodes/" + json::pointer_escape(node.name);
            }
            if (parent.type == TreeNodeType::StateMachine) {
                return pointer_of(node.parent) + "/states/" + json::pointer_escape(node.name) +
                       "/node";
            }
            return pointer_of(node.parent) + "/points/" + node.name + "/node";
        };
        for (std::size_t i = 0; i < resource.nodes.size(); ++i) {
            const TreeNode& node = resource.nodes[i];
            if (node.type == TreeNodeType::Animation && !animations.contains(node.animation)) {
                std::string message = path + ": " + pointer_of(static_cast<int>(i)) +
                                      "/animation: the libraries have no animation \"" +
                                      node.animation + "\"";
                if (const std::string_view hint = closest_match(node.animation, animation_names);
                    !hint.empty()) {
                    message += "; did you mean \"" + std::string(hint) + "\"?";
                }
                errors.push_back({at + "/libraries", std::move(message)});
            }
            for (std::size_t j = 0; j < node.filters.size(); ++j) {
                if (tracks.contains(node.filters[j])) {
                    continue;
                }
                std::string message = path + ": " + pointer_of(static_cast<int>(i)) + "/filters/" +
                                      std::to_string(j) + ": no animation has a track \"" +
                                      node.filters[j] + "\"";
                if (const std::string_view hint = closest_match(node.filters[j], track_names);
                    !hint.empty()) {
                    message += "; did you mean \"" + std::string(hint) + "\"?";
                }
                errors.push_back({at + "/libraries", std::move(message)});
            }
        }
        if (const json::Value* track = component->find("root_motion_track");
            track != nullptr && track->is_string() && !track->as_string().empty() &&
            !nodes_moved.contains(track->as_string())) {
            const std::vector<std::string_view> moved(nodes_moved.begin(), nodes_moved.end());
            std::string message =
                "no animation from a model moves a node named \"" + track->as_string() + "\"";
            if (const std::string_view hint = closest_match(track->as_string(), moved);
                !hint.empty()) {
                message += "; did you mean \"" + std::string(hint) + "\"?";
            }
            errors.push_back({at + "/root_motion_track", std::move(message)});
        }
        const std::vector<TreeParameter> parameters = tree_parameters(resource);
        std::vector<std::string_view> inputs;
        for (const TreeParameter& parameter : parameters) {
            if (parameter.input) {
                inputs.emplace_back(parameter.key);
            }
        }
        if (const json::Value* values = component->find("parameters");
            values != nullptr && values->is_object()) {
            for (const auto& [key, value] : values->as_object()) {
                const std::string value_at = at + "/parameters/" + json::pointer_escape(key);
                const auto found = std::ranges::find(parameters, key, &TreeParameter::key);
                if (found == parameters.end() || !found->input) {
                    std::string message;
                    if (found == parameters.end()) {
                        message.append(path).append(" has no parameter \"").append(key) += '"';
                        if (const std::string_view hint = closest_match(key, inputs);
                            !hint.empty()) {
                            message.append("; did you mean \"").append(hint) += "\"?";
                        }
                    } else {
                        message.append("\"").append(key) +=
                            "\" is kept by the tree as it plays, in state";
                    }
                    errors.push_back({value_at, std::move(message)});
                    continue;
                }
                if (std::string problem = check_parameter(resource, *found, value);
                    !problem.empty()) {
                    errors.push_back({value_at, std::move(problem)});
                }
            }
        }
        const json::Value* targets = component->find("targets");
        if (extends && targets == nullptr) {
            return;
        }
        std::vector<std::string_view> bound;
        if (targets != nullptr && targets->is_object()) {
            for (const auto& [name, entity] : targets->as_object()) {
                bound.emplace_back(name);
            }
        }
        for (const std::string& name : targeted) {
            if (std::ranges::find(bound, name) != bound.end()) {
                continue;
            }
            std::string message = "no entity for target \"" + name +
                                  "\", which the libraries' tracks name; add it to targets";
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
                      "/entities/" + json::pointer_escape(id) + "/components/AnimationTree",
                      entity.find("extends") != nullptr || id.find('/') != std::string::npos);
            }
        }
    } else if (const json::Value* components = doc.find("components"); components != nullptr) {
        check(*components, "/components/AnimationTree", doc.find("extends") != nullptr);
    }
    return errors;
}

} // namespace slash::animation
