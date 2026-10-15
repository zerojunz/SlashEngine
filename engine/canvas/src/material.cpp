#include "slash/canvas/material.hpp"

#include "slash/core/text.hpp"
#include "slash/render/image.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <set>
#include <string>
#include <utility>

namespace slash::canvas {
namespace {

using render::Op;

// What a value is. A float is kept in every component of its register.
enum class Kind : std::uint8_t { Bad, Float, Vec2, Vec3, Vec4, Texture };

int size_of(Kind kind) {
    switch (kind) {
    case Kind::Float:
        return 1;
    case Kind::Vec2:
        return 2;
    case Kind::Vec3:
        return 3;
    case Kind::Vec4:
        return 4;
    default:
        return 0;
    }
}

Kind vector_of(int size) {
    switch (size) {
    case 1:
        return Kind::Float;
    case 2:
        return Kind::Vec2;
    case 3:
        return Kind::Vec3;
    case 4:
        return Kind::Vec4;
    default:
        return Kind::Bad;
    }
}

// "float", "vec2", "vec3", "vec4" or "texture".
std::string short_kind_name(Kind kind) {
    switch (kind) {
    case Kind::Float:
        return "float";
    case Kind::Vec2:
        return "vec2";
    case Kind::Vec3:
        return "vec3";
    case Kind::Vec4:
        return "vec4";
    case Kind::Texture:
        return "texture";
    case Kind::Bad:
        break;
    }
    return "invalid";
}

std::string kind_name(Kind kind) {
    return "a " + short_kind_name(kind);
}

// How an op's args combine.
enum class Rule : std::uint8_t {
    Unary,     // T -> T
    Binary,    // T, T -> T; a float stands for a vector of any size
    Ternary,   // likewise, with three
    Dot,       // T, T -> float
    Length,    // T -> float
    Distance,  // T, T -> float
    Normalize, // T -> T
    Compose,   // floats and vectors -> a vector of compose_size
    Texture,   // texture, vec2 -> vec4
    Color3,    // vec3 -> vec3
    Atan,      // T -> T, or T, T -> T as atan2
};

struct OpInfo {
    std::string_view name;
    std::string_view args; // as the docs write them
    Op op;
    Rule rule;
    int compose_size = 0;
};

constexpr std::array<OpInfo, 40> kOps{{
    {.name = "add", .args = "a, b", .op = Op::Add, .rule = Rule::Binary},
    {.name = "sub", .args = "a, b", .op = Op::Sub, .rule = Rule::Binary},
    {.name = "mul", .args = "a, b", .op = Op::Mul, .rule = Rule::Binary},
    {.name = "div", .args = "a, b", .op = Op::Div, .rule = Rule::Binary},
    {.name = "mod", .args = "a, b", .op = Op::Mod, .rule = Rule::Binary},
    {.name = "pow", .args = "a, b", .op = Op::Pow, .rule = Rule::Binary},
    {.name = "min", .args = "a, b", .op = Op::Min, .rule = Rule::Binary},
    {.name = "max", .args = "a, b", .op = Op::Max, .rule = Rule::Binary},
    {.name = "step", .args = "edge, x", .op = Op::Step, .rule = Rule::Binary},
    {.name = "abs", .args = "a", .op = Op::Abs, .rule = Rule::Unary},
    {.name = "sign", .args = "a", .op = Op::Sign, .rule = Rule::Unary},
    {.name = "floor", .args = "a", .op = Op::Floor, .rule = Rule::Unary},
    {.name = "ceil", .args = "a", .op = Op::Ceil, .rule = Rule::Unary},
    {.name = "round", .args = "a", .op = Op::Round, .rule = Rule::Unary},
    {.name = "fract", .args = "a", .op = Op::Fract, .rule = Rule::Unary},
    {.name = "sqrt", .args = "a", .op = Op::Sqrt, .rule = Rule::Unary},
    {.name = "sin", .args = "a", .op = Op::Sin, .rule = Rule::Unary},
    {.name = "cos", .args = "a", .op = Op::Cos, .rule = Rule::Unary},
    {.name = "tan", .args = "a", .op = Op::Tan, .rule = Rule::Unary},
    {.name = "asin", .args = "a", .op = Op::Asin, .rule = Rule::Unary},
    {.name = "acos", .args = "a", .op = Op::Acos, .rule = Rule::Unary},
    {.name = "atan", .args = "y_over_x, or y, x", .op = Op::Atan, .rule = Rule::Atan},
    {.name = "exp", .args = "a", .op = Op::Exp, .rule = Rule::Unary},
    {.name = "log", .args = "a", .op = Op::Log, .rule = Rule::Unary},
    {.name = "saturate", .args = "a", .op = Op::Saturate, .rule = Rule::Unary},
    {.name = "one_minus", .args = "a", .op = Op::OneMinus, .rule = Rule::Unary},
    {.name = "negate", .args = "a", .op = Op::Negate, .rule = Rule::Unary},
    {.name = "mix", .args = "a, b, t", .op = Op::Mix, .rule = Rule::Ternary},
    {.name = "clamp", .args = "x, min, max", .op = Op::Clamp, .rule = Rule::Ternary},
    {.name = "smoothstep", .args = "edge0, edge1, x", .op = Op::Smoothstep, .rule = Rule::Ternary},
    {.name = "dot", .args = "a, b", .op = Op::Dot, .rule = Rule::Dot},
    {.name = "length", .args = "a", .op = Op::Length, .rule = Rule::Length},
    {.name = "distance", .args = "a, b", .op = Op::Distance, .rule = Rule::Distance},
    {.name = "normalize", .args = "a", .op = Op::Normalize, .rule = Rule::Normalize},
    {.name = "vec2", .args = "", .op = Op::Merge, .rule = Rule::Compose, .compose_size = 2},
    {.name = "vec3", .args = "", .op = Op::Merge, .rule = Rule::Compose, .compose_size = 3},
    {.name = "vec4", .args = "", .op = Op::Merge, .rule = Rule::Compose, .compose_size = 4},
    {.name = "texture", .args = "texture, uv", .op = Op::Sample, .rule = Rule::Texture},
    {.name = "rgb_to_hsv", .args = "rgb", .op = Op::RgbToHsv, .rule = Rule::Color3},
    {.name = "hsv_to_rgb", .args = "hsv", .op = Op::HsvToRgb, .rule = Rule::Color3},
}};

const OpInfo* find_op(std::string_view name) {
    for (const OpInfo& info : kOps) {
        if (info.name == name) {
            return &info;
        }
    }
    return nullptr;
}

std::vector<std::string_view> op_names() {
    std::vector<std::string_view> names;
    names.reserve(kOps.size());
    for (const OpInfo& info : kOps) {
        names.push_back(info.name);
    }
    return names;
}

// Inputs, as Godot's canvas_item shaders name them, lowercased.
struct Input {
    std::string_view name;
    Kind kind;
    std::uint32_t register_index; // unused for time and texture
};
constexpr std::array<Input, 6> kInputs{{
    {.name = "uv", .kind = Kind::Vec2, .register_index = render::kUvRegister},
    {.name = "color", .kind = Kind::Vec4, .register_index = render::kColorRegister},
    {.name = "fragcoord", .kind = Kind::Vec2, .register_index = render::kFragCoordRegister},
    {.name = "texture_pixel_size", .kind = Kind::Vec2, .register_index = render::kTexelRegister},
    {.name = "time", .kind = Kind::Float, .register_index = 0},
    {.name = "texture", .kind = Kind::Texture, .register_index = 0},
}};

const Input* find_input(std::string_view name) {
    for (const Input& input : kInputs) {
        if (input.name == name) {
            return &input;
        }
    }
    return nullptr;
}

constexpr std::array<std::string_view, 6> kParamTypes = {"float", "vec2",  "vec3",
                                                         "vec4",  "color", "texture"};
constexpr std::array<std::string_view, 3> kFilters = {"inherit", "linear", "nearest"};

Kind param_kind(std::string_view type) {
    if (type == "float") {
        return Kind::Float;
    }
    if (type == "vec2") {
        return Kind::Vec2;
    }
    if (type == "vec3") {
        return Kind::Vec3;
    }
    if (type == "texture") {
        return Kind::Texture;
    }
    return Kind::Vec4; // vec4 and color
}

bool valid_name(std::string_view name) {
    if (name.empty() || (name.front() >= '0' && name.front() <= '9')) {
        return false;
    }
    return std::ranges::all_of(
        name, [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; });
}

const std::string kNameRule =
    "names use lowercase letters, digits and _, and don't start with a digit";

render::Filter resolve_filter(std::string_view filter, render::Filter fallback) {
    if (filter == "nearest") {
        return render::Filter::Nearest;
    }
    if (filter == "linear") {
        return render::Filter::Linear;
    }
    return fallback;
}

std::string path_error() {
    return "expected a path inside the project folder, separated by '/', with no \".\" or "
           "\"..\" parts";
}

// A number as people write it: 2, not 2.0.
std::string number_text(double value) {
    constexpr double kWhole = 1.0e15;
    if (value == std::floor(value) && std::abs(value) < kWhole) {
        return std::to_string(static_cast<std::int64_t>(value));
    }
    return json::write(json::Value(value), json::Style::Compact);
}

// Where an operand's value comes from.
enum class Source : std::uint8_t { Register, Literal, Param, Time, Node, Texture };

// A value an arg reads: its kind, where it comes from and which components.
struct Ref {
    Kind kind = Kind::Bad;
    Source source = Source::Register;
    std::uint32_t index = 0;        // a register or node
    std::array<float, 4> literal{}; // Source::Literal
    std::string param;              // Source::Param, or Source::Texture ("" is the sprite's)
    render::Swizzle swizzle = render::kIdentitySwizzle;
};

struct Node {
    Op op = Op::Mov;
    std::uint32_t aux = 0;
    std::vector<Ref> args;
    int compose_size = 0; // a vecN: its args' components laid end to end
    std::string texture;  // Op::Sample: the texture param, "" for the sprite's
    int uses = 0;
    bool emitted = false;
    std::uint32_t register_index = 0;
};

json::Value text_schema(std::string_view description) {
    json::Value schema;
    schema["type"] = "string";
    schema["description"] = description;
    return schema;
}

json::Value schema(bool list_ops) {
    json::Value numbers;
    numbers["type"] = "array";
    numbers["items"]["type"] = "number";
    numbers["minItems"] = 2;
    numbers["maxItems"] = 4;

    json::Value node;
    node["type"] = "object";
    node["additionalProperties"] = false;
    node["required"] = json::Array{"op"};
    node["description"] = R"(An op and its args, as {"op": "mul", "args": ["color", 2]}.)";
    json::Value op = text_schema("The operation.");
    if (list_ops) {
        json::Array names;
        for (const std::string_view name : op_names()) {
            names.emplace_back(name);
        }
        op["enum"] = std::move(names);
    }
    node["properties"]["op"] = std::move(op);
    node["properties"]["args"]["type"] = "array";
    node["properties"]["args"]["items"]["$ref"] = "#/$defs/arg";
    node["properties"]["args"]["description"] = "The op's inputs, in order.";

    json::Value arg;
    arg["description"] = "A number, an array of 2 to 4 numbers, a node, or the name of a node, "
                         "param or input (uv, color, time, texture, texture_pixel_size, "
                         "fragcoord), with components to pick after a dot, as \"base.rgb\".";
    arg["anyOf"] =
        json::Array{json::Value(json::Object{{"type", "number"}}), numbers,
                    text_schema(R"(A node, param or input, as "tint" or "uv.x".)"), node};

    json::Value param;
    param["type"] = "object";
    param["additionalProperties"] = false;
    param["required"] = json::Array{"type"};
    json::Array types;
    for (const std::string_view type : kParamTypes) {
        types.emplace_back(type);
    }
    param["properties"]["type"]["enum"] = std::move(types);
    param["properties"]["type"]["description"] =
        "float, vec2, vec3, vec4, color (a vec4 from 0 to 1) or texture.";
    param["properties"]["value"]["anyOf"] =
        json::Array{json::Value(json::Object{{"type", "number"}}), numbers,
                    json::Value(json::Object{{"type", "string"}})};
    param["properties"]["value"]["description"] =
        "The default: a number, an array of numbers, or a texture path (\"\" is white). Left "
        "out: 0, or white for colors and textures.";
    param["properties"]["min"]["type"] = "number";
    param["properties"]["min"]["description"] = "Numbers: the smallest value allowed.";
    param["properties"]["max"]["type"] = "number";
    param["properties"]["max"]["description"] = "Numbers: the largest value allowed.";
    param["properties"]["repeat"]["type"] = "boolean";
    param["properties"]["repeat"]["default"] = false;
    param["properties"]["repeat"]["description"] =
        "Textures: tile past the edges instead of stretching them.";
    json::Array filters;
    for (const std::string_view filter : kFilters) {
        filters.emplace_back(filter);
    }
    param["properties"]["filter"]["enum"] = std::move(filters);
    param["properties"]["filter"]["default"] = "inherit";
    param["properties"]["filter"]["description"] =
        "Textures: how it is sampled; inherit uses the sprite's filter.";
    param["properties"]["doc"] = text_schema("What the param does, for whoever sets it.");

    json::Value out;
    out["$schema"] = "https://json-schema.org/draft/2020-12/schema";
    out["title"] = "SlashEngine material";
    out["description"] = "A canvas material: a node graph that colors each pixel of a sprite, "
                         "like Godot's canvas_item shaders.";
    out["type"] = "object";
    out["additionalProperties"] = false;
    out["required"] = json::Array{"format"};
    json::Value& properties = out["properties"];
    properties["format"]["const"] = 1;
    properties["format"]["description"] = "Format version. Must be 1.";
    json::Array blends;
    for (const auto mode : {render::BlendMode::Mix, render::BlendMode::Add, render::BlendMode::Sub,
                            render::BlendMode::Mul, render::BlendMode::PremulAlpha}) {
        blends.emplace_back(render::blend_mode_name(mode));
    }
    properties["blend"]["enum"] = std::move(blends);
    properties["blend"]["default"] = "mix";
    properties["blend"]["description"] =
        "How the color combines with what is under it, as Godot's blend modes.";
    properties["params"]["type"] = "object";
    properties["params"]["additionalProperties"] = param;
    properties["params"]["description"] =
        "Values a sprite can set through Sprite2D's material_params, by name.";
    properties["nodes"]["type"] = "object";
    properties["nodes"]["additionalProperties"]["$ref"] = "#/$defs/node";
    properties["nodes"]["description"] = "Named nodes, for args that use a value more than once.";
    properties["color"]["$ref"] = "#/$defs/arg";
    properties["color"]["description"] = "The pixel's color, a vec4. Left out: the texture times "
                                         "the vertex color.";
    out["$defs"]["node"] = std::move(node);
    out["$defs"]["arg"] = std::move(arg);
    return out;
}

// Compiles a validated document's graph, collecting every error it finds.
class Compiler {
public:
    explicit Compiler(const json::Value& doc) : doc_(doc) {}

