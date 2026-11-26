#include "slash/command/builtin.hpp"
#include "slash/command/command.hpp"
#include "slash/command/session.hpp"
#include "slash/core/json.hpp"
#include "slash/platform/file.hpp"
#include "slash/procedural/blender_nodes.hpp"
#include "slash/procedural/texture_graph.hpp"
#include "slash/scene3d/blend.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"

#include <doctest/doctest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace command = slash::command;
namespace json = slash::json;
namespace procedural = slash::procedural;
namespace scene3d = slash::scene3d;
namespace world = slash::world;

namespace {

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE_MESSAGE(parsed.ok(), (parsed.ok() ? std::string() : parsed.error().message));
    return std::move(parsed).value();
}

// Two materials as Blender's node script writes them: a rusty metal whose color is a ramp over
// noise multiplied by a tint, roughness from Voronoi distance, a bump from the noise, and a
// Magic Texture the graph has no op for; and a plain one.
constexpr std::string_view kDump = R"({"format": 1, "blender": [4, 2], "materials": {
  "Rusty": {
    "nodes": {
      "Material Output": {"type": "ShaderNodeOutputMaterial", "inputs": {}, "outputs": {},
                          "props": {}},
      "Principled BSDF": {"type": "ShaderNodeBsdfPrincipled",
                          "inputs": {"Base Color": [0.8, 0.8, 0.8, 1], "Roughness": 0.5,
                                     "Metallic": 0.25, "Emission Color": [0, 0, 0, 1],
                                     "Emission Strength": 0, "Normal": [0, 0, 0]},
                          "outputs": {}, "props": {}},
      "Noise Texture": {"type": "ShaderNodeTexNoise",
                        "inputs": {"Scale": 8, "Detail": 3, "Roughness": 0.6,
                                   "Lacunarity": 2, "Distortion": 0, "Vector": [0, 0, 0]},
                        "outputs": {}, "props": {"noise_dimensions": "3D"}},
      "Color Ramp": {"type": "ShaderNodeValToRGB", "inputs": {"Fac": 0.5}, "outputs": {},
                     "props": {},
                     "ramp": {"interpolation": "LINEAR", "elements": [
                       {"position": 0.3, "color": [0.05, 0.02, 0.01, 1]},
                       {"position": 0.7, "color": [0.4, 0.12, 0.03, 1]}]}},
      "Tint": {"type": "ShaderNodeRGB", "inputs": {}, "outputs": {"Color": [0.214, 0.214, 0.214, 1]},
               "props": {}},
      "Mix": {"type": "ShaderNodeMix",
              "inputs": {"Factor_Float": 1, "A_Color": [1, 1, 1, 1], "B_Color": [1, 1, 1, 1]},
              "outputs": {}, "props": {"data_type": "RGBA", "blend_type": "MULTIPLY"}},
      "Voronoi Texture": {"type": "ShaderNodeTexVoronoi",
                          "inputs": {"Scale": 12, "Randomness": 0.8},
                          "outputs": {}, "props": {"feature": "F1", "distance": "EUCLIDEAN"}},
      "Math": {"type": "ShaderNodeMath", "inputs": {"Value": 0.5, "Value_001": 0.8},
               "outputs": {}, "props": {"operation": "MULTIPLY", "use_clamp": false}},
      "Bump": {"type": "ShaderNodeBump", "inputs": {"Strength": 0.4, "Distance": 1,
                                                    "Height": 1},
               "outputs": {}, "props": {}},
      "Magic Texture": {"type": "ShaderNodeTexMagic", "inputs": {"Scale": 5}, "outputs": {},
                        "props": {}},
      "Mix.001": {"type": "ShaderNodeMixRGB",
                  "inputs": {"Fac": 0.25, "Color1": [1, 1, 1, 1], "Color2": [1, 1, 1, 1]},
                  "outputs": {}, "props": {"blend_type": "MIX"}}
    },
    "links": [
      {"from": "Principled BSDF", "from_socket": "BSDF", "to": "Material Output",
       "to_socket": "Surface"},
      {"from": "Noise Texture", "from_socket": "Fac", "to": "Color Ramp", "to_socket": "Fac"},
      {"from": "Color Ramp", "from_socket": "Color", "to": "Mix", "to_socket": "A_Color"},
      {"from": "Tint", "from_socket": "Color", "to": "Mix", "to_socket": "B_Color"},
      {"from": "Mix", "from_socket": "Result_Color", "to": "Mix.001", "to_socket": "Color1"},
      {"from": "Magic Texture", "from_socket": "Color", "to": "Mix.001", "to_socket": "Color2"},
      {"from": "Mix.001", "from_socket": "Color", "to": "Principled BSDF",
       "to_socket": "Base Color"},
      {"from": "Voronoi Texture", "from_socket": "Distance", "to": "Math", "to_socket": "Value"},
      {"from": "Math", "from_socket": "Value", "to": "Principled BSDF", "to_socket": "Roughness"},
      {"from": "Noise Texture", "from_socket": "Fac", "to": "Bump", "to_socket": "Height"},
      {"from": "Bump", "from_socket": "Normal", "to": "Principled BSDF", "to_socket": "Normal"}
    ]
  },
  "Plain": {
    "nodes": {
      "Principled BSDF": {"type": "ShaderNodeBsdfPrincipled",
                          "inputs": {"Base Color": [0.214, 0.0, 0.0, 1], "Roughness": 0.3,
                                     "Metallic": 0},
                          "outputs": {}, "props": {}}
    },
    "links": []
  }
}})";

struct Engine {
    world::ComponentRegistry components;
    command::Registry commands;
    std::unique_ptr<command::Session> session;

