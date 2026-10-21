#include "slash/canvas/material.hpp"

#include "graph.hpp"

#include "slash/core/text.hpp"
#include "slash/render/image.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <tuple>
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

// Where an operand's value comes from.
enum class Source : std::uint8_t { Register, Literal, Param, Time, Node, Texture };

// A particle graph's stages, as Godot's particle shaders' start() and process().
constexpr std::uint32_t kStartStage = 1;
constexpr std::uint32_t kProcessStage = 2;

// An input: its kind, and where it is.
struct Input {
    std::string_view name;
    Kind kind;
    Source source;
    std::uint32_t register_index; // for Source::Register
    render::Swizzle swizzle;      // the components of the register it is
    std::uint32_t stages;         // the particle graph stages that have it; 0 for all
};

constexpr Input register_input(std::string_view name, Kind kind, std::uint32_t index,
                               const render::Swizzle& swizzle = render::kIdentitySwizzle,
                               std::uint32_t stages = 0) {
    return {.name = name,
            .kind = kind,
            .source = Source::Register,
            .register_index = index,
            .swizzle = swizzle,
            .stages = stages};
}

// A float kept in one component of a register.
constexpr render::Swizzle lane(std::uint8_t component) {
    return {component, component, component, component};
}

// The zw half of a register, as a vec2.
constexpr render::Swizzle kZw{2, 3, 3, 3};

// Inputs, as Godot's canvas_item shaders name them, lowercased.
constexpr std::array<Input, 6> kInputs{{
    register_input("uv", Kind::Vec2, render::kUvRegister),
    register_input("color", Kind::Vec4, render::kColorRegister),
    register_input("fragcoord", Kind::Vec2, render::kFragCoordRegister),
    register_input("texture_pixel_size", Kind::Vec2, render::kTexelRegister),
    {.name = "time",
     .kind = Kind::Float,
     .source = Source::Time,
     .register_index = 0,
     .swizzle = render::kIdentitySwizzle,
     .stages = 0},
    {.name = "texture",
     .kind = Kind::Texture,
     .source = Source::Texture,
     .register_index = 0,
     .swizzle = render::kIdentitySwizzle,
     .stages = 0},
}};

// A particle graph's inputs, in the registers render/particles.hpp lays out; Godot's particle
// shaders' built-ins, lowercased, where it has them.
constexpr std::array<Input, 20> kParticleInputs{{
    register_input("position", Kind::Vec2, render::kParticleMotionRegister),
    register_input("velocity", Kind::Vec2, render::kParticleMotionRegister, kZw),
    register_input("color", Kind::Vec4, render::kParticleColorRegister),
    register_input("angle", Kind::Float, render::kParticleLookRegister, lane(0), kProcessStage),
    register_input("scale", Kind::Float, render::kParticleLookRegister, lane(1), kProcessStage),
    register_input("frame", Kind::Float, render::kParticleLookRegister, lane(2), kProcessStage),
    register_input("age", Kind::Float, render::kParticleLifeRegister, lane(0), kProcessStage),
    register_input("life", Kind::Float, render::kParticleLifeRegister, lane(1), kProcessStage),
    register_input("lifetime", Kind::Float, render::kParticleLifeRegister, lane(2)),
    register_input("delta", Kind::Float, render::kParticleLifeRegister, lane(3), kProcessStage),
    register_input("time", Kind::Float, render::kParticleNumberRegister, lane(0)),
    register_input("index", Kind::Float, render::kParticleNumberRegister, lane(1)),
    register_input("number", Kind::Float, render::kParticleNumberRegister, lane(2)),
    register_input("amount_ratio", Kind::Float, render::kParticleNumberRegister, lane(3)),
    register_input("random", Kind::Vec4, render::kParticleRandomRegister),
    register_input("emitter_position", Kind::Vec2, render::kParticleEmitterRegister),
    register_input("emitter_velocity", Kind::Vec2, render::kParticleEmitterRegister, kZw),
    register_input("collision_normal", Kind::Vec2, render::kParticleContactRegister,
                   render::kIdentitySwizzle, kProcessStage),
    register_input("collision_depth", Kind::Float, render::kParticleContactRegister, lane(2),
                   kProcessStage),
    register_input("collided", Kind::Float, render::kParticleContactRegister, lane(3),
                   kProcessStage),
}};

