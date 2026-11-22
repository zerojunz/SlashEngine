#include "slash/procedural/texture_graph.hpp"

#include "json_fields.hpp"

#include "slash/core/expression.hpp"
#include "slash/core/text.hpp"
#include "slash/procedural/gradient.hpp"
#include "slash/procedural/noise.hpp"
#include "slash/world/ragdoll.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>

namespace slash::procedural {
namespace {

using detail::boolean_field;
using detail::integer_field;
using detail::number_field;
using render::GraphOp;

constexpr std::string_view kGraphType = "graph";
// How many files deep "extends" may go.
constexpr int kMaxExtends = 8;
constexpr std::int64_t kDefaultSize = 512;

std::atomic<render::RenderDevice*> g_graph_device{nullptr};

std::string quoted(std::string_view text) {
    return "\"" + std::string(text) + "\"";
}

// ---- Ops ----

struct OpSpec {
    std::string_view name;
    GraphOp op;
    // The node's fields naming the nodes it reads, as in0 to in3.
    std::array<std::string_view, 4> inputs;
    // How many of the first inputs must be given.
    int required = 0;
    std::string_view doc;
};

constexpr std::array<OpSpec, 29> kOps{{
    {"constant", GraphOp::Constant, {}, 0, "One color everywhere."},
    {"noise",
     GraphOp::Noise,
     {},
     0,
     "Godot's FastNoiseLite noise as a NoiseTexture2D makes it: gray, value * 0.5 + 0.5."},
    {"gradient",
     GraphOp::Gradient,
     {},
     0,
     "Godot's GradientTexture2D: a gradient from fill_from to fill_to (UV)."},
    {"shape", GraphOp::Shape, {}, 0, "A disc, box, polygon or ring in each cell. Gray."},
    {"bricks",
     GraphOp::Bricks,
     {},
     0,
     "Bricks. Red: height (0 in the mortar, rising over the bevel to 1); green: a random value "
     "per brick; blue: an ID per brick from 0 to 1."},
    {"checker", GraphOp::Checker, {}, 0, "A checkerboard. Gray."},
    {"waves", GraphOp::Waves, {}, 0, "Parallel bands. Gray."},
    {"image", GraphOp::Image, {}, 0, "An image file, stretched over the texture."},
    {"levels", GraphOp::Levels, {"input"}, 1, "Input and output levels with a gamma, on RGB."},
    {"invert", GraphOp::Invert, {"input"}, 1, "RGB = 1 - RGB."},
    {"clamp", GraphOp::Clamp, {"input"}, 1, "Every channel held to 0-1."},
    {"gradient_map",
     GraphOp::GradientMap,
     {"input"},
     1,
     "The input's luminance through a gradient."},
    {"hsv", GraphOp::Hsv, {"input"}, 1, "Hue shifted, saturation and value scaled."},
    {"blur", GraphOp::Blur, {"input"}, 1, "A blur of some radius in pixels."},
    {"sharpen", GraphOp::Sharpen, {"input"}, 1, "Sharpened against its four neighbours."},
    {"warp",
     GraphOp::Warp,
     {"input", "by"},
     2,
     "The input moved by another image: down its slope, or along an angle by its red."},
    {"transform",
     GraphOp::Transform,
     {"input"},
     1,
     "The input repeated, turned about the center and moved."},
    {"edge_detect", GraphOp::EdgeDetect, {"input"}, 1, "The Sobel edges of the luminance."},
    {"grayscale", GraphOp::Grayscale, {"input"}, 1, "RGB = luminance."},
    {"threshold", GraphOp::Threshold, {"input"}, 1, "Luminance above a level, softened."},
    {"posterize", GraphOp::Posterize, {"input"}, 1, "RGB in a number of steps."},
    {"merge",
     GraphOp::Merge,
     {"red", "green", "blue", "alpha"},
     0,
     "Channels from other nodes or numbers, as an ORM or mask texture packs them."},
    {"swizzle", GraphOp::Channels, {"input"}, 1, "The input's channels reordered."},
    {"normal_from_height",
     GraphOp::NormalFromHeight,
     {"height"},
     1,
     "Tangent-space normals from a height's red, as 0.5 + 0.5 n, green up (OpenGL)."},
    {"ao_from_height",
     GraphOp::AoFromHeight,
     {"height"},
     1,
     "Ambient occlusion from a height's red. Gray."},
    {"curvature_from_height",
     GraphOp::CurvatureFromHeight,
     {"height"},
     1,
     "Curvature from a height's red: 0.5 flat, higher on ridges. Gray."},
    {"blend",
     GraphOp::Blend,
     {"base", "layer", "mask"},
     2,
     "A layer over a base, through a mask's red. Alpha is the base's."},
    {"math", GraphOp::Math, {"a", "b"}, 1, "Math on every channel, with b or a value."},
    {"remap", GraphOp::Remap, {"input"}, 1, "RGB from one range to another."},
}};

const OpSpec* find_op(std::string_view name) {
    for (const OpSpec& spec : kOps) {
        if (spec.name == name) {
            return &spec;
        }
    }
    return nullptr;
}

std::vector<std::string_view> op_names() {
    std::vector<std::string_view> names;
    names.reserve(kOps.size());
    for (const OpSpec& spec : kOps) {
        names.push_back(spec.name);
    }
    return names;
}

// Names of a setting's values, in the order the shader numbers them.
constexpr std::array<std::string_view, 3> kFills{"linear", "radial", "square"};
constexpr std::array<std::string_view, 3> kRepeats{"none", "repeat", "mirror"};
constexpr std::array<std::string_view, 4> kShapes{"disc", "box", "polygon", "ring"};
constexpr std::array<std::string_view, 3> kWaveShapes{"sine", "triangle", "square"};
constexpr std::array<std::string_view, 2> kWarpModes{"slope", "direction"};
constexpr std::array<std::string_view, 11> kBlendModes{
    "mix",    "add",     "subtract",   "multiply",   "screen", "overlay",
    "darken", "lighten", "difference", "soft_light", "divide"};
constexpr std::array<std::string_view, 10> kMathOps{
    "add", "subtract", "multiply", "divide", "min", "max", "pow", "abs", "fract", "one_minus"};
constexpr std::array<std::string_view, 2> kOutputKinds{"color", "gray"};

template <std::size_t N>
json::Value names_field(const std::array<std::string_view, N>& names, std::string_view fallback,
                        std::string_view description) {
    json::Value field;
    field["type"] = "string";
    json::Array values;
    for (const std::string_view name : names) {
        values.emplace_back(name);
    }
    field["enum"] = std::move(values);
    field["default"] = fallback;
    field["description"] = description;
    return field;
}

json::Value numbers_field(std::span<const float> fallback, std::string_view description) {
    json::Value field;
    field["type"] = "array";
    field["items"]["type"] = "number";
    field["minItems"] = static_cast<std::int64_t>(fallback.size());
    field["maxItems"] = static_cast<std::int64_t>(fallback.size());
    json::Array values;
    for (const float value : fallback) {
        values.emplace_back(value);
    }
    field["default"] = std::move(values);
    field["description"] = description;
    return field;
}

json::Value pair_field(float x, float y, std::string_view description) {
    const std::array<float, 2> fallback{x, y};
    return numbers_field(fallback, description);
}

json::Value node_field(std::string_view description) {
    json::Value field;
    field["type"] = "string";
    field["description"] = description;
    return field;
}

// A node's schema: its op, the nodes it reads and its settings, as they are once parameters
// and expressions are worked out.
json::Value op_schema(const OpSpec& spec) {
    json::Value schema;
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["required"] = json::Array{"op"};
    schema["description"] = spec.doc;
    json::Value& p = schema["properties"];
    p["op"]["const"] = spec.name;
    p["op"]["description"] = "What the node does.";
    json::Array required{"op"};
    for (int k = 0; k < 4; ++k) {
        const std::string_view input = spec.inputs.at(static_cast<std::size_t>(k));
        if (input.empty() || spec.op == GraphOp::Merge) {
            continue;
        }
        p[input] = node_field(k < spec.required ? "The node read." : "The node read, if any.");
        if (k < spec.required) {
            required.emplace_back(input);
        }
    }
    schema["required"] = std::move(required);
    switch (spec.op) {
    case GraphOp::Constant: {
        const std::array<float, 4> black{0.0F, 0.0F, 0.0F, 1.0F};
        p["color"] = numbers_field(black, "The color, RGBA from 0 to 1.");
        break;
    }
    case GraphOp::Noise:
        p["noise"] = noise_schema();
        p["noise"]["description"] = "The FastNoiseLite settings, by Godot's names.";
        p["scale"] = pair_field(0.0F, 0.0F,
                                "Noise units across and down: the image's size in pixels when "
                                "left out, as a NoiseTexture2D samples it.");
        p["scale"].as_object().erase("default");
        p["seamless"] = boolean_field(false, "Make it tile as Godot's get_seamless_image does.");
        p["seamless_blend_skirt"] =
            number_field(0.1F, "The share of the image blended over the seams.", 0.0, 1.0);
        break;
    case GraphOp::Gradient:
        p["fill"] = names_field(kFills, "linear", "The gradient's shape.");
        p["fill_from"] = pair_field(0.0F, 0.0F, "Where the gradient starts (UV).");
        p["fill_to"] = pair_field(1.0F, 0.0F, "Where the gradient ends (UV).");
        p["repeat"] = names_field(kRepeats, "none", "What happens past fill_to.");
        p["gradient"] = gradient_schema();
        break;
    case GraphOp::Shape:
        p["kind"] = names_field(kShapes, "disc", "The shape.");
        p["cells"] = pair_field(1.0F, 1.0F, "Cells across and down, a shape in each.");
        p["size"] = number_field(0.8F, "The shape's size, a share of its cell.", 0.0, 1.0);
        p["sides"] = integer_field(6, "A polygon's sides.", 3, 64);
        p["softness"] = number_field(0.05F, "The share of the size its edge fades over.", 0.0, 1.0);
        p["rotation"] = number_field(0.0F, "Rotation in turns.");
        p["thickness"] = number_field(0.2F, "A ring's thickness, a share of the size.", 0.0, 1.0);
        break;
    case GraphOp::Bricks:
        p["across"] = number_field(4.0F, "Bricks across.", 1.0, 256.0);
        p["down"] = number_field(8.0F, "Rows down.", 1.0, 256.0);
        p["mortar"] = number_field(0.05F, "Mortar width, a share of a brick.", 0.0, 1.0);
        p["offset"] = number_field(0.5F, "Each row's shift, a share of a brick.");
        p["bevel"] = number_field(0.1F, "Bevel width, a share of a brick.", 0.0, 1.0);
        p["seed"] = integer_field(0, "Seed of the per-brick random values.");
        break;
    case GraphOp::Checker:
        p["cells"] = pair_field(8.0F, 8.0F, "Squares across and down.");
        break;
    case GraphOp::Waves:
        p["count"] = number_field(8.0F, "Bands across the image.");
        p["angle"] = number_field(0.0F, "Their direction in turns.");
        p["shape"] = names_field(kWaveShapes, "sine", "Each band's profile.");
        p["phase"] = number_field(0.0F, "Shift in turns of a band.");
        break;
    case GraphOp::Image:
        p["path"] =
            node_field("The image file (PNG, JPEG, KTX2 or another texture graph's output).");
        schema["required"].as_array().emplace_back("path");
        break;
    case GraphOp::Levels:
        p["in"] = pair_field(0.0F, 1.0F, "Input low and high.");
        p["gamma"] = number_field(1.0F, "Gamma: above 1 brightens the midtones.", 0.0001, 100.0);
        p["out"] = pair_field(0.0F, 1.0F, "Output low and high.");
        break;
    case GraphOp::GradientMap:
        p["gradient"] = gradient_schema();
        break;
    case GraphOp::Hsv:
        p["hue"] = number_field(0.0F, "Hue shift in turns.");
        p["saturation"] = number_field(1.0F, "Saturation times.");
        p["value"] = number_field(1.0F, "Value times.");
        break;
    case GraphOp::Blur:
        p["radius"] = number_field(2.0F, "Radius in pixels.", 0.0, 64.0);
        break;
    case GraphOp::Sharpen:
        p["amount"] = number_field(1.0F, "How much sharper.");
        break;
    case GraphOp::Warp:
        p["strength"] = number_field(0.05F, "How far it moves, in UV.");
        p["mode"] = names_field(kWarpModes, "slope",
                                "slope: down by's slope; direction: along angle by by's red.");
        p["angle"] = number_field(0.0F, "The direction in turns, for direction mode.");
        break;
    case GraphOp::Transform:
        p["repeat"] = pair_field(1.0F, 1.0F, "Copies across and down.");
        p["rotation"] = number_field(0.0F, "Rotation in turns about the center.");
        p["offset"] = pair_field(0.0F, 0.0F, "Shift in UV.");
        break;
    case GraphOp::EdgeDetect:
        p["strength"] = number_field(1.0F, "Edges times.");
        break;
    case GraphOp::Threshold:
        p["level"] = number_field(0.5F, "The luminance where it turns white.");
        p["softness"] = number_field(0.05F, "Half the width of the change.", 0.0, 1.0);
        break;
    case GraphOp::Posterize:
        p["steps"] = integer_field(4, "Steps a channel.", 2, 256);
        break;
    case GraphOp::Merge:
        for (const std::string_view channel : spec.inputs) {
            p[channel]["description"] =
                "A node (its red), {\"node\": name, \"channel\": \"r\", \"g\", \"b\" or \"a\"}, "
                "or a number. Left out: 0, alpha 1.";
        }
        break;
    case GraphOp::Channels:
        p["channels"] = node_field("Four of r, g, b, a, 0 and 1: the output's channels, as "
                                   "\"rrr1\" for a gray image of the red.");
        p["channels"]["default"] = "rgba";
        break;
    case GraphOp::NormalFromHeight:
        p["strength"] = number_field(1.0F, "How steep the height is.");
        break;
    case GraphOp::AoFromHeight:
        p["radius"] = number_field(8.0F, "How far it looks, in pixels.", 1.0, 64.0);
        p["strength"] = number_field(1.0F, "How dark.");
        break;
    case GraphOp::CurvatureFromHeight:
        p["strength"] = number_field(1.0F, "Curvature times.");
        break;
    case GraphOp::Blend:
        p["mode"] = names_field(kBlendModes, "mix", "How the layer combines with the base.");
        p["opacity"] = number_field(1.0F, "The layer's opacity.", 0.0, 1.0);
        break;
    case GraphOp::Math:
        p["operation"] = names_field(kMathOps, "add", "The operation, a then b.");
        p["value"]["description"] =
            "b where b is left out: a number for every channel, or four numbers.";
        break;
    case GraphOp::Remap:
        p["from"] = pair_field(0.0F, 1.0F, "The range in.");
        p["to"] = pair_field(0.0F, 1.0F, "The range out.");
        p["clamp"] = boolean_field(false, "Hold the result inside to.");
        break;
    case GraphOp::Invert:
    case GraphOp::Clamp:
    case GraphOp::Grayscale:
        break;
    }
    return schema;
}

json::Value top_schema() {
    json::Value schema;
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["required"] = json::Array{"format", "type"};
    schema["description"] =
        "A texture graph: nodes over whole images, baked in a compute pass, with named outputs.";
    json::Value& p = schema["properties"];
    p["format"]["const"] = 1;
    p["format"]["description"] = "Format version. Must be 1.";
    p["type"]["const"] = kGraphType;
    p["type"]["description"] = "\"graph\" for a texture graph.";
    p["extends"] = node_field("A graph file this one is a JSON merge patch over.");
    p["width"] = integer_field(kDefaultSize, "Width in pixels.", 1, render::kMaxGraphSize);
    p["height"] = integer_field(kDefaultSize, "Height in pixels.", 1, render::kMaxGraphSize);
    p["tiling"] = boolean_field(true, "Whether it tiles: filters wrap across the edges, else "
                                      "they clamp at them.");
    json::Value& parameters = p["parameters"];
    parameters["type"] = "object";
    parameters["description"] =
        "Values settings name as \"$name\" or in \"=\" expressions: a number, a list of numbers "
        "(a color or point), or an expression over the other parameters.";
    parameters["additionalProperties"]["type"] = json::Array{"number", "array", "string"};
    json::Value& nodes = p["nodes"];
    nodes["type"] = "object";
    nodes["description"] = "The nodes by name, each with its op (see $defs).";
    nodes["additionalProperties"]["type"] = "object";
    nodes["additionalProperties"]["required"] = json::Array{"op"};
    nodes["additionalProperties"]["properties"]["op"]["type"] = "string";
    json::Value& outputs = p["outputs"];
    outputs["type"] = "array";
    outputs["minItems"] = 1;
    outputs["description"] = "The textures the graph makes; a path names one after \"::\", and "
                             "the first when it names none.";
    json::Value& output = outputs["items"];
    output["type"] = "object";
    output["additionalProperties"] = false;
    output["required"] = json::Array{"name", "node"};
    output["properties"]["name"] =
        node_field("The output's name, as in \"wall.texture.json::normal\".");
    output["properties"]["node"] = node_field("The node it is.");
    output["properties"]["as"] = names_field(
        kOutputKinds, "color", "color: the node's RGBA; gray: its red in every color channel.");
    return schema;
}

// ---- Reading ----

bool is_identifier(std::string_view name) {
    if (name.empty() || (name.front() >= '0' && name.front() <= '9')) {
        return false;
    }
    return std::ranges::all_of(name, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               c == '_';
    });
}

float number(const json::Value& node, std::string_view key, float fallback) {
    const json::Value* value = node.find(key);
    return value != nullptr && value->is_number() ? static_cast<float>(value->as_double())
                                                  : fallback;
}

std::array<float, 2> pair(const json::Value& node, std::string_view key,
                          std::array<float, 2> fallback) {
    const json::Value* value = node.find(key);
    if (value == nullptr || !value->is_array() || value->as_array().size() != 2) {
        return fallback;
    }
    return {static_cast<float>(value->as_array()[0].as_double()),
            static_cast<float>(value->as_array()[1].as_double())};
}

template <std::size_t N>
float named(const json::Value& node, std::string_view key,
            const std::array<std::string_view, N>& names) {
    const json::Value* value = node.find(key);
    if (value == nullptr || !value->is_string()) {
        return 0.0F;
    }
    for (std::size_t i = 0; i < N; ++i) {
        if (names.at(i) == value->as_string()) {
            return static_cast<float>(i);
        }
    }
    return 0.0F;
}

// A merge channel: the node it reads and which of its channels, or a number.
struct ChannelSource {
    std::string node;
    int channel = 0;
    float value = 0.0F;
};

int channel_index(std::string_view name) {
    constexpr std::string_view kNames = "rgba";
    const std::size_t at = name.size() == 1 ? kNames.find(name.front()) : std::string_view::npos;
    return at == std::string_view::npos ? -1 : static_cast<int>(at);
}

// ---- Checking and compiling ----

// Works through a graph document: its parameters, then each node's settings with them, the
// links between nodes and the outputs. Collects every error.
class Graph {
public:
    Graph(const json::Value& doc, std::vector<json::ValidationError>& errors)
        : doc_(doc), errors_(errors) {}

