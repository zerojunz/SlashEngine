#pragma once

#include "slash/core/result.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// A game's own canvas shaders: Godot's canvas_item shaders, written in HLSL with fragment() and
// vertex() functions. The engine's shader tools (slash-cli, slash-mcp and exports) compile one
// into a CompiledShader for every backend, which RenderDevice::create_shader takes, and the CPU
// runs it with ShaderVm: the software device draws with it, so frame captures show a game's
// shaders on any machine, and material.eval reads its colors.
namespace slash::render {

// A shader made with RenderDevice::create_shader.
// NOLINTNEXTLINE(performance-enum-size): ids count up, past 255
enum class ShaderId : std::uint32_t { None = 0 };

// A game's shader compiled for every backend: the material shader's vs_main and fs_main with the
// game's code in them, as SPIR-V for Vulkan, MSL for Metal, GLSL ES 3.00 for OpenGL ES and WebGL
// 2, WGSL for WebGPU, and HLSL, which Direct3D 12 compiles when the shader is made.
struct CompiledShader {
    std::vector<std::uint32_t> spirv_vertex;
    std::vector<std::uint32_t> spirv_fragment;
    std::string msl_vertex;
    std::string msl_fragment;
    std::string glsl_vertex;
    std::string glsl_fragment;
    std::string wgsl_vertex;
    std::string wgsl_fragment;
    std::string hlsl;
    // SPIR-V the CPU runs with ShaderVm: the game's vertex(), if it has one, and its fragment().
    // See shaders/canvas.hlsl for what each reads and writes.
    std::vector<std::uint32_t> cpu_vertex;
    std::vector<std::uint32_t> cpu_fragment;
    // The shader uses NORMAL_MAP or SPECULAR_SHININESS, so lights use them even with no normal or
    // specular map, as Godot's do.
    bool normal_map_used = false;
    bool specular_used = false;

    bool operator==(const CompiledShader&) const = default;
};

// The constant a game's shader reads the time from, in x: the last of a material's constants.
// Number params take the ones before it, so a shader has at most 31.
inline constexpr std::uint32_t kShaderTimeConstant = 31;

// What a game's shader is compiled from: its code and the material params it reads, which the
// compiler declares for it.
struct ShaderSource {
    std::string path; // the shader file's project path, which errors name
    std::string code;
    struct Param {
        std::string name;
        // A number param's components, 1 to 4, read from constant slot; 0 for a texture param,
        // bound at texture slot (1 to 3).
        std::uint32_t components = 1;
        std::uint32_t slot = 0;

        bool operator==(const Param&) const = default;
    };
    std::vector<Param> params;

