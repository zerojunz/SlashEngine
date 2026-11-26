// Blender shader node trees (as scene3d::blend_nodes_script writes them) turned into texture
// graphs: each node a material's Principled BSDF reads is followed back and written as the
// graph's ops, Blender's colors (scene-linear) as sRGB.

#include "slash/procedural/blender_nodes.hpp"

#include "slash/core/text.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace slash::procedural {
namespace {

constexpr double kGray = 0.5;

// A scene-linear channel as sRGB.
double to_srgb(double linear) {
    const double c = std::clamp(linear, 0.0, 1.0);
    return c <= 0.0031308 ? c * 12.92 : (1.055 * std::pow(c, 1.0 / 2.4)) - 0.055;
}

double rounded(double value) {
    return std::round(value * 10000.0) / 10000.0;
}

json::Value color_of(const json::Array& linear) {
    json::Array out;
    for (std::size_t k = 0; k < 3; ++k) {
        out.emplace_back(rounded(to_srgb(linear[k].as_double())));
    }
    out.emplace_back(linear.size() > 3 ? rounded(linear[3].as_double()) : 1.0);
    return out;
}

// Blender's names for a mix's blend types, as the graph's blend modes.
constexpr std::array<std::pair<std::string_view, std::string_view>, 11> kBlendTypes{{
    {"MIX", "mix"},
    {"ADD", "add"},
    {"SUBTRACT", "subtract"},
    {"MULTIPLY", "multiply"},
    {"SCREEN", "screen"},
    {"OVERLAY", "overlay"},
    {"DARKEN", "darken"},
    {"LIGHTEN", "lighten"},
    {"DIFFERENCE", "difference"},
    {"SOFT_LIGHT", "soft_light"},
    {"DIVIDE", "divide"},
}};

// Blender's math operations, as the graph's.
constexpr std::array<std::pair<std::string_view, std::string_view>, 9> kMathOperations{{
    {"ADD", "add"},
    {"SUBTRACT", "subtract"},
    {"MULTIPLY", "multiply"},
    {"DIVIDE", "divide"},
    {"MINIMUM", "min"},
    {"MAXIMUM", "max"},
    {"POWER", "pow"},
    {"ABSOLUTE", "abs"},
    {"FRACT", "fract"},
}};

template <std::size_t N>
std::optional<std::string_view>
lookup(const std::array<std::pair<std::string_view, std::string_view>, N>& table,
       std::string_view key) {
    for (const auto& [from, to] : table) {
        if (from == key) {
            return to;
        }
    }
    return std::nullopt;
}

class Converter {
public:
    Converter(const json::Value& nodes, const json::Value& links) : nodes_(nodes), links_(links) {}

    Result<json::Value> run(std::uint32_t size) {
        const std::string surface = principled();
        if (surface.empty()) {
            return make_error(ErrorCode::InvalidArgument,
                              "it has no Principled BSDF (or Diffuse BSDF) for the graph to "
                              "follow");
        }
        json::Array outputs;
        const auto add = [&](std::string_view name, const std::string& node, bool gray) {
            json::Value output;
            output["name"] = name;
            output["node"] = node;
            if (gray) {
                output["as"] = "gray";
            }
            outputs.push_back(std::move(output));
        };
        add("albedo", input(surface, {"Base Color", "Color"}, Kind::Color), false);
        if (has_socket(surface, "Roughness")) {
            add("roughness", input(surface, {"Roughness"}, Kind::Gray), true);
        }
        if (has_socket(surface, "Metallic")) {
            add("metallic", input(surface, {"Metallic"}, Kind::Gray), true);
        }
        if (linked(surface, "Normal") != nullptr) {
            add("normal", input(surface, {"Normal"}, Kind::Color), false);
        }
        const std::initializer_list<std::string_view> emission{"Emission Color", "Emission"};
        bool glows = false;
        for (const std::string_view socket : emission) {
            if (linked(surface, socket) != nullptr) {
                glows = true;
            } else if (const json::Value* value = default_of(surface, socket);
                       value != nullptr && value->is_array()) {
                const json::Array& c = value->as_array();
                const double strength = number(surface, "Emission Strength", 1.0);
                glows = strength > 0.0 && (c[0].as_double() > 0.0 || c[1].as_double() > 0.0 ||
                                           c[2].as_double() > 0.0);
            }
            if (glows) {
                add("emission", input(surface, emission, Kind::Color), false);
                break;
            }
        }

        json::Value graph;
        graph["format"] = 1;
        graph["type"] = "graph";
        graph["width"] = size;
        graph["height"] = size;
        graph["nodes"] = std::move(made_);
        graph["outputs"] = std::move(outputs);
        return graph;
    }