    // Checks the document; resolved_ then holds each node's settings with parameters and
    // expressions replaced. complete: the document must have nodes and outputs (it extends
    // nothing, or was merged with what it extends).
    void check(const json::Object& overrides, bool complete) {
        if (!doc_.is_object()) {
            errors_.push_back({"", "expected an object"});
            return;
        }
        for (json::ValidationError& error : json::validate(doc_, top_schema())) {
            errors_.push_back(std::move(error));
        }
        if (!errors_.empty()) {
            return;
        }
        read_parameters(overrides);
        const json::Value* nodes = doc_.find("nodes");
        if (nodes != nullptr) {
            for (const auto& [name, node] : nodes->as_object()) {
                node_names_.push_back(name);
            }
            for (const auto& [name, node] : nodes->as_object()) {
                check_node(name, node);
            }
        }
        const json::Value* outputs = doc_.find("outputs");
        if (complete && (nodes == nullptr || outputs == nullptr)) {
            errors_.push_back({nodes == nullptr ? "/nodes" : "/outputs",
                               "missing: a graph that extends nothing names its nodes and "
                               "outputs"});
            return;
        }
        if (outputs != nullptr) {
            check_outputs(*outputs);
        }
        if (errors_.empty() && outputs != nullptr) {
            order_nodes(*outputs);
        }
    }