// What a particle graph's stage can change: a part of the output registers.
struct Output {
    std::string_view name;
    Kind kind;
    std::uint32_t bit;            // render::kParticleOut*
    std::uint32_t register_index; // where it goes
    std::uint32_t component;      // its first component there
    std::uint32_t stages;
    std::string_view doc;
};
constexpr std::array<Output, 7> kParticleOutputs{{
    {.name = "position",
     .kind = Kind::Vec2,
     .bit = render::kParticleOutPosition,
     .register_index = render::kParticleOutRegister,
     .component = 0,
     .stages = kStartStage | kProcessStage,
     .doc = "Where the particle is, in pixels; in process, where it ends the step instead of "
            "moving by its velocity."},
    {.name = "velocity",
     .kind = Kind::Vec2,
     .bit = render::kParticleOutVelocity,
     .register_index = render::kParticleOutRegister,
     .component = 2,
     .stages = kStartStage | kProcessStage,
     .doc = "Its velocity in pixels per second, which carries on into the next step."},
    {.name = "color",
     .kind = Kind::Vec4,
     .bit = render::kParticleOutColor,
     .register_index = render::kParticleOutRegister + 1,
     .component = 0,
     .stages = kProcessStage,
     .doc = "Its color, multiplying the texture."},
    {.name = "angle",
     .kind = Kind::Float,
     .bit = render::kParticleOutAngle,
     .register_index = render::kParticleOutRegister + 2,
     .component = 0,
     .stages = kProcessStage,
     .doc = "Its rotation, in radians."},
    {.name = "scale",
     .kind = Kind::Float,
     .bit = render::kParticleOutScale,
     .register_index = render::kParticleOutRegister + 2,
     .component = 1,
     .stages = kProcessStage,
     .doc = "Its size, times the texture's."},
    {.name = "frame",
     .kind = Kind::Float,
     .bit = render::kParticleOutFrame,
     .register_index = render::kParticleOutRegister + 2,
     .component = 2,
     .stages = kProcessStage,
     .doc = "Where it is in the sprite sheet, from 0 to 1."},
    {.name = "active",
     .kind = Kind::Float,
     .bit = render::kParticleOutActive,
     .register_index = render::kParticleOutRegister + 2,
     .component = 3,
     .stages = kStartStage | kProcessStage,
     .doc = "Below 0.5, the particle dies."},
}};

// What a graph is compiled for: a canvas material's pixels or a particle graph's stage.
struct GraphSpec {
    std::span<const Input> inputs;
    std::uint32_t first_free;
    std::string_view noun; // what has the graph, for messages
};
constexpr GraphSpec kMaterialSpec{
    .inputs = kInputs, .first_free = render::kFirstFreeRegister, .noun = "a material"};
constexpr GraphSpec kParticleSpec{.inputs = kParticleInputs,
                                  .first_free = render::kParticleFirstFreeRegister,
                                  .noun = "a particle graph"};

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

json::Value numbers_schema() {
    json::Value numbers;
    numbers["type"] = "array";
    numbers["items"]["type"] = "number";
    numbers["minItems"] = 2;
    numbers["maxItems"] = 4;
    return numbers;
}

json::Value node_schema(bool list_ops) {
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
    return node;
}

// An arg, with the inputs a graph has.
json::Value arg_schema(std::span<const Input> inputs, json::Value node) {
    std::string names;
    for (const Input& input : inputs) {
        names += (names.empty() ? "" : ", ") + std::string(input.name);
    }
    json::Value arg;
    arg["description"] = "A number, an array of 2 to 4 numbers, a node, or the name of a node, "
                         "param or input (" +
                         names + "), with components to pick after a dot, as \"base.rgb\".";
    arg["anyOf"] = json::Array{json::Value(json::Object{{"type", "number"}}), numbers_schema(),
                               text_schema(R"(A node, param or input, as "tint" or "uv.x".)"),
                               std::move(node)};
    return arg;
}