    [[nodiscard]] std::vector<std::string> warnings() const { return warnings_; }

private:
    enum class Kind : std::uint8_t { Color, Gray };

    const json::Value& nodes_;
    const json::Value& links_;
    json::Object made_;
    std::map<std::string, std::string, std::less<>> done_;
    std::vector<std::string> warnings_;
    std::set<std::string, std::less<>> warned_;

    void warn(std::string message) {
        if (warned_.insert(message).second) {
            warnings_.push_back(std::move(message));
        }
    }

    [[nodiscard]] const json::Value& node(std::string_view name) const {
        return *nodes_.find(name);
    }

    [[nodiscard]] std::string type_of(std::string_view name) const {
        const json::Value* type = node(name).find("type");
        return type != nullptr && type->is_string() ? type->as_string() : std::string();
    }

    // The node the graph follows from: the material output's surface, if a BSDF, else the
    // first Principled BSDF.
    [[nodiscard]] std::string principled() const {
        const auto bsdf = [&](std::string_view name) {
            const std::string type = type_of(name);
            return type == "ShaderNodeBsdfPrincipled" || type == "ShaderNodeBsdfDiffuse";
        };
        for (const auto& [name, each] : nodes_.as_object()) {
            if (type_of(name) == "ShaderNodeOutputMaterial") {
                if (const json::Value* link = linked(name, "Surface");
                    link != nullptr && bsdf(link->find("from")->as_string())) {
                    return link->find("from")->as_string();
                }
            }
        }
        for (const auto& [name, each] : nodes_.as_object()) {
            if (bsdf(name)) {
                return name;
            }
        }
        return {};
    }

    [[nodiscard]] const json::Value* linked(std::string_view name, std::string_view socket) const {
        for (const json::Value& link : links_.as_array()) {
            if (link.find("to")->as_string() == name &&
                link.find("to_socket")->as_string() == socket) {
                return &link;
            }
        }
        return nullptr;
    }

    [[nodiscard]] const json::Value* default_of(std::string_view name,
                                                std::string_view socket) const {
        const json::Value* inputs = node(name).find("inputs");
        return inputs != nullptr ? inputs->find(socket) : nullptr;
    }

    [[nodiscard]] bool has_socket(std::string_view name, std::string_view socket) const {
        return default_of(name, socket) != nullptr || linked(name, socket) != nullptr;
    }

    [[nodiscard]] double number(std::string_view name, std::string_view socket,
                                double fallback) const {
        const json::Value* value = default_of(name, socket);
        return value != nullptr && value->is_number() ? value->as_double() : fallback;
    }

    [[nodiscard]] bool flag(std::string_view name, std::string_view key) const {
        const json::Value* props = node(name).find("props");
        const json::Value* value = props != nullptr ? props->find(key) : nullptr;
        return value != nullptr && value->is_bool() && value->as_bool();
    }

    [[nodiscard]] std::string prop(std::string_view name, std::string_view key) const {
        const json::Value* props = node(name).find("props");
        const json::Value* value = props != nullptr ? props->find(key) : nullptr;
        return value != nullptr && value->is_string() ? value->as_string() : std::string();
    }

    // A new node's name: the Blender node's, as lowercase letters, digits and underscores.
    std::string fresh(std::string_view base) {
        std::string name;
        for (const char c : base) {
            const auto u = static_cast<unsigned char>(c);
            if (std::isalnum(u) != 0) {
                name += static_cast<char>(std::tolower(u));
            } else if (!name.empty() && name.back() != '_') {
                name += '_';
            }
        }
        while (!name.empty() && name.back() == '_') {
            name.pop_back();
        }
        if (name.empty() || std::isdigit(static_cast<unsigned char>(name.front())) != 0) {
            name.insert(0, "n_");
        }
        std::string out = name;
        for (int k = 2; made_.contains(out); ++k) {
            out = name + "_" + std::to_string(k);
        }
        return out;
    }