    [[nodiscard]] const std::map<std::string, json::Value, std::less<>>& resolved() const {
        return resolved_;
    }
    [[nodiscard]] const std::vector<std::string>& order() const { return order_; }

private:
    // ---- Parameters ----

    void read_parameters(const json::Object& overrides) {
        if (const json::Value* parameters = doc_.find("parameters"); parameters != nullptr) {
            for (const auto& [name, value] : parameters->as_object()) {
                const std::string at = "/parameters/" + json::pointer_escape(name);
                if (!is_identifier(name) || kReserved.contains(name)) {
                    errors_.push_back({at, "isn't a parameter name: use letters, digits and "
                                           "'_', not starting with a digit"});
                    continue;
                }
                raw_[name] = value;
                names_.push_back(name);
            }
        }
        for (const auto& [name, value] : overrides) {
            const std::string at = "/parameters/" + json::pointer_escape(name);
            const auto found = raw_.find(name);
            if (found == raw_.end()) {
                std::string message = "isn't a parameter of the graph";
                if (const std::string_view near = closest_match(name, names_); !near.empty()) {
                    message += " (did you mean " + quoted(near) + "?)";
                }
                errors_.push_back({at, std::move(message)});
                continue;
            }
            if (value.is_array() != found->second.is_array() ||
                (value.is_array() && value.as_array().size() != found->second.as_array().size())) {
                errors_.push_back(
                    {at, "must be of the parameter's kind: " +
                             std::string(found->second.is_array()
                                             ? "a list of " +
                                                   std::to_string(found->second.as_array().size()) +
                                                   " numbers"
                                             : "a number")});
                continue;
            }
            found->second = value;
        }
        for (const std::string& name : names_) {
            (void)parameter(name);
        }
    }