// A param; particle graphs' have no textures.
json::Value param_schema(bool textures) {
    json::Value param;
    param["type"] = "object";
    param["additionalProperties"] = false;
    param["required"] = json::Array{"type"};
    json::Array types;
    for (const std::string_view type : kParamTypes) {
        if (textures || type != "texture") {
            types.emplace_back(type);
        }
    }
    param["properties"]["type"]["enum"] = std::move(types);
    param["properties"]["type"]["description"] =
        textures ? "float, vec2, vec3, vec4, color (a vec4 from 0 to 1) or texture."
                 : "float, vec2, vec3, vec4 or color (a vec4 from 0 to 1).";
    json::Array values{json::Value(json::Object{{"type", "number"}}), numbers_schema()};
    if (textures) {
        values.emplace_back(json::Object{{"type", "string"}});
    }
    param["properties"]["value"]["anyOf"] = std::move(values);
    param["properties"]["value"]["description"] =
        textures ? "The default: a number, an array of numbers, or a texture path (\"\" is "
                   "white). Left out: 0, or white for colors and textures."
                 : "A number or an array of numbers. Left out: 0, or white for colors.";
    param["properties"]["min"]["type"] = "number";
    param["properties"]["min"]["description"] = "Numbers: the smallest value allowed.";
    param["properties"]["max"]["type"] = "number";
    param["properties"]["max"]["description"] = "Numbers: the largest value allowed.";
    if (textures) {
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
    }
    param["properties"]["doc"] = text_schema("What the param does, for whoever sets it.");
    return param;
}

json::Value schema(bool list_ops) {
    json::Value node = node_schema(list_ops);
    json::Value arg = arg_schema(kInputs, node);
    const json::Value param = param_schema(true);

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
    properties["light_mode"]["enum"] = json::Array{"normal", "unshaded", "light_only"};
    properties["light_mode"]["default"] = "normal";
    properties["light_mode"]["description"] =
        "How 2D lights shade it, as Godot's: normal, unshaded (lights and CanvasModulate leave it "
        "alone) or light_only (shown only where lights reach).";
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
    properties["shader"] = text_schema(
        "A game's own shader instead of nodes and color: an .hlsl file's project path, with "
        "fragment() and vertex() functions as Godot's canvas_item shaders.");

    json::Value compiled;
    compiled["type"] = "object";
    compiled["additionalProperties"] = false;
    compiled["required"] = json::Array{"cpu_fragment"};
    compiled["description"] = "The shader compiled for each backend, which exports write; never "
                              "written by hand.";
    for (const char* key : {"spirv_vertex", "spirv_fragment", "cpu_vertex", "cpu_fragment"}) {
        compiled["properties"][key] = text_schema("SPIR-V, in base64.");
    }
    for (const char* key : {"msl_vertex", "msl_fragment", "glsl_vertex", "glsl_fragment", "hlsl"}) {
        compiled["properties"][key] = text_schema("Source code.");
    }
    for (const char* key : {"normal_map_used", "specular_used"}) {
        compiled["properties"][key]["type"] = "boolean";
    }
    properties["compiled"] = std::move(compiled);
    out["$defs"]["node"] = std::move(node);
    out["$defs"]["arg"] = std::move(arg);
    return out;
}

// Compiles a validated document's graph, collecting every error it finds.
class Compiler {
public:
    // A stage of 0 checks a particle graph's nodes with every stage's inputs.
    explicit Compiler(const json::Value& doc, const GraphSpec& spec = kMaterialSpec,
                      std::uint32_t stage = 0)
        : doc_(doc), spec_(spec), stage_(stage) {}

