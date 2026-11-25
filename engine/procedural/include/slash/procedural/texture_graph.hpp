#pragma once

#include "slash/core/json.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/core/result.hpp"
#include "slash/render/device.hpp"
#include "slash/render/shader.hpp"
#include "slash/render/texture_graph.hpp"

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace slash::jobs {
class Pool;
}

// Texture graphs: a .texture.json file of "type": "graph" is a graph of nodes over whole images,
// after Substance Designer, Unreal's Texture Graph and Houdini's COPs, that agents write and
// tune as text. It compiles to a render::TextureGraphProgram, which a render device bakes in a
// compute pass (RenderDevice::bake_texture_graph) or the CPU twin runs, and each of its outputs
// is a texture: "wall.texture.json" is the first, "wall.texture.json::normal" the one named so.
//
//   {
//     "format": 1,
//     "type": "graph",
//     "width": 512, "height": 512, "tiling": true,
//     "parameters": {"mortar": 0.06, "tint": [0.62, 0.3, 0.22, 1]},
//     "nodes": {
//       "bricks": {"op": "bricks", "across": 4, "down": 8, "mortar": "$mortar"},
//       "grain": {"op": "noise", "noise": {"frequency": 0.02}},
//       "height": {"op": "blend", "base": "bricks", "layer": "grain", "mode": "multiply"},
//       "color": {"op": "gradient_map", "input": "height", "gradient": {...}},
//       "normal": {"op": "normal_from_height", "height": "height", "strength": 4}
//     },
//     "outputs": [{"name": "albedo", "node": "color"}, {"name": "normal", "node": "normal"}]
//   }
//
// A setting is a value, "$name" (a parameter) or "=" and an expression over the parameters
// ("=mortar * 2").
// this one is a JSON merge patch over. Every op and its settings are in texture_graph_schema().
namespace slash::procedural {

// Reads what a graph names: another graph's text ("extends") and image files ("image" nodes).
// Paths are the project's, as a scene names them.
struct GraphReader {
    std::function<Result<std::string>(std::string_view path)> text;
    std::function<Result<render::Image>(std::string_view path)> image;
};

// One output of a compiled graph: its name, the slot holding it and how it becomes RGBA8.
struct GraphOutput {
    std::string name;
    std::uint32_t slot = 0;
    // Gray outputs take the slot's red into every color channel, with alpha 1.
    bool gray = false;
};

struct CompiledGraph {
    render::TextureGraphProgram program;
    std::vector<GraphOutput> outputs;
    // The files the graph read: the ones it extends and its images.
    std::vector<std::string> files;
};

// Whether a .texture.json document is a graph ("type": "graph").
[[nodiscard]] bool is_texture_graph(const json::Value& doc);

// JSON Schema for a graph document: its fields and, under "$defs", each op's node.
[[nodiscard]] json::Value texture_graph_schema();

// Every error in a graph document, each with the JSON Pointer of the bad value: unknown ops and
// nodes (with the closest name), settings out of range, references to missing parameters, links
// to missing nodes and cycles. Files it extends or reads are not opened.
[[nodiscard]] std::vector<json::ValidationError> validate_texture_graph(const json::Value& doc);

// The program a graph document compiles to, with parameters' values replaced by those in
// overrides (by name; each must be a parameter's and of its kind). Only the nodes the outputs
// need run, in an order where each reads what ran before, and slots are shared by nodes whose
// values aren't needed at once.
[[nodiscard]] Result<CompiledGraph> compile_texture_graph(const json::Value& doc,
                                                          const GraphReader& reader,
                                                          const json::Object& overrides = {});

// What a graph is made of, after what it extends is merged under it, for agents to read before
// they edit it: its size and tiling, the files it extends, each parameter's value (with its
// expression and the nodes reading it), each node's op, the nodes it reads and the parameters it
// uses, each output's node, kind and the nodes it needs in the order they run, the nodes no
// output needs ("unused") and its "expect". Fails with the first error a compile would.
[[nodiscard]] Result<json::Value> describe_texture_graph(const json::Value& doc,
                                                         const GraphReader& reader);

// Bakes a compiled graph's outputs as RGBA8 images, in its outputs' order: on device when it is
// given (RenderDevice::bake_texture_graph), else with the CPU twin on pool.
[[nodiscard]] Result<std::vector<render::Image>> bake_texture_graph(const CompiledGraph& graph,
                                                                    render::RenderDevice* device,
                                                                    jobs::Pool* pool = nullptr);

// One output's texels as RGBA8, rounded to the nearest step.
[[nodiscard]] render::Image graph_texels_image(const render::GraphTexels& texels,
                                               std::uint32_t width, std::uint32_t height,
                                               bool gray);

// A graph file's output by name ("" for the first), baked: the image a texture path names.
[[nodiscard]] Result<render::Image>
texture_graph_image(const json::Value& doc, std::string_view output, const GraphReader& reader,
                    render::RenderDevice* device = nullptr, jobs::Pool* pool = nullptr);

// The most node pixels a live graph may work out for each pixel it draws: each output filters
// four texels, and each texel runs its node and, at each pixel its op reads, the nodes before.
inline constexpr std::int64_t kMaxLiveGraphCost = 2048;

// A game shader's code (render::ShaderSource::code) whose fragment() works a graph out as it
// draws, per pixel, instead of sampling a baked texture: a material whose "shader" names a
// .texture.json graph. Its params are the material's: a number or array param named as one of
// the graph's parameters moves it as the material draws (an animated material_params value
// flows), and a graph parameter named "time" reads TIME unless the material has a "time" param.
// Settings must follow moved parameters in a straight line, and moving them must leave the
// nodes that run and their links as they are. A canvas_item shader multiplies COLOR by the
// first output; a spatial one sets ALBEDO, NORMAL_MAP, ROUGHNESS, METALLIC, AO, EMISSION or
// ALPHA from outputs named albedo (or color), normal, roughness, metallic, ao, emission and alpha,
// and orm sets AO, ROUGHNESS and METALLIC from red, green and blue. Each output is filtered as
// a texture of the graph's size, so at texel centers it draws the baked texels. Fails for image
// nodes, compositor shaders and outputs costing more than kMaxLiveGraphCost. The code includes
// shaders/texture_graph_common.hlsl and texture_graph_ops.hlsl by name, which the shader tools'
// compiler has built in.
[[nodiscard]] Result<std::string>
texture_graph_shader(const json::Value& doc, const GraphReader& reader, render::ShaderType type,
                     std::span<const render::ShaderSource::Param> params = {});

// Sets the device that bakes graphs loaded as assets (canvas's .texture.json loader); null
// bakes them with the CPU twin. A renderer sets its device while it lives and clears it after.
void set_graph_device(render::RenderDevice* device);
[[nodiscard]] render::RenderDevice* graph_device();

} // namespace slash::procedural