    std::optional<json::Value> parameter(const std::string& name) {
        if (const auto found = values_.find(name); found != values_.end()) {
            return found->second;
        }
        const std::string at = "/parameters/" + json::pointer_escape(name);
        if (std::ranges::find(stack_, name) != stack_.end()) {
            errors_.push_back({at, "depends on itself"});
            values_[name] = std::nullopt;
            return std::nullopt;
        }
        const json::Value& value = raw_.at(name);
        std::optional<json::Value> out;
        if (value.is_number()) {
            out = json::Value(value.as_double());
        } else if (value.is_array() &&
                   std::ranges::all_of(value.as_array(),
                                       [](const json::Value& v) { return v.is_number(); })) {
            out = value;
        } else if (value.is_string() && value.as_string().starts_with('=')) {
            stack_.push_back(name);
            out = evaluate(value.as_string(), at);
            stack_.pop_back();
        } else {
            errors_.push_back(
                {at, "expected a number, a list of numbers or an expression starting with '='"});
        }
        values_[name] = out;
        return out;
    }

    std::optional<json::Value> evaluate(const std::string& text, const std::string& at) {
        auto parsed = Expression::parse(std::string_view(text).substr(1));
        if (!parsed) {
            errors_.push_back({at, "expression " + quoted(text) + ": " + parsed.error().message});
            return std::nullopt;
        }
        for (const std::string& name : parsed.value().names()) {
            if (std::ranges::find(names_, name) != names_.end()) {
                if (!parameter(name)) {
                    return std::nullopt;
                }
                continue;
            }
            std::string message = "expression " + quoted(text) + " reads " + quoted(name) +
                                  ", which isn't a parameter";
            if (const std::string_view near = closest_match(name, names_); !near.empty()) {
                message += " (did you mean " + quoted(near) + "?)";
            }
            errors_.push_back({at, std::move(message)});
            return std::nullopt;
        }
        auto result =
            parsed.value().execute([&](std::string_view name) -> std::optional<json::Value> {
                const auto found = values_.find(name);
                return found == values_.end() ? std::nullopt : found->second;
            });
        if (!result) {
            errors_.push_back({at, "expression " + quoted(text) + ": " + result.error().message});
            return std::nullopt;
        }
        if (!result.value().is_number()) {
            errors_.push_back({at, "expression " + quoted(text) + " isn't a number"});
            return std::nullopt;
        }
        return json::Value(result.value().as_double());
    }

    // A setting with its "$name" and "=expression" strings worked out, in place.
    void resolve(json::Value& value, const std::string& at) {
        if (value.is_string()) {
            const std::string& text = value.as_string();
            if (text.starts_with('$')) {
                const std::string name = text.substr(1);
                if (std::ranges::find(names_, name) == names_.end()) {
                    std::string message = quoted(text) + " names no parameter";
                    if (const std::string_view near = closest_match(name, names_); !near.empty()) {
                        message += " (did you mean \"$" + std::string(near) + "\"?)";
                    }
                    errors_.push_back({at, std::move(message)});
                    return;
                }
                const std::optional<json::Value> found = parameter(name);
                value = found ? *found : json::Value(0.0);
            } else if (text.starts_with('=')) {
                const std::optional<json::Value> found = evaluate(text, at);
                value = found ? *found : json::Value(0.0);
            }
            return;
        }
        if (value.is_array()) {
            auto& items = value.as_array();
            for (std::size_t i = 0; i < items.size(); ++i) {
                resolve(items[i], at + "/" + std::to_string(i));
            }
            return;
        }
        if (value.is_object()) {
            for (auto& [key, item] : value.as_object()) {
                resolve(item, at + "/" + json::pointer_escape(key));
            }
        }
    }

    // ---- Nodes ----

    void link(const std::string& at, const json::Value& value) {
        if (!value.is_string()) {
            errors_.push_back({at, "expected a node's name"});
            return;
        }
        if (std::ranges::find(node_names_, value.as_string()) == node_names_.end()) {
            std::string message = quoted(value.as_string()) + " isn't a node";
            if (const std::string_view near = closest_match(value.as_string(), node_names_);
                !near.empty()) {
                message += " (did you mean " + quoted(near) + "?)";
            }
            errors_.push_back({at, std::move(message)});
        }
    }