    std::string add(std::string_view base, json::Value op) {
        std::string name = fresh(base);
        made_[name] = std::move(op);
        return name;
    }

    std::string constant(const json::Value& color) {
        const std::string key = "constant:" + json::write(color, json::Style::Compact);
        if (const auto found = done_.find(key); found != done_.end()) {
            return found->second;
        }
        std::string name = add("constant", json::Object{{"op", "constant"}, {"color", color}});
        done_.emplace(key, name);
        return name;
    }

    std::string gray(double value) {
        const double v = rounded(std::clamp(value, 0.0, 1.0));
        return constant(json::Array{v, v, v, 1.0});
    }

    // The node an input reads: what is linked to the first of sockets the node has, else its
    // value as a constant.
    std::string input(std::string_view name, const std::vector<std::string_view>& sockets,
                      Kind kind) {
        for (const std::string_view socket : sockets) {
            if (const json::Value* link = linked(name, socket); link != nullptr) {
                return follow(link->find("from")->as_string(),
                              link->find("from_socket")->as_string());
            }
        }
        for (const std::string_view socket : sockets) {
            const json::Value* value = default_of(name, socket);
            if (value == nullptr) {
                continue;
            }
            if (value->is_number()) {
                return gray(value->as_double());
            }
            if (value->is_array() && value->as_array().size() == 4 && kind == Kind::Color) {
                return constant(color_of(value->as_array()));
            }
        }
        return gray(kGray);
    }

    // An input as a number where it isn't linked (for a setting), else nothing.
    [[nodiscard]] std::optional<double> unlinked(std::string_view name, std::string_view socket,
                                                 double fallback) const {
        if (linked(name, socket) != nullptr) {
            return std::nullopt;
        }
        return number(name, socket, fallback);
    }

    std::string follow(const std::string& name, const std::string& socket) {
        const std::string key = name + "\n" + socket;
        if (const auto found = done_.find(key); found != done_.end()) {
            return found->second;
        }
        if (nodes_.find(name) == nullptr) {
            return gray(kGray);
        }
        // A placeholder against loops; Blender's node trees have none.
        done_.emplace(key, gray(kGray));
        std::string made = translate(name, socket);
        done_[key] = made;
        return made;
    }

    void settings_linked(std::string_view name, const std::vector<std::string_view>& sockets) {
        for (const std::string_view socket : sockets) {
            if (linked(name, socket) != nullptr) {
                warn("\"" + std::string(name) + "\"'s " + std::string(socket) +
                     " is linked; the graph uses its value");
            }
        }
        if (linked(name, "Vector") != nullptr) {
            warn("texture coordinates and mappings into \"" + std::string(name) +
                 "\" are left out; it spans the texture once");
        }
    }

