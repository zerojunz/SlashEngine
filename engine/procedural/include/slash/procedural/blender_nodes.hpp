#pragma once

#include "slash/core/json.hpp"
#include "slash/core/result.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Blender shader nodes as texture graphs. The engine never reads a .blend file: the artist's own
// Blender writes a material's node tree as JSON (scene3d::blend_nodes_script), and this turns it
// into a .texture.json graph whose outputs (albedo, roughness, metallic, normal, emission) fill a
// standard material or a layer stack's layer. Nodes the graph has no op for are left out, each
// named in a warning.
namespace slash::procedural {

struct BlenderGraph {
    json::Value graph;                 // a .texture.json document of "type": "graph"
    std::string material;              // the material it came from
    std::vector<std::string> warnings; // what didn't carry over, one line each
};

// The graph of a material in a node dump (all of them when material is empty and the dump has
// one), at size x size pixels. A material that isn't there names the nearest.
[[nodiscard]] Result<BlenderGraph>
texture_graph_from_blender(const json::Value& dump, std::string_view material, std::uint32_t size);

// The materials a node dump holds, by name.
[[nodiscard]] std::vector<std::string> blender_materials(const json::Value& dump);

} // namespace slash::procedural