    void run() {
        if (const json::Value* blend = doc_.find("blend"); blend != nullptr) {
            // The schema allows only known names.
            material_.blend =
                render::parse_blend_mode(blend->as_string()).value_or(render::BlendMode::Mix);
        }
        read_params();
        if (const json::Value* nodes = doc_.find("nodes"); nodes != nullptr) {
            nodes_ = &nodes->as_object();
            for (const auto& [name, node] : *nodes_) {
                const std::string at = "/nodes/" + json::pointer_escape(name);
                if (!valid_name(name)) {
                    fail(at, kNameRule);
                } else if (find_input(name) != nullptr) {
                    fail(at, "\"" + name + "\" is an input; name the node something else");
                } else if (material_.params.contains(name)) {
                    fail(at, "\"" + name + "\" is also a param; names must differ");
                }
                (void)named(name, at);
            }
        }
        const json::Value* color = doc_.find("color");
        if (color == nullptr) {
            return;
        }
        const Ref root = resolve(*color, "/color");
        if (root.kind != Kind::Bad && root.kind != Kind::Vec4) {
            fail("/color", "color is " + kind_name(root.kind) +
                               R"(; make a vec4, as {"op": "vec4", "args": [rgb, 1]})");
        }
        if (errors_.empty()) {
            emit_program(root);
        }
    }