    void run() {
        if (const json::Value* blend = doc_.find("blend"); blend != nullptr) {
            // The schema allows only known names.
            material_.blend =
                render::parse_blend_mode(blend->as_string()).value_or(render::BlendMode::Mix);
        }
        if (const json::Value* mode = doc_.find("light_mode"); mode != nullptr) {
            material_.light_mode =
                render::parse_light_mode(mode->as_string()).value_or(render::LightMode::Normal);
        }
        read_params();
        if (const json::Value* shader = doc_.find("shader"); shader != nullptr) {
            read_shader(shader->as_string());
            return;
        }
        if (doc_.find("compiled") != nullptr) {
            fail("/compiled", "only a material with a shader has compiled code");
        }
        read_nodes(true);
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
    [[nodiscard]] std::uint32_t outputs() const { return outputs_; }

    // The named nodes, checking their names; with compile_all, each is compiled, so every mistake
    // in them is found, used or not.
    void read_nodes(bool compile_all) {
        const json::Value* nodes = doc_.find("nodes");
        if (nodes == nullptr) {
            return;
        }
        nodes_ = &nodes->as_object();
        for (const auto& [name, node] : *nodes_) {
            const std::string at = "/nodes/" + json::pointer_escape(name);
            if (!valid_name(name)) {
                fail(at, kNameRule);
            } else if (input(name) != nullptr) {
                fail(at, "\"" + name + "\" is an input; name the node something else");
            } else if (material_.params.contains(name)) {
                fail(at, "\"" + name + "\" is also a param; names must differ");
            }
            if (compile_all) {
                (void)named(name, at);
            }
        }
    }

    // A particle graph stage's outputs (at), each of the output's kind.
    std::vector<std::pair<const Output*, Ref>> read_outputs(const json::Value& stage,
                                                            const std::string& at) {
        std::vector<std::pair<const Output*, Ref>> out;
        for (const Output& output : kParticleOutputs) {
            const json::Value* value = stage.find(output.name);
            if (value == nullptr) {
                continue;
            }
            const std::string path = at + "/" + std::string(output.name);
            const Ref ref = resolve(*value, path);
            if (ref.kind == Kind::Bad) {
                continue;
            }
            if (ref.kind != output.kind) {
                std::string message = std::string(output.name) + " is " + kind_name(output.kind) +
                                      ", not " + kind_name(ref.kind);
                if (output.kind != Kind::Float && ref.kind == Kind::Float) {
                    message += R"(; make one, as {"op": ")" + short_kind_name(output.kind) +
                               R"(", "args": [x]})";
                }
                fail(path, std::move(message));
                continue;
            }
            out.emplace_back(&output, ref);
        }
        return out;
    }

    // Compiles a particle graph stage: each output's value merged into its part of the output
    // registers, which the graph reads none of.
    void emit_outputs(const json::Value& stage, const std::string& at) {
        const std::vector<std::pair<const Output*, Ref>> wanted = read_outputs(stage, at);
        if (!errors_.empty()) {
            return;
        }
        path_ = at;
        for (std::uint32_t r = spec_.first_free; r < render::kRegisterCount; ++r) {
            free_.insert(r);
        }
        for (const auto& [output, ref] : wanted) {
            count_uses(ref);
        }
        std::vector<render::Instruction>& code = material_.program.code;
        for (const auto& [output, ref] : wanted) {
            if (ref.source == Source::Node && !emit(ref.index, false)) {
                return;
            }
            const int size = size_of(output->kind);
            if (size == 4) {
                code.push_back(render::instruction(Op::Mov, output->register_index, operand(ref)));
            } else {
                // Its components moved into place, as a vecN's args are.
                const auto offset = static_cast<int>(output->component);
                Ref placed = ref;
                std::uint32_t mask = 0;
                for (int i = 0; i < 4; ++i) {
                    const int from = std::clamp(i - offset, 0, size - 1);
                    placed.swizzle.at(static_cast<std::size_t>(i)) =
                        ref.swizzle.at(static_cast<std::size_t>(from));
                    if (i >= offset && i < offset + size) {
                        mask |= 1U << static_cast<std::uint32_t>(i);
                    }
                }
                code.push_back(render::instruction(Op::Merge, output->register_index,
                                                   render::operand(output->register_index),
                                                   operand(placed), 0, mask));
            }
            release(ref);
            outputs_ |= output->bit;
        }
        check_limits();
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
            if (input(name) != nullptr) {
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

private:
    void fail(const std::string& path, std::string message) {
        errors_.push_back({path, std::move(message)});
    }

    [[nodiscard]] const Input* input(std::string_view name) const {
        for (const Input& each : spec_.inputs) {
            if (each.name == name) {
                return &each;
            }
        }
        return nullptr;
    }

    // A game's shader: every param gets a slot, in name order, and the time the last constant.
    void read_shader(const std::string& path) {
        if (!is_project_path(path) || !path.ends_with(kShaderSuffix)) {
            fail("/shader", "expected an .hlsl file's path inside the project folder, separated "
                            "by '/', with no \".\" or \"..\" parts");
        }
        for (const char* key : {"nodes", "color"}) {
            if (doc_.find(key) != nullptr) {
                fail(std::string("/") + key, std::string("a material with a shader has no ") + key +
                                                 "; the shader colors the pixels");
            }
        }
        material_.shader = path;
        material_.time_constant = render::kShaderTimeConstant;
        material_.program.constants.assign(render::cpu_shader::kConstants, {});
        std::uint32_t constant = 0;
        std::uint32_t texture = 0;
        for (auto& [name, param] : material_.params) {
            const std::string at = "/params/" + json::pointer_escape(name);
            const bool sampled = param.type == "texture";
            if (render::shader_name_reserved(name) ||
                (sampled && render::shader_name_reserved(name + "_sampler"))) {
                fail(at, "\"" + name +
                             "\" is taken in shaders, by HLSL or the code around the shader; "
                             "name the param something else");
            } else if (sampled) {
                param.slot = ++texture;
            } else if (constant == render::kShaderTimeConstant) {
                fail(at, "a shader reads at most " + std::to_string(render::kShaderTimeConstant) +
                             " number params");
            } else {
                param.slot = constant;
                material_.program.constants.at(constant++) = param.value;
            }
        }
        if (const json::Value* compiled = doc_.find("compiled"); compiled != nullptr) {
            read_compiled(*compiled);
        }
    }

    void read_compiled(const json::Value& compiled) {
        auto shader = std::make_shared<render::CompiledShader>();
        const auto words = [&](const char* key, std::vector<std::uint32_t>& out) {
            const json::Value* value = compiled.find(key);
            if (value == nullptr) {
                return;
            }
            const std::optional<std::string> bytes = base64_decode(value->as_string());
            if (!bytes || bytes->size() % 4 != 0) {
                fail("/compiled/" + std::string(key), "expected SPIR-V in base64");
                return;
            }
            out.resize(bytes->size() / 4);
            for (std::size_t i = 0; i < out.size(); ++i) {
                std::uint32_t word = 0;
                for (std::size_t k = 0; k < 4; ++k) {
                    word |= std::uint32_t{static_cast<unsigned char>((*bytes)[(i * 4) + k])}
                            << (8 * k);
                }
                out[i] = word;
            }
        };
        const auto text = [&](const char* key, std::string& out) {
            if (const json::Value* value = compiled.find(key); value != nullptr) {
                out = value->as_string();
            }
        };
        const auto flag = [&](const char* key) {
            const json::Value* value = compiled.find(key);
            return value != nullptr && value->as_bool();
        };
        words("spirv_vertex", shader->spirv_vertex);
        words("spirv_fragment", shader->spirv_fragment);
        words("cpu_vertex", shader->cpu_vertex);
        words("cpu_fragment", shader->cpu_fragment);
        text("msl_vertex", shader->msl_vertex);
        text("msl_fragment", shader->msl_fragment);
        text("glsl_vertex", shader->glsl_vertex);
        text("glsl_fragment", shader->glsl_fragment);
        text("hlsl", shader->hlsl);
        shader->normal_map_used = flag("normal_map_used");
        shader->specular_used = flag("specular_used");
        material_.compiled = std::move(shader);
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
        names.reserve(spec_.inputs.size() + material_.params.size() +
                      (nodes_ != nullptr ? nodes_->size() : 0));
        for (const Input& each : spec_.inputs) {
            names.push_back(each.name);
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
        if (const Input* found = input(base); found != nullptr) {
            if (stage_ != 0 && found->stages != 0 && (found->stages & stage_) == 0) {
                fail(at, "\"" + base + "\" is only known in process, not start");
                return {};
            }
            ref.kind = found->kind;
            ref.source = found->source;
            ref.index = found->register_index;
            ref.swizzle = found->swizzle;
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
        if (--node.uses == 0 && node.register_index >= spec_.first_free) {
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
        fail(path_, "the graph needs more than " +
                        std::to_string(render::kRegisterCount - spec_.first_free) +
                        " values at once; compute fewer things side by side");
        return false;
    }

    void check_limits() {
        const std::size_t instructions = material_.program.code.size();
        if (instructions > render::kMaxInstructions) {
            fail(path_, "the graph makes " + std::to_string(instructions) + " instructions; " +
                            std::string(spec_.noun) + " runs at most " +
                            std::to_string(render::kMaxInstructions) +
                            ". Name values used more than once, so they are computed once");
        }
        const std::size_t constants = material_.program.constants.size();
        if (constants > render::kMaxConstants) {
            fail(path_, "the graph uses " + std::to_string(constants) + " constants and params; " +
                            std::string(spec_.noun) + " holds at most " +
                            std::to_string(render::kMaxConstants));
        }
    }

    void emit_program(const Ref& root) {
        for (std::uint32_t r = spec_.first_free; r < render::kRegisterCount; ++r) {
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
        check_limits();
    }

    const json::Value& doc_;
    const GraphSpec& spec_;
    std::uint32_t stage_ = 0;
    std::string path_ = "/color"; // where errors about the whole program go
    std::uint32_t outputs_ = 0;
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
// runs instructions, fail first, which also bounds the compiler's recursion. The graph's roots
// are the nodes and roots' values.
std::optional<json::ValidationError>
check_size(const json::Value& doc, std::span<const std::string_view> roots, std::string_view noun) {
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
            error = {at, std::string(noun) + " has at most " +
                             std::to_string(render::kMaxInstructions) +
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
    for (const std::string_view root : roots) {
        const json::Value* value = doc.find(root);
        if (value == nullptr) {
            continue;
        }
        const std::string at = "/" + std::string(root);
        if (root == "color") {
            walk(walk, *value, at, 1);
        } else if (value->is_object()) {
            for (const auto& [name, output] : value->as_object()) {
                walk(walk, output, at + "/" + json::pointer_escape(name), 1);
            }
        }
    }
    return error;
}

constexpr std::array<std::string_view, 1> kMaterialRoots{"color"};
constexpr std::array<std::string_view, 2> kParticleRoots{"start", "process"};

// Every error in a document; with none, the compiled material goes in out.
std::vector<json::ValidationError> compile(const json::Value& doc, CanvasMaterial* out) {
    if (auto error = check_size(doc, kMaterialRoots, "a material")) {
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

namespace detail {

void add_particle_graph_schema(json::Value& schema, bool list_ops) {
    json::Value node = node_schema(list_ops);
    json::Value arg = arg_schema(kParticleInputs, node);
    json::Value& properties = schema["properties"];
    properties["params"]["type"] = "object";
    properties["params"]["additionalProperties"] = param_schema(false);
    properties["params"]["description"] =
        "Named values the graphs read, as a particle shader's uniforms, by name.";
    properties["nodes"]["type"] = "object";
    properties["nodes"]["additionalProperties"]["$ref"] = "#/$defs/node";
    properties["nodes"]["description"] =
        "Named nodes the graphs share, for args that use a value more than once.";
    for (const auto& [key, stage, what] :
         {std::tuple{"start", kStartStage,
                     "What each particle starts with, after the material places it, as a "
                     "particle shader's start(): outputs by name, each an arg."},
          std::tuple{"process", kProcessStage,
                     "What changes each step, after the material works it out, as a particle "
                     "shader's process(): outputs by name, each an arg."}}) {
        json::Value outputs;
        outputs["type"] = "object";
        outputs["additionalProperties"] = false;
        outputs["description"] = what;
        for (const Output& output : kParticleOutputs) {
            if ((output.stages & stage) == 0) {
                continue;
            }
            json::Value value;
            value["$ref"] = "#/$defs/arg";
            value["description"] =
                std::string(short_kind_name(output.kind)) + ": " + std::string(output.doc);
            outputs["properties"][output.name] = std::move(value);
        }
        properties[key] = std::move(outputs);
    }
    schema["$defs"]["node"] = std::move(node);
    schema["$defs"]["arg"] = std::move(arg);
}

std::optional<json::ValidationError> check_particle_graph_size(const json::Value& doc) {
    return check_size(doc, kParticleRoots, "a process material's graphs");
}

std::vector<json::ValidationError> compile_particle_graphs(const json::Value& doc,
                                                           render::ParticleProgram* start,
                                                           render::ParticleProgram* process) {
    // Every node and output with every stage's inputs first, so each mistake is found once.
    Compiler check(doc, kParticleSpec);
    check.read_params();
    check.read_nodes(true);
    for (const std::string_view key : kParticleRoots) {
        if (const json::Value* stage = doc.find(key); stage != nullptr) {
            (void)check.read_outputs(*stage, "/" + std::string(key));
        }
    }
    if (!check.errors().empty()) {
        return std::move(check.errors());
    }
    std::vector<json::ValidationError> errors;
    for (const auto& [key, stage, out] :
         {std::tuple{"start", kStartStage, start}, std::tuple{"process", kProcessStage, process}}) {
        const json::Value* outputs = doc.find(key);
        if (outputs == nullptr) {
            continue;
        }
        Compiler compiler(doc, kParticleSpec, stage);
        compiler.read_params();
        compiler.read_nodes(false);
        compiler.emit_outputs(*outputs, "/" + std::string(key));
        if (!compiler.errors().empty()) {
            errors.insert(errors.end(), compiler.errors().begin(), compiler.errors().end());
        } else if (out != nullptr) {
            out->program = std::move(compiler.material().program);
            out->outputs = compiler.outputs();
        }
    }
    return errors;
}

} // namespace detail

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

render::ShaderSource shader_source(const CanvasMaterial& material, std::string code) {
    render::ShaderSource source{.path = material.shader, .code = std::move(code), .params = {}};
    for (const auto& [name, param] : material.params) {
        if (!param.slot) {
            continue;
        }
        const auto components = param.type == "texture"
                                    ? 0U
                                    : static_cast<std::uint32_t>(size_of(param_kind(param.type)));
        source.params.push_back({.name = name, .components = components, .slot = *param.slot});
    }
    return source;
}

Result<std::shared_ptr<const render::CompiledShader>>
compile_material_shader(const CanvasMaterial& material, const world::FileReader& read) {
    if (material.compiled || material.shader.empty()) {
        return material.compiled;
    }
    auto code = read(material.shader);
    if (!code) {
        return code.error();
    }
    return render::compile_shader(shader_source(material, std::move(code).value()));
}

std::vector<json::ValidationError> check_material_shader(const json::Value& doc,
                                                         const world::FileReader& read) {
    auto material = parse_material(doc);
    if (!material || material.value().shader.empty()) {
        return {};
    }
    auto compiled = compile_material_shader(material.value(), read);
    if (compiled) {
        return {};
    }
    std::vector<json::ValidationError> errors;
    std::istringstream lines(compiled.error().message);
    std::string line;
    while (std::getline(lines, line)) {
        errors.push_back({"/shader", line});
    }
    return errors;
}

json::Value compiled_shader_json(const render::CompiledShader& shader,
                                 const ShaderTargets& targets) {
    json::Value out = json::Object{};
    const auto words = [&](const char* key, const std::vector<std::uint32_t>& code) {
        if (code.empty()) {
            return;
        }
        std::string bytes(code.size() * 4, '\0');
        for (std::size_t i = 0; i < code.size(); ++i) {
            for (std::size_t k = 0; k < 4; ++k) {
                bytes[(i * 4) + k] = static_cast<char>((code[i] >> (8 * k)) & 0xFFU);
            }
        }
        out[key] = base64_encode(bytes);
    };
    const auto text = [&](const char* key, const std::string& code) {
        if (!code.empty()) {
            out[key] = code;
        }
    };
    if (targets.spirv) {
        words("spirv_vertex", shader.spirv_vertex);
        words("spirv_fragment", shader.spirv_fragment);
    }
    if (targets.msl) {
        text("msl_vertex", shader.msl_vertex);
        text("msl_fragment", shader.msl_fragment);
    }
    if (targets.glsl) {
        text("glsl_vertex", shader.glsl_vertex);
        text("glsl_fragment", shader.glsl_fragment);
    }
    if (targets.hlsl) {
        text("hlsl", shader.hlsl);
    }
    words("cpu_vertex", shader.cpu_vertex);
    words("cpu_fragment", shader.cpu_fragment);
    if (shader.normal_map_used) {
        out["normal_map_used"] = true;
    }
    if (shader.specular_used) {
        out["specular_used"] = true;
    }
    return out;
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
    instance.light_mode = material.light_mode;
    instance.program = material.program;
    instance.shader = material.shader;
    instance.compiled = material.compiled;
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
    // What an 8-bit target keeps; NaN becomes 0, as on the GPU.
    const auto keep = [&](std::array<float, 4> color) {
        for (float& channel : color) {
            channel = channel > 0.0F ? std::min(channel, 1.0F) : 0.0F;
        }
        colors.push_back(color);
    };
    if (!instance.shader.empty()) {
        if (!instance.compiled) {
            return make_error(ErrorCode::InvalidArgument,
                              instance.shader + ": the shader isn't compiled");
        }
        auto loaded = render::ShaderVm::load(instance.compiled->cpu_fragment);
        if (!loaded) {
            return make_error(loaded.error().code, instance.shader + ": " + loaded.error().message);
        }
        render::ShaderVm vm = std::move(loaded).value();
        // The constants, then the texel size, as render::cpu_shader lays them out.
        std::array<std::byte, render::cpu_shader::kUniformBytes> uniforms{};
        const std::array<float, 4> texel{pixel.texel[0], pixel.texel[1], 0.0F, 0.0F};
        std::memcpy(uniforms.data(), instance.program.constants.data(),
                    std::min(instance.program.constants.size(), render::cpu_shader::kConstants) *
                        sizeof(texel));
        std::memcpy(uniforms.data() + (render::cpu_shader::kConstants * sizeof(texel)),
                    texel.data(), sizeof(texel));
        const std::array<std::span<const std::byte>, 1> buffers{uniforms};
        const auto image_of = [&](std::uint32_t binding) {
            return slots.at(std::min<std::size_t>(binding, slots.size() - 1)).image;
        };
        const render::ShaderVm::Textures textures{
            .sample = [&](std::uint32_t binding, float u,
                          float v) { return sample(std::min(binding, 3U), u, v); },
            .fetch = [&](std::uint32_t binding, std::int32_t x,
                         std::int32_t y) -> std::array<float, 4> {
                const render::Image* image = image_of(binding);
                if (image == nullptr) {
                    return {1.0F, 1.0F, 1.0F, 1.0F};
                }
                if (x < 0 || y < 0 || std::cmp_greater_equal(x, image->width) ||
                    std::cmp_greater_equal(y, image->height)) {
                    return {0.0F, 0.0F, 0.0F, 0.0F};
                }
                const std::size_t at =
                    ((static_cast<std::size_t>(y) * image->width) + static_cast<std::size_t>(x)) *
                    4;
                const auto channel = [&](std::size_t i) {
                    return static_cast<float>(image->pixels[at + i]) / 255.0F;
                };
                return {channel(0), channel(1), channel(2), channel(3)};
            },
            .size = [&](std::uint32_t binding) -> std::array<std::uint32_t, 2> {
                const render::Image* image = image_of(binding);
                if (image == nullptr) {
                    return {1, 1};
                }
                return {image->width, image->height};
            }};
        namespace cpu = render::cpu_shader;
        for (std::size_t first = 0; first < uvs.size(); first += render::ShaderVm::kLanes) {
            render::ShaderVm::Lanes lanes;
            lanes.count =
                static_cast<std::uint32_t>(std::min(render::ShaderVm::kLanes, uvs.size() - first));
            for (std::uint32_t lane = 0; lane < lanes.count; ++lane) {
                const std::array<float, 2>& uv = uvs[first + lane];
                auto& in = lanes.inputs.at(lane);
                in.at(cpu::kUv) = {uv[0], uv[1], 0.0F, 0.0F};
                in.at(cpu::kColor) = inputs.color;
                in.at(cpu::kFragCoord) = {inputs.frag_coord[0], inputs.frag_coord[1], 0.0F, 0.0F};
                in.at(cpu::kTextureColor) = sample(0, uv[0], uv[1]);
                in.at(cpu::kNormalTexel) = {0.5F, 0.5F, 1.0F, 1.0F};
                in.at(cpu::kSpecular) = {1.0F, 1.0F, 1.0F, 1.0F};
            }
            vm.run(lanes, buffers, textures);
            for (std::uint32_t lane = 0; lane < lanes.count; ++lane) {
                // A discarded pixel draws nothing.
                keep(lanes.discarded.at(lane) ? std::array<float, 4>{}
                                              : lanes.outputs.at(lane).at(cpu::kOutColor));
            }
        }
        return colors;
    }
    for (const auto& uv : uvs) {
        pixel.uv = uv;
        if (instance.program.code.empty()) {
            const std::array<float, 4> texel = sample(0, uv[0], uv[1]);
            std::array<float, 4> color{};
            for (std::size_t i = 0; i < 4; ++i) {
                color.at(i) = texel.at(i) * inputs.color.at(i);
            }
            keep(color);
        } else {
            keep(render::run_program(instance.program, pixel, sample));
        }
    }
    return colors;
}

} // namespace slash::canvas
