#include "slash/procedural/paint.hpp"

#include "json_fields.hpp"

#include "slash/core/jobs.hpp"
#include "slash/core/text.hpp"
#include "slash/procedural/noise.hpp"
#include "slash/world/ragdoll.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace slash::procedural {
namespace {

using detail::boolean_field;
using detail::integer_field;
using detail::number_field;

constexpr std::string_view kPaintType = "paint";
constexpr int kMaxExtends = 8;
constexpr std::string_view kSourcePrefix = "paint:sources/";
constexpr std::string_view kImagePrefix = "paint:images/";

constexpr std::array<std::string_view, 11> kBlendModes{
    "mix",    "add",     "subtract",   "multiply",   "screen", "overlay",
    "darken", "lighten", "difference", "soft_light", "divide"};
constexpr std::array<std::string_view, 5> kCombine{"multiply", "add", "subtract", "max", "min"};
constexpr std::array<std::string_view, 3> kAxes{"x", "y", "z"};
// The maps that are gray images baked from the model (or stand-ins on a sprite).
constexpr std::array<std::string_view, 5> kGrayMaps{"ao", "curvature", "thickness", "sky",
                                                    "height"};

std::string quoted(std::string_view text) {
    return "\"" + std::string(text) + "\"";
}

template <typename Range>
bool contains(const Range& range, std::string_view name) {
    return std::ranges::find(range, name) != std::ranges::end(range);
}

bool is_color_channel(std::string_view channel) {
    return channel == "albedo" || channel == "normal" || channel == "emission";
}

// ---- Schema ----

json::Value text_field(std::string_view description) {
    json::Value field;
    field["type"] = "string";
    field["description"] = description;
    return field;
}

template <std::size_t N>
json::Value names_field(const std::array<std::string_view, N>& names,
                        std::string_view description) {
    json::Value field;
    field["type"] = "string";
    json::Array values;
    for (const std::string_view name : names) {
        values.emplace_back(name);
    }
    field["enum"] = std::move(values);
    field["description"] = description;
    return field;
}

// A setting the stack's graph works out: a number, "$name" or an "=" expression.
json::Value setting_field(std::string_view description) {
    json::Value field;
    field["type"] = json::Array{"number", "string"};
    field["description"] = description;
    return field;
}

json::Value point_field(std::string_view description) {
    json::Value field;
    field["type"] = "array";
    field["items"]["type"] = "number";
    field["minItems"] = 2;
    field["maxItems"] = 3;
    field["description"] = description;
    return field;
}

json::Value size_field(std::string_view description) {
    json::Value field;
    field["type"] = json::Array{"number", "array"};
    field["items"]["type"] = "number";
    field["minItems"] = 2;
    field["maxItems"] = 2;
    field["description"] = description;
    return field;
}

json::Value graph_field(std::string_view description) {
    json::Value field;
    field["type"] = json::Array{"string", "object"};
    field["description"] = description;
    return field;
}

json::Value term_schema() {
    json::Value schema;
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["description"] =
        "One term of a mask: a map, part, material or graph, then range, breakup, above, "
        "below and invert in that order.";
    json::Value& p = schema["properties"];
    p["map"] = names_field(kPaintMaps, "A map the model's shape gives each texel.");
    p["axis"] = names_field(kAxes, "world_normal and position: the axis read. Default y.");
    p["scale"] =
        number_field(8.0F, "noise: features across the model's (or sprite's) size.", 0.001, 4096.0);
    p["seed"] = integer_field(0, "noise: its seed.");
    p["octaves"] = integer_field(4, "noise: its octaves.", 1, 9);
    json::Value names;
    names["type"] = json::Array{"string", "array"};
    names["items"]["type"] = "string";
    names["minItems"] = 1;
    p["part"] = names;
    p["part"]["description"] = "A part's name, or a list: 1 on them, 0 elsewhere.";
    p["material"] = names;
    p["material"]["description"] = "A material's name, or a list: 1 on them, 0 elsewhere.";
    p["graph"] = graph_field("A texture graph file or document whose output's red is the mask.");
    p["output"] = text_field("graph: the output read. Default the first.");
    p["repeat"] = size_field("graph: copies across and down the texture. Default 1.");
    p["parameters"] =
        json::Object{{"type", "object"}, {"description", "graph: its parameters' values by name."}};
    json::Value range = point_field("Remaps [low, high] to 0 to 1, clamped.");
    range["minItems"] = 2;
    range["maxItems"] = 2;
    p["range"] = std::move(range);
    json::Value& breakup = p["breakup"];
    breakup["type"] = "object";
    breakup["additionalProperties"] = false;
    breakup["description"] = "Noise added before the thresholds, so edges wander.";
    breakup["properties"]["amount"] = setting_field("How far the value moves either way.");
    breakup["properties"]["scale"] =
        number_field(12.0F, "Features across the size.", 0.001, 4096.0);
    breakup["properties"]["seed"] = integer_field(0, "Its seed.");
    breakup["required"] = json::Array{"amount"};
    p["above"] = setting_field("1 where the value is over this, softened.");
    p["below"] = setting_field("1 where the value is under this, softened.");
    p["softness"] = setting_field("Half the width of a threshold's change. Default 0.05.");
    p["invert"] = boolean_field(false, "1 minus the term.");
    p["op"] =
        names_field(kCombine, "How the term joins the terms before it. Default multiply (both).");
    return schema;
}

json::Value layer_schema() {
    json::Value schema;
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["required"] = json::Array{"name"};
    schema["description"] = "A layer: a material laid over the channels below through a mask.";
    json::Value& p = schema["properties"];
    p["name"] = text_field("The layer's name, unique in the stack.");
    p["name"]["minLength"] = 1;
    p["enabled"] = boolean_field(true, "false leaves the layer out.");
    json::Value& fill = p["fill"];
    fill["type"] = "object";
    fill["additionalProperties"] = false;
    fill["description"] = "Flat values by channel: numbers, colors, \"$name\" or \"=\" "
                          "expressions.";
    for (const std::string_view channel : kPaintChannels) {
        json::Value value;
        value["type"] = json::Array{"number", "array", "string"};
        value["items"]["type"] = json::Array{"number", "string"};
        value["minItems"] = 3;
        value["maxItems"] = 4;
        fill["properties"][channel] = std::move(value);
    }
    p["graph"] = graph_field("A texture graph file or document: its outputs named as channels "
                             "(albedo or color, roughness, metallic, ao, height, normal, "
                             "emission, orm) paint them.");
    p["parameters"] = json::Object{
        {"type", "object"},
        {"description", "graph: its parameters' values by name, or \"$name\" of the stack's."}};
    p["repeat"] = size_field("graph: copies across and down the texture. Default 1.");
    json::Value& decals = p["decals"];
    decals["type"] = "array";
    decals["description"] = "Images projected onto the model, later ones over earlier; they "
                            "paint albedo and mask the layer by their alpha.";
    json::Value& decal = decals["items"];
    decal["type"] = "object";
    decal["additionalProperties"] = false;
    decal["required"] = json::Array{"image", "at"};
    decal["properties"]["image"] = text_field("The image, project path.");
    decal["properties"]["at"] =
        point_field("Its center: a point on the model, or a sprite's pixel.");
    json::Value direction = point_field("Model: the way it projects. Default [0, 0, -1].");
    direction["minItems"] = 3;
    decal["properties"]["direction"] = direction;
    json::Value up = point_field("Model: the way its top faces. Default [0, 1, 0].");
    up["minItems"] = 3;
    decal["properties"]["up"] = std::move(up);
    decal["properties"]["size"] = size_field(
        "Its width (its height by the image's shape) or [width, height], in the model's units "
        "or a sprite's pixels.");
    decal["properties"]["depth"] = number_field(
        0.0F, "Model: how far along direction it reaches. Default its larger side.", 0.0, 1e9);
    decal["properties"]["rotation"] = number_field(0.0F, "Sprite: turns, clockwise.");
    json::Value& strokes = p["strokes"];
    strokes["type"] = "array";
    strokes["description"] = "Brush strokes along points on the model (or pixels of a sprite), "
                             "adding to the mask; erase strokes take from it.";
    json::Value& stroke = strokes["items"];
    stroke["type"] = "object";
    stroke["additionalProperties"] = false;
    stroke["required"] = json::Array{"points", "size"};
    stroke["properties"]["points"]["type"] = "array";
    stroke["properties"]["points"]["minItems"] = 1;
    stroke["properties"]["points"]["items"] = point_field("A point the stroke passes.");
    stroke["properties"]["points"]["description"] = "The points it joins in order; one is a dab.";
    stroke["properties"]["size"] =
        number_field(0.0F, "The brush's radius, in the model's units or pixels.", 0.0, 1e9);
    stroke["properties"]["size"]["exclusiveMinimum"] = 0;
    stroke["properties"]["size"].as_object().erase("minimum");
    stroke["properties"]["hardness"] =
        number_field(0.5F, "The share of the radius at full strength.", 0.0, 1.0);
    stroke["properties"]["erase"] = boolean_field(false, "Take the stroke out of the mask.");
    json::Value mask;
    mask["description"] = "A term, or terms joined in order; none paints the whole layer.";
    mask["anyOf"] = json::Array{json::Object{{"$ref", "#/$defs/term"}},
                                json::Object{{"type", "array"},
                                             {"minItems", 1},
                                             {"items", json::Object{{"$ref", "#/$defs/term"}}}}};
    p["mask"] = std::move(mask);
    json::Value blend;
    blend["description"] = "How the layer combines with what is below: a mode, or modes by "
                           "channel. Default mix.";
    json::Value per_channel;
    per_channel["type"] = "object";
    per_channel["additionalProperties"] = false;
    for (const std::string_view channel : kPaintChannels) {
        per_channel["properties"][channel] = names_field(kBlendModes, "The channel's mode.");
    }
    blend["anyOf"] =
        json::Array{names_field(kBlendModes, "Every channel's mode."), std::move(per_channel)};
    p["blend"] = std::move(blend);
    p["opacity"] = setting_field("The layer's strength, 0 to 1. Default 1.");
    json::Value channels;
    channels["type"] = "array";
    channels["items"] = names_field(kPaintChannels, "A channel.");
    channels["uniqueItems"] = true;
    channels["description"] = "Only these channels; default every one the layer gives.";
    p["channels"] = std::move(channels);
    return schema;
}

// ---- Smart materials ----

struct Builtin {
    std::string_view name;
    std::string_view text;
};

// Masks read maps every model has (curvature, ambient occlusion, the world normal, noise), so
// each fits whatever it is dropped on; on a sprite ao reads 1.
constexpr std::array<Builtin, 5> kBuiltins{{
    {"worn_painted_metal", R"({
  "format": 1, "type": "paint",
  "parameters": {"paint": [0.18, 0.32, 0.45], "wear": 0.5, "dirt": 0.4},
  "layers": [
    {"name": "metal", "fill": {"albedo": [0.56, 0.57, 0.58], "metallic": 1, "roughness": 0.35}},
    {"name": "paint", "fill": {"albedo": "$paint", "metallic": 0, "roughness": 0.55}},
    {"name": "worn", "fill": {"albedo": [0.56, 0.57, 0.58], "metallic": 1, "roughness": 0.3},
     "mask": {"map": "curvature", "above": "=0.8 - wear * 0.25", "softness": 0.03,
              "breakup": {"amount": 0.12, "scale": 18}}},
    {"name": "dirt", "fill": {"albedo": [0.22, 0.18, 0.13], "roughness": 0.9},
     "mask": {"map": "ao", "below": "=0.35 + dirt * 0.3", "softness": 0.15},
     "opacity": "$dirt"}
  ]
})"},
    {"rusty_iron", R"({
  "format": 1, "type": "paint",
  "parameters": {"rust": 0.5},
  "layers": [
    {"name": "iron", "fill": {"albedo": [0.35, 0.35, 0.37], "metallic": 1, "roughness": 0.45}},
    {"name": "rust", "fill": {"albedo": [0.42, 0.2, 0.09], "metallic": 0, "roughness": 0.9,
                              "height": 0.56},
     "mask": [{"map": "noise", "scale": 6, "above": "=0.75 - rust * 0.5", "softness": 0.08},
              {"map": "ao", "below": "=0.6 + rust * 0.4", "softness": 0.2, "op": "max"}]},
    {"name": "flakes", "fill": {"albedo": [0.3, 0.13, 0.06], "roughness": 1},
     "mask": [{"map": "noise", "scale": 40, "seed": 3, "above": 0.62},
              {"map": "noise", "scale": 6, "above": "=0.8 - rust * 0.5"}]}
  ]
})"},
    {"mossy_stone", R"({
  "format": 1, "type": "paint",
  "parameters": {"moss": 0.5},
  "layers": [
    {"name": "stone", "fill": {"albedo": [0.46, 0.45, 0.42], "roughness": 0.85}},
    {"name": "grain", "fill": {"albedo": [0.36, 0.35, 0.33]},
     "mask": {"map": "noise", "scale": 30, "seed": 1, "above": 0.55, "softness": 0.15}},
    {"name": "cracks", "fill": {"albedo": [0.25, 0.24, 0.22], "roughness": 0.95},
     "mask": {"map": "curvature", "below": 0.42, "softness": 0.04}},
    {"name": "moss", "fill": {"albedo": [0.25, 0.34, 0.12], "roughness": 0.95, "height": 0.55},
     "mask": [{"map": "world_normal", "axis": "y", "above": "=0.7 - moss * 0.6",
               "softness": 0.15, "breakup": {"amount": 0.25, "scale": 10}},
              {"map": "ao", "below": 0.75, "softness": 0.25, "op": "max"}]}
  ]
})"},
    {"dusty_wood", R"({
  "format": 1, "type": "paint",
  "parameters": {"dust": 0.5},
  "layers": [
    {"name": "wood", "fill": {"albedo": [0.42, 0.27, 0.15], "roughness": 0.7}},
    {"name": "grain", "fill": {"albedo": [0.33, 0.2, 0.1]},
     "mask": {"map": "noise", "scale": 24, "seed": 5, "above": 0.55, "softness": 0.1}},
    {"name": "edges", "fill": {"albedo": [0.55, 0.38, 0.22], "roughness": 0.6},
     "mask": {"map": "curvature", "above": 0.62, "softness": 0.04,
              "breakup": {"amount": 0.08, "scale": 20}}},
    {"name": "dust", "fill": {"albedo": [0.62, 0.58, 0.52], "roughness": 1},
     "mask": {"map": "world_normal", "axis": "y", "above": "=0.85 - dust * 0.5",
              "softness": 0.1, "breakup": {"amount": 0.15, "scale": 14}},
     "opacity": "=0.4 + dust * 0.6"}
  ]
})"},
    {"scuffed_plastic", R"({
  "format": 1, "type": "paint",
  "parameters": {"color": [0.7, 0.16, 0.12], "scuffs": 0.5},
  "layers": [
    {"name": "plastic", "fill": {"albedo": "$color", "roughness": 0.4}},
    {"name": "scuffs", "fill": {"roughness": 0.75},
     "mask": [{"map": "curvature", "above": "=0.75 - scuffs * 0.2", "softness": 0.05},
              {"map": "noise", "scale": 30, "seed": 7, "above": 0.45, "softness": 0.1}]},
    {"name": "grime", "fill": {"albedo": [0.12, 0.11, 0.1], "roughness": 0.8},
     "mask": {"map": "ao", "below": 0.5, "softness": 0.2}, "opacity": "=scuffs * 0.8"}
  ]
})"},
}};