    void check_node(const std::string& name, const json::Value& node) {
        const std::string at = "/nodes/" + json::pointer_escape(name);
        const std::string& op = node.find("op")->as_string();
        const OpSpec* spec = find_op(op);
        if (spec == nullptr) {
            std::string message = "unknown op " + quoted(op);
            if (const std::string_view near = closest_match(op, op_names()); !near.empty()) {
                message += " (did you mean " + quoted(near) + "?)";
            }
            errors_.push_back({at + "/op", std::move(message)});
            return;
        }
        json::Value resolved = node;
        resolve(resolved, at);
        for (json::ValidationError& error : json::validate(resolved, op_schema(*spec))) {
            error.path = at + error.path;
            errors_.push_back(std::move(error));
        }
        if (spec->op == GraphOp::Merge) {
            for (const std::string_view channel : spec->inputs) {
                if (const json::Value* source = resolved.find(channel); source != nullptr) {
                    check_channel(at + "/" + std::string(channel), *source);
                }
            }
        } else {
            for (const std::string_view input : spec->inputs) {
                if (const json::Value* value = resolved.find(input);
                    !input.empty() && value != nullptr) {
                    link(at + "/" + std::string(input), *value);
                }
            }
        }
        if (const json::Value* noise = resolved.find("noise"); noise != nullptr) {
            for (json::ValidationError& error : validate_noise(*noise, at + "/noise")) {
                errors_.push_back(std::move(error));
            }
        }
        if (const json::Value* gradient = resolved.find("gradient"); gradient != nullptr) {
            check_gradient(at + "/gradient", *gradient);
        }
        if (const json::Value* channels = resolved.find("channels"); channels != nullptr) {
            const std::string& text = channels->is_string() ? channels->as_string() : "";
            if (text.size() != 4 || !std::ranges::all_of(text, [](char c) {
                    return std::string_view("rgba01").find(c) != std::string_view::npos;
                })) {
                errors_.push_back({at + "/channels", "expected four of r, g, b, a, 0 and 1"});
            }
        }
        if (const json::Value* value = resolved.find("value"); value != nullptr) {
            const bool four_numbers =
                value->is_array() && value->as_array().size() == 4 &&
                std::ranges::all_of(value->as_array(),
                                    [](const json::Value& v) { return v.is_number(); });
            if (!value->is_number() && !four_numbers) {
                errors_.push_back({at + "/value", "expected a number or four numbers"});
            }
        }
        resolved_[name] = std::move(resolved);
    }

    void check_channel(const std::string& at, const json::Value& source) {
        if (source.is_number()) {
            return;
        }
        if (source.is_string()) {
            link(at, source);
            return;
        }
        if (source.is_object() && source.find("node") != nullptr) {
            link(at + "/node", *source.find("node"));
            const json::Value* channel = source.find("channel");
            if (channel != nullptr &&
                (!channel->is_string() || channel_index(channel->as_string()) < 0)) {
                errors_.push_back({at + "/channel", R"(expected "r", "g", "b" or "a")"});
            }
            for (const auto& [key, value] : source.as_object()) {
                if (key != "node" && key != "channel") {
                    errors_.push_back({at + "/" + json::pointer_escape(key),
                                       R"(unknown property: a channel has "node" and "channel")"});
                }
            }
            return;
        }
        errors_.push_back(
            {at, R"(expected a node's name, {"node": name, "channel": "r"} or a number)"});
    }

    void check_gradient(const std::string& at, const json::Value& value) {
        for (json::ValidationError& error : validate_gradient(value, at)) {
            errors_.push_back(std::move(error));
        }
        auto gradient = parse_gradient(value);
        if (!gradient) {
            return;
        }
        if (gradient.value().points.size() > render::kMaxGraphRampPoints) {
            errors_.push_back({at + "/points", "a graph's gradient holds at most " +
                                                   std::to_string(render::kMaxGraphRampPoints) +
                                                   " points"});
        }
        if (gradient.value().interpolation_mode != GradientInterpolation::Linear ||
            gradient.value().interpolation_color_space != GradientColorSpace::Srgb) {
            errors_.push_back(
                {at, "a graph's gradient blends linearly in sRGB (interpolation_mode \"linear\", "
                     "interpolation_color_space \"srgb\")"});
        }
    }

    // ---- Outputs and order ----

    void check_outputs(const json::Value& outputs) {
        std::set<std::string, std::less<>> seen;
        const auto& items = outputs.as_array();
        for (std::size_t i = 0; i < items.size(); ++i) {
            const std::string at = "/outputs/" + std::to_string(i);
            const std::string& name = items[i].find("name")->as_string();
            if (name.empty() || name.find("::") != std::string::npos || !seen.insert(name).second) {
                errors_.push_back({at + "/name", "expected a name of its own, without \"::\""});
            }
            link(at + "/node", *items[i].find("node"));
        }
    }

    // The nodes the outputs need, each after the nodes it reads (depth first, in the outputs'
    // order and then each node's inputs' order).
    void order_nodes(const json::Value& outputs) {
        std::set<std::string, std::less<>> done;
        std::vector<std::string> path;
        for (const json::Value& output : outputs.as_array()) {
            visit(output.find("node")->as_string(), done, path);
        }
    }

    static std::vector<std::string> inputs_of(const OpSpec& spec, const json::Value& node) {
        std::vector<std::string> out;
        for (const std::string_view input : spec.inputs) {
            const json::Value* value = input.empty() ? nullptr : node.find(input);
            if (value == nullptr) {
                continue;
            }
            if (value->is_string()) {
                out.push_back(value->as_string());
            } else if (value->is_object() && value->find("node") != nullptr) {
                out.push_back(value->find("node")->as_string());
            }
        }
        return out;
    }

    void visit(const std::string& name, std::set<std::string, std::less<>>& done,
               std::vector<std::string>& path) {
        if (done.contains(name)) {
            return;
        }
        if (std::ranges::find(path, name) != path.end()) {
            std::string chain;
            for (const std::string& step : path) {
                chain += step + " -> ";
            }
            errors_.push_back(
                {"/nodes/" + json::pointer_escape(name), "reads itself through " + chain + name});
            done.insert(name);
            return;
        }
        path.push_back(name);
        const json::Value& node = resolved_.at(name);
        for (const std::string& input : inputs_of(*find_op(node.find("op")->as_string()), node)) {
            visit(input, done, path);
        }
        path.pop_back();
        done.insert(name);
        order_.push_back(name);
    }

    static inline const std::set<std::string, std::less<>> kReserved = {
        "PI", "TAU", "INF", "NAN", "true", "false", "null", "and", "or", "not"};