    std::string translate(const std::string& name, const std::string& socket) {
        const std::string type = type_of(name);
        if (type == "NodeReroute") {
            return input(name, {"Input"}, Kind::Color);
        }
        if (type == "ShaderNodeRGB" || type == "ShaderNodeValue") {
            const json::Value* outputs = node(name).find("outputs");
            const json::Value* value = outputs != nullptr ? outputs->find(socket) : nullptr;
            if (value != nullptr && value->is_array() && value->as_array().size() == 4) {
                return constant(color_of(value->as_array()));
            }
            return gray(value != nullptr && value->is_number() ? value->as_double() : kGray);
        }
        if (type == "ShaderNodeTexNoise") {
            settings_linked(name, {"Scale", "Detail", "Roughness"});
            const double scale = number(name, "Scale", 5.0);
            json::Value noise;
            noise["noise_type"] = "perlin";
            noise["fractal_type"] = "fbm";
            noise["fractal_octaves"] =
                std::clamp(static_cast<int>(std::lround(number(name, "Detail", 2.0))) + 1, 1, 16);
            noise["fractal_gain"] = rounded(number(name, "Roughness", 0.5));
            noise["fractal_lacunarity"] = rounded(number(name, "Lacunarity", 2.0));
            noise["frequency"] = 1;
            if (number(name, "Distortion", 0.0) > 0.0) {
                warn("\"" + name + "\"'s distortion is left out");
            }
            if (socket == "Color") {
                warn("\"" + name + "\"'s Color reads as its gray Fac");
            }
            return add(name, json::Object{{"op", "noise"},
                                          {"noise", std::move(noise)},
                                          {"scale", json::Array{scale, scale}}});
        }
        if (type == "ShaderNodeTexVoronoi") {
            settings_linked(name, {"Scale", "Randomness"});
            const double scale = number(name, "Scale", 5.0);
            json::Value noise;
            noise["noise_type"] = "cellular";
            noise["fractal_type"] = "none";
            noise["frequency"] = 1;
            noise["cellular_jitter"] = rounded(number(name, "Randomness", 1.0));
            noise["cellular_distance_function"] =
                prop(name, "distance") == "MANHATTAN" ? "manhattan" : "euclidean";
            noise["cellular_return_type"] = socket == "Distance" ? "distance" : "cell_value";
            if (const std::string feature = prop(name, "feature");
                !feature.empty() && feature != "F1") {
                warn("\"" + name + "\"'s " + feature + " feature reads as F1");
            }
            return add(name, json::Object{{"op", "noise"},
                                          {"noise", std::move(noise)},
                                          {"scale", json::Array{scale, scale}}});
        }
        if (type == "ShaderNodeTexWave") {
            settings_linked(name, {"Scale"});
            const std::string direction = prop(name, "bands_direction");
            double angle = 0.0;
            if (direction == "Y") {
                angle = 0.25;
            } else if (direction == "DIAGONAL") {
                angle = 0.125;
            }
            if (prop(name, "wave_type") == "RINGS") {
                warn("\"" + name + "\"'s rings are drawn as bands");
            }
            const std::string profile = prop(name, "wave_profile");
            std::string shape = "sine";
            if (profile == "TRI") {
                shape = "triangle";
            } else if (profile == "SAW") {
                warn("\"" + name + "\"'s saw profile is drawn as a sine");
            }
            return add(name, json::Object{{"op", "waves"},
                                          {"count", rounded(number(name, "Scale", 5.0))},
                                          {"angle", angle},
                                          {"shape", shape}});
        }
        if (type == "ShaderNodeTexChecker") {
            settings_linked(name, {"Scale"});
            const double scale = number(name, "Scale", 5.0);
            std::string checker =
                add(name, json::Object{{"op", "checker"}, {"cells", json::Array{scale, scale}}});
            if (socket == "Fac") {
                return checker;
            }
            return add(name + " color",
                       json::Object{{"op", "blend"},
                                    {"base", input(name, {"Color2"}, Kind::Color)},
                                    {"layer", input(name, {"Color1"}, Kind::Color)},
                                    {"mask", checker}});
        }
        if (type == "ShaderNodeTexBrick") {
            settings_linked(name, {"Scale", "Brick Width", "Row Height", "Mortar Size"});
            const double scale = number(name, "Scale", 5.0);
            const double width = std::max(number(name, "Brick Width", 0.5), 0.01);
            const double height = std::max(number(name, "Row Height", 0.25), 0.01);
            std::string bricks =
                add(name,
                    json::Object{
                        {"op", "bricks"},
                        {"across", rounded(std::clamp(scale / width, 1.0, 256.0))},
                        {"down", rounded(std::clamp(scale / height, 1.0, 256.0))},
                        {"mortar",
                         rounded(std::clamp(number(name, "Mortar Size", 0.02) / height, 0.0, 1.0))},
                        {"bevel", 0.0}});
            std::string mortar =
                add(name + " mortar",
                    json::Object{{"op", "swizzle"}, {"input", bricks}, {"channels", "rrr1"}});
            mortar = add(name + " fac", json::Object{{"op", "invert"}, {"input", mortar}});
            if (socket == "Fac") {
                return mortar;
            }
            std::string random =
                add(name + " random",
                    json::Object{{"op", "swizzle"}, {"input", bricks}, {"channels", "ggg1"}});
            std::string colors =
                add(name + " colors", json::Object{{"op", "blend"},
                                                   {"base", input(name, {"Color1"}, Kind::Color)},
                                                   {"layer", input(name, {"Color2"}, Kind::Color)},
                                                   {"mask", random}});
            return add(name + " color",
                       json::Object{{"op", "blend"},
                                    {"base", colors},
                                    {"layer", input(name, {"Mortar"}, Kind::Color)},
                                    {"mask", mortar}});
        }
        if (type == "ShaderNodeTexGradient") {
            settings_linked(name, {});
            const std::string kind = prop(name, "gradient_type");
            std::string fill = "linear";
            if (kind == "SPHERICAL" || kind == "QUADRATIC_SPHERE") {
                fill = "radial";
            } else if (!kind.empty() && kind != "LINEAR") {
                warn("\"" + name + "\"'s " + kind + " gradient is drawn linear");
            }
            json::Value op = json::Object{{"op", "gradient"}, {"fill", fill}};
            if (fill == "radial") {
                op["fill_from"] = json::Array{0.5, 0.5};
                op["fill_to"] = json::Array{1.0, 0.5};
            }
            return add(name, std::move(op));
        }
        if (type == "ShaderNodeTexImage") {
            settings_linked(name, {});
            const json::Value* image = node(name).find("image");
            if (image == nullptr || !image->is_string() || image->as_string().starts_with("..")) {
                warn("\"" + name + "\" has no image in the project; it reads gray");
                return gray(kGray);
            }
            std::string read =
                add(name, json::Object{{"op", "image"}, {"path", image->as_string()}});
            if (socket == "Alpha") {
                return add(name + " alpha",
                           json::Object{{"op", "swizzle"}, {"input", read}, {"channels", "aaa1"}});
            }
            return read;
        }
        if (type == "ShaderNodeValToRGB") {
            json::Array points;
            if (const json::Value* ramp = node(name).find("ramp"); ramp != nullptr) {
                for (const json::Value& element : ramp->find("elements")->as_array()) {
                    points.emplace_back(json::Object{
                        {"offset",
                         rounded(std::clamp(element.find("position")->as_double(), 0.0, 1.0))},
                        {"color", color_of(element.find("color")->as_array())}});
                }
            }
            json::Value gradient;
            gradient["points"] = std::move(points);
            const json::Value* ramp = node(name).find("ramp");
            const std::string interpolation =
                ramp != nullptr && ramp->find("interpolation") != nullptr
                    ? ramp->find("interpolation")->as_string()
                    : "LINEAR";
            if (interpolation == "CONSTANT") {
                gradient["interpolation_mode"] = "constant";
            } else if (interpolation != "LINEAR") {
                gradient["interpolation_mode"] = "cubic";
            }
            std::string mapped = add(name, json::Object{{"op", "gradient_map"},
                                                        {"input", input(name, {"Fac"}, Kind::Gray)},
                                                        {"gradient", std::move(gradient)}});
            if (socket == "Alpha") {
                return add(
                    name + " alpha",
                    json::Object{{"op", "swizzle"}, {"input", mapped}, {"channels", "aaa1"}});
            }
            return mapped;
        }
        if (type == "ShaderNodeMixRGB" || type == "ShaderNodeMix") {
            std::vector<std::string_view> a{"Color1", "A_Color"};
            std::vector<std::string_view> b{"Color2", "B_Color"};
            if (type == "ShaderNodeMix" && prop(name, "data_type") == "FLOAT") {
                a = {"A_Float"};
                b = {"B_Float"};
            } else if (type == "ShaderNodeMix" && prop(name, "data_type") == "VECTOR") {
                warn("\"" + name + "\" mixes vectors, which the graph mixes as colors");
                a = {"A_Vector"};
                b = {"B_Vector"};
            }
            const std::string blend_type = prop(name, "blend_type");
            std::string_view mode = "mix";
            if (const auto known = lookup(kBlendTypes, blend_type)) {
                mode = *known;
            } else if (!blend_type.empty()) {
                warn("\"" + name + "\"'s " + blend_type + " blend mixes");
            }
            json::Value op = json::Object{{"op", "blend"},
                                          {"base", input(name, a, Kind::Color)},
                                          {"layer", input(name, b, Kind::Color)},
                                          {"mode", mode}};
            const std::initializer_list<std::string_view> factor{"Fac", "Factor_Float"};
            bool masked = false;
            for (const std::string_view socket_name : factor) {
                if (linked(name, socket_name) != nullptr) {
                    op["mask"] = input(name, {socket_name}, Kind::Gray);
                    masked = true;
                    break;
                }
            }
            if (!masked) {
                double fac = 0.5;
                for (const std::string_view socket_name : factor) {
                    fac = number(name, socket_name, fac);
                }
                op["opacity"] = rounded(std::clamp(fac, 0.0, 1.0));
            }
            return add(name, std::move(op));
        }
        if (type == "ShaderNodeMath") {
            const std::string operation = prop(name, "operation");
            const auto known = lookup(kMathOperations, operation);
            std::string a = input(name, {"Value"}, Kind::Gray);
            if (!known) {
                warn("\"" + name + "\"'s " + operation +
                     " isn't carried over; it passes its "
                     "first value");
                return a;
            }
            json::Value op = json::Object{{"op", "math"}, {"a", a}, {"operation", *known}};
            if (*known != "abs" && *known != "fract") {
                if (const auto value = unlinked(name, "Value_001", 0.5)) {
                    op["value"] = rounded(*value);
                } else {
                    op["b"] = input(name, {"Value_001"}, Kind::Gray);
                }
            }
            if (flag(name, "use_clamp")) {
                warn("\"" + name + "\"'s clamp is left out");
            }
            return add(name, std::move(op));
        }
        if (type == "ShaderNodeInvert") {
            const std::string in = input(name, {"Color"}, Kind::Color);
            std::string inverted = add(name, json::Object{{"op", "invert"}, {"input", in}});
            if (const auto fac = unlinked(name, "Fac", 1.0); fac && *fac < 1.0) {
                return add(name + " fac", json::Object{{"op", "blend"},
                                                       {"base", in},
                                                       {"layer", inverted},
                                                       {"opacity", rounded(*fac)}});
            }
            return inverted;
        }
        if (type == "ShaderNodeHueSaturation") {
            return add(name, json::Object{{"op", "hsv"},
                                          {"input", input(name, {"Color"}, Kind::Color)},
                                          {"hue", rounded(number(name, "Hue", 0.5) - 0.5)},
                                          {"saturation", rounded(number(name, "Saturation", 1.0))},
                                          {"value", rounded(number(name, "Value", 1.0))}});
        }
        if (type == "ShaderNodeRGBToBW") {
            return add(name, json::Object{{"op", "grayscale"},
                                          {"input", input(name, {"Color"}, Kind::Color)}});
        }
        if (type == "ShaderNodeSeparateColor" || type == "ShaderNodeSeparateRGB") {
            std::string channels = "rrr1";
            if (socket == "Green" || socket == "G") {
                channels = "ggg1";
            } else if (socket == "Blue" || socket == "B") {
                channels = "bbb1";
            }
            return add(name + " " + socket,
                       json::Object{{"op", "swizzle"},
                                    {"input", input(name, {"Color", "Image"}, Kind::Color)},
                                    {"channels", channels}});
        }
        if (type == "ShaderNodeCombineColor" || type == "ShaderNodeCombineRGB") {
            json::Value op = json::Object{{"op", "merge"}};
            const std::array<std::pair<std::string_view, std::vector<std::string_view>>, 3>
                channels{
                    {{"red", {"Red", "R"}}, {"green", {"Green", "G"}}, {"blue", {"Blue", "B"}}}};
            for (const auto& [channel, sockets] : channels) {
                bool done = false;
                for (const std::string_view socket_name : sockets) {
                    if (linked(name, socket_name) != nullptr) {
                        op[channel] = input(name, {socket_name}, Kind::Gray);
                        done = true;
                        break;
                    }
                }
                if (!done) {
                    double value = 0.0;
                    for (const std::string_view socket_name : sockets) {
                        value = number(name, socket_name, value);
                    }
                    op[channel] = rounded(value);
                }
            }
            return add(name, std::move(op));
        }
        if (type == "ShaderNodeMapRange") {
            settings_linked(name, {"From Min", "From Max", "To Min", "To Max"});
            return add(name,
                       json::Object{{"op", "remap"},
                                    {"input", input(name, {"Value"}, Kind::Gray)},
                                    {"from", json::Array{rounded(number(name, "From Min", 0.0)),
                                                         rounded(number(name, "From Max", 1.0))}},
                                    {"to", json::Array{rounded(number(name, "To Min", 0.0)),
                                                       rounded(number(name, "To Max", 1.0))}},
                                    {"clamp", flag(name, "clamp")}});
        }
        if (type == "ShaderNodeClamp") {
            return add(
                name, json::Object{{"op", "clamp"}, {"input", input(name, {"Value"}, Kind::Gray)}});
        }
        if (type == "ShaderNodeGamma") {
            const double gamma = std::max(number(name, "Gamma", 1.0), 0.0001);
            return add(name, json::Object{{"op", "levels"},
                                          {"input", input(name, {"Color"}, Kind::Color)},
                                          {"gamma", rounded(1.0 / gamma)}});
        }
        if (type == "ShaderNodeBump") {
            return add(name, json::Object{{"op", "normal_from_height"},
                                          {"height", input(name, {"Height"}, Kind::Gray)},
                                          {"strength", rounded(number(name, "Strength", 1.0))}});
        }
        if (type == "ShaderNodeNormalMap") {
            return input(name, {"Color"}, Kind::Color);
        }
        if (type == "ShaderNodeBrightContrast") {
            warn("\"" + name + "\"'s brightness and contrast are left out");
            return input(name, {"Color"}, Kind::Color);
        }
        warn("\"" + name + "\" (" + type + ") has no graph op; it reads gray");
        return gray(kGray);
    }
};

} // namespace

