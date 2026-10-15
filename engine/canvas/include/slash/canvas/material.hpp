#pragma once

#include "slash/core/json.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/core/result.hpp"
#include "slash/render/device.hpp"
#include "slash/render/material.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// .material.json files: canvas materials, Godot's ShaderMaterial for 2D written as a node graph.
// Sprite2D's "material" names one.
//
//   {
//     "blend": "mix",
//     "color": {"op": "mix", "args": ["base", "flash_color", "flash"]},
//     "format": 1,
//     "nodes": {
//       "base": {"op": "mul", "args": [{"op": "texture", "args": ["texture", "uv"]}, "color"]}
//     },
//     "params": {
//       "flash": {"type": "float", "value": 0, "min": 0, "max": 1},
//       "flash_color": {"type": "color", "value": [1, 1, 1, 1]}
//     }
//   }
//
// "color" is the pixel's color, a vec4. Left out, it is the texture times the vertex color, as
// a sprite with no material draws. An arg is a number, an array of 2 to 4 numbers, a node
// written in place, or a name: a node in "nodes", a param, or an input. Inputs, as in Godot's
// canvas shaders: uv, color (the vertex color, Sprite2D's modulate), time, texture (the
// sprite's), texture_pixel_size and fragcoord. A name can pick components, as in "base.rgb" or
// "uv.yx". The graph compiles to a material program (slash/render/material.hpp).
namespace slash::canvas {

inline constexpr std::string_view kMaterialSuffix = ".material.json";

// Godot's TIME wraps every hour, which keeps it precise in a float.
inline constexpr double kMaterialTimeWrap = 3600.0;

struct MaterialParam {
    std::string type = "float";     // "float", "vec2", "vec3", "vec4", "color" or "texture"
    std::array<float, 4> value{};   // a number param's value; a float fills every component
    std::string texture;            // a texture param's project path; "" samples white
    bool repeat = false;            // textures: tile instead of stretching the edges
    std::string filter = "inherit"; // textures: "inherit", "linear" or "nearest"
    std::optional<double> min;      // numbers: the smallest value allowed in each component
    std::optional<double> max;
    // Where the program reads it: a constant, or a texture's slot (1 to 3). None when the
    // graph doesn't use it.
    std::optional<std::uint32_t> slot;
};

struct CanvasMaterial {
    render::BlendMode blend = render::BlendMode::Mix;
    std::map<std::string, MaterialParam, std::less<>> params;
    // The graph, compiled. Its constants hold the params' values. No code means no graph:
    // the texture times the vertex color.
    render::MaterialProgram program;
    // The constant that holds the time, when the graph reads it.
    std::optional<std::uint32_t> time_constant;
};

// JSON Schema for .material.json files.
[[nodiscard]] json::Value material_schema();

// Every error in a .material.json document, each with the JSON Pointer of the bad value: the
// schema's, then names, types and args the graph gets wrong, and the material shader's limits.
[[nodiscard]] std::vector<json::ValidationError> validate_material(const json::Value& doc);

// The compiled material in a .material.json document, or its first error.
[[nodiscard]] Result<CanvasMaterial> parse_material(const json::Value& doc);

// Reads a param's value as material files and Sprite2D's material_params write it: a number, an
// array of numbers or a texture path, by the param's type. The error says what was expected.
[[nodiscard]] Result<void> read_param_value(const json::Value& value, MaterialParam& param);

// A texture a material samples in slot 1, 2 or 3.
struct MaterialTextureRef {
    std::string path; // "" samples white
    bool repeat = false;
    std::string filter = "inherit";
};

// A material ready to draw one sprite: its params' values and the time filled in.
struct MaterialInstance {
    render::BlendMode blend = render::BlendMode::Mix;
    render::MaterialProgram program;
    std::array<MaterialTextureRef, render::kMaterialTextureSlots - 1> textures;
};

// The material with params overridden, as Sprite2D's material_params, at a time in seconds.
// Fails on a param the material doesn't have, with "did you mean", or a value of the wrong type
// or outside the param's range, and names that param in failed_param when given.
[[nodiscard]] Result<MaterialInstance> instantiate(const CanvasMaterial& material,
                                                   const std::map<std::string, json::Value>& params,
                                                   double time,
                                                   std::string* failed_param = nullptr);

// What a pixel of a sprite starts with, for evaluate.
struct PixelInputs {
    std::array<float, 4> color{1.0F, 1.0F, 1.0F, 1.0F}; // the vertex color
    std::array<float, 2> frag_coord{};
};

// A texture's image by project path, or the error loading it.
using ImageSource = std::function<Result<const render::Image*>(const std::string& path)>;

// The colors a sprite draws at each texture coordinate before blending, from 0 to 1, computed
// on the CPU as the GPU computes them. texture is the sprite's own texture ("" for none, which
// is white), sampled with filter, as are texture params that "inherit".
[[nodiscard]] Result<std::vector<std::array<float, 4>>>
evaluate(const MaterialInstance& instance, const std::string& texture, render::Filter filter,
         const PixelInputs& inputs, std::span<const std::array<float, 2>> uvs,
         const ImageSource& images);

} // namespace slash::canvas