    std::vector<json::ValidationError>& errors() { return errors_; }
    CanvasMaterial& material() { return material_; }

private:
    void fail(const std::string& path, std::string message) {
        errors_.push_back({path, std::move(message)});
    }

    void read_params() {
        const json::Value* params = doc_.find("params");
        if (params == nullptr) {
            return;
        }
        int textures = 0;
        for (const auto& [name, json_param] : params->as_object()) {
            const std::string at = "/params/" + json::pointer_escape(name);
            if (!valid_name(name)) {
                fail(at, kNameRule);
                continue;
            }
            if (find_input(name) != nullptr) {
                fail(at, "\"" + name + "\" is an input; name the param something else");
                continue;
            }
            MaterialParam param;
            param.type = json_param.find("type")->as_string();
            const bool texture = param.type == "texture";
            if (texture) {
                ++textures;
                if (textures == render::kMaterialTextureSlots) {
                    fail(at, "a material samples at most " +
                                 std::to_string(render::kMaterialTextureSlots - 1) +
                                 " textures of its own, besides the sprite's");
                }
            } else if (param.type == "color") {
                param.value = {1.0F, 1.0F, 1.0F, 1.0F};
            }
            for (const char* key : {"repeat", "filter"}) {
                if (!texture && json_param.find(key) != nullptr) {
                    fail(at + "/" + key, "only texture params have " + std::string(key));
                }
            }
            for (const char* key : {"min", "max"}) {
                if (texture && json_param.find(key) != nullptr) {
                    fail(at + "/" + key, "texture params have no " + std::string(key));
                }
            }
            if (const json::Value* repeat = json_param.find("repeat"); repeat != nullptr) {
                param.repeat = repeat->as_bool();
            }
            if (const json::Value* filter = json_param.find("filter"); filter != nullptr) {
                param.filter = filter->as_string();
            }
            if (const json::Value* min = json_param.find("min"); min != nullptr) {
                param.min = min->as_double();
            }
            if (const json::Value* max = json_param.find("max"); max != nullptr) {
                param.max = max->as_double();
            }
            if (param.min && param.max && *param.min > *param.max) {
                fail(at + "/max", "max is below min");
            }
            if (const json::Value* value = json_param.find("value"); value != nullptr) {
                if (auto read = read_param_value(*value, param); !read) {
                    fail(at + "/value", read.error().message);
                }
            }
            material_.params.emplace(name, std::move(param));
        }
    }