    bool operator==(const ShaderSource&) const = default;
};

// Whether a material param can't take this name in a game's shader: the code around the shader
// (shaders/canvas.hlsl and the shader tools' wrappers) declares it, or HLSL keeps it as a keyword,
// type or intrinsic function. A texture param's sampler is also declared, as name_sampler.
[[nodiscard]] bool shader_name_reserved(std::string_view name);

// Compiles a game's shader, or fails with every error in its code, one per line, each starting
// "<path>:<line>: ".
using ShaderCompiler = std::function<Result<CompiledShader>(const ShaderSource& source)>;

// Sets the compiler compile_shader uses, for the whole process; the shader tools install theirs
// (tools/shader_compiler). A game has none: its export compiles its shaders.
void set_shader_compiler(ShaderCompiler compiler);
[[nodiscard]] bool has_shader_compiler();

// The source compiled with the installed compiler. Results are kept by source, so compiling one
// again costs nothing. Fails when no compiler is installed.
[[nodiscard]] Result<std::shared_ptr<const CompiledShader>>
compile_shader(const ShaderSource& source);

// Where the CPU modules read their inputs and write their outputs, by location, and what their
// uniform buffer holds. Keep in step with shaders/canvas.hlsl and the shader tools' wrappers.
namespace cpu_shader {

// cpu_vertex reads VERTEX (xy), UV (xy) and COLOR, and writes them back at the same locations.
inline constexpr std::uint32_t kVertex = 0;
inline constexpr std::uint32_t kUv = 1;
inline constexpr std::uint32_t kColor = 2;
// cpu_fragment reads UV (xy) and the vertex color as above, then FRAGCOORD (xy), the draw's
// texture at UV, the normal map's texel and SPECULAR_SHININESS (the specular map's texel times
// the specular color, or white times it). It writes COLOR at 0, NORMAL_MAP (rgb) at 1 and
// SPECULAR_SHININESS at 2.
inline constexpr std::uint32_t kFragCoord = 3;
inline constexpr std::uint32_t kTextureColor = 4;
inline constexpr std::uint32_t kNormalTexel = 5;
inline constexpr std::uint32_t kSpecular = 6;
inline constexpr std::uint32_t kOutColor = 0;
inline constexpr std::uint32_t kOutNormalMap = 1;
inline constexpr std::uint32_t kOutSpecular = 2;
// Both read one uniform buffer, binding 0: a material's 32 constants (params, and the time in
// the last one's x), then one over the draw's texture size (xy).
inline constexpr std::size_t kConstants = 32;
inline constexpr std::size_t kUniformBytes = (kConstants + 1) * 16;

} // namespace cpu_shader

// Runs SPIR-V on the CPU, as a GPU runs it: the CPU twin of a game's shaders. It takes the part
// of SPIR-V that glslang makes from HLSL for pixel and vertex work (arithmetic, vectors and
// matrices, GLSL.std.450, branches, loops and calls, texture sampling, discard and derivatives),
// with inputs and outputs by location, uniform buffers and textures by binding. load refuses
// anything else, naming it.
//
// A run is up to four invocations. Four in a 2x2 quad, as GPUs shade pixels, take ddx and ddy as
// differences across it (a GPU's fine derivatives) until they branch apart; otherwise those are
// 0. Floating-point results may differ from a GPU's in the last bits.
class ShaderVm {
public:
    using Float4 = std::array<float, 4>;
    static constexpr std::size_t kLanes = 4;
    static constexpr std::size_t kLocations = 8;
    // A loop that runs longer than this many instructions stops, where a GPU would hang.
    static constexpr std::uint64_t kMaxSteps = std::uint64_t{1} << 22;

    // What the shader's textures read, by binding. sample takes a texture coordinate from 0 to 1
    // and filters as the texture's sampler does; fetch takes a texel and gives 0 outside; size is
    // the texture's width and height.
    struct Textures {
        std::function<Float4(std::uint32_t binding, float u, float v)> sample;
        std::function<Float4(std::uint32_t binding, std::int32_t x, std::int32_t y)> fetch;
        std::function<std::array<std::uint32_t, 2>(std::uint32_t binding)> size;
    };

    struct Lanes {
        std::array<std::array<Float4, kLocations>, kLanes> inputs{};
        std::array<std::array<Float4, kLocations>, kLanes> outputs{};
        std::uint32_t count = 1; // invocations, 1 to 4
        // The four are a quad: lanes 0 and 1 the top row, left to right, then 2 and 3.
        bool quad = false;
        std::array<bool, kLanes> discarded{};
    };

    // A module whose entry point the CPU can run. The error names what it can't.
    [[nodiscard]] static Result<ShaderVm> load(std::span<const std::uint32_t> words);

    // Runs the entry point. uniforms holds each uniform buffer's bytes by binding.
    void run(Lanes& lanes, std::span<const std::span<const std::byte>> uniforms,
             const Textures& textures);

    ShaderVm();
    ShaderVm(const ShaderVm& other);
    ShaderVm& operator=(const ShaderVm& other);
    ShaderVm(ShaderVm&&) noexcept;
    ShaderVm& operator=(ShaderVm&&) noexcept;
    ~ShaderVm();

    struct Module;

private:
    std::unique_ptr<Module> module_;
};

} // namespace slash::render
