#pragma once

#include "slash/core/json.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/core/result.hpp"
#include "slash/procedural/texture_graph.hpp"
#include "slash/render/device.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace slash::jobs {
class Pool;
}

// Paint stacks: a .paint.json file is a model's (or a sprite's) texturing as layers, bottom to
// top, after Substance Painter's layer stacks, that agents write as text. Each layer is a
// material (flat values, a texture graph, decals) laid over the channels below it through a
// mask: mesh maps over thresholds (curvature for worn edges, ambient occlusion for dirt, the
// world normal for dust), part and material names, noise, a graph, strokes along points on the
// model and decals projected onto it. A stack with no model or sprite is a smart material,
// which others start from with "extends".
//
//   {
//     "format": 1,
//     "type": "paint",
//     "model": "props/chest.obj",
//     "size": 1024,
//     "parameters": {"wear": 0.5},
//     "layers": [
//       {"name": "wood", "fill": {"albedo": [0.45, 0.3, 0.18], "roughness": 0.7}},
//       {"name": "iron", "fill": {"albedo": [0.3, 0.3, 0.32], "metallic": 1, "roughness": 0.4},
//        "mask": {"part": "bands"}},
//       {"name": "worn", "fill": {"albedo": [0.7, 0.7, 0.72], "roughness": 0.25},
//        "mask": [{"part": "bands"}, {"map": "curvature", "above": "=0.75 - wear * 0.2",
//                                    "breakup": {"amount": 0.1, "scale": 12}}]}
//     ]
//   }
//
// A stack compiles to a texture graph (slash/procedural/texture_graph.hpp) whose images are the
// mesh maps and the masks worked out on the CPU (strokes, decals, names, noise in the model's
// space), so it bakes in a compute pass on a device, or with the CPU twin, as graphs do.
namespace slash::procedural {

inline constexpr std::string_view kPaintSuffix = ".paint.json";
// Paths of the smart materials the engine ships, for "extends".
inline constexpr std::string_view kBuiltinPaintPrefix = "builtin:paint/";

// The channels a stack paints, in the order they are listed.
inline constexpr std::array<std::string_view, 7> kPaintChannels{
    "albedo", "roughness", "metallic", "ao", "height", "normal", "emission"};
// The textures a stack can bake: its channels, and orm (ao, roughness and metallic in red,
// green and blue).
inline constexpr std::array<std::string_view, 8> kPaintOutputs{
    "albedo", "orm", "normal", "roughness", "metallic", "ao", "height", "emission"};
// The maps a mask reads by name.
inline constexpr std::array<std::string_view, 9> kPaintMaps{
    "ao", "curvature", "thickness", "sky", "height", "world_normal", "position", "noise", "alpha"};

// Whether a document is a paint stack ("type": "paint").
[[nodiscard]] bool is_paint_stack(const json::Value& doc);

// JSON Schema for a paint stack.
[[nodiscard]] json::Value paint_schema();

// Every error in a stack document, each with the JSON Pointer of the bad value: the schema's,
// layers named twice, masks reading maps a sprite or a stack without a high model lacks, and
// fields that only models or only sprites take. Files it names are not opened.
[[nodiscard]] std::vector<json::ValidationError> validate_paint(const json::Value& doc);

// The names of the smart materials the engine ships ("builtin:paint/<name>"), and one's text.
[[nodiscard]] std::vector<std::string_view> builtin_paint_names();
[[nodiscard]] std::optional<std::string_view> builtin_paint(std::string_view path);

// The stack with what it extends merged under it: objects merge as a JSON merge patch does,
// and layers merge by name, a layer of the same name changing the one it extends in place and
// the others going on top. reader.text reads the files; builtin paths need no reader.
[[nodiscard]] Result<json::Value> resolve_paint(const json::Value& doc, const GraphReader& reader,
                                                std::vector<std::string>* files = nullptr);

// The mesh maps a resolved stack's masks, fills and bases read from its model: ao, curvature,
// thickness, sky, height and normal (the baked normal map).
[[nodiscard]] std::vector<std::string> paint_needs(const json::Value& resolved);

// What a stack paints on, each texel's facts as floats, width x height, rows from the top.
struct PaintSurface {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    bool sprite = false;
    // Whether a texel is painted: covered by a triangle or padding (or, on a sprite, every
    // texel); 1 where it counts toward stats (a triangle covers it, or a sprite's alpha is over
    // half), 2 where it is painted but not counted, 0 elsewhere.
    std::vector<std::uint8_t> covered;
    // Gray maps by name (ao, curvature, thickness, sky, height): 0 to 1 a texel.
    std::map<std::string, std::vector<float>, std::less<>> maps;
    // The baked normal map (RGB, 0 to 1), the sprite's colors (RGBA), or empty.
    std::vector<float> normal_map;
    std::vector<float> sprite_colors;
    // Each texel's place (the model's units, or a sprite's pixels) and unit normal: 3 floats.
    std::vector<float> position;
    std::vector<float> normal;
    // The place's box, which "position" masks read from 0 to 1.
    std::array<float, 3> min{};
    std::array<float, 3> max{};
    std::vector<std::int32_t> part;
    std::vector<std::int32_t> material;
    std::vector<std::string> part_names;
    std::vector<std::string> material_names;
};

// A sprite's surface: its colors, its alpha, its normal map's normals when given, and its
// outline's edge as curvature (1 at the edge, falling to 0.5 edge_width pixels in). Places are
// pixels; texels over half opaque count toward stats.
[[nodiscard]] Result<PaintSurface> paint_sprite_surface(const render::Image& sprite,
                                                        const render::Image* normal_map,
                                                        float edge_width);

// A stack compiled: the texture graph it bakes as, its images, and what its outputs are.
struct CompiledPaint {
    json::Value graph;                    // the texture graph document, for agents to read
    CompiledGraph program;                // its compiled form, images in place
    std::vector<std::string> outputs;     // the textures, in program.outputs' order first
    std::vector<std::string> mask_layers; // then each masked layer's mask, as "mask.<layer>"
    std::vector<std::string> files;       // the files it read
};

// Compiles a resolved stack over a surface. reader reads the layers' texture graphs and decal
// images; overrides replace parameters' values by name. Errors point into the stack.
[[nodiscard]] Result<CompiledPaint>
compile_paint(const json::Value& resolved, const PaintSurface& surface, const GraphReader& reader,
              const json::Object& overrides = {}, jobs::Pool* pool = nullptr);

// Measures of one baked texture over the surface's counted texels, each from 0 to 1: its mean
// luminance (value), each channel's mean (r, g, b, a), the darkest and brightest luminance
// (low, high) and the luminance's standard deviation (contrast).
[[nodiscard]] json::Value measure_paint(const render::Image& image, const PaintSurface& surface);

} // namespace slash::procedural