    // A named node, compiled the first time it is asked for.
    Ref named(const std::string& name, const std::string& at) {
        if (const auto done = refs_.find(name); done != refs_.end()) {
            return done->second;
        }
        if (const auto open = std::ranges::find(visiting_, name); open != visiting_.end()) {
            std::string chain;
            for (auto it = open; it != visiting_.end(); ++it) {
                chain += *it + " -> ";
            }
            fail(at, "\"" + name + "\" depends on itself: " + chain + name);
            return {};
        }
        visiting_.push_back(name);
        const Ref ref = node(nodes_->at(name), "/nodes/" + json::pointer_escape(name));
        visiting_.pop_back();
        refs_.emplace(name, ref);
        return ref;
    }

    [[nodiscard]] std::vector<std::string_view> known_names() const {
        std::vector<std::string_view> names;
        names.reserve(kInputs.size() + material_.params.size() +
                      (nodes_ != nullptr ? nodes_->size() : 0));
        for (const Input& input : kInputs) {
            names.push_back(input.name);
        }
        for (const auto& [name, param] : material_.params) {
            names.emplace_back(name);
        }
        if (nodes_ != nullptr) {
            for (const auto& [name, json_node] : *nodes_) {
                names.emplace_back(name);
            }
        }
        return names;
    }

    // Picks components, as "uv.yx" or "base.rgb".
    Ref pick(Ref ref, const std::string& base, std::string_view components, const std::string& at) {
        const int size = size_of(ref.kind);
        if (ref.kind == Kind::Texture) {
            fail(at, "\"" + base + "\" is a texture; it has no components to pick");
            return {};
        }
        if (size == 1) {
            fail(at, "\"" + base + "\" is a float; it has no components to pick");
            return {};
        }
        constexpr std::string_view kXyzw = "xyzw";
        constexpr std::string_view kRgba = "rgba";
        const std::string_view set =
            !components.empty() && kRgba.find(components.front()) != std::string_view::npos ? kRgba
                                                                                            : kXyzw;
        if (components.empty() || components.size() > 4 ||
            !std::ranges::all_of(components,
                                 [&](char c) { return set.find(c) != std::string_view::npos; })) {
            fail(at,
                 "pick 1 to 4 components, all from xyzw or all from rgba, as \"" + base + ".xy\"");
            return {};
        }
        render::Swizzle picked{};
        for (std::size_t i = 0; i < 4; ++i) {
            const char c = components.at(std::min(i, components.size() - 1));
            const auto component = static_cast<int>(set.find(c));
            if (component >= size) {
                fail(at, "\"" + base + "\" is " + kind_name(ref.kind) + ", so it has no " +
                             std::string(1, c));
                return {};
            }
            picked.at(i) = ref.swizzle.at(static_cast<std::size_t>(component));
        }
        ref.swizzle = picked;
        ref.kind = vector_of(static_cast<int>(components.size()));
        return ref;
    }

    Ref resolve(const json::Value& arg, const std::string& at) {
        if (arg.is_number()) {
            const auto value = static_cast<float>(arg.as_double());
            return {.kind = Kind::Float,
                    .source = Source::Literal,
                    .index = 0,
                    .literal = {value, value, value, value},
                    .param = {},
                    .swizzle = render::kIdentitySwizzle};
        }
        if (arg.is_array()) {
            Ref ref{.kind = vector_of(static_cast<int>(arg.size())),
                    .source = Source::Literal,
                    .index = 0,
                    .literal = {},
                    .param = {},
                    .swizzle = render::kIdentitySwizzle};
            for (std::size_t i = 0; i < arg.size(); ++i) {
                ref.literal.at(i) = static_cast<float>(arg.as_array()[i].as_double());
            }
            return ref;
        }
        if (arg.is_object()) {
            return node(arg, at);
        }
        const std::string& text = arg.as_string();
        const std::size_t dot = text.find('.');
        const std::string base = text.substr(0, dot);
        Ref ref;
        if (const Input* input = find_input(base); input != nullptr) {
            ref.kind = input->kind;
            ref.index = input->register_index;
            if (input->name == "time") {
                ref.source = Source::Time;
            } else if (input->kind == Kind::Texture) {
                ref.source = Source::Texture;
            }
        } else if (const auto param = material_.params.find(base);
                   param != material_.params.end()) {
            ref.kind = param_kind(param->second.type);
            ref.source = ref.kind == Kind::Texture ? Source::Texture : Source::Param;
            ref.param = base;
        } else if (nodes_ != nullptr && nodes_->contains(base)) {
            ref = named(base, at);
            if (ref.kind == Kind::Bad) {
                return ref;
            }
        } else {
            std::string message = "no node, param or input \"" + base + "\"";
            if (const std::string_view hint = closest_match(base, known_names()); !hint.empty()) {
                message += "; did you mean \"" + std::string(hint) + "\"?";
            }
            fail(at, std::move(message));
            return {};
        }
        if (dot == std::string::npos) {
            return ref;
        }
        return pick(std::move(ref), base, std::string_view(text).substr(dot + 1), at);
    }