// ---- Validation ----

void check_names(const json::Value& doc, std::vector<json::ValidationError>& errors) {
    const json::Value* layers = doc.find("layers");
    if (layers == nullptr || !layers->is_array()) {
        return;
    }
    std::set<std::string> seen;
    for (std::size_t i = 0; i < layers->as_array().size(); ++i) {
        const json::Value& layer = layers->as_array()[i];
        const json::Value* name = layer.is_object() ? layer.find("name") : nullptr;
        if (name == nullptr || !name->is_string()) {
            continue;
        }
        if (!seen.insert(name->as_string()).second) {
            errors.push_back({"/layers/" + std::to_string(i) + "/name",
                              quoted(name->as_string()) + " names another layer too"});
        }
    }
}

// What only models or only sprites take, and points of the right size.
void check_kind(const json::Value& doc, std::vector<json::ValidationError>& errors) {
    const bool model = doc.find("model") != nullptr;
    const bool sprite = doc.find("sprite") != nullptr;
    if (model && sprite) {
        errors.push_back({"/sprite", "give a model or a sprite, not both"});
        return;
    }
    if (!model) {
        for (const std::string_view key : {"high", "maps"}) {
            if (doc.find(key) != nullptr) {
                errors.push_back({"/" + std::string(key), "only a stack with a model takes it"});
            }
        }
    }
    if (!sprite) {
        for (const std::string_view key : {"normal_map", "edge_width"}) {
            if (doc.find(key) != nullptr) {
                errors.push_back({"/" + std::string(key), "only a stack with a sprite takes it"});
            }
        }
    } else if (doc.find("size") != nullptr) {
        errors.push_back({"/size", "a sprite's stack paints at the sprite's size"});
    }
    if (!model && !sprite) {
        return; // a smart material: either
    }
    const std::size_t dims = model ? 3 : 2;
    const char* what = model ? "a point on the model is [x, y, z]" : "a sprite's pixel is [x, y]";
    const json::Value* layers = doc.find("layers");
    if (layers == nullptr || !layers->is_array()) {
        return;
    }
    for (std::size_t i = 0; i < layers->as_array().size(); ++i) {
        const json::Value& layer = layers->as_array()[i];
        if (!layer.is_object()) {
            continue;
        }
        const std::string at = "/layers/" + std::to_string(i);
        if (const json::Value* strokes = layer.find("strokes");
            strokes != nullptr && strokes->is_array()) {
            for (std::size_t s = 0; s < strokes->as_array().size(); ++s) {
                const json::Value* points = strokes->as_array()[s].find("points");
                if (points == nullptr || !points->is_array()) {
                    continue;
                }
                for (std::size_t k = 0; k < points->as_array().size(); ++k) {
                    if (points->as_array()[k].size() != dims) {
                        errors.push_back(
                            {at + "/strokes/" + std::to_string(s) + "/points/" + std::to_string(k),
                             what});
                    }
                }
            }
        }
        if (const json::Value* decals = layer.find("decals");
            decals != nullptr && decals->is_array()) {
            for (std::size_t d = 0; d < decals->as_array().size(); ++d) {
                const json::Value& decal = decals->as_array()[d];
                const std::string where = at + "/decals/" + std::to_string(d);
                if (const json::Value* point = decal.find("at");
                    point != nullptr && point->size() != dims) {
                    errors.push_back({where + "/at", what});
                }
                for (const std::string_view key :
                     model ? std::array<std::string_view, 2>{"rotation", ""}
                           : std::array<std::string_view, 2>{"direction", "up"}) {
                    if (!key.empty() && decal.find(key) != nullptr) {
                        errors.push_back({where + "/" + std::string(key),
                                          model ? "only a sprite's decal turns"
                                                : "a sprite's decal faces the viewer"});
                    }
                }
                if (!model && decal.find("depth") != nullptr) {
                    errors.push_back({where + "/depth", "a sprite's decal has no depth"});
                }
            }
        }
    }
}

// A base value: a number, a color, a parameter, or a map's name.
void check_base(const json::Value& doc, std::vector<json::ValidationError>& errors) {
    const json::Value* base = doc.find("base");
    if (base == nullptr || !base->is_object()) {
        return;
    }
    for (const auto& [channel, value] : base->as_object()) {
        if (!value.is_string()) {
            continue;
        }
        const std::string& text = value.as_string();
        if (text.starts_with('$') || text.starts_with('=')) {
            continue;
        }
        const bool fits = (channel == "albedo" && text == "sprite") ||
                          (channel == "normal" && text == "normal") ||
                          (!is_color_channel(channel) && contains(kGrayMaps, text));
        if (!fits) {
            errors.push_back({"/base/" + json::pointer_escape(channel),
                              quoted(text) + " isn't a map " + channel +
                                  " can start from (gray channels: ao, curvature, thickness, sky "
                                  "or height; normal: normal; albedo: sprite)"});
        }
    }
}

// ---- Resolving ----

Result<std::string> read_text(const GraphReader& reader, std::string_view path) {
    if (const std::optional<std::string_view> builtin = builtin_paint(path)) {
        return std::string(*builtin);
    }
    if (path.starts_with(kBuiltinPaintPrefix)) {
        const std::vector<std::string_view> names = builtin_paint_names();
        std::string message = quoted(path) + " isn't a smart material the engine ships";
        if (const std::string_view near =
                closest_match(path.substr(kBuiltinPaintPrefix.size()), names);
            !near.empty()) {
            message +=
                " (did you mean \"" + std::string(kBuiltinPaintPrefix) + std::string(near) + "\"?)";
        }
        return make_error(ErrorCode::NotFound, message);
    }
    if (!reader.text) {
        return make_error(ErrorCode::InvalidArgument, "nothing reads " + quoted(path) + " here");
    }
    return reader.text(path);
}

