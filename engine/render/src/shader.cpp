#include "slash/render/shader.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <list>
#include <mutex>
#include <string>
#include <utility>

namespace slash::render {

namespace {

// Compiles kept, newest first. A project has a few shaders; each kept compile is a few hundred
// kilobytes of code.
constexpr std::size_t kKeptCompiles = 32;

struct Compiled {
    ShaderSource source;
    Result<std::shared_ptr<const CompiledShader>> result;
};

struct Compilers {
    std::mutex mutex;
    ShaderCompiler compiler;
    std::list<Compiled> kept;
};

Compilers& compilers() {
    static Compilers instance;
    return instance;
}

// Lowercase names a param could take that the code around a game's shader declares: the globals
// and functions in shaders/canvas.hlsl, the wrappers' (tools/shader_compiler) and the names the
// GL device binds by, each between spaces. Keep in step with both; a test checks canvas.hlsl's.
constexpr std::string_view kTakenNames =
    " ambient atlas_sampler canvas_pixel code constants counts cpu_fragment cpu_vertex field "
    "fragment fs_main input layer light_atlas light_count light_normal light_pixel lights "
    "normal_map normal_sampler normal_used output pixel shade_canvas_pixel shading "
    "shadow_amount shadow_depth shadow_map shadow_sampler shine_used specular specular_map "
    "specular_sampler texel texture texture0 texture1 texture2 texture3 to_lights_x "
    "to_lights_y v_color v_uv v_world vertex vertex_constants vertex_texel view_projection "
    "vs_main ";

// HLSL's lowercase keywords, besides its numeric types, with those glslang keeps (GLSL's layout
// and C++'s).
constexpr std::string_view kKeywords =
    " asm auto break case catch cbuffer centroid char class column_major compile const "
    "const_cast continue default discard do else enum explicit export extern false for "
    "friend globallycoherent goto groupshared if in inline inout interface layout line "
    "lineadj linear long matrix mutable namespace new nointerpolation noperspective operator "
    "out packoffset pass point precise private protected public register reinterpret_cast "
    "return row_major sample sampler sampler_state shared short signed sizeof snorm static "
    "static_cast string struct switch tbuffer technique template this throw triangle "
    "triangleadj true try typedef typename uint64_t uniform union unorm unsigned using "
    "vector virtual void volatile while ";

// HLSL's intrinsic functions, which a param of the same name would hide, as Godot refuses
// built-in function names.
constexpr std::string_view kIntrinsics =
    " abs acos all any asdouble asfloat asin asint asuint atan atan2 ceil clamp clip cos "
    "cosh countbits cross ddx ddx_coarse ddx_fine ddy ddy_coarse ddy_fine degrees "
    "determinant distance dot dst exp exp2 f16tof32 f32tof16 faceforward firstbithigh "
    "firstbitlow floor fma fmod frac frexp fwidth isfinite isinf isnan ldexp length lerp lit "
    "log log10 log2 mad max min modf msad4 mul noise normalize pow radians rcp reflect "
    "refract reversebits round rsqrt saturate sign sin sincos sinh smoothstep sqrt step tan "
    "tanh transpose trunc ";

constexpr std::array<std::string_view, 11> kNumericTypes{
    "bool",       "double",   "dword",      "float",    "half",     "int",
    "min10float", "min12int", "min16float", "min16int", "min16uint"};

// A numeric type: float, float3 or float4x4, and the like for the others (and uint).
bool numeric_type(std::string_view name) {
    for (const std::string_view base : kNumericTypes) {
        for (const std::string_view type : {base, std::string_view(base == "int" ? "uint" : "")}) {
            if (type.empty() || !name.starts_with(type)) {
                continue;
            }
            const std::string_view rest = name.substr(type.size());
            const auto dimension = [](char c) { return c >= '1' && c <= '4'; };
            if (rest.empty() || (rest.size() == 1 && dimension(rest[0])) ||
                (rest.size() == 3 && dimension(rest[0]) && rest[1] == 'x' && dimension(rest[2]))) {
                return true;
            }
        }
    }
    return false;
}

} // namespace

bool shader_name_reserved(std::string_view name) {
    // GLSL keeps these for itself.
    if (name.starts_with("gl_") || name.find("__") != std::string_view::npos) {
        return true;
    }
    const std::string spaced = " " + std::string(name) + " ";
    return kTakenNames.find(spaced) != std::string_view::npos ||
           kKeywords.find(spaced) != std::string_view::npos ||
           kIntrinsics.find(spaced) != std::string_view::npos || numeric_type(name);
}

void set_shader_compiler(ShaderCompiler compiler) {
    Compilers& state = compilers();
    const std::scoped_lock lock(state.mutex);
    state.compiler = std::move(compiler);
    state.kept.clear();
}

bool has_shader_compiler() {
    Compilers& state = compilers();
    const std::scoped_lock lock(state.mutex);
    return static_cast<bool>(state.compiler);
}

Result<std::shared_ptr<const CompiledShader>> compile_shader(const ShaderSource& source) {
    Compilers& state = compilers();
    ShaderCompiler compiler;
    {
        const std::scoped_lock lock(state.mutex);
        for (auto it = state.kept.begin(); it != state.kept.end(); ++it) {
            if (it->source == source) {
                state.kept.splice(state.kept.begin(), state.kept, it);
                return state.kept.front().result;
            }
        }
        if (!state.compiler) {
            return make_error(ErrorCode::InvalidArgument,
                              source.path + ": this program can't compile shaders; slash-cli, "
                                            "slash-mcp and exports can");
        }
        compiler = state.compiler;
    }
    Result<std::shared_ptr<const CompiledShader>> result = [&]() -> decltype(result) {
        auto compiled = compiler(source);
        if (!compiled) {
            return compiled.error();
        }
        return std::make_shared<const CompiledShader>(std::move(compiled).value());
    }();
    const std::scoped_lock lock(state.mutex);
    state.kept.push_front({.source = source, .result = result});
    if (state.kept.size() > kKeptCompiles) {
        state.kept.pop_back();
    }
    return result;
}

} // namespace slash::render