std::vector<std::string> blender_materials(const json::Value& dump) {
    std::vector<std::string> out;
    const json::Value* materials = dump.is_object() ? dump.find("materials") : nullptr;
    if (materials != nullptr && materials->is_object()) {
        for (const auto& [name, each] : materials->as_object()) {
            out.push_back(name);
        }
    }
    return out;
}

Result<BlenderGraph> texture_graph_from_blender(const json::Value& dump, std::string_view material,
                                                std::uint32_t size) {
    const std::vector<std::string> names = blender_materials(dump);
    if (names.empty()) {
        return make_error(ErrorCode::InvalidArgument, "it holds no materials with shader nodes");
    }
    std::string chosen(material);
    if (chosen.empty()) {
        if (names.size() > 1) {
            std::string message =
                "it holds " + std::to_string(names.size()) + " materials; name one of";
            for (const std::string& name : names) {
                message += " \"" + name + "\"";
            }
            return make_error(ErrorCode::InvalidArgument, message);
        }
        chosen = names.front();
    }
    const json::Value* found = dump.find("materials")->find(chosen);
    if (found == nullptr) {
        std::string message = "it has no material \"" + chosen + "\"";
        if (const std::string_view near = closest_match(chosen, names); !near.empty()) {
            message += " (did you mean \"" + std::string(near) + "\"?)";
        }
        return make_error(ErrorCode::NotFound, message);
    }
    const json::Value* nodes = found->find("nodes");
    const json::Value* links = found->find("links");
    if (nodes == nullptr || !nodes->is_object() || links == nullptr || !links->is_array()) {
        return make_error(ErrorCode::InvalidArgument,
                          "material \"" + chosen + "\" has no nodes and links");
    }
    Converter converter(*nodes, *links);
    auto graph = converter.run(size);
    if (!graph) {
        return make_error(graph.error().code,
                          "material \"" + chosen + "\": " + graph.error().message);
    }
    return BlenderGraph{
        .graph = std::move(graph).value(), .material = chosen, .warnings = converter.warnings()};
}

} // namespace slash::procedural