    // Kinds that combine elementwise: all the same, or floats with one vector size.
    Kind unify(const std::vector<Ref>& args, const OpInfo& info, const std::string& at) {
        Kind out = Kind::Float;
        for (const Ref& arg : args) {
            if (arg.kind == Kind::Texture) {
                fail(at, std::string(info.name) +
                             " can't take a texture; sample it with the texture op first");
                return Kind::Bad;
            }
            if (arg.kind == Kind::Float || arg.kind == out) {
                continue;
            }
            if (out != Kind::Float) {
                std::string kinds;
                for (const Ref& each : args) {
                    kinds += (kinds.empty() ? "" : ", ") + short_kind_name(each.kind);
                }
                fail(at, std::string(info.name) +
                             " takes args of one size, or floats with them: got " + kinds);
                return Kind::Bad;
            }
            out = arg.kind;
        }
        return out;
    }

    Ref node(const json::Value& json_node, const std::string& at) {
        const std::string& name = json_node.find("op")->as_string();
        const OpInfo* info = find_op(name);
        if (info == nullptr) {
            std::string message = "no op \"" + name + "\"";
            if (const std::string_view hint = closest_match(name, op_names()); !hint.empty()) {
                message += "; did you mean \"" + std::string(hint) + "\"?";
            }
            fail(at + "/op", std::move(message));
            return {};
        }
        std::vector<Ref> args;
        bool bad = false;
        if (const json::Value* json_args = json_node.find("args"); json_args != nullptr) {
            for (std::size_t i = 0; i < json_args->size(); ++i) {
                args.push_back(
                    resolve(json_args->as_array()[i], at + "/args/" + std::to_string(i)));
                bad = bad || args.back().kind == Kind::Bad;
            }
        }
        if (bad) {
            return {};
        }
        const std::size_t count = args.size();
        std::size_t wanted = 1;
        switch (info->rule) {
        case Rule::Binary:
        case Rule::Dot:
        case Rule::Distance:
        case Rule::Texture:
            wanted = 2;
            break;
        case Rule::Ternary:
            wanted = 3;
            break;
        case Rule::Atan:
            wanted = count == 2 ? 2 : 1;
            break;
        case Rule::Compose:
            wanted = count == 0 ? 1 : count;
            break;
        default:
            break;
        }
        if (count != wanted) {
            std::string message = std::string(info->name) + " takes ";
            if (info->rule == Rule::Atan) {
                message += "1 or 2 args";
            } else if (info->rule == Rule::Compose) {
                message += "floats and vectors with " + std::to_string(info->compose_size) +
                           " components in all, or one float";
            } else {
                message += std::to_string(wanted) + (wanted == 1 ? " arg" : " args");
            }
            if (!info->args.empty()) {
                message += " (" + std::string(info->args) + ")";
            }
            fail(at + "/args", message + ", not " + std::to_string(count));
            return {};
        }

        Node out;
        out.op = info->op;
        Kind kind = Kind::Bad;
        switch (info->rule) {
        case Rule::Unary:
        case Rule::Binary:
        case Rule::Ternary:
        case Rule::Normalize:
        case Rule::Atan:
            kind = unify(args, *info, at);
            if (info->rule == Rule::Atan && count == 2) {
                out.op = Op::Atan2;
            }
            out.aux = static_cast<std::uint32_t>(size_of(kind));
            break;
        case Rule::Dot:
        case Rule::Distance:
        case Rule::Length: {
            const Kind operand = unify(args, *info, at);
            if (operand != Kind::Bad && count == 2 && args[0].kind != args[1].kind) {
                fail(at, std::string(info->name) + " takes two args of one size: got " +
                             kind_name(args[0].kind) + " and " + kind_name(args[1].kind));
                return {};
            }
            out.aux = static_cast<std::uint32_t>(size_of(operand));
            kind = operand == Kind::Bad ? Kind::Bad : Kind::Float;
            break;
        }
        case Rule::Compose: {
            int components = 0;
            for (const Ref& arg : args) {
                if (arg.kind == Kind::Texture) {
                    fail(at, std::string(info->name) +
                                 " can't take a texture; sample it with the texture op first");
                    return {};
                }
                components += size_of(arg.kind);
            }
            const bool broadcast = count == 1 && args[0].kind == Kind::Float;
            if (!broadcast && components != info->compose_size) {
                fail(at + "/args", std::string(info->name) + " takes floats and vectors with " +
                                       std::to_string(info->compose_size) +
                                       " components in all, or one float; these have " +
                                       std::to_string(components));
                return {};
            }
            out.compose_size = info->compose_size;
            kind = vector_of(info->compose_size);
            break;
        }
        case Rule::Texture:
            if (args[0].kind != Kind::Texture) {
                fail(at + "/args/0", "the texture op's first arg is a texture: \"texture\" or a "
                                     "texture param, not " +
                                         kind_name(args[0].kind));
                return {};
            }
            if (args[1].kind != Kind::Vec2) {
                fail(at + "/args/1",
                     "the texture op's second arg is a vec2 uv, not " + kind_name(args[1].kind));
                return {};
            }
            out.texture = args[0].param;
            args.erase(args.begin());
            kind = Kind::Vec4;
            break;
        case Rule::Color3:
            if (args[0].kind != Kind::Vec3) {
                fail(at + "/args/0", std::string(info->name) + " takes a vec3, not " +
                                         kind_name(args[0].kind) + "; pick .rgb or .xyz");
                return {};
            }
            kind = Kind::Vec3;
            break;
        }
        if (kind == Kind::Bad) {
            return {};
        }
        out.args = std::move(args);
        nodes_list_.push_back(std::move(out));
        return {.kind = kind,
                .source = Source::Node,
                .index = static_cast<std::uint32_t>(nodes_list_.size() - 1),
                .literal = {},
                .param = {},
                .swizzle = render::kIdentitySwizzle};
    }

