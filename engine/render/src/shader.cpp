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

// The same for a spatial shader: the globals and functions of shaders/mesh.hlsl and the
// tonemap.hlsl it includes, its wrappers' and game_surface's parameters, which hide params of the
// same name where game_surface reads them. A test checks mesh.hlsl's.
constexpr std::string_view kSpatialTakenNames =
    " add_light agx_contrast_approx albedo albedo_sampler albedo_texture ambient apply_decal "
    "apply_decals apply_tonemapping before bicubic_w0 bicubic_w1 bicubic_w2 bicubic_w3 binormal "
    "camera camera_z capture capture_light clip clip_rows cluster cluster_stride clustered_lights "
    "clusters color compare_atlas corner cpu_fragment cpu_light cpu_vertex d_ggx d_ggx_anisotropic "
    "decal_atlas "
    "decal_fade "
    "decal_info "
    "decal_level decal_sample decal_texel decals depth_blended_weights directional_atlas "
    "directional_sampler directional_shadows emission emission_sampler emission_texture filters "
    "finish flip fog fog_color fog_depth fog_froxels fog_process fog_volume found frag_coord "
    "fragment frame_direction front fs_main game_constants game_inv_projection game_inv_view "
    "game_albedo game_light game_model game_normal_model game_previous_model game_projection "
    "game_specular game_surface "
    "game_texel game_vertex game_view game_viewport ground_normal_0 ground_normal_1 "
    "ground_normal_2 ground_normal_3 ground_normal_sampler_0 ground_normal_sampler_1 "
    "ground_normal_sampler_2 ground_normal_sampler_3 ground_sampler_2 ground_sampler_3 "
    "ground_texture_2 ground_texture_3 has_rect input jitter light_compute light_shadow lightmap "
    "lightmap_light lightmap_uv lights linear_to_srgb lit_color mapped material_layers "
    "metallic_channel "
    "metallic_sampler metallic_texture model moved normal normal_model normal_sampler "
    "normal_texture omni_attenuation options output pass_options position positional_atlas "
    "positional_sampler positional_shadows previous previous_model previous_view_projection "
    "probe_atlas probe_sampler probes projection radiance_map radiance_sampler range_mask "
    "reflect_probes reflections roughness_channel roughness_sampler roughness_texture "
    "sample_fog_froxels sample_probe sample_shadow scene_voxel_gi schlick_fresnel screen "
    "screen_sampler screen_texture shading sky sky_rows smooth srgb_to_linear ssao surface "
    "surface_out surface_steps tangent terrain_opacity terrain_uv texture tonemap tonemap_aces "
    "tonemap_agx tonemap_filmic tonemap_reinhard trace_voxel_gis unpack_ground_normal uv uv1 uv2 "
    "v_binormal v_color v_ggx v_ggx_anisotropic v_normal v_tangent v_uv v_uv2 v_world vertex view "
    "view_projection "
    "view_rows volumetric_fog volumetric_fog_process voxel_cone_trace voxel_cone_trace_45_degrees "
    "voxel_gi voxel_gi_compute voxel_gi_level voxel_gi_sample voxel_gis vs_main world ";

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

bool shader_name_reserved(std::string_view name, ShaderType type) {
    // GLSL keeps these for itself.
    if (name.starts_with("gl_") || name.find("__") != std::string_view::npos) {
        return true;
    }
    const std::string spaced = " " + std::string(name) + " ";
    const std::string_view taken = type == ShaderType::Spatial ? kSpatialTakenNames : kTakenNames;
    return taken.find(spaced) != std::string_view::npos ||
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