// The layer named name in a list, or none.
json::Value* layer_named(json::Array& layers, std::string_view name) {
    for (json::Value& layer : layers) {
        const json::Value* got = layer.find("name");
        if (got != nullptr && got->is_string() && got->as_string() == name) {
            return &layer;
        }
    }
    return nullptr;
}

// ---- Compiling ----

float clamp01(float v) {
    return std::clamp(v, 0.0F, 1.0F);
}

float smoothstep(float a, float b, float x) {
    if (b <= a) {
        return x < a ? 0.0F : 1.0F;
    }
    const float t = clamp01((x - a) / (b - a));
    return t * t * (3.0F - (2.0F * t));
}

struct Vec {
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
};

Vec operator-(Vec a, Vec b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
Vec operator+(Vec a, Vec b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
Vec operator*(Vec a, float s) {
    return {a.x * s, a.y * s, a.z * s};
}
float dot(Vec a, Vec b) {
    return (a.x * b.x) + (a.y * b.y) + (a.z * b.z);
}
Vec cross(Vec a, Vec b) {
    return {(a.y * b.z) - (a.z * b.y), (a.z * b.x) - (a.x * b.z), (a.x * b.y) - (a.y * b.x)};
}
float length(Vec a) {
    return std::sqrt(dot(a, a));
}
Vec normalized(Vec a) {
    const float l = length(a);
    return l > 0.0F ? a * (1.0F / l) : Vec{};
}

Vec point_of(const json::Value& value) {
    const json::Array& items = value.as_array();
    return {static_cast<float>(items[0].as_double()), static_cast<float>(items[1].as_double()),
            items.size() > 2 ? static_cast<float>(items[2].as_double()) : 0.0F};
}

// An RGBA8 image's color at UV, bilinearly, clamped (wrapped when repeating) at its edges.
std::array<float, 4> sample(const render::Image& image, float u, float v, bool wrap) {
    const auto w = static_cast<int>(image.width);
    const auto h = static_cast<int>(image.height);
    const float fx = (u * static_cast<float>(w)) - 0.5F;
    const float fy = (v * static_cast<float>(h)) - 0.5F;
    const auto x0 = static_cast<int>(std::floor(fx));
    const auto y0 = static_cast<int>(std::floor(fy));
    const float tx = fx - static_cast<float>(x0);
    const float ty = fy - static_cast<float>(y0);
    const auto texel = [&](int x, int y) {
        if (wrap) {
            x = ((x % w) + w) % w;
            y = ((y % h) + h) % h;
        } else {
            x = std::clamp(x, 0, w - 1);
            y = std::clamp(y, 0, h - 1);
        }
        const std::size_t at =
            ((static_cast<std::size_t>(y) * image.width) + static_cast<std::size_t>(x)) * 4;
        return std::array<float, 4>{static_cast<float>(image.pixels[at]) / 255.0F,
                                    static_cast<float>(image.pixels[at + 1]) / 255.0F,
                                    static_cast<float>(image.pixels[at + 2]) / 255.0F,
                                    static_cast<float>(image.pixels[at + 3]) / 255.0F};
    };
    const auto a = texel(x0, y0);
    const auto b = texel(x0 + 1, y0);
    const auto c = texel(x0, y0 + 1);
    const auto d = texel(x0 + 1, y0 + 1);
    std::array<float, 4> out{};
    for (std::size_t k = 0; k < 4; ++k) {
        const float top = a.at(k) + ((b.at(k) - a.at(k)) * tx);
        const float bottom = c.at(k) + ((d.at(k) - c.at(k)) * tx);
        out.at(k) = top + ((bottom - top) * ty);
    }
    return out;
}

std::array<float, 2> repeat_of(const json::Value* value) {
    if (value == nullptr) {
        return {1.0F, 1.0F};
    }
    if (value->is_number()) {
        const auto r = static_cast<float>(value->as_double());
        return {r, r};
    }
    return {static_cast<float>(value->as_array()[0].as_double()),
            static_cast<float>(value->as_array()[1].as_double())};
}

// What a layer's graph made, at the stack's size: each output's RGBA floats.
using Baked = std::vector<std::pair<std::string, std::vector<float>>>;

// Parameters that are colors of three numbers gain an alpha of 1, since the graph's color
// settings take four.
json::Object with_alpha(json::Object parameters) {
    for (auto& [name, value] : parameters) {
        if (value.is_array() && value.as_array().size() == 3 &&
            std::ranges::all_of(value.as_array(),
                                [](const json::Value& item) { return item.is_number(); })) {
            value.as_array().emplace_back(1);
        }
    }
    return parameters;
}

class Compiler {
public:
    Compiler(const json::Value& stack, const PaintSurface& surface, const GraphReader& reader,
             jobs::Pool* pool)
        : stack_(stack), surface_(surface), reader_(reader), pool_(pool),
          texels_(std::size_t{surface.width} * surface.height) {
        if (const json::Value* parameters = stack.find("parameters");
            parameters != nullptr && parameters->is_object()) {
            parameters_ = with_alpha(parameters->as_object());
        }
        const float dx = surface.max[0] - surface.min[0];
        const float dy = surface.max[1] - surface.min[1];
        const float dz = surface.max[2] - surface.min[2];
        diagonal_ = std::max(std::sqrt((dx * dx) + (dy * dy) + (dz * dz)), 1e-6F);
    }

    Result<CompiledPaint> run(const json::Object& overrides) {
        build_bases();
        const json::Value* layers = stack_.find("layers");
        std::set<std::string> touched;
        if (layers != nullptr) {
            for (std::size_t i = 0; i < layers->as_array().size(); ++i) {
                const json::Value& layer = layers->as_array()[i];
                if (const json::Value* enabled = layer.find("enabled");
                    enabled != nullptr && !enabled->as_bool()) {
                    continue;
                }
                if (auto done = add_layer(layer, "/layers/" + std::to_string(i), touched); !done) {
                    return done.error();
                }
            }
        }
        if (auto done = add_outputs(touched); !done) {
            return done.error();
        }

        json::Value graph;
        graph["format"] = 1;
        graph["type"] = "graph";
        graph["width"] = surface_.width;
        graph["height"] = surface_.height;
        graph["tiling"] = false;
        if (!parameters_.empty()) {
            graph["parameters"] = parameters_;
        }
        graph["nodes"] = std::move(nodes_);
        graph["outputs"] = std::move(outputs_);

        GraphReader images;
        images.graph_image = [this](std::string_view path) -> std::optional<render::GraphImage> {
            return image_at(path);
        };
        auto compiled = compile_texture_graph(graph, images, with_alpha(overrides));
        if (!compiled) {
            return make_error(compiled.error().code, pointed(compiled.error().message));
        }
        CompiledPaint out;
        out.graph = std::move(graph);
        out.program = std::move(compiled).value();
        out.outputs = std::move(output_names_);
        out.mask_layers = std::move(mask_layers_);
        out.files = std::move(files_);
        return out;
    }

private:
    // ---- The graph's nodes ----

    std::string add(json::Value node, const std::string& origin) {
        std::string name = "n" + std::to_string(next_++);
        nodes_[name] = std::move(node);
        origin_[name] = origin;
        return name;
    }

    // An error message with the graph's node pointers turned back into the stack's.
    [[nodiscard]] std::string pointed(const std::string& message) const {
        constexpr std::string_view kNodes = "/nodes/";
        if (!message.starts_with(kNodes)) {
            return message;
        }
        const std::size_t end = message.find_first_of("/:", kNodes.size());
        const std::string node = message.substr(kNodes.size(), end - kNodes.size());
        const auto found = origin_.find(node);
        if (found == origin_.end()) {
            return message;
        }
        std::string rest = end == std::string::npos ? std::string() : message.substr(end);
        if (rest.starts_with('/')) {
            const std::size_t colon = rest.find(':');
            rest = colon == std::string::npos ? std::string() : rest.substr(colon);
        }
        return found->second + rest;
    }

    std::string constant(const json::Value& color, const std::string& origin) {
        json::Value node;
        node["op"] = "constant";
        node["color"] = color;
        return add(std::move(node), origin);
    }

    // A gray constant: a number or a setting in every color channel.
    std::string gray(const json::Value& value, const std::string& origin) {
        return constant(json::Array{value, value, value, 1}, origin);
    }

    std::string math(const std::string& a, std::string_view operation,
                     const std::optional<std::string>& b, const json::Value& value,
                     const std::string& origin) {
        json::Value node;
        node["op"] = "math";
        node["a"] = a;
        node["operation"] = operation;
        if (b) {
            node["b"] = *b;
        } else if (!value.is_null()) {
            node["value"] = value;
        }
        return add(std::move(node), origin);
    }

    // ---- Images the graph reads ----

    // A gray source worked out on the CPU, packed four to an image; the node reading it.
    std::string source(const std::string& key, const std::function<std::vector<float>()>& make,
                       const std::string& origin) {
        auto found = source_of_.find(key);
        if (found == source_of_.end()) {
            sources_.push_back(make());
            found = source_of_.emplace(key, sources_.size() - 1).first;
        }
        const std::size_t index = found->second;
        const std::size_t image = index / 4;
        std::string& reader = source_nodes_[index];
        if (reader.empty()) {
            std::string& image_node = source_images_[image];
            if (image_node.empty()) {
                json::Value node;
                node["op"] = "image";
                node["path"] = std::string(kSourcePrefix) + std::to_string(image);
                image_node = add(std::move(node), origin);
            }
            constexpr std::string_view kLetters = "rgba";
            const char letter = kLetters[index % 4];
            json::Value node;
            node["op"] = "swizzle";
            node["input"] = image_node;
            node["channels"] = std::string(3, letter) + "1";
            reader = add(std::move(node), origin);
        }
        return reader;
    }

    // An RGBA image the graph reads whole; the node reading it.
    std::string color_image(std::vector<float> texels, const std::string& origin) {
        images_.push_back(std::move(texels));
        json::Value node;
        node["op"] = "image";
        node["path"] = std::string(kImagePrefix) + std::to_string(images_.size() - 1);
        return add(std::move(node), origin);
    }

    [[nodiscard]] std::optional<render::GraphImage> image_at(std::string_view path) const {
        render::GraphImage out;
        out.width = surface_.width;
        out.height = surface_.height;
        if (path.starts_with(kSourcePrefix)) {
            const std::size_t image = std::stoul(std::string(path.substr(kSourcePrefix.size())));
            out.texels.assign(texels_ * 4, 0.0F);
            for (std::size_t c = 0; c < 4; ++c) {
                const std::size_t index = (image * 4) + c;
                if (index >= sources_.size()) {
                    break;
                }
                const std::vector<float>& values = sources_[index];
                for (std::size_t at = 0; at < texels_; ++at) {
                    out.texels[(at * 4) + c] = values[at];
                }
            }
            return out;
        }
        if (path.starts_with(kImagePrefix)) {
            const std::size_t image = std::stoul(std::string(path.substr(kImagePrefix.size())));
            out.texels = images_.at(image);
            return out;
        }
        return std::nullopt;
    }

    // ---- Surface facts ----

    [[nodiscard]] bool painted(std::size_t at) const { return surface_.covered[at] != 0; }

    [[nodiscard]] Vec position(std::size_t at) const {
        return {surface_.position[at * 3], surface_.position[(at * 3) + 1],
                surface_.position[(at * 3) + 2]};
    }

    [[nodiscard]] Vec normal(std::size_t at) const {
        return {surface_.normal[at * 3], surface_.normal[(at * 3) + 1],
                surface_.normal[(at * 3) + 2]};
    }

    // A gray map's texels: the model's, or a sprite's stand-in.
    Result<std::vector<float>> gray_map(std::string_view name, const std::string& at) const {
        if (const auto found = surface_.maps.find(name); found != surface_.maps.end()) {
            return found->second;
        }
        if (surface_.sprite || name == "height") {
            // A sprite has no occlusion, sky or depth to measure; a model with no high mesh
            // is flat. Smart materials made for models still apply.
            const float value = name == "height" ? 0.5F : 1.0F;
            return std::vector<float>(texels_, value);
        }
        return make_error(ErrorCode::InvalidArgument, at + ": the stack needs the model's " +
                                                          std::string(name) +
                                                          " map, which wasn't baked");
    }

    [[nodiscard]] std::vector<float> noise(float scale, int seed, int octaves) const {
        NoiseSettings settings;
        settings.seed = seed;
        settings.frequency = 1.0F;
        settings.fractal_octaves = octaves;
        const Noise noise(settings);
        std::vector<float> out(texels_, 0.5F);
        const auto rows = static_cast<std::size_t>(surface_.height);
        const auto run = [&](std::size_t row) {
            for (std::size_t at = row * surface_.width; at < (row + 1) * surface_.width; ++at) {
                if (!painted(at)) {
                    continue;
                }
                float value = 0.0F;
                if (surface_.sprite) {
                    const auto side = static_cast<float>(std::max(surface_.width, surface_.height));
                    const Vec p = position(at);
                    value = noise.get_noise_2d(p.x / side * scale, p.y / side * scale);
                } else {
                    const Vec p = position(at);
                    const float k = scale / diagonal_;
                    value =
                        noise.get_noise_3d((p.x - surface_.min[0]) * k, (p.y - surface_.min[1]) * k,
                                           (p.z - surface_.min[2]) * k);
                }
                out[at] = clamp01((value * 0.5F) + 0.5F);
            }
        };
        if (pool_ != nullptr) {
            pool_->parallel_for(rows, run);
        } else {
            for (std::size_t row = 0; row < rows; ++row) {
                run(row);
            }
        }
        return out;
    }

    // ---- Graphs a layer or mask reads ----

    Result<const Baked*> bake_graph(const json::Value& layer, const json::Value& graph,
                                    const std::string& at) {
        json::Object overrides;
        if (const json::Value* given = layer.find("parameters");
            given != nullptr && given->is_object()) {
            for (const auto& [name, value] : given->as_object()) {
                if (value.is_string() && value.as_string().starts_with('$')) {
                    const std::string param = value.as_string().substr(1);
                    const auto found = parameters_.find(param);
                    if (found == parameters_.end() ||
                        !(found->second.is_number() || found->second.is_array())) {
                        return make_error(ErrorCode::InvalidArgument,
                                          at + "/parameters/" + json::pointer_escape(name) + ": " +
                                              quoted(value.as_string()) +
                                              " names no number or color parameter of the stack");
                    }
                    overrides.emplace(name, found->second);
                    continue;
                }
                overrides.emplace(name, value);
            }
        }
        const std::array<float, 2> repeat = repeat_of(layer.find("repeat"));
        const std::string key = json::write(graph, json::Style::Compact) +
                                json::write(json::Value(overrides), json::Style::Compact) +
                                std::to_string(repeat[0]) + "x" + std::to_string(repeat[1]);
        if (const auto found = baked_.find(key); found != baked_.end()) {
            return &found->second;
        }
        json::Value doc;
        std::string file = "the graph";
        if (graph.is_string()) {
            file = quoted(graph.as_string());
            auto text = reader_.text ? reader_.text(graph.as_string())
                                     : Result<std::string>(make_error(ErrorCode::InvalidArgument,
                                                                      "nothing reads files here"));
            if (!text) {
                return make_error(text.error().code,
                                  at + "/graph: " + file + ": " + text.error().message);
            }
            auto parsed = json::parse(text.value());
            if (!parsed) {
                return make_error(ErrorCode::InvalidArgument,
                                  at + "/graph: " + file + ": " + parsed.error().message);
            }
            doc = std::move(parsed).value();
            files_.push_back(graph.as_string());
        } else {
            doc = graph;
        }
        if (!is_texture_graph(doc)) {
            return make_error(ErrorCode::InvalidArgument,
                              at + "/graph: " + file +
                                  R"( isn't a texture graph ("type": "graph"))");
        }
        if (const auto errors = validate_texture_graph(doc); !errors.empty()) {
            return make_error(ErrorCode::InvalidArgument,
                              at + "/graph: " + file + " at " +
                                  (errors.front().path.empty() ? "its top" : errors.front().path) +
                                  ": " + errors.front().message);
        }
        auto compiled = compile_texture_graph(doc, reader_, overrides);
        if (!compiled) {
            return make_error(compiled.error().code,
                              at + "/graph: " + file + ": " + compiled.error().message);
        }
        for (const std::string& read : compiled.value().files) {
            files_.push_back(read);
        }
        auto images = bake_texture_graph(compiled.value(), graph_device(), pool_);
        if (!images) {
            return make_error(images.error().code,
                              at + "/graph: " + file + ": " + images.error().message);
        }
        Baked out;
        for (std::size_t i = 0; i < images.value().size(); ++i) {
            const render::Image& image = images.value()[i];
            std::vector<float> texels(texels_ * 4, 0.0F);
            for (std::uint32_t y = 0; y < surface_.height; ++y) {
                for (std::uint32_t x = 0; x < surface_.width; ++x) {
                    const float u = (static_cast<float>(x) + 0.5F) /
                                    static_cast<float>(surface_.width) * repeat[0];
                    const float v = (static_cast<float>(y) + 0.5F) /
                                    static_cast<float>(surface_.height) * repeat[1];
                    const std::array<float, 4> c = sample(image, u, v, true);
                    const std::size_t texel = (std::size_t{y} * surface_.width) + x;
                    std::copy(c.begin(), c.end(),
                              texels.begin() + static_cast<std::ptrdiff_t>(texel * 4));
                }
            }
            out.emplace_back(compiled.value().outputs[i].name, std::move(texels));
        }
        return &baked_.emplace(key, std::move(out)).first->second;
    }

    // ---- Masks ----

    Result<std::string> term(const json::Value& t, const std::string& at) {
        std::string value;
        if (const json::Value* map = t.find("map"); map != nullptr) {
            const std::string& name = map->as_string();
            const json::Value* axis_value = t.find("axis");
            const std::string axis_name = axis_value != nullptr ? axis_value->as_string() : "y";
            const auto axis =
                static_cast<std::size_t>(std::ranges::find(kAxes, axis_name) - kAxes.begin());
            if (contains(kGrayMaps, name)) {
                auto texels = gray_map(name, at + "/map");
                if (!texels) {
                    return texels.error();
                }
                value = source("map:" + name, [&] { return std::move(texels).value(); }, at);
            } else if (name == "world_normal") {
                value = source(
                    "world_normal:" + axis_name,
                    [&] {
                        std::vector<float> out(texels_, 0.0F);
                        for (std::size_t i = 0; i < texels_; ++i) {
                            if (painted(i)) {
                                out[i] = surface_.normal[(i * 3) + axis];
                            }
                        }
                        return out;
                    },
                    at);
            } else if (name == "position") {
                value = source(
                    "position:" + axis_name,
                    [&] {
                        std::vector<float> out(texels_, 0.0F);
                        const float lo = surface_.min.at(axis);
                        const float range = surface_.max.at(axis) - lo;
                        for (std::size_t i = 0; i < texels_; ++i) {
                            if (painted(i)) {
                                out[i] = range > 0.0F
                                             ? (surface_.position[(i * 3) + axis] - lo) / range
                                             : 0.5F;
                            }
                        }
                        return out;
                    },
                    at);
            } else if (name == "alpha") {
                value = source(
                    "alpha",
                    [&] {
                        if (surface_.sprite_colors.empty()) {
                            return std::vector<float>(texels_, 1.0F);
                        }
                        std::vector<float> out(texels_, 0.0F);
                        for (std::size_t i = 0; i < texels_; ++i) {
                            out[i] = surface_.sprite_colors[(i * 4) + 3];
                        }
                        return out;
                    },
                    at);
            } else { // noise
                float scale = 8.0F;
                int seed = 0;
                int octaves = 4;
                detail::read(t, "scale", scale);
                detail::read(t, "seed", seed);
                detail::read(t, "octaves", octaves);
                value = source(
                    "noise:" + std::to_string(scale) + ":" + std::to_string(seed) + ":" +
                        std::to_string(octaves),
                    [&] { return noise(scale, seed, octaves); }, at);
            }
        } else if (t.find("part") != nullptr || t.find("material") != nullptr) {
            const bool part = t.find("part") != nullptr;
            const std::string field = part ? "part" : "material";
            const std::vector<std::string>& known =
                part ? surface_.part_names : surface_.material_names;
            const std::vector<std::int32_t>& ids = part ? surface_.part : surface_.material;
            if (surface_.sprite) {
                return make_error(ErrorCode::InvalidArgument,
                                  at + "/" + field + ": a sprite has no " + field +
                                      "s; mask it by a map, a graph or strokes");
            }
            const json::Value& given = *t.find(field);
            std::vector<std::string> names;
            if (given.is_string()) {
                names.push_back(given.as_string());
            } else {
                for (const json::Value& name : given.as_array()) {
                    names.push_back(name.as_string());
                }
            }
            std::set<std::int32_t> wanted;
            for (std::size_t i = 0; i < names.size(); ++i) {
                const auto found = std::ranges::find(known, names[i]);
                if (found == known.end()) {
                    std::string message = quoted(names[i]) + " isn't a " + field + " of the model";
                    if (const std::string_view near = closest_match(names[i], known);
                        !near.empty()) {
                        message += " (did you mean " + quoted(near) + "?)";
                    }
                    std::string where = at;
                    where += "/";
                    where += field;
                    if (!given.is_string()) {
                        where += "/";
                        where += std::to_string(i);
                    }
                    where += ": ";
                    where += message;
                    return make_error(ErrorCode::InvalidArgument, where);
                }
                wanted.insert(static_cast<std::int32_t>(found - known.begin()));
            }
            std::string key = field + ":";
            for (const std::int32_t id : wanted) {
                key += std::to_string(id) + ",";
            }
            value = source(
                key,
                [&] {
                    std::vector<float> out(texels_, 0.0F);
                    for (std::size_t i = 0; i < texels_; ++i) {
                        if (painted(i) && wanted.contains(ids[i])) {
                            out[i] = 1.0F;
                        }
                    }
                    return out;
                },
                at);
        } else if (const json::Value* graph = t.find("graph"); graph != nullptr) {
            auto baked = bake_graph(t, *graph, at);
            if (!baked) {
                return baked.error();
            }
            const Baked& outputs = *baked.value();
            std::size_t index = 0;
            if (const json::Value* output = t.find("output"); output != nullptr) {
                const auto found = std::ranges::find_if(
                    outputs, [&](const auto& o) { return o.first == output->as_string(); });
                if (found == outputs.end()) {
                    std::vector<std::string> names;
                    for (const auto& o : outputs) {
                        names.push_back(o.first);
                    }
                    std::string message =
                        quoted(output->as_string()) + " isn't an output of the graph";
                    if (const std::string_view near = closest_match(output->as_string(), names);
                        !near.empty()) {
                        message += " (did you mean " + quoted(near) + "?)";
                    }
                    return make_error(ErrorCode::InvalidArgument, at + "/output: " + message);
                }
                index = static_cast<std::size_t>(found - outputs.begin());
            }
            const std::vector<float>& rgba = outputs[index].second;
            value = source(
                "graph:" + std::to_string(reinterpret_cast<std::uintptr_t>(&rgba)),
                [&] {
                    std::vector<float> out(texels_, 0.0F);
                    for (std::size_t i = 0; i < texels_; ++i) {
                        out[i] = rgba[i * 4];
                    }
                    return out;
                },
                at);
        } else {
            return make_error(ErrorCode::InvalidArgument,
                              at + ": give a term a map, part, material or graph");
        }

        if (const json::Value* range = t.find("range"); range != nullptr) {
            json::Value node;
            node["op"] = "remap";
            node["input"] = value;
            node["from"] = *range;
            node["to"] = json::Array{0, 1};
            node["clamp"] = true;
            value = add(std::move(node), at + "/range");
        }
        if (const json::Value* breakup = t.find("breakup"); breakup != nullptr) {
            float scale = 12.0F;
            int seed = 0;
            detail::read(*breakup, "scale", scale);
            detail::read(*breakup, "seed", seed);
            const std::string n = source(
                "noise:" + std::to_string(scale) + ":" + std::to_string(seed + 101) + ":4",
                [&] { return noise(scale, seed + 101, 4); }, at + "/breakup");
            // value + (noise - 0.5) * 2 * amount
            const std::string centered =
                math(n, "subtract", std::nullopt, json::Value(0.5), at + "/breakup");
            const json::Value& amount = *breakup->find("amount");
            const json::Value twice =
                amount.is_number()
                    ? json::Value(amount.as_double() * 2.0)
                    : json::Value(amount.as_string().starts_with('$')
                                      ? "=" + amount.as_string().substr(1) + " * 2"
                                      : "=(" + amount.as_string().substr(1) + ") * 2");
            const std::string moved =
                math(centered, "multiply", std::nullopt, twice, at + "/breakup/amount");
            value = math(value, "add", moved, json::Value(), at + "/breakup");
        }
        const json::Value* softness = t.find("softness");
        const json::Value soft = softness != nullptr ? *softness : json::Value(0.05);
        const auto threshold = [&](const json::Value& level, const std::string& where) {
            json::Value node;
            node["op"] = "threshold";
            node["input"] = value;
            node["level"] = level;
            node["softness"] = soft;
            return add(std::move(node), where);
        };
        const json::Value* above = t.find("above");
        const json::Value* below = t.find("below");
        if (above != nullptr && below != nullptr) {
            const std::string over = threshold(*above, at + "/above");
            const std::string under = threshold(*below, at + "/below");
            const std::string not_under =
                math(under, "one_minus", std::nullopt, json::Value(), at + "/below");
            value = math(over, "multiply", not_under, json::Value(), at);
        } else if (above != nullptr) {
            value = threshold(*above, at + "/above");
        } else if (below != nullptr) {
            const std::string under = threshold(*below, at + "/below");
            value = math(under, "one_minus", std::nullopt, json::Value(), at + "/below");
        }
        if (const json::Value* invert = t.find("invert"); invert != nullptr && invert->as_bool()) {
            value = math(value, "one_minus", std::nullopt, json::Value(), at + "/invert");
        }
        return value;
    }

    // The CPU's strokes for a layer: what they paint and what they erase, 0 to 1 a texel.
    [[nodiscard]] std::pair<std::vector<float>, std::vector<float>>
    strokes(const json::Value& list) const {
        std::vector<float> paint(texels_, 0.0F);
        std::vector<float> erase(texels_, 0.0F);
        bool any_erase = false;
        for (const json::Value& stroke : list.as_array()) {
            const auto radius = static_cast<float>(stroke.find("size")->as_double());
            float hardness = 0.5F;
            detail::read(stroke, "hardness", hardness);
            bool erasing = false;
            detail::read(stroke, "erase", erasing);
            any_erase = any_erase || erasing;
            std::vector<Vec> points;
            for (const json::Value& point : stroke.find("points")->as_array()) {
                points.push_back(point_of(point));
            }
            std::vector<float>& into = erasing ? erase : paint;
            const std::size_t segments = std::max<std::size_t>(1, points.size() - 1);
            const auto rows = static_cast<std::size_t>(surface_.height);
            const auto run = [&](std::size_t row) {
                for (std::size_t at = row * surface_.width; at < (row + 1) * surface_.width; ++at) {
                    if (!painted(at)) {
                        continue;
                    }
                    const Vec p = position(at);
                    float nearest = std::numeric_limits<float>::max();
                    for (std::size_t s = 0; s < segments; ++s) {
                        const Vec a = points[s];
                        const Vec b = points[std::min(s + 1, points.size() - 1)];
                        const Vec ab = b - a;
                        const float len2 = dot(ab, ab);
                        const float t = len2 > 0.0F ? clamp01(dot(p - a, ab) / len2) : 0.0F;
                        nearest = std::min(nearest, length(p - (a + (ab * t))));
                    }
                    const float d = nearest / radius;
                    const float strength = d >= 1.0F ? 0.0F : 1.0F - smoothstep(hardness, 1.0F, d);
                    into[at] = std::max(into[at], strength);
                }
            };
            if (pool_ != nullptr) {
                pool_->parallel_for(rows, run);
            } else {
                for (std::size_t row = 0; row < rows; ++row) {
                    run(row);
                }
            }
        }
        if (!any_erase) {
            erase.clear();
        }
        return {std::move(paint), std::move(erase)};
    }

    // A layer's decals composited on the CPU: RGBA floats, alpha where they cover.
    Result<std::vector<float>> decals(const json::Value& list, const std::string& at) {
        std::vector<float> out(texels_ * 4, 0.0F);
        for (std::size_t d = 0; d < list.as_array().size(); ++d) {
            const json::Value& decal = list.as_array()[d];
            const std::string where = at + "/" + std::to_string(d);
            const std::string& path = decal.find("image")->as_string();
            if (!reader_.image) {
                return make_error(ErrorCode::InvalidArgument,
                                  where + "/image: nothing reads images here");
            }
            auto image = reader_.image(path);
            if (!image) {
                return make_error(image.error().code,
                                  where + "/image: " + quoted(path) + ": " + image.error().message);
            }
            files_.push_back(path);
            const render::Image& picture = image.value();
            const float aspect = picture.width > 0 ? static_cast<float>(picture.height) /
                                                         static_cast<float>(picture.width)
                                                   : 1.0F;
            float w = 1.0F;
            float h = 0.0F;
            if (const json::Value* size = decal.find("size"); size != nullptr) {
                if (size->is_number()) {
                    w = static_cast<float>(size->as_double());
                    h = w * aspect;
                } else {
                    w = static_cast<float>(size->as_array()[0].as_double());
                    h = static_cast<float>(size->as_array()[1].as_double());
                }
            } else if (surface_.sprite) {
                w = static_cast<float>(picture.width);
                h = static_cast<float>(picture.height);
            } else {
                w = diagonal_ * 0.25F;
                h = w * aspect;
            }
            const Vec center = point_of(*decal.find("at"));
            Vec right{1.0F, 0.0F, 0.0F};
            Vec up{0.0F, 1.0F, 0.0F};
            Vec forward{0.0F, 0.0F, -1.0F};
            float depth = std::max(w, h);
            if (surface_.sprite) {
                float turns = 0.0F;
                detail::read(decal, "rotation", turns);
                const float angle = turns * 6.2831853F;
                // Rows run down a sprite, so clockwise on screen turns right toward +y.
                right = {std::cos(angle), std::sin(angle), 0.0F};
                up = {std::sin(angle), -std::cos(angle), 0.0F};
            } else {
                if (const json::Value* given = decal.find("direction")) {
                    forward = normalized(point_of(*given));
                }
                if (const json::Value* given = decal.find("up")) {
                    up = point_of(*given);
                }
                right = normalized(cross(forward, up));
                if (length(right) == 0.0F) {
                    return make_error(ErrorCode::InvalidArgument,
                                      where + "/up: up runs along direction; give another");
                }
                up = cross(right, forward);
                detail::read(decal, "depth", depth);
                if (depth <= 0.0F) {
                    depth = std::max(w, h);
                }
            }
            for (std::size_t i = 0; i < texels_; ++i) {
                if (!painted(i)) {
                    continue;
                }
                const Vec offset = position(i) - center;
                const float u = (dot(offset, right) / w) + 0.5F;
                const float v = 0.5F - (dot(offset, up) / h);
                if (u < 0.0F || u > 1.0F || v < 0.0F || v > 1.0F) {
                    continue;
                }
                float facing = 1.0F;
                if (!surface_.sprite) {
                    if (std::abs(dot(offset, forward)) > depth * 0.5F) {
                        continue;
                    }
                    // Faces turned from the projection get none of it, those side on fade.
                    facing = clamp01((-dot(normal(i), forward) - 0.1F) / 0.2F);
                    if (facing <= 0.0F) {
                        continue;
                    }
                }
                const std::array<float, 4> c = sample(picture, u, v, false);
                const float a = c[3] * facing;
                float* px = &out[i * 4];
                for (std::size_t k = 0; k < 3; ++k) {
                    px[k] = (c.at(k) * a) + (px[k] * (1.0F - a));
                }
                px[3] = a + (px[3] * (1.0F - a));
            }
        }
        // Colors where the decals are faint come from where they are strong: un-premultiplied.
        for (std::size_t i = 0; i < texels_; ++i) {
            float* px = &out[i * 4];
            if (px[3] > 0.0F) {
                for (std::size_t k = 0; k < 3; ++k) {
                    px[k] = clamp01(px[k] / px[3]);
                }
            }
        }
        return out;
    }

    // ---- Layers ----

    // The bases every channel starts from.
    void build_bases() {
        const json::Value* base = stack_.find("base");
        const bool high = stack_.find("high") != nullptr;
        for (const std::string_view channel : kPaintChannels) {
            const std::string at = "/base/" + std::string(channel);
            const json::Value* given = base != nullptr ? base->find(channel) : nullptr;
            json::Value value;
            if (given != nullptr) {
                value = *given;
            } else if (channel == "albedo") {
                value = surface_.sprite ? json::Value("sprite") : json::Array{0.8, 0.8, 0.8};
            } else if (channel == "normal") {
                value = !surface_.normal_map.empty() && (high || surface_.sprite)
                            ? json::Value("normal")
                            : json::Array{0.5, 0.5, 1.0};
            } else if (channel == "emission") {
                value = json::Array{0.0, 0.0, 0.0};
            } else if (channel == "metallic") {
                value = 0.0;
            } else if (channel == "ao") {
                // A model's own ambient occlusion, as its baked map, under everything painted.
                value = !surface_.sprite && surface_.maps.contains("ao") ? json::Value("ao")
                                                                         : json::Value(1.0);
            } else {
                value = 0.5; // roughness and height
            }
            acc_[std::string(channel)] = value_node(channel, value, at);
            if (given != nullptr && channel == "emission") {
                emits_ = true;
            }
        }
    }

    // A node holding a channel's value: a number, a color, a setting or a map.
    std::string value_node(std::string_view channel, const json::Value& value,
                           const std::string& at) {
        if (value.is_string() && !value.as_string().starts_with('$') &&
            !value.as_string().starts_with('=')) {
            const std::string& name = value.as_string();
            if (name == "sprite") {
                std::vector<float> texels = surface_.sprite_colors;
                texels.resize(texels_ * 4, 1.0F);
                return color_image(std::move(texels), at);
            }
            if (name == "normal") {
                std::vector<float> texels(texels_ * 4, 1.0F);
                for (std::size_t i = 0; i < texels_; ++i) {
                    for (std::size_t k = 0; k < 3; ++k) {
                        const float flat = k == 2 ? 1.0F : 0.5F;
                        texels[(i * 4) + k] =
                            surface_.normal_map.empty() ? flat : surface_.normal_map[(i * 3) + k];
                    }
                }
                return color_image(std::move(texels), at);
            }
            auto texels = gray_map(name, at);
            return source(
                "map:" + name,
                [&] {
                    return texels ? std::move(texels).value() : std::vector<float>(texels_, 0.5F);
                },
                at);
        }
        if (is_color_channel(channel)) {
            if (value.is_number()) {
                return gray(value, at);
            }
            if (value.is_array()) {
                json::Array color = value.as_array();
                if (color.size() == 3) {
                    color.emplace_back(1);
                }
                return constant(json::Value(std::move(color)), at);
            }
            return constant(value, at); // "$tint": a color parameter
        }
        return gray(value, at);
    }

    Result<void> add_layer(const json::Value& layer, const std::string& at,
                           std::set<std::string>& touched) {
        const std::string& name = layer.find("name")->as_string();
        std::map<std::string, std::string> content;

        // A graph's outputs by channel name.
        if (const json::Value* graph = layer.find("graph"); graph != nullptr) {
            auto baked = bake_graph(layer, *graph, at);
            if (!baked) {
                return baked.error();
            }
            bool any = false;
            for (const auto& [output, rgba] : *baked.value()) {
                const std::string channel = output == "color" ? "albedo" : output;
                if (channel == "orm") {
                    const std::string whole = color_image(rgba, at + "/graph");
                    const std::array<std::pair<std::string, char>, 3> parts{
                        {{"ao", 'r'}, {"roughness", 'g'}, {"metallic", 'b'}}};
                    for (const auto& [part, letter] : parts) {
                        json::Value node;
                        node["op"] = "swizzle";
                        node["input"] = whole;
                        node["channels"] = std::string(3, letter) + "1";
                        content.try_emplace(part, add(std::move(node), at + "/graph"));
                    }
                    any = true;
                    continue;
                }
                if (!contains(kPaintChannels, channel)) {
                    continue;
                }
                std::string node = color_image(rgba, at + "/graph");
                if (!is_color_channel(channel)) {
                    json::Value red;
                    red["op"] = "swizzle";
                    red["input"] = node;
                    red["channels"] = "rrr1";
                    node = add(std::move(red), at + "/graph");
                }
                content[channel] = node;
                any = true;
            }
            if (!any) {
                return make_error(ErrorCode::InvalidArgument,
                                  at + "/graph: none of its outputs is named as a channel "
                                       "(albedo or color, roughness, metallic, ao, height, "
                                       "normal, emission or orm)");
            }
        }

        // Decals: their colors paint albedo and their alpha masks the layer.
        std::optional<std::string> decal_alpha;
        if (const json::Value* list = layer.find("decals");
            list != nullptr && !list->as_array().empty()) {
            auto made = decals(*list, at + "/decals");
            if (!made) {
                return made.error();
            }
            std::vector<float> alpha(texels_, 0.0F);
            for (std::size_t i = 0; i < texels_; ++i) {
                alpha[i] = made.value()[(i * 4) + 3];
            }
            content["albedo"] = color_image(std::move(made).value(), at + "/decals");
            decal_alpha =
                source("decals:" + name, [&] { return std::move(alpha); }, at + "/decals");
        }

        // Flat values win over the graph's and the decals'.
        if (const json::Value* fill = layer.find("fill"); fill != nullptr) {
            for (const auto& [channel, value] : fill->as_object()) {
                std::string where = at + "/fill/";
                where += channel;
                content[channel] = value_node(channel, value, where);
            }
        }
        if (content.empty()) {
            return make_error(ErrorCode::InvalidArgument,
                              at + ": the layer paints nothing: give it a fill, a graph or "
                                   "decals");
        }

        // The mask: its terms, the decals' alpha, then strokes.
        std::optional<std::string> mask;
        if (const json::Value* given = layer.find("mask"); given != nullptr) {
            const bool many = given->is_array();
            const std::size_t count = many ? given->as_array().size() : 1;
            for (std::size_t k = 0; k < count; ++k) {
                const json::Value& t = many ? given->as_array()[k] : *given;
                const std::string where = at + "/mask" + (many ? "/" + std::to_string(k) : "");
                auto value = term(t, where);
                if (!value) {
                    return value.error();
                }
                if (!mask) {
                    mask = value.value();
                    continue;
                }
                const json::Value* op = t.find("op");
                const std::string operation = op != nullptr ? op->as_string() : "multiply";
                mask = math(*mask, operation, value.value(), json::Value(), where + "/op");
            }
            if (count > 1) {
                json::Value node;
                node["op"] = "clamp";
                node["input"] = *mask;
                mask = add(std::move(node), at + "/mask");
            }
        }
        if (decal_alpha && mask) {
            mask = math(*mask, "multiply", decal_alpha, json::Value(), at + "/decals");
        } else if (decal_alpha) {
            mask = decal_alpha;
        }
        if (const json::Value* list = layer.find("strokes");
            list != nullptr && !list->as_array().empty()) {
            auto made = strokes(*list);
            std::vector<float>& paint = made.first;
            std::vector<float>& erase = made.second;
            const bool painting = std::ranges::any_of(list->as_array(), [](const json::Value& s) {
                const json::Value* e = s.find("erase");
                return e == nullptr || !e->as_bool();
            });
            if (painting) {
                const std::string p =
                    source("strokes:" + name, [&] { return std::move(paint); }, at + "/strokes");
                mask = mask ? math(*mask, "max", p, json::Value(), at + "/strokes") : p;
            }
            if (!erase.empty()) {
                const std::string e =
                    source("erase:" + name, [&] { return std::move(erase); }, at + "/strokes");
                const std::string keep =
                    math(e, "one_minus", std::nullopt, json::Value(), at + "/strokes");
                mask = mask ? math(*mask, "multiply", keep, json::Value(), at + "/strokes") : keep;
            }
        }

        // Only the channels asked for, each blended over what is below.
        std::vector<std::string> channels;
        if (const json::Value* only = layer.find("channels"); only != nullptr) {
            for (const json::Value& channel : only->as_array()) {
                channels.push_back(channel.as_string());
            }
        } else {
            for (const auto& [channel, node] : content) {
                channels.push_back(channel);
            }
        }
        const json::Value* blend = layer.find("blend");
        const json::Value* opacity = layer.find("opacity");
        for (const std::string& channel : channels) {
            const auto found = content.find(channel);
            if (found == content.end()) {
                continue;
            }
            std::string mode = "mix";
            if (blend != nullptr) {
                if (blend->is_string()) {
                    mode = blend->as_string();
                } else if (const json::Value* by = blend->find(channel); by != nullptr) {
                    mode = by->as_string();
                }
            }
            json::Value node;
            node["op"] = "blend";
            node["base"] = acc_[channel];
            node["layer"] = found->second;
            if (mask) {
                node["mask"] = *mask;
            }
            node["mode"] = mode;
            if (opacity != nullptr) {
                node["opacity"] = *opacity;
            }
            acc_[channel] = add(std::move(node), opacity != nullptr ? at + "/opacity" : at);
            touched.insert(channel);
        }
        if (mask) {
            json::Value output;
            output["name"] = "mask." + name;
            output["node"] = *mask;
            output["as"] = "gray";
            mask_outputs_.emplace_back(std::move(output));
            mask_layers_.push_back(name);
        }
        return {};
    }

    Result<void> add_outputs(const std::set<std::string>& touched) {
        std::vector<std::string> names;
        if (const json::Value* given = stack_.find("outputs"); given != nullptr) {
            for (const json::Value& name : given->as_array()) {
                names.push_back(name.as_string());
            }
        } else {
            names = {"albedo"};
            if (!surface_.sprite) {
                names.emplace_back("orm");
            }
            // A sprite with no normal map gets one only when a layer bends it.
            if (!surface_.sprite || !surface_.normal_map.empty() || touched.contains("normal") ||
                touched.contains("height")) {
                names.emplace_back("normal");
            }
            if (touched.contains("height")) {
                names.emplace_back("height");
            }
            if (emits_ || touched.contains("emission")) {
                names.emplace_back("emission");
            }
        }
        // The height's slopes join the normal, as overlay joins two normal maps.
        float strength = 2.0F;
        detail::read(stack_, "height_normal", strength);
        std::string normal = acc_["normal"];
        if (touched.contains("height") && strength > 0.0F) {
            json::Value slopes;
            slopes["op"] = "normal_from_height";
            slopes["height"] = acc_["height"];
            slopes["strength"] = strength;
            const std::string from_height = add(std::move(slopes), "/height_normal");
            json::Value joined;
            joined["op"] = "blend";
            joined["base"] = normal;
            joined["layer"] = from_height;
            joined["mode"] = "overlay";
            normal = add(std::move(joined), "/height_normal");
        }
        json::Array outputs;
        for (const std::string& name : names) {
            json::Value output;
            output["name"] = name;
            if (name == "albedo") {
                // The albedo's alpha is the sprite's, or 1.
                json::Value merged;
                merged["op"] = "merge";
                merged["red"] = json::Object{{"node", acc_["albedo"]}, {"channel", "r"}};
                merged["green"] = json::Object{{"node", acc_["albedo"]}, {"channel", "g"}};
                merged["blue"] = json::Object{{"node", acc_["albedo"]}, {"channel", "b"}};
                if (!surface_.sprite_colors.empty()) {
                    json::Value term;
                    term["map"] = "alpha";
                    auto alpha = this->term(term, "/sprite");
                    if (!alpha) {
                        return alpha.error();
                    }
                    merged["alpha"] = alpha.value();
                } else {
                    merged["alpha"] = 1;
                }
                output["node"] = add(std::move(merged), "/outputs");
            } else if (name == "orm") {
                json::Value merged;
                merged["op"] = "merge";
                merged["red"] = acc_["ao"];
                merged["green"] = acc_["roughness"];
                merged["blue"] = acc_["metallic"];
                merged["alpha"] = 1;
                output["node"] = add(std::move(merged), "/outputs");
            } else if (name == "normal") {
                output["node"] = normal;
            } else {
                output["node"] = acc_[name];
                if (!is_color_channel(name)) {
                    output["as"] = "gray";
                }
            }
            outputs.emplace_back(std::move(output));
            output_names_.push_back(name);
        }
        for (json::Value& mask : mask_outputs_) {
            outputs.emplace_back(std::move(mask));
        }
        outputs_ = std::move(outputs);
        return {};
    }

    const json::Value& stack_;
    const PaintSurface& surface_;
    const GraphReader& reader_;
    jobs::Pool* pool_;
    std::size_t texels_;
    float diagonal_ = 1.0F;
    json::Object parameters_;
    json::Value nodes_ = json::Object{};  // NOLINT(readability-redundant-member-init)
    json::Value outputs_ = json::Array{}; // NOLINT(readability-redundant-member-init)
    std::map<std::string, std::string> origin_;
    int next_ = 0;
    std::vector<std::vector<float>> sources_;
    std::map<std::string, std::size_t> source_of_;
    std::map<std::size_t, std::string> source_nodes_;
    std::map<std::size_t, std::string> source_images_;
    std::vector<std::vector<float>> images_;
    std::map<std::string, Baked> baked_;
    std::map<std::string, std::string> acc_;
    bool emits_ = false;
    std::vector<json::Value> mask_outputs_;
    std::vector<std::string> output_names_;
    std::vector<std::string> mask_layers_;
    std::vector<std::string> files_;
};

void add_need(std::vector<std::string>& needs, const std::string& map) {
    if (!contains(needs, map)) {
        needs.push_back(map);
    }
}

} // namespace

bool is_paint_stack(const json::Value& doc) {
    const json::Value* type = doc.is_object() ? doc.find("type") : nullptr;
    return type != nullptr && type->is_string() && type->as_string() == kPaintType;
}

json::Value paint_schema() {
    json::Value schema;
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["required"] = json::Array{"format", "type"};
    schema["description"] = "A paint stack: a model's or sprite's texturing as layers, bottom "
                            "to top; with neither, a smart material others extend.";
    json::Value& p = schema["properties"];
    p["format"]["const"] = 1;
    p["format"]["description"] = "Format version. Must be 1.";
    p["type"]["const"] = kPaintType;
    p["type"]["description"] = "\"paint\" for a paint stack.";
    p["extends"] = text_field("A stack (or \"builtin:paint/<name>\") this one starts from: its "
                              "fields merged under, its layers below, a layer of the same name "
                              "changed in place.");
    p["model"] = text_field("The model painted, project path; its first texture coordinates.");
    p["high"] = text_field("A detailed model its height and normal maps bake from.");
    p["sprite"] = text_field("The sprite painted, an image's project path.");
    p["normal_map"] = text_field("Sprite: its normal map, OpenGL style (green up).");
    p["edge_width"] = number_field(4.0F,
                                   "Sprite: the pixels its curvature (the outline's edge) "
                                   "reaches in.",
                                   0.5, 256.0);
    p["size"] = size_field("Texels wide and high, or [width, height]. Default 1024.");
    p["size"]["items"]["type"] = "integer";
    p["size"]["type"] = json::Array{"integer", "array"};
    p["size"]["minimum"] = 16;
    p["size"]["maximum"] = render::kMaxGraphSize;
    p["size"]["items"]["minimum"] = 16;
    p["size"]["items"]["maximum"] = render::kMaxGraphSize;
    json::Value& maps = p["maps"];
    maps["type"] = "object";
    maps["additionalProperties"] = false;
    maps["description"] = "How the model's mesh maps bake, as model.bake_maps takes them.";
    maps["properties"]["rays"] = integer_field(32, "Rays a texel traces.", 1, 1024);
    for (const std::string_view distance :
         {"occlusion_distance", "thickness_distance", "curvature_radius", "cage_distance"}) {
        maps["properties"][distance]["type"] = "number";
        maps["properties"][distance]["exclusiveMinimum"] = 0;
    }
    maps["properties"]["padding"] = integer_field(4, "Texels islands grow by.", 0, 64);
    p["parameters"]["type"] = "object";
    p["parameters"]["description"] =
        "Values settings name as \"$name\" or in \"=\" expressions: a number, a list of numbers "
        "(a color), or an expression over the other parameters.";
    p["parameters"]["additionalProperties"]["type"] = json::Array{"number", "array", "string"};
    json::Value& base = p["base"];
    base["type"] = "object";
    base["additionalProperties"] = false;
    base["description"] = "What each channel starts from under the layers: a value, a setting, "
                          "or a map (gray channels: ao, curvature, thickness, sky, height; "
                          "normal: normal, the baked normal map; albedo: sprite).";
    for (const std::string_view channel : kPaintChannels) {
        base["properties"][channel]["type"] = json::Array{"number", "array", "string"};
    }
    p["layers"]["type"] = "array";
    p["layers"]["items"]["$ref"] = "#/$defs/layer";
    p["layers"]["description"] = "The layers, bottom to top.";
    json::Value outputs;
    outputs["type"] = "array";
    outputs["items"] = names_field(kPaintOutputs, "A texture to bake.");
    outputs["uniqueItems"] = true;
    outputs["minItems"] = 1;
    outputs["description"] = "The textures paint.bake writes. Default albedo, orm (models), "
                             "normal, and height and emission where layers paint them.";
    p["outputs"] = std::move(outputs);
    p["height_normal"] = number_field(2.0F,
                                      "How steep painted height makes the normal map; 0 "
                                      "keeps them apart.",
                                      0.0, 64.0);
    p["material"] = text_field("Model: the .standard_material.json paint.bake points at the "
                               "textures. Default beside the stack, named after it.");
    json::Value& expect = p["expect"];
    expect["type"] = "object";
    expect["description"] =
        "What the bake should measure, which texture.check holds it to: \"<output>.<measure>\" "
        "and [min, max] (value, r, g, b, a, low, high, contrast; 0 to 1), or \"mask.<layer>\" "
        "and the share of the surface its mask covers.";
    expect["additionalProperties"]["type"] = "array";
    expect["additionalProperties"]["items"]["type"] = "number";
    expect["additionalProperties"]["minItems"] = 2;
    expect["additionalProperties"]["maxItems"] = 2;
    schema["$defs"]["layer"] = layer_schema();
    schema["$defs"]["term"] = term_schema();
    return schema;
}

std::vector<json::ValidationError> validate_paint(const json::Value& doc) {
    std::vector<json::ValidationError> errors = json::validate(doc, paint_schema());
    if (!errors.empty()) {
        return errors;
    }
    check_names(doc, errors);
    check_kind(doc, errors);
    check_base(doc, errors);
    return errors;
}

std::vector<std::string_view> builtin_paint_names() {
    std::vector<std::string_view> out;
    out.reserve(kBuiltins.size());
    for (const Builtin& builtin : kBuiltins) {
        out.push_back(builtin.name);
    }
    return out;
}

std::optional<std::string_view> builtin_paint(std::string_view path) {
    if (!path.starts_with(kBuiltinPaintPrefix)) {
        return std::nullopt;
    }
    const std::string_view name = path.substr(kBuiltinPaintPrefix.size());
    for (const Builtin& builtin : kBuiltins) {
        if (builtin.name == name) {
            return builtin.text;
        }
    }
    return std::nullopt;
}

Result<json::Value> resolve_paint(const json::Value& doc, const GraphReader& reader,
                                  std::vector<std::string>* files) {
    std::vector<json::Value> chain{doc};
    std::vector<std::string> seen;
    for (int depth = 0;; ++depth) {
        const json::Value* extends = chain.back().find("extends");
        if (extends == nullptr || !extends->is_string()) {
            break;
        }
        const std::string path = extends->as_string();
        if (depth >= kMaxExtends || contains(seen, path)) {
            return make_error(
                ErrorCode::InvalidArgument,
                "/extends: " + quoted(path) +
                    (depth >= kMaxExtends ? " is more than 8 files deep" : " extends itself"));
        }
        seen.push_back(path);
        auto text = read_text(reader, path);
        if (!text) {
            return make_error(text.error().code,
                              "/extends: " + quoted(path) + ": " + text.error().message);
        }
        auto parsed = json::parse(text.value());
        if (!parsed) {
            return make_error(ErrorCode::InvalidArgument,
                              "/extends: " + quoted(path) + ": " + parsed.error().message);
        }
        if (!is_paint_stack(parsed.value())) {
            return make_error(ErrorCode::InvalidArgument,
                              "/extends: " + quoted(path) +
                                  R"( isn't a paint stack ("type": "paint"))");
        }
        if (const auto errors = validate_paint(parsed.value()); !errors.empty()) {
            return make_error(ErrorCode::InvalidArgument,
                              "/extends: " + quoted(path) + " at " +
                                  (errors.front().path.empty() ? "its top" : errors.front().path) +
                                  ": " + errors.front().message);
        }
        if (files != nullptr && !path.starts_with(kBuiltinPaintPrefix)) {
            files->push_back(path);
        }
        chain.push_back(std::move(parsed).value());
    }
    // From the deepest up: each over the one it extends.
    json::Value out = chain.back();
    out.as_object().erase("extends");
    for (std::size_t i = chain.size() - 1; i-- > 0;) {
        json::Value patch = chain[i];
        patch.as_object().erase("extends");
        json::Value layers = json::Array{};
        if (json::Value* own = patch.find("layers"); own != nullptr) {
            layers = std::move(*own);
            patch.as_object().erase("layers");
        }
        json::Value below = json::Array{};
        if (json::Value* theirs = out.find("layers"); theirs != nullptr) {
            below = std::move(*theirs);
        }
        out = world::merge_patch(out, patch);
        for (json::Value& layer : layers.as_array()) {
            const std::string& name = layer.find("name")->as_string();
            if (json::Value* same = layer_named(below.as_array(), name); same != nullptr) {
                *same = world::merge_patch(*same, layer);
            } else {
                below.as_array().push_back(std::move(layer));
            }
        }
        if (!below.as_array().empty()) {
            out["layers"] = std::move(below);
        }
    }
    return out;
}

std::vector<std::string> paint_needs(const json::Value& resolved) {
    std::vector<std::string> needs;
    const json::Value* base = resolved.find("base");
    const auto wants = [&](const json::Value& value) {
        if (value.is_string() &&
            (contains(kGrayMaps, value.as_string()) || value.as_string() == "normal")) {
            add_need(needs, value.as_string());
        }
    };
    if (base != nullptr && base->is_object()) {
        for (const auto& [channel, value] : base->as_object()) {
            wants(value);
        }
    }
    // A model's ambient occlusion starts from its baked map.
    if (resolved.find("model") != nullptr && (base == nullptr || base->find("ao") == nullptr)) {
        add_need(needs, "ao");
    }
    // With a high model the normal starts from its baked normal map.
    if (resolved.find("high") != nullptr && (base == nullptr || base->find("normal") == nullptr)) {
        add_need(needs, "normal");
    }
    const json::Value* layers = resolved.find("layers");
    if (layers == nullptr) {
        return needs;
    }
    const auto term = [&](const json::Value& t) {
        if (const json::Value* map = t.find("map");
            map != nullptr && map->is_string() && contains(kGrayMaps, map->as_string())) {
            add_need(needs, map->as_string());
        }
    };
    for (const json::Value& layer : layers->as_array()) {
        if (const json::Value* enabled = layer.find("enabled");
            enabled != nullptr && !enabled->as_bool()) {
            continue;
        }
        if (const json::Value* fill = layer.find("fill"); fill != nullptr) {
            for (const auto& [channel, value] : fill->as_object()) {
                if (!is_color_channel(channel) || value.is_string()) {
                    wants(value);
                }
            }
        }
        if (const json::Value* mask = layer.find("mask"); mask != nullptr) {
            if (mask->is_array()) {
                std::ranges::for_each(mask->as_array(), term);
            } else {
                term(*mask);
            }
        }
    }
    return needs;
}

Result<PaintSurface> paint_sprite_surface(const render::Image& sprite,
                                          const render::Image* normal_map, float edge_width) {
    if (sprite.width == 0 || sprite.height == 0) {
        return make_error(ErrorCode::InvalidArgument, "the sprite has no pixels");
    }
    if (sprite.width > render::kMaxGraphSize || sprite.height > render::kMaxGraphSize) {
        return make_error(ErrorCode::InvalidArgument, "the sprite is larger than " +
                                                          std::to_string(render::kMaxGraphSize) +
                                                          " pixels a side");
    }
    if (normal_map != nullptr &&
        (normal_map->width != sprite.width || normal_map->height != sprite.height)) {
        return make_error(ErrorCode::InvalidArgument, "the normal map isn't the sprite's size (" +
                                                          std::to_string(sprite.width) + " x " +
                                                          std::to_string(sprite.height) + ")");
    }
    PaintSurface out;
    out.sprite = true;
    out.width = sprite.width;
    out.height = sprite.height;
    const std::size_t texels = std::size_t{sprite.width} * sprite.height;
    out.covered.assign(texels, 2);
    out.sprite_colors.resize(texels * 4);
    out.position.resize(texels * 3);
    out.normal.assign(texels * 3, 0.0F);
    out.max = {static_cast<float>(sprite.width), static_cast<float>(sprite.height), 0.0F};
    for (std::size_t i = 0; i < texels; ++i) {
        for (std::size_t c = 0; c < 4; ++c) {
            out.sprite_colors[(i * 4) + c] =
                static_cast<float>(sprite.pixels[(i * 4) + c]) / 255.0F;
        }
        if (sprite.pixels[(i * 4) + 3] >= 128) {
            out.covered[i] = 1;
        }
        const std::size_t column = i % sprite.width;
        const std::size_t row = i / sprite.width;
        out.position[i * 3] = static_cast<float>(column) + 0.5F;
        out.position[(i * 3) + 1] = static_cast<float>(row) + 0.5F;
        if (normal_map != nullptr) {
            Vec n{(static_cast<float>(normal_map->pixels[i * 4]) / 127.5F) - 1.0F,
                  (static_cast<float>(normal_map->pixels[(i * 4) + 1]) / 127.5F) - 1.0F,
                  (static_cast<float>(normal_map->pixels[(i * 4) + 2]) / 127.5F) - 1.0F};
            n = normalized(n);
            out.normal[i * 3] = n.x;
            out.normal[(i * 3) + 1] = n.y;
            out.normal[(i * 3) + 2] = n.z;
        } else {
            out.normal[(i * 3) + 2] = 1.0F;
        }
    }
    if (normal_map != nullptr) {
        out.normal_map.resize(texels * 3);
        for (std::size_t i = 0; i < texels; ++i) {
            for (std::size_t c = 0; c < 3; ++c) {
                out.normal_map[(i * 3) + c] =
                    static_cast<float>(normal_map->pixels[(i * 4) + c]) / 255.0F;
            }
        }
    }
    // The outline's edge: how near each opaque pixel is to a clear one (or the sprite's edge).
    const auto reach = static_cast<int>(std::ceil(edge_width));
    const auto w = static_cast<int>(sprite.width);
    const auto h = static_cast<int>(sprite.height);
    const auto opaque = [&](int x, int y) {
        if (x < 0 || y < 0 || x >= w || y >= h) {
            return false;
        }
        return sprite.pixels[((static_cast<std::size_t>(y) * sprite.width) +
                              static_cast<std::size_t>(x)) *
                                 4 +
                             3] >= 128;
    };
    std::vector<float> curvature(texels, 0.5F);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            if (!opaque(x, y)) {
                continue;
            }
            float nearest = edge_width;
            for (int dy = -reach; dy <= reach; ++dy) {
                for (int dx = -reach; dx <= reach; ++dx) {
                    if (!opaque(x + dx, y + dy)) {
                        nearest = std::min(
                            nearest, std::sqrt(static_cast<float>((dx * dx) + (dy * dy))) - 0.5F);
                    }
                }
            }
            curvature[(static_cast<std::size_t>(y) * sprite.width) + static_cast<std::size_t>(x)] =
                0.5F + (0.5F * (1.0F - clamp01(nearest / edge_width)));
        }
    }
    out.maps.emplace("curvature", std::move(curvature));
    return out;
}