    // Emitting the program.

    void count_uses(const Ref& ref) {
        if (ref.source != Source::Node) {
            return;
        }
        Node& node = nodes_list_.at(ref.index);
        if (node.uses++ > 0) {
            return;
        }
        for (const Ref& arg : node.args) {
            count_uses(arg);
        }
    }

    // A literal's constant, shared with equal literals but not with params or the time.
    std::uint32_t constant(const std::array<float, 4>& value) {
        std::vector<std::array<float, 4>>& constants = material_.program.constants;
        for (std::size_t i = 0; i < constants.size(); ++i) {
            if (constants[i] == value && !reserved_.contains(i)) {
                return static_cast<std::uint32_t>(i);
            }
        }
        constants.push_back(value);
        return static_cast<std::uint32_t>(constants.size() - 1);
    }

    // A constant of its own, which instantiate overwrites.
    std::uint32_t reserved_constant(const std::array<float, 4>& value) {
        material_.program.constants.push_back(value);
        const std::size_t index = material_.program.constants.size() - 1;
        reserved_.insert(index);
        return static_cast<std::uint32_t>(index);
    }

    std::uint32_t operand(const Ref& ref) {
        std::uint32_t index = 0;
        switch (ref.source) {
        case Source::Register:
            index = ref.index;
            break;
        case Source::Literal:
            index = render::kConstantOperand + constant(ref.literal);
            break;
        case Source::Param: {
            MaterialParam& param = material_.params.at(ref.param);
            if (!param.slot) {
                param.slot = reserved_constant(param.value);
            }
            index = render::kConstantOperand + *param.slot;
            break;
        }
        case Source::Time:
            if (!material_.time_constant) {
                material_.time_constant = reserved_constant({});
            }
            index = render::kConstantOperand + *material_.time_constant;
            break;
        case Source::Node:
            index = nodes_list_.at(ref.index).register_index;
            break;
        case Source::Texture:
            break;
        }
        return render::operand(index, ref.swizzle);
    }

    std::optional<std::uint32_t> allocate() {
        if (free_.empty()) {
            return std::nullopt;
        }
        const std::uint32_t r = *free_.begin();
        free_.erase(free_.begin());
        return r;
    }

    // Lets go of an arg's register once nothing else reads it.
    void release(const Ref& ref) {
        if (ref.source != Source::Node) {
            return;
        }
        Node& node = nodes_list_.at(ref.index);
        if (--node.uses == 0 && node.register_index >= render::kFirstFreeRegister) {
            free_.insert(node.register_index);
        }
    }

    std::uint32_t texture_slot(const std::string& param_name) {
        if (param_name.empty()) {
            return 0;
        }
        MaterialParam& param = material_.params.at(param_name);
        if (!param.slot) {
            param.slot = ++textures_used_;
        }
        return *param.slot;
    }

    bool emit(std::uint32_t id, bool last) {
        Node& node = nodes_list_.at(id);
        if (node.emitted) {
            return true;
        }
        for (const Ref& arg : node.args) {
            if (arg.source == Source::Node && !emit(arg.index, false)) {
                return false;
            }
        }
        std::vector<render::Instruction>& code = material_.program.code;
        if (node.compose_size > 0) {
            // Each arg merged into place: vec4(rgb, a) takes x, y and z from rgb, then w from a.
            // The destination is written before the last arg is read, so it is a fresh register,
            // or register 0 for the color when no later arg reads the texture coordinate there.
            const bool reads_uv =
                std::ranges::any_of(node.args.begin() + 1, node.args.end(), [](const Ref& arg) {
                    return arg.source == Source::Register && arg.index == render::kUvRegister;
                });
            const auto allocated =
                last && !reads_uv ? std::optional<std::uint32_t>(render::kUvRegister) : allocate();
            if (!allocated) {
                return out_of_registers();
            }
            const std::uint32_t destination = *allocated;
            int offset = 0;
            for (std::size_t n = 0; n < node.args.size(); ++n) {
                const Ref& arg = node.args[n];
                const int size = node.args.size() == 1 ? node.compose_size : size_of(arg.kind);
                Ref placed = arg;
                std::uint32_t mask = 0;
                for (int i = 0; i < 4; ++i) {
                    const int from = std::clamp(i - offset, 0, size_of(arg.kind) - 1);
                    placed.swizzle.at(static_cast<std::size_t>(i)) =
                        arg.swizzle.at(static_cast<std::size_t>(from));
                    if (i >= offset && i < offset + size) {
                        mask |= 1U << static_cast<std::uint32_t>(i);
                    }
                }
                const std::uint32_t source = operand(placed);
                code.push_back(n == 0 ? render::instruction(Op::Mov, destination, source)
                                      : render::instruction(Op::Merge, destination,
                                                            render::operand(destination), source, 0,
                                                            mask));
                offset += size;
            }
            for (const Ref& arg : node.args) {
                release(arg);
            }
            node.register_index = destination;
        } else {
            std::array<std::uint32_t, 3> operands{};
            for (std::size_t n = 0; n < node.args.size(); ++n) {
                operands.at(n) = operand(node.args[n]);
            }
            for (const Ref& arg : node.args) {
                release(arg);
            }
            // The last node writes register 0, the program's color; its args are read first.
            std::optional<std::uint32_t> destination =
                last ? std::optional<std::uint32_t>(render::kUvRegister) : allocate();
            if (!destination) {
                return out_of_registers();
            }
            const std::uint32_t aux = node.op == Op::Sample ? texture_slot(node.texture) : node.aux;
            code.push_back(render::instruction(node.op, *destination, operands[0], operands[1],
                                               operands[2], aux));
            node.register_index = *destination;
        }
        node.emitted = true;
        return true;
    }