    const json::Value& doc_;
    std::vector<json::ValidationError>& errors_;
    std::map<std::string, json::Value, std::less<>> raw_;
    std::vector<std::string> names_;
    std::map<std::string, std::optional<json::Value>, std::less<>> values_;
    std::vector<std::string> stack_;
    std::vector<std::string> node_names_;
    std::map<std::string, json::Value, std::less<>> resolved_;
    std::vector<std::string> order_;
};

// The document with what it extends merged under it.
Result<json::Value> merged_document(const json::Value& doc, const GraphReader& reader,
                                    std::vector<std::string>& files) {
    json::Value current = doc;
    std::vector<std::string> chain;
    for (int depth = 0;; ++depth) {
        const json::Value* extends = current.is_object() ? current.find("extends") : nullptr;
        if (extends == nullptr || !extends->is_string()) {
            return current;
        }
        const std::string path = extends->as_string();
        if (depth >= kMaxExtends || std::ranges::find(chain, path) != chain.end()) {
            return make_error(
                ErrorCode::InvalidArgument,
                "/extends: " + quoted(path) +
                    (depth >= kMaxExtends ? " is more than 8 files deep" : " extends itself"));
        }
        chain.push_back(path);
        if (!reader.text) {
            return make_error(ErrorCode::InvalidArgument,
                              "/extends: nothing reads " + quoted(path) + " here");
        }
        auto text = reader.text(path);
        if (!text) {
            return make_error(text.error().code,
                              "/extends: " + quoted(path) + ": " + text.error().message);
        }
        auto base = json::parse(text.value());
        if (!base) {
            return make_error(ErrorCode::InvalidArgument,
                              "/extends: " + quoted(path) + ": " + base.error().message);
        }
        files.push_back(path);
        json::Value patch = current;
        patch.as_object().erase("extends");
        current = world::merge_patch(base.value(), patch);
    }
}

void fill_ramp(render::GraphPass& pass, const json::Value* value) {
    const Gradient gradient = value != nullptr ? parse_gradient(*value).value() : Gradient{};
    const std::size_t count = std::min(gradient.points.size(), render::kMaxGraphRampPoints);
    pass.params.at(render::kGraphRampAt) = static_cast<float>(count);
    for (std::size_t i = 0; i < count; ++i) {
        const GradientStop& stop = gradient.points[i];
        const std::size_t at = render::kGraphRampAt + 1 + i * 5;
        pass.params.at(at) = stop.offset;
        pass.params.at(at + 1) = stop.color.x;
        pass.params.at(at + 2) = stop.color.y;
        pass.params.at(at + 3) = stop.color.z;
        pass.params.at(at + 4) = stop.color.w;
    }
}

// A node's params, from its resolved settings.
void fill_params(const OpSpec& spec, const json::Value& node, std::uint32_t width,
                 std::uint32_t height, render::GraphPass& pass) {
    auto& p = pass.params;
    const auto set_pair = [&](std::size_t at, std::array<float, 2> v) {
        p.at(at) = v[0];
        p.at(at + 1) = v[1];
    };
    switch (spec.op) {
    case GraphOp::Constant: {
        const json::Value* color = node.find("color");
        for (std::size_t k = 0; k < 4; ++k) {
            const float otherwise = k == 3 ? 1.0F : 0.0F;
            p.at(k) =
                color != nullptr ? static_cast<float>(color->as_array()[k].as_double()) : otherwise;
        }
        break;
    }
    case GraphOp::Noise: {
        const json::Value* noise = node.find("noise");
        const NoiseSettings s = noise != nullptr ? parse_noise(*noise).value() : NoiseSettings{};
        const auto e = [](auto value) { return static_cast<float>(static_cast<int>(value)); };
        p[0] = e(s.noise_type);
        p[1] = static_cast<float>(s.seed);
        p[2] = s.frequency;
        p[3] = e(s.fractal_type);
        p[4] = static_cast<float>(s.fractal_octaves);
        p[5] = s.fractal_lacunarity;
        p[6] = s.fractal_gain;
        p[7] = s.fractal_weighted_strength;
        p[8] = s.fractal_ping_pong_strength;
        p[9] = e(s.cellular_distance_function);
        p[10] = e(s.cellular_return_type);
        p[11] = s.cellular_jitter;
        p[12] = s.domain_warp_enabled ? 1.0F : 0.0F;
        p[13] = e(s.domain_warp_type);
        p[14] = s.domain_warp_amplitude;
        p[15] = s.domain_warp_frequency;
        p[16] = e(s.domain_warp_fractal_type);
        p[17] = static_cast<float>(s.domain_warp_fractal_octaves);
        p[18] = s.domain_warp_fractal_lacunarity;
        p[19] = s.domain_warp_fractal_gain;
        p[20] = s.offset.x;
        p[21] = s.offset.y;
        p[22] = s.offset.z;
        set_pair(23, pair(node, "scale", {static_cast<float>(width), static_cast<float>(height)}));
        const json::Value* seamless = node.find("seamless");
        p[25] = seamless != nullptr && seamless->as_bool() ? 1.0F : 0.0F;
        p[26] = number(node, "seamless_blend_skirt", 0.1F);
        break;
    }
    case GraphOp::Gradient:
        p[0] = named(node, "fill", kFills);
        set_pair(1, pair(node, "fill_from", {0.0F, 0.0F}));
        set_pair(3, pair(node, "fill_to", {1.0F, 0.0F}));
        p[5] = named(node, "repeat", kRepeats);
        fill_ramp(pass, node.find("gradient"));
        break;
    case GraphOp::Shape:
        p[0] = named(node, "kind", kShapes);
        set_pair(1, pair(node, "cells", {1.0F, 1.0F}));
        p[3] = number(node, "size", 0.8F);
        p[4] = number(node, "sides", 6.0F);
        p[5] = number(node, "softness", 0.05F);
        p[6] = number(node, "rotation", 0.0F);
        p[7] = number(node, "thickness", 0.2F);
        break;
    case GraphOp::Bricks:
        p[0] = number(node, "across", 4.0F);
        p[1] = number(node, "down", 8.0F);
        p[2] = number(node, "mortar", 0.05F);
        p[3] = number(node, "offset", 0.5F);
        p[4] = number(node, "bevel", 0.1F);
        p[5] = number(node, "seed", 0.0F);
        break;
    case GraphOp::Checker:
        set_pair(0, pair(node, "cells", {8.0F, 8.0F}));
        break;
    case GraphOp::Waves:
        p[0] = number(node, "count", 8.0F);
        p[1] = number(node, "angle", 0.0F);
        p[2] = named(node, "shape", kWaveShapes);
        p[3] = number(node, "phase", 0.0F);
        break;
    case GraphOp::Levels:
        set_pair(0, pair(node, "in", {0.0F, 1.0F}));
        p[2] = number(node, "gamma", 1.0F);
        set_pair(3, pair(node, "out", {0.0F, 1.0F}));
        break;
    case GraphOp::GradientMap:
        fill_ramp(pass, node.find("gradient"));
        break;
    case GraphOp::Hsv:
        p[0] = number(node, "hue", 0.0F);
        p[1] = number(node, "saturation", 1.0F);
        p[2] = number(node, "value", 1.0F);
        break;
    case GraphOp::Blur:
        p[0] = number(node, "radius", 2.0F);
        break;
    case GraphOp::Sharpen:
        p[0] = number(node, "amount", 1.0F);
        break;
    case GraphOp::Warp:
        p[0] = number(node, "strength", 0.05F);
        p[1] = named(node, "mode", kWarpModes);
        p[2] = number(node, "angle", 0.0F);
        break;
    case GraphOp::Transform:
        set_pair(0, pair(node, "repeat", {1.0F, 1.0F}));
        p[2] = number(node, "rotation", 0.0F);
        set_pair(3, pair(node, "offset", {0.0F, 0.0F}));
        break;
    case GraphOp::EdgeDetect:
    case GraphOp::NormalFromHeight:
    case GraphOp::CurvatureFromHeight:
        p[0] = number(node, "strength", 1.0F);
        break;
    case GraphOp::Threshold:
        p[0] = number(node, "level", 0.5F);
        p[1] = number(node, "softness", 0.05F);
        break;
    case GraphOp::Posterize:
        p[0] = number(node, "steps", 4.0F);
        break;
    case GraphOp::Channels: {
        const json::Value* channels = node.find("channels");
        const std::string text = channels != nullptr ? channels->as_string() : "rgba";
        for (std::size_t k = 0; k < 4; ++k) {
            const auto at = std::string_view("rgba01").find(text.at(k));
            p.at(k) = static_cast<float>(at);
        }
        break;
    }
    case GraphOp::AoFromHeight:
        p[0] = number(node, "radius", 8.0F);
        p[1] = number(node, "strength", 1.0F);
        break;
    case GraphOp::Blend:
        p[0] = named(node, "mode", kBlendModes);
        p[1] = number(node, "opacity", 1.0F);
        break;
    case GraphOp::Math: {
        p[0] = named(node, "operation", kMathOps);
        const json::Value* value = node.find("value");
        for (std::size_t k = 0; k < 4; ++k) {
            if (value == nullptr) {
                p.at(2 + k) = 0.0F;
            } else if (value->is_number()) {
                p.at(2 + k) = static_cast<float>(value->as_double());
            } else {
                p.at(2 + k) = static_cast<float>(value->as_array()[k].as_double());
            }
        }
        break;
    }
    case GraphOp::Remap:
        set_pair(0, pair(node, "from", {0.0F, 1.0F}));
        set_pair(2, pair(node, "to", {0.0F, 1.0F}));
        p[4] = node.find("clamp") != nullptr && node.find("clamp")->as_bool() ? 1.0F : 0.0F;
        break;
    case GraphOp::Image:
    case GraphOp::Merge:
    case GraphOp::Invert:
    case GraphOp::Clamp:
    case GraphOp::Grayscale:
        break;
    }
}

// Hands out slots, lowest first, and takes them back.
class SlotPool {
public:
    std::uint32_t take() {
        if (!free_.empty()) {
            const std::uint32_t slot = *free_.begin();
            free_.erase(free_.begin());
            return slot;
        }
        return count_++;
    }
    void give(std::uint32_t slot) { free_.insert(slot); }
    [[nodiscard]] std::uint32_t count() const { return count_; }

private:
    std::set<std::uint32_t> free_;
    std::uint32_t count_ = 0;
};

render::GraphImage graph_image_of(const render::Image& image) {
    render::GraphImage out;
    out.width = image.width;
    out.height = image.height;
    out.texels.resize(image.pixels.size());
    for (std::size_t i = 0; i < image.pixels.size(); ++i) {
        out.texels[i] = static_cast<float>(image.pixels[i]) / 255.0F;
    }
    return out;
}

} // namespace

