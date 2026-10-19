#pragma once

#include "slash/core/result.hpp"
#include "slash/render/shader.hpp"

#include <string>

// Compiles a game's own canvas shaders, as Godot compiles its canvas_item shaders. A shader is
// HLSL with a fragment() function, a vertex() function or both, which read and write Godot's
// built-ins as globals (COLOR, UV, VERTEX, TIME and the rest; see docs/reference/scene-format.md).
// The compiler declares the material's params by name and wraps the code in the canvas shader's
// entry points (engine/render/shaders/canvas.hlsl), then compiles it with glslang to SPIR-V and
// with SPIRV-Cross to MSL and GLSL ES, plus the modules the CPU runs with render::ShaderVm.
//
// slash-cli, slash-mcp and exports link it; games never do, and draw what their export compiled.
namespace slash::shader_compiler {

// The shader compiled for every backend, or every error in its code, one per line, each starting
// "<path>:<line>: ".
[[nodiscard]] Result<render::CompiledShader> compile(const render::ShaderSource& source);

// The HLSL compile wraps the code in: for the GPU's two stages (also what Direct3D 12 compiles),
// and for the CPU's vertex and fragment modules.
struct Wrapped {
    std::string gpu;
    std::string cpu_vertex;
    std::string cpu_fragment;
};
[[nodiscard]] Wrapped wrap(const render::ShaderSource& source);

// Makes render::compile_shader compile with compile, for the whole process.
void install();

} // namespace slash::shader_compiler