    explicit Engine(std::string root) {
        world::register_builtin_components(components);
        command::register_builtin_commands(commands);
        command::SessionOptions options;
        options.project_root = std::move(root);
        options.capture_logs = false;
        options.capabilities = {command::Capability::Read, command::Capability::Edit,
                                command::Capability::Filesystem};
        session = std::make_unique<command::Session>(components, commands, std::move(options));
    }

    command::Reply call(std::string_view name, std::string_view params) {
        return session->call(name, parse(params));
    }
};

} // namespace

TEST_CASE("Blender shader nodes become a texture graph") {
    const json::Value dump = parse(kDump);
    CHECK(procedural::blender_materials(dump) == std::vector<std::string>{"Plain", "Rusty"});
    auto made = procedural::texture_graph_from_blender(dump, "Rusty", 32);
    REQUIRE_MESSAGE(made.ok(), (made.ok() ? std::string() : made.error().message));
    const json::Value& graph = made.value().graph;
    CHECK(made.value().material == "Rusty");
    CHECK(graph.find("width")->as_int() == 32);
    // The outputs the Principled BSDF reads: no emission, since it is black.
    const json::Array& outputs = graph.find("outputs")->as_array();
    REQUIRE(outputs.size() == 4);
    CHECK(outputs[0].find("name")->as_string() == "albedo");
    CHECK(outputs[1].find("name")->as_string() == "roughness");
    CHECK(outputs[1].find("as")->as_string() == "gray");
    CHECK(outputs[3].find("name")->as_string() == "normal");
    const json::Value& nodes = *graph.find("nodes");
    // The noise, read twice (by the ramp and the bump), is one node, its Scale across the
    // texture and its Detail as octaves.
    const json::Value& noise = *nodes.find("noise_texture");
    CHECK(noise.find("op")->as_string() == "noise");
    CHECK(*noise.find("scale") == parse("[8, 8]"));
    CHECK(noise.find("noise")->find("fractal_octaves")->as_int() == 4);
    CHECK(nodes.find("noise_texture_2") == nullptr);
    CHECK(nodes.find("color_ramp")->find("input")->as_string() == "noise_texture");
    // Blender's colors are scene-linear; the graph's are sRGB (0.214 linear is about 0.5).
    const json::Value& tint = *nodes.find(nodes.find("mix")->find("layer")->as_string());
    CHECK(tint.find("color")->as_array()[0].as_double() == doctest::Approx(0.5).epsilon(0.01));
    CHECK(nodes.find("mix")->find("mode")->as_string() == "multiply");
    CHECK(nodes.find("mix_001")->find("opacity")->as_double() == doctest::Approx(0.25));
    CHECK(nodes.find("math")->find("value")->as_double() == doctest::Approx(0.8));
    CHECK(nodes.find("voronoi_texture")->find("noise")->find("cellular_return_type")->as_string() ==
          "distance");
    CHECK(nodes.find("bump")->find("op")->as_string() == "normal_from_height");
    // What didn't carry over: the Magic Texture.
    REQUIRE(made.value().warnings.size() == 1);
    CHECK(made.value().warnings[0].find("Magic Texture") != std::string::npos);
    // The graph it makes checks and compiles.
    CHECK(procedural::validate_texture_graph(graph).empty());

    // With two materials, one must be named; a wrong name names the nearest.
    CHECK_FALSE(procedural::texture_graph_from_blender(dump, "", 32).ok());
    auto wrong = procedural::texture_graph_from_blender(dump, "Rusti", 32);
    REQUIRE_FALSE(wrong.ok());
    CHECK(wrong.error().message.find("\"Rusty\"") != std::string::npos);

    // Blender's script opens the file and writes every material's nodes.
    const std::string script = scene3d::blend_nodes_script("props/crate.blend", "out.json");
    CHECK(script.find("open_mainfile") != std::string::npos);
    CHECK(script.find("'props/crate.blend'") != std::string::npos);
    CHECK(script.find("color_ramp") != std::string::npos);
}

TEST_CASE("texture.from_blender writes a material's nodes as a graph") {
    const std::string root = "blender_nodes_project";
    std::filesystem::remove_all(root);
    REQUIRE(slash::platform::create_directories(root + "/textures").ok());
    REQUIRE(slash::platform::write_file(root + "/textures/rusty.nodes.json", kDump).ok());
    Engine engine(root);
    const command::Reply made = engine.call("texture.from_blender", R"({
        "nodes": "textures/rusty.nodes.json", "material": "Plain",
        "out": "textures/plain.texture.json", "size": 16})");
    REQUIRE_MESSAGE(made.ok, made.message);
    CHECK(made.value.find("material")->as_string() == "Plain");
    CHECK(*made.value.find("materials") == parse(R"(["Plain", "Rusty"])"));
    // Measured as texture.edit measures it: the red base color, as sRGB.
    const json::Value* albedo = made.value.find("outputs")->find("albedo");
    REQUIRE(albedo != nullptr);
    CHECK(albedo->find("r")->as_double() == doctest::Approx(0.5).epsilon(0.02));
    CHECK(std::filesystem::exists(root + "/textures/plain.texture.json"));

    // A wrong material names the nearest; running Blender needs the build capability.
    const command::Reply wrong = engine.call("texture.from_blender", R"({
        "nodes": "textures/rusty.nodes.json", "material": "Plian",
        "out": "textures/plain.texture.json"})");
    CHECK_FALSE(wrong.ok);
    CHECK(wrong.message.find("\"Plain\"") != std::string::npos);
    const command::Reply denied = engine.call("texture.from_blender", R"({
        "blend": "textures/rusty.blend", "out": "textures/rusty.texture.json"})");
    CHECK(denied.code == command::code::kPermissionDenied);
    CHECK_FALSE(engine.call("texture.from_blender", R"({"out": "textures/x.texture.json"})").ok);
    std::filesystem::remove_all(root);
}