    bool out_of_registers() {
        fail("/color", "the graph needs more than " +
                           std::to_string(render::kRegisterCount - render::kFirstFreeRegister) +
                           " values at once; compute fewer things side by side");
        return false;
    }

    void emit_program(const Ref& root) {
        for (std::uint32_t r = render::kFirstFreeRegister; r < render::kRegisterCount; ++r) {
            free_.insert(r);
        }
        count_uses(root);
        // The color's own node writes register 0, which saves a final move.
        const bool direct = root.source == Source::Node && root.swizzle == render::kIdentitySwizzle;
        if (root.source == Source::Node && !emit(root.index, direct)) {
            return;
        }
        if (!direct || nodes_list_.at(root.index).register_index != render::kUvRegister) {
            material_.program.code.push_back(
                render::instruction(Op::Mov, render::kUvRegister, operand(root)));
        }
        const std::size_t instructions = material_.program.code.size();
        if (instructions > render::kMaxInstructions) {
            fail("/color", "the graph makes " + std::to_string(instructions) +
                               " instructions; a material runs at most " +
                               std::to_string(render::kMaxInstructions) +
                               ". Name values used more than once, so they are computed once");
        }
        const std::size_t constants = material_.program.constants.size();
        if (constants > render::kMaxConstants) {
            fail("/color", "the graph uses " + std::to_string(constants) +
                               " constants and params; a material holds at most " +
                               std::to_string(render::kMaxConstants));
        }
    }