Result<CompiledPaint> compile_paint(const json::Value& resolved, const PaintSurface& surface,
                                    const GraphReader& reader, const json::Object& overrides,
                                    jobs::Pool* pool) {
    if (surface.width == 0 || surface.height == 0 ||
        surface.covered.size() != std::size_t{surface.width} * surface.height) {
        return make_error(ErrorCode::InvalidArgument, "the surface to paint is empty");
    }
    Compiler compiler(resolved, surface, reader, pool);
    return compiler.run(overrides);
}

json::Value measure_paint(const render::Image& image, const PaintSurface& surface) {
    std::array<double, 4> sums{};
    double luminance_sum = 0.0;
    double luminance_squares = 0.0;
    double low = 1.0;
    double high = 0.0;
    std::size_t count = 0;
    const std::size_t texels = image.pixels.size() / 4;
    for (std::size_t i = 0; i < texels; ++i) {
        if (i < surface.covered.size() && surface.covered[i] != 1) {
            continue;
        }
        const std::uint8_t* pixel = &image.pixels[i * 4];
        for (std::size_t c = 0; c < 4; ++c) {
            sums.at(c) += pixel[c];
        }
        const double luminance =
            ((0.2126 * pixel[0]) + (0.7152 * pixel[1]) + (0.0722 * pixel[2])) / 255.0;
        luminance_sum += luminance;
        luminance_squares += luminance * luminance;
        low = std::min(low, luminance);
        high = std::max(high, luminance);
        ++count;
    }
    const auto rounded = [](double v) -> json::Value {
        const double out = std::round(v * 1000.0) / 1000.0;
        if (out == std::floor(out)) {
            return static_cast<std::int64_t>(out);
        }
        return out;
    };
    const double n = count > 0 ? static_cast<double>(count) : 1.0;
    const double mean = luminance_sum / n;
    json::Value out;
    out["value"] = rounded(mean);
    constexpr std::array<std::string_view, 4> kChannels{"r", "g", "b", "a"};
    for (std::size_t c = 0; c < 4; ++c) {
        out[kChannels.at(c)] = rounded(sums.at(c) / n / 255.0);
    }
    out["low"] = rounded(count > 0 ? low : 0.0);
    out["high"] = rounded(high);
    out["contrast"] = rounded(std::sqrt(std::max(0.0, (luminance_squares / n) - (mean * mean))));
    return out;
}

} // namespace slash::procedural
