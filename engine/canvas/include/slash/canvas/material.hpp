#pragma once

#include "slash/core/json.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/core/result.hpp"
#include "slash/render/device.hpp"
#include "slash/render/material.hpp"
#include "slash/render/shader.hpp"
#include "slash/world/world.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
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
//
// Instead of a graph, "shader" can name a game's own shader, an .hlsl file with fragment() and
// vertex() functions as Godot's canvas_item shaders (see docs/reference/scene-format.md):
//
//   {"format": 1, "params": {"amount": {"type": "float", "value": 0.5}},
//    "shader": "shaders/dissolve.hlsl"}
//
// The shader tools compile it for every backend (render::compile_shader), and exports write the
// compiled code into the file as "compiled", since games can't compile shaders.
namespace slash::canvas {

inline constexpr std::string_view kMaterialSuffix = ".material.json";
inline constexpr std::string_view kShaderSuffix = ".hlsl";

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
    // graph doesn't use it. A shader's params all have one, in name order: numbers from
    // constant 0, textures from slot 1.
    std::optional<std::uint32_t> slot;
};

struct CanvasMaterial {
    render::BlendMode blend = render::BlendMode::Mix;
    render::LightMode light_mode = render::LightMode::Normal; // how 2D lights shade it
    std::map<std::string, MaterialParam, std::less<>> params;
    // The graph, compiled. Its constants hold the params' values. No code means no graph:
    // the texture times the vertex color.
    render::MaterialProgram program;
    // The constant that holds the time, when the graph reads it; a shader's is
    // render::kShaderTimeConstant.
    std::optional<std::uint32_t> time_constant;
    // A game's shader instead of a graph: the .hlsl file's project path.
    std::string shader;
    // The shader compiled, when the file holds it ("compiled", which exports write); otherwise
    // compile_material_shader compiles the .hlsl file.
    std::shared_ptr<const render::CompiledShader> compiled;
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

// What a shader material's shader is compiled from: the .hlsl file's code, with the params it
// declares for it.
[[nodiscard]] render::ShaderSource shader_source(const CanvasMaterial& material, std::string code);

// A shader material's shader, compiled: its "compiled" when the file holds one, else its .hlsl
// file read with read and compiled with render::compile_shader. The error lists every error in
// the code, one per line, each starting "<file>:<line>: ".
[[nodiscard]] Result<std::shared_ptr<const render::CompiledShader>>
compile_material_shader(const CanvasMaterial& material, const world::FileReader& read);

// Every error compiling the shader a valid .material.json document names, each at "/shader";
// none for a graph material. Validation and exports check this; it needs a shader compiler
// (render::has_shader_compiler).
[[nodiscard]] std::vector<json::ValidationError>
check_material_shader(const json::Value& doc, const world::FileReader& read);

// The backends whose code compiled_shader_json writes. The CPU's modules are always written.
struct ShaderTargets {
    bool spirv = true; // Vulkan
    bool msl = true;   // Metal
    bool glsl = true;  // OpenGL ES and WebGL 2
    bool hlsl = true;  // Direct3D 12
};

// A compiled shader as a material file's "compiled", for exports to write.
[[nodiscard]] json::Value compiled_shader_json(const render::CompiledShader& shader,
                                               const ShaderTargets& targets = {});

// A texture a material samples in slot 1, 2 or 3.
struct MaterialTextureRef {
    std::string path; // "" samples white
    bool repeat = false;
    std::string filter = "inherit";
};

// A material ready to draw one sprite: its params' values and the time filled in.
struct MaterialInstance {
    render::BlendMode blend = render::BlendMode::Mix;
    render::LightMode light_mode = render::LightMode::Normal;
    render::MaterialProgram program;
    std::array<MaterialTextureRef, render::kMaterialTextureSlots - 1> textures;
    // A shader material's shader file, and the shader compiled, which evaluate runs: instantiate
    // copies the material's "compiled", and callers fill it from compile_material_shader
    // otherwise.
    std::string shader;
    std::shared_ptr<const render::CompiledShader> compiled;
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
// is white), sampled with filter, as are texture params that "inherit". A shader runs its
// fragment() at each, with no vertex() and derivatives of 0.
[[nodiscard]] Result<std::vector<std::array<float, 4>>>
evaluate(const MaterialInstance& instance, const std::string& texture, render::Filter filter,
         const PixelInputs& inputs, std::span<const std::array<float, 2>> uvs,
         const ImageSource& images);

} // namespace slash::canvas