    const json::Value& doc_;
    const json::Object* nodes_ = nullptr;
    CanvasMaterial material_;
    std::vector<json::ValidationError> errors_;
    std::map<std::string, Ref, std::less<>> refs_;
    std::vector<std::string> visiting_;
    std::vector<Node> nodes_list_;
    std::set<std::uint32_t> free_;
    std::set<std::size_t> reserved_;
    std::uint32_t textures_used_ = 0;
};

// Graphs the schema check would recurse into too deeply, or with more nodes than the shader
// runs instructions, fail first, which also bounds the compiler's recursion.
std::optional<json::ValidationError> check_size(const json::Value& doc) {
    constexpr int kMaxNesting = 16;
    std::size_t count = 0;
    std::optional<json::ValidationError> error;
    const auto walk = [&](const auto& self, const json::Value& node, const std::string& at,
                          int depth) -> void {
        if (error || !node.is_object()) {
            return;
        }
        if (depth > kMaxNesting) {
            error = {at, "nodes nest at most " + std::to_string(kMaxNesting) +
                             " deep; name some in \"nodes\" and use their names as args"};
            return;
        }
        if (++count > render::kMaxInstructions) {
            error = {at, "a material has at most " + std::to_string(render::kMaxInstructions) +
                             " nodes, since each runs at least one instruction"};
            return;
        }
        const json::Value* args = node.find("args");
        if (args == nullptr || !args->is_array()) {
            return;
        }
        for (std::size_t i = 0; i < args->size(); ++i) {
            self(self, args->as_array()[i], at + "/args/" + std::to_string(i), depth + 1);
        }
    };
    if (!doc.is_object()) {
        return std::nullopt;
    }
    if (const json::Value* nodes = doc.find("nodes"); nodes != nullptr && nodes->is_object()) {
        for (const auto& [name, node] : nodes->as_object()) {
            walk(walk, node, "/nodes/" + json::pointer_escape(name), 1);
        }
    }
    if (const json::Value* color = doc.find("color"); color != nullptr) {
        walk(walk, *color, "/color", 1);
    }
    return error;
}

// Every error in a document; with none, the compiled material goes in out.
std::vector<json::ValidationError> compile(const json::Value& doc, CanvasMaterial* out) {
    if (auto error = check_size(doc)) {
        return {*std::move(error)};
    }
    // Ops are checked by the compiler, which says "did you mean" instead of listing them all.
    std::vector<json::ValidationError> errors = json::validate(doc, schema(false));
    if (!errors.empty()) {
        return errors;
    }
    Compiler compiler(doc);
    compiler.run();
    if (compiler.errors().empty() && out != nullptr) {
        *out = std::move(compiler.material());
    }
    return std::move(compiler.errors());
}

} // namespace

json::Value material_schema() {
    return schema(true);
}

std::vector<json::ValidationError> validate_material(const json::Value& doc) {
    return compile(doc, nullptr);
}

Result<CanvasMaterial> parse_material(const json::Value& doc) {
    CanvasMaterial material;
    const std::vector<json::ValidationError> errors = compile(doc, &material);
    if (errors.empty()) {
        return material;
    }
    const json::ValidationError& first = errors.front();
    return make_error(ErrorCode::InvalidArgument,
                      (first.path.empty() ? std::string("root") : first.path) + ": " +
                          first.message);
}

Result<void> read_param_value(const json::Value& value, MaterialParam& param) {
    if (param.type == "texture") {
        if (!value.is_string()) {
            return make_error(ErrorCode::InvalidArgument,
                              "expected a texture path, or \"\" for white");
        }
        if (!value.as_string().empty() && !is_project_path(value.as_string())) {
            return make_error(ErrorCode::InvalidArgument, path_error());
        }
        param.texture = value.as_string();
        return {};
    }
    std::array<float, 4> read{};
    if (param.type == "float") {
        if (!value.is_number()) {
            return make_error(ErrorCode::InvalidArgument,
                              "expected a number, got " +
                                  std::string(json::to_string(value.type())));
        }
        read.fill(static_cast<float>(value.as_double()));
    } else {
        const bool color = param.type == "color";
        const std::size_t size =
            color ? 4 : static_cast<std::size_t>(size_of(param_kind(param.type)));
        const bool fits = value.is_array() &&
                          (value.size() == size || (color && value.size() == 3)) &&
                          std::ranges::all_of(value.as_array(), &json::Value::is_number);
        if (!fits) {
            return make_error(ErrorCode::InvalidArgument,
                              color ? "expected [r, g, b] or [r, g, b, a]"
                                    : "expected an array of " + std::to_string(size) + " numbers");
        }
        read = {0.0F, 0.0F, 0.0F, color ? 1.0F : 0.0F};
        for (std::size_t i = 0; i < value.size(); ++i) {
            read.at(i) = static_cast<float>(value.as_array()[i].as_double());
        }
    }
    // The components given: a float's one, or the array's.
    const std::size_t components = value.is_array() ? value.size() : 1;
    for (std::size_t i = 0; i < components; ++i) {
        const double number = read.at(i);
        if ((param.min && number < *param.min) || (param.max && number > *param.max)) {
            std::string range;
            if (param.min && param.max) {
                range = "from " + number_text(*param.min) + " to " + number_text(*param.max);
            } else if (param.min) {
                range = "at least " + number_text(*param.min);
            } else {
                range = "at most " + number_text(*param.max);
            }
            return make_error(ErrorCode::InvalidArgument,
                              number_text(number) + " is outside the param's range, " + range);
        }
    }
    param.value = read;
    return {};
}

Result<MaterialInstance> instantiate(const CanvasMaterial& material,
                                     const std::map<std::string, json::Value>& params, double time,
                                     std::string* failed_param) {
    MaterialInstance instance;
    instance.blend = material.blend;
    instance.program = material.program;
    const auto place = [&](const MaterialParam& param) {
        if (!param.slot) {
            return;
        }
        if (param.type == "texture") {
            instance.textures.at(*param.slot - 1) = {
                .path = param.texture, .repeat = param.repeat, .filter = param.filter};
        } else {
            instance.program.constants.at(*param.slot) = param.value;
        }
    };
    for (const auto& [name, param] : material.params) {
        place(param);
    }
    for (const auto& [name, value] : params) {
        const auto found = material.params.find(name);
        if (failed_param != nullptr) {
            *failed_param = name;
        }
        if (found == material.params.end()) {
            std::vector<std::string_view> names;
            names.reserve(material.params.size());
            for (const auto& [known, param] : material.params) {
                names.emplace_back(known);
            }
            std::string message = "the material has no param \"" + name + "\"";
            if (const std::string_view hint = closest_match(name, names); !hint.empty()) {
                message += "; did you mean \"" + std::string(hint) + "\"?";
            }
            return make_error(ErrorCode::InvalidArgument, std::move(message));
        }
        MaterialParam param = found->second;
        if (auto read = read_param_value(value, param); !read) {
            return read.error();
        }
        place(param);
    }
    if (failed_param != nullptr) {
        failed_param->clear();
    }
    if (material.time_constant) {
        const auto wrapped = static_cast<float>(std::fmod(std::max(time, 0.0), kMaterialTimeWrap));
        instance.program.constants.at(*material.time_constant) = {wrapped, wrapped, wrapped,
                                                                  wrapped};
    }
    return instance;
}

Result<std::vector<std::array<float, 4>>> evaluate(const MaterialInstance& instance,
                                                   const std::string& texture,
                                                   render::Filter filter, const PixelInputs& inputs,
                                                   std::span<const std::array<float, 2>> uvs,
                                                   const ImageSource& images) {
    struct Slot {
        const render::Image* image = nullptr;
        render::Filter filter = render::Filter::Linear;
        bool repeat = false;
    };
    std::array<Slot, render::kMaterialTextureSlots> slots{};
    const auto load = [&](const std::string& path, Slot& slot) -> Result<void> {
        if (path.empty()) {
            return {};
        }
        auto image = images(path);
        if (!image) {
            return image.error();
        }
        slot.image = image.value();
        return {};
    };
    slots[0].filter = filter;
    if (auto loaded = load(texture, slots[0]); !loaded) {
        return loaded.error();
    }
    for (std::size_t i = 1; i < slots.size(); ++i) {
        const MaterialTextureRef& ref = instance.textures.at(i - 1);
        slots.at(i).repeat = ref.repeat;
        slots.at(i).filter = resolve_filter(ref.filter, filter);
        if (auto loaded = load(ref.path, slots.at(i)); !loaded) {
            return loaded.error();
        }
    }
    const auto sample = [&](std::uint32_t index, float u, float v) {
        const Slot& slot = slots.at(index);
        if (slot.image == nullptr) {
            return std::array<float, 4>{1.0F, 1.0F, 1.0F, 1.0F};
        }
        return render::sample_image(*slot.image, slot.filter, slot.repeat, u, v);
    };
    render::PixelInput pixel;
    pixel.color = inputs.color;
    pixel.frag_coord = inputs.frag_coord;
    pixel.texel = {1.0F, 1.0F};
    if (slots[0].image != nullptr) {
        pixel.texel = {1.0F / static_cast<float>(slots[0].image->width),
                       1.0F / static_cast<float>(slots[0].image->height)};
    }
    std::vector<std::array<float, 4>> colors;
    colors.reserve(uvs.size());
    for (const auto& uv : uvs) {
        pixel.uv = uv;
        std::array<float, 4> color{};
        if (instance.program.code.empty()) {
            const std::array<float, 4> texel = sample(0, uv[0], uv[1]);
            for (std::size_t i = 0; i < 4; ++i) {
                color.at(i) = texel.at(i) * inputs.color.at(i);
            }
        } else {
            color = render::run_program(instance.program, pixel, sample);
        }
        // What an 8-bit target keeps; NaN becomes 0, as on the GPU.
        for (float& channel : color) {
            channel = channel > 0.0F ? std::min(channel, 1.0F) : 0.0F;
        }
        colors.push_back(color);
    }
    return colors;
}

} // namespace slash::canvas