bool is_texture_graph(const json::Value& doc) {
    const json::Value* type = doc.is_object() ? doc.find("type") : nullptr;
    return type != nullptr && type->is_string() && type->as_string() == kGraphType;
}

json::Value texture_graph_schema() {
    json::Value schema = top_schema();
    for (const OpSpec& spec : kOps) {
        schema["$defs"][spec.name] = op_schema(spec);
    }
    return schema;
}

std::vector<json::ValidationError> validate_texture_graph(const json::Value& doc) {
    std::vector<json::ValidationError> errors;
    Graph graph(doc, errors);
    const bool extends = doc.is_object() && doc.find("extends") != nullptr;
    graph.check({}, !extends);
    return errors;
}

Result<CompiledGraph> compile_texture_graph(const json::Value& doc, const GraphReader& reader,
                                            const json::Object& overrides) {
    CompiledGraph out;
    auto merged = merged_document(doc, reader, out.files);
    if (!merged) {
        return merged.error();
    }
    const json::Value& graph_doc = merged.value();
    std::vector<json::ValidationError> errors;
    Graph graph(graph_doc, errors);
    graph.check(overrides, true);
    if (!errors.empty()) {
        return detail::first_error(errors);
    }
    render::TextureGraphProgram& program = out.program;
    const auto size = [&](std::string_view key) {
        const json::Value* value = graph_doc.find(key);
        return static_cast<std::uint32_t>(value != nullptr ? value->as_int() : kDefaultSize);
    };
    program.width = size("width");
    program.height = size("height");
    const json::Value* tiling = graph_doc.find("tiling");
    program.tiling = tiling == nullptr || tiling->as_bool();

    // When each node's value is last read, and which nodes are outputs (kept to the end).
    const std::vector<std::string>& order = graph.order();
    const auto& nodes = graph.resolved();
    std::map<std::string, std::size_t, std::less<>> last_read;
    const auto sources_of = [&](const OpSpec& spec, const json::Value& node) {
        std::array<std::optional<ChannelSource>, 4> sources;
        for (std::size_t k = 0; k < 4; ++k) {
            const std::string_view input = spec.inputs.at(k);
            const json::Value* value = input.empty() ? nullptr : node.find(input);
            if (value == nullptr) {
                continue;
            }
            ChannelSource source;
            if (value->is_string()) {
                source.node = value->as_string();
            } else if (value->is_object()) {
                source.node = value->find("node")->as_string();
                const json::Value* channel = value->find("channel");
                source.channel = channel != nullptr ? channel_index(channel->as_string()) : 0;
            } else {
                source.value = static_cast<float>(value->as_double());
            }
            sources.at(k) = source;
        }
        return sources;
    };
    for (std::size_t i = 0; i < order.size(); ++i) {
        const json::Value& node = nodes.at(order[i]);
        const OpSpec& spec = *find_op(node.find("op")->as_string());
        for (const auto& source : sources_of(spec, node)) {
            if (source && !source->node.empty()) {
                last_read[source->node] = i;
            }
        }
    }
    std::set<std::string, std::less<>> kept;
    for (const json::Value& output : graph_doc.find("outputs")->as_array()) {
        kept.insert(output.find("node")->as_string());
    }

    SlotPool slots;
    std::map<std::string, std::uint32_t, std::less<>> slot_of;
    std::map<std::string, std::uint32_t, std::less<>> image_of;
    for (std::size_t i = 0; i < order.size(); ++i) {
        const std::string& name = order[i];
        const json::Value& node = nodes.at(name);
        const OpSpec& spec = *find_op(node.find("op")->as_string());
        render::GraphPass pass;
        pass.op = spec.op;
        fill_params(spec, node, program.width, program.height, pass);
        const auto sources = sources_of(spec, node);
        for (std::size_t k = 0; k < 4; ++k) {
            const auto& source = sources.at(k);
            if (!source) {
                if (spec.op == GraphOp::Merge) {
                    pass.params.at(4 + k) = k == 3 ? 1.0F : 0.0F;
                }
                continue;
            }
            if (source->node.empty()) {
                pass.params.at(4 + k) = source->value;
                continue;
            }
            pass.in.at(k) = slot_of.at(source->node);
            if (spec.op == GraphOp::Merge) {
                pass.params.at(k) = static_cast<float>(source->channel);
            }
        }
        if (spec.op == GraphOp::Image) {
            const std::string& path = node.find("path")->as_string();
            auto found = image_of.find(path);
            if (found == image_of.end()) {
                if (program.images.size() >= render::kMaxGraphImages) {
                    return make_error(ErrorCode::InvalidArgument,
                                      "/nodes/" + json::pointer_escape(name) +
                                          "/path: a graph reads at most " +
                                          std::to_string(render::kMaxGraphImages) + " images");
                }
                if (!reader.image) {
                    return make_error(ErrorCode::InvalidArgument,
                                      "/nodes/" + json::pointer_escape(name) +
                                          "/path: nothing reads images here");
                }
                auto image = reader.image(path);
                if (!image) {
                    return make_error(image.error().code, "/nodes/" + json::pointer_escape(name) +
                                                              "/path: " + quoted(path) + ": " +
                                                              image.error().message);
                }
                found =
                    image_of.emplace(path, static_cast<std::uint32_t>(program.images.size())).first;
                program.images.push_back(graph_image_of(image.value()));
                out.files.push_back(path);
            }
            pass.params[0] = static_cast<float>(found->second);
        }
        const std::uint32_t slot = slots.take();
        if (spec.op == GraphOp::Blur) {
            // Across into a slot of its own, then down into the node's.
            const std::uint32_t across = slots.take();
            render::GraphPass first = pass;
            first.out = across;
            first.params[1] = 0.0F;
            program.passes.push_back(first);
            pass.in[0] = across;
            pass.params[1] = 1.0F;
            pass.out = slot;
            program.passes.push_back(pass);
            slots.give(across);
        } else {
            pass.out = slot;
            program.passes.push_back(pass);
        }
        slot_of[name] = slot;
        for (const auto& source : sources) {
            if (source && !source->node.empty() && !kept.contains(source->node) &&
                last_read.at(source->node) == i) {
                slots.give(slot_of.at(source->node));
            }
        }
        // A node nothing reads that isn't an output can't be in the order; outputs stay.
    }
    program.slots = slots.count();
    for (const json::Value& output : graph_doc.find("outputs")->as_array()) {
        const json::Value* as = output.find("as");
        out.outputs.push_back({output.find("name")->as_string(),
                               slot_of.at(output.find("node")->as_string()),
                               as != nullptr && as->as_string() == "gray"});
    }
    if (auto checked = render::check_texture_graph(program); !checked) {
        return checked.error();
    }
    return out;
}

render::Image graph_texels_image(const render::GraphTexels& texels, std::uint32_t width,
                                 std::uint32_t height, bool gray) {
    render::Image image;
    image.width = width;
    image.height = height;
    const std::size_t count = static_cast<std::size_t>(width) * height;
    image.pixels.resize(count * 4);
    const auto byte = [](float value) {
        return static_cast<std::uint8_t>(std::floor(std::clamp(value, 0.0F, 1.0F) * 255.0F + 0.5F));
    };
    for (std::size_t i = 0; i < count; ++i) {
        const float* in = texels.data() + i * 4;
        std::uint8_t* out = image.pixels.data() + i * 4;
        if (gray) {
            out[0] = out[1] = out[2] = byte(in[0]);
            out[3] = 255;
        } else {
            for (std::size_t k = 0; k < 4; ++k) {
                out[k] = byte(in[k]);
            }
        }
    }
    return image;
}

Result<std::vector<render::Image>>
bake_texture_graph(const CompiledGraph& graph, render::RenderDevice* device, jobs::Pool* pool) {
    std::vector<std::uint32_t> slots;
    slots.reserve(graph.outputs.size());
    for (const GraphOutput& output : graph.outputs) {
        slots.push_back(output.slot);
    }
    auto texels = device != nullptr ? device->bake_texture_graph(graph.program, slots)
                                    : render::run_texture_graph(graph.program, slots, pool);
    if (!texels) {
        return texels.error();
    }
    std::vector<render::Image> images;
    images.reserve(graph.outputs.size());
    for (std::size_t i = 0; i < graph.outputs.size(); ++i) {
        images.push_back(graph_texels_image(texels.value().at(i), graph.program.width,
                                            graph.program.height, graph.outputs[i].gray));
    }
    return images;
}

Result<render::Image> texture_graph_image(const json::Value& doc, std::string_view output,
                                          const GraphReader& reader, render::RenderDevice* device,
                                          jobs::Pool* pool) {
    auto compiled = compile_texture_graph(doc, reader);
    if (!compiled) {
        return compiled.error();
    }
    CompiledGraph& graph = compiled.value();
    std::size_t index = 0;
    if (!output.empty()) {
        const auto found = std::ranges::find(graph.outputs, output, &GraphOutput::name);
        if (found == graph.outputs.end()) {
            std::vector<std::string_view> names;
            names.reserve(graph.outputs.size());
            for (const GraphOutput& each : graph.outputs) {
                names.push_back(each.name);
            }
            std::string message = "the graph has no output " + quoted(output);
            if (const std::string_view near = closest_match(output, names); !near.empty()) {
                message += " (did you mean " + quoted(near) + "?)";
            }
            return make_error(ErrorCode::NotFound, std::move(message));
        }
        index = static_cast<std::size_t>(found - graph.outputs.begin());
    }
    // Bake only the output asked for.
    graph.outputs = {graph.outputs.at(index)};
    auto images = bake_texture_graph(graph, device, pool);
    if (!images) {
        return images.error();
    }
    return std::move(images.value().front());
}

void set_graph_device(render::RenderDevice* device) {
    g_graph_device.store(device);
}

render::RenderDevice* graph_device() {
    return g_graph_device.load();
}

} // namespace slash::procedural
