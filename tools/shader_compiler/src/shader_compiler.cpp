#include "slash/shader_compiler/shader_compiler.hpp"

#include <SPIRV/GlslangToSpv.h>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <mutex>
#include <span>
#include <spirv_glsl.hpp>
#include <spirv_msl.hpp>
#include <src/tint/api/tint.h>
#include <src/tint/lang/core/ir/module.h>
#include <src/tint/lang/spirv/reader/reader.h>
#include <src/tint/lang/wgsl/writer/writer.h>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace slash::shader_compiler {

namespace detail {
extern const unsigned char kCanvasHlsl[];
extern const std::size_t kCanvasHlsl_size;
extern const unsigned char kMeshHlsl[];
extern const std::size_t kMeshHlsl_size;
extern const unsigned char kTonemapHlsl[];
extern const std::size_t kTonemapHlsl_size;
extern const unsigned char kShadowHlsl[];
extern const std::size_t kShadowHlsl_size;
} // namespace detail

namespace {

// What errors in the code wrapped around a shader name as their file.
constexpr std::string_view kWrapperName = "slash-wrapper";

// What a shader defines and reads, from its tokens outside comments and strings.
struct Scan {
    bool fragment = false; // defines fragment()
    bool vertex = false;   // defines vertex()
    bool normal_map = false;
    bool specular = false;
    bool alpha = false;         // ALPHA
    bool alpha_scissor = false; // ALPHA_SCISSOR_THRESHOLD
};

Scan scan(std::string_view code) {
    Scan out;
    std::vector<std::string> tokens;
    std::vector<int> depths;
    int depth = 0;
    std::size_t i = 0;
    while (i < code.size()) {
        const char c = code[i];
        if (c == '/' && i + 1 < code.size() && code[i + 1] == '/') {
            while (i < code.size() && code[i] != '\n') {
                ++i;
            }
        } else if (c == '/' && i + 1 < code.size() && code[i + 1] == '*') {
            const std::size_t end = code.find("*/", i + 2);
            i = end == std::string_view::npos ? code.size() : end + 2;
        } else if (c == '"') {
            ++i;
            while (i < code.size() && code[i] != '"' && code[i] != '\n') {
                i += code[i] == '\\' ? std::size_t{2} : std::size_t{1};
            }
            ++i;
        } else if (std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_') {
            const std::size_t start = i;
            while (i < code.size() &&
                   (std::isalnum(static_cast<unsigned char>(code[i])) != 0 || code[i] == '_')) {
                ++i;
            }
            tokens.emplace_back(code.substr(start, i - start));
            depths.push_back(depth);
        } else {
            if (c == '{') {
                ++depth;
            } else if (c == '}') {
                --depth;
            }
            if (std::isspace(static_cast<unsigned char>(c)) == 0) {
                tokens.emplace_back(1, c);
                depths.push_back(depth);
            }
            ++i;
        }
    }
    for (std::size_t k = 0; k < tokens.size(); ++k) {
        const std::string& token = tokens[k];
        const bool defined = depths[k] == 0 && k + 1 < tokens.size() && tokens[k + 1] == "(";
        if (token == "fragment" && defined) {
            out.fragment = true;
        } else if (token == "vertex" && defined) {
            out.vertex = true;
        } else if (token == "NORMAL_MAP") {
            out.normal_map = true;
        } else if (token == "SPECULAR_SHININESS") {
            out.specular = true;
        } else if (token == "ALPHA") {
            out.alpha = true;
        } else if (token == "ALPHA_SCISSOR_THRESHOLD") {
            out.alpha_scissor = true;
        }
    }
    return out;
}

std::string_view canvas_hlsl() {
    return {reinterpret_cast<const char*>(detail::kCanvasHlsl), detail::kCanvasHlsl_size};
}

// mesh.hlsl with the tonemap.hlsl it includes in its place.
std::string mesh_hlsl() {
    std::string out(reinterpret_cast<const char*>(detail::kMeshHlsl), detail::kMeshHlsl_size);
    const std::string include = "#include \"tonemap.hlsl\"\n";
    const std::size_t at = out.find(include);
    if (at != std::string::npos) {
        out.replace(at, include.size(),
                    std::string_view(reinterpret_cast<const char*>(detail::kTonemapHlsl),
                                     detail::kTonemapHlsl_size));
    }
    return out;
}

constexpr std::array<std::string_view, 5> kSwizzles{"", ".x", ".xy", ".xyz", ""};
constexpr std::array<std::string_view, 5> kTypes{"", "float", "float2", "float3", "float4"};

// The globals every wrapper declares before the shader's code: the material's params, Godot's
// built-ins and the texture() call.
std::string declarations(const render::ShaderSource& source) {
    std::string out = "Texture2D<float4> TEXTURE : register(t0, space2);\n"
                      "SamplerState TEXTURE_sampler : register(s0, space2);\n";
    for (const auto& param : source.params) {
        if (param.components == 0) {
            const std::string slot = std::to_string(param.slot);
            out += "Texture2D<float4> " + param.name + " : register(t" + slot + ", space2);\n";
            out += "SamplerState " + param.name + "_sampler : register(s" + slot + ", space2);\n";
        } else {
            out += "static " + std::string(kTypes.at(param.components)) + " " + param.name + ";\n";
        }
    }
    out += "static float2 VERTEX;\n"
           "static float2 UV;\n"
           "static float4 COLOR;\n"
           "static float TIME;\n"
           "static float2 TEXTURE_PIXEL_SIZE;\n"
           "static float4 FRAGCOORD;\n"
           "static float3 NORMAL_MAP;\n"
           "static float4 SPECULAR_SHININESS;\n"
           "static const float PI = 3.14159265358979323846;\n"
           "static const float TAU = 6.28318530717958647692;\n"
           "static const float E = 2.71828182845904523536;\n"
           "#define texture(t, uv) ((t).SampleLevel(t##_sampler, (uv), 0.0))\n";
    return out;
}

// Statements that read the params and the time from an array of constants, and the texel size
// unless texel is empty.
std::string load_params(const render::ShaderSource& source, std::string_view constants,
                        std::string_view texel) {
    std::string out;
    for (const auto& param : source.params) {
        if (param.components != 0) {
            out += "    " + param.name + " = " + std::string(constants) + "[" +
                   std::to_string(param.slot) + "]" + std::string(kSwizzles.at(param.components)) +
                   ";\n";
        }
    }
    out += "    TIME = " + std::string(constants) + "[" +
           std::to_string(render::kShaderTimeConstant) + "].x;\n";
    if (!texel.empty()) {
        out += "    TEXTURE_PIXEL_SIZE = " + std::string(texel) + ".xy;\n";
    }
    return out;
}

// The code before and after the shader's.
struct Parts {
    std::string before;
    std::string after;
};

Parts gpu_parts(const render::ShaderSource& source, const Scan& found) {
    Parts parts;
    parts.before = std::string(canvas_hlsl());
    parts.before += "\n// The game's shader, as a canvas material's (tools/shader_compiler).\n"
                    "cbuffer VertexProgram : register(b1, space1) {\n"
                    "    float4 vertex_constants[32];\n"
                    "    float4 vertex_texel;\n"
                    "};\n";
    parts.before += declarations(source);
    parts.after = "\nVertexOutput vs_main(VertexInput input) {\n";
    parts.after += load_params(source, "vertex_constants", "vertex_texel");
    parts.after += "    VERTEX = input.position;\n"
                   "    UV = input.uv;\n"
                   "    COLOR = input.color;\n";
    if (found.vertex) {
        parts.after += "    vertex();\n";
    }
    parts.after += "    VertexOutput output;\n"
                   "    output.position = mul(view_projection, float4(VERTEX, 0.0, 1.0));\n"
                   "    output.uv = UV;\n"
                   "    output.color = COLOR;\n"
                   "    output.world = VERTEX;\n"
                   "    return output;\n"
                   "}\n\n"
                   "float4 fs_main(VertexOutput input) : SV_Target0 {\n"
                   "    CanvasPixel pixel = canvas_pixel(input);\n";
    parts.after += load_params(source, "constants", "texel");
    parts.after += "    UV = input.uv;\n"
                   "    FRAGCOORD = float4(input.position.xy, 0.0, 1.0);\n"
                   "    COLOR = texture(TEXTURE, UV) * input.color;\n"
                   "    bool normal_used = (shading.w & 1) != 0;\n"
                   "    NORMAL_MAP = float3(0.5, 0.5, 1.0);\n"
                   "    if (normal_used) {\n"
                   "        NORMAL_MAP = normal_map.SampleLevel(normal_sampler, UV, 0.0).rgb;\n"
                   "    }\n"
                   "    bool shine_used = (shading.w & 2) != 0;\n"
                   "    SPECULAR_SHININESS = specular_map.SampleLevel(specular_sampler, UV, 0.0) * "
                   "specular;\n";
    if (found.fragment) {
        parts.after += "    fragment();\n";
    }
    parts.after += std::string("    return shade_canvas_pixel(pixel, COLOR, NORMAL_MAP, ") +
                   (found.normal_map ? "true" : "normal_used") + ", SPECULAR_SHININESS, " +
                   (found.specular ? "true" : "shine_used") + ");\n}\n";
    return parts;
}

// The CPU's modules read the constants from binding 0 (render::cpu_shader).
std::string cpu_uniforms() {
    return "cbuffer Params : register(b0, space3) {\n"
           "    float4 constants[32];\n"
           "    float4 texel;\n"
           "};\n";
}

Parts cpu_vertex_parts(const render::ShaderSource& source) {
    Parts parts;
    parts.before = cpu_uniforms() + declarations(source);
    parts.after = "\nstruct CpuVertex {\n"
                  "    [[vk::location(0)]] float2 vertex : TEXCOORD0;\n"
                  "    [[vk::location(1)]] float2 uv : TEXCOORD1;\n"
                  "    [[vk::location(2)]] float4 color : TEXCOORD2;\n"
                  "};\n\n"
                  "CpuVertex cpu_vertex(CpuVertex input) {\n";
    parts.after += load_params(source, "constants", "texel");
    parts.after += "    VERTEX = input.vertex;\n"
                   "    UV = input.uv;\n"
                   "    COLOR = input.color;\n"
                   "    vertex();\n"
                   "    CpuVertex output;\n"
                   "    output.vertex = VERTEX;\n"
                   "    output.uv = UV;\n"
                   "    output.color = COLOR;\n"
                   "    return output;\n"
                   "}\n";
    return parts;
}

Parts cpu_fragment_parts(const render::ShaderSource& source, const Scan& found) {
    Parts parts;
    parts.before = cpu_uniforms() + declarations(source);
    parts.after = "\nstruct CpuPixel {\n"
                  "    [[vk::location(1)]] float2 uv : TEXCOORD1;\n"
                  "    [[vk::location(2)]] float4 color : TEXCOORD2;\n"
                  "    [[vk::location(3)]] float2 frag_coord : TEXCOORD3;\n"
                  "    [[vk::location(4)]] float4 texture_color : TEXCOORD4;\n"
                  "    [[vk::location(5)]] float4 normal_texel : TEXCOORD5;\n"
                  "    [[vk::location(6)]] float4 specular : TEXCOORD6;\n"
                  "};\n\n"
                  "struct CpuColors {\n"
                  "    [[vk::location(0)]] float4 color : SV_Target0;\n"
                  "    [[vk::location(1)]] float4 normal_map : SV_Target1;\n"
                  "    [[vk::location(2)]] float4 specular : SV_Target2;\n"
                  "};\n\n"
                  "CpuColors cpu_fragment(CpuPixel input) {\n";
    parts.after += load_params(source, "constants", "texel");
    parts.after += "    UV = input.uv;\n"
                   "    FRAGCOORD = float4(input.frag_coord, 0.0, 1.0);\n"
                   "    COLOR = input.texture_color * input.color;\n"
                   "    NORMAL_MAP = input.normal_texel.rgb;\n"
                   "    SPECULAR_SHININESS = input.specular;\n";
    if (found.fragment) {
        parts.after += "    fragment();\n";
    }
    parts.after += "    CpuColors output;\n"
                   "    output.color = COLOR;\n"
                   "    output.normal_map = float4(NORMAL_MAP, 1.0);\n"
                   "    output.specular = SPECULAR_SHININESS;\n"
                   "    return output;\n"
                   "}\n";
    return parts;
}

// A spatial shader's stages: the Compatibility renderer's, then Forward+'s, lit by clusters, and
// its prepass's without and with TAA's motion vectors (shaders/mesh_*.hlsl), and the shadow
// passes' (shaders/shadow.hlsl).
enum class MeshStage : std::uint8_t { Compatibility, Clustered, NormalRoughness, Motion, Shadow };

// Which backend a stage's code is for, where mesh.hlsl declares storage buffers apart for it.
enum class MeshTarget : std::uint8_t { Other, Msl, Wgsl };

// Where a spatial shader's wrappers declare its uniform buffer and textures, as SDL binds them
// for the stage: vertex stages read textures with SampleLevel, as they have no derivatives.
struct SpatialBindings {
    std::string_view uniforms;      // the uniform buffer's register
    std::string_view texture_space; // the texture params' space
    bool vertex = false;
};

constexpr SpatialBindings kVertexBindings{
    .uniforms = "b2, space1", .texture_space = "space0", .vertex = true};
// WebGPU's vertex stages read the texture params in the fragment stage's group, as the
// WebGPU device lays its groups out (Forward+'s clusters take group 0).
constexpr SpatialBindings kWgslVertexBindings{
    .uniforms = "b2, space1", .texture_space = "space2", .vertex = true};
constexpr SpatialBindings kMeshFragmentBindings{.uniforms = "b3, space3",
                                                .texture_space = "space2"};
constexpr SpatialBindings kShadowFragmentBindings{.uniforms = "b1, space3",
                                                  .texture_space = "space2"};
constexpr SpatialBindings kCpuFragmentBindings{.uniforms = "b0, space3", .texture_space = "space2"};
constexpr SpatialBindings kCpuVertexBindings{
    .uniforms = "b0, space3", .texture_space = "space2", .vertex = true};

// The globals a spatial shader's wrappers declare before its code: the uniform buffer, the
// texture params, Godot's built-ins and texture().
std::string spatial_declarations(const render::ShaderSource& source,
                                 const SpatialBindings& bindings) {
    std::string out = "cbuffer GameMaterial : register(" + std::string(bindings.uniforms) +
                      ") {\n"
                      "    float4 game_constants[32];\n"
                      "    float4x4 game_model;\n"
                      "    float4x4 game_view;\n"
                      "    float4x4 game_inv_view;\n"
                      "    float4x4 game_projection;\n"
                      "    float4x4 game_inv_projection;\n"
                      "    float4 game_viewport;\n"
                      "    float4x4 game_normal_model;\n"
                      "    float4x4 game_previous_model;\n"
                      "};\n"
                      "float4 game_texel(float4 texel, bool srgb) {\n"
                      "    // Godot's approximation of sRGB to linear, as tonemap.hlsl's.\n"
                      "    float3 c = texel.rgb;\n"
                      "    float3 linear_color = c * (c * (c * 0.305306011 + 0.682171111) + "
                      "0.012522878);\n"
                      "    return srgb ? float4(linear_color, texel.a) : texel;\n"
                      "}\n";
    for (const auto& param : source.params) {
        if (param.components == 0) {
            const std::string at =
                std::to_string(param.slot) + ", " + std::string(bindings.texture_space);
            out += "Texture2D<float4> " + param.name + " : register(t" + at + ");\n";
            out += "SamplerState " + param.name + "_sampler : register(s" + at + ");\n";
            out += "static const bool " + param.name +
                   "_srgb = " + (param.source_color ? "true" : "false") + ";\n";
        } else {
            out += "static " + std::string(kTypes.at(param.components)) + " " + param.name + ";\n";
        }
    }
    out += "static float3 VERTEX;\n"
           "static float3 NORMAL;\n"
           "static float3 TANGENT;\n"
           "static float3 BINORMAL;\n"
           "static float3 VIEW;\n"
           "static float2 UV;\n"
           "static float2 UV2;\n"
           "static float4 COLOR;\n"
           "static float4 FRAGCOORD;\n"
           "static float2 SCREEN_UV;\n"
           "static bool FRONT_FACING;\n"
           "static float TIME;\n"
           "static float4x4 MODEL_MATRIX;\n"
           "static float3x3 MODEL_NORMAL_MATRIX;\n"
           "static float4x4 VIEW_MATRIX;\n"
           "static float4x4 INV_VIEW_MATRIX;\n"
           "static float4x4 PROJECTION_MATRIX;\n"
           "static float4x4 INV_PROJECTION_MATRIX;\n"
           "static float4x4 MODELVIEW_MATRIX;\n"
           "static float3x3 MODELVIEW_NORMAL_MATRIX;\n"
           "static float3 CAMERA_POSITION_WORLD;\n"
           "static float3 CAMERA_DIRECTION_WORLD;\n"
           "static float3 NODE_POSITION_WORLD;\n"
           "static float3 NODE_POSITION_VIEW;\n"
           "static float2 VIEWPORT_SIZE;\n"
           "static float3 ALBEDO;\n"
           "static float ALPHA;\n"
           "static float METALLIC;\n"
           "static float ROUGHNESS;\n"
           "static float SPECULAR;\n"
           "static float3 EMISSION;\n"
           "static float AO;\n"
           "static float AO_LIGHT_AFFECT;\n"
           "static float3 NORMAL_MAP;\n"
           "static float NORMAL_MAP_DEPTH;\n"
           "static float ALPHA_SCISSOR_THRESHOLD;\n"
           "static float game_specular;\n"
           "static const float PI = 3.14159265358979323846;\n"
           "static const float TAU = 6.28318530717958647692;\n"
           "static const float E = 2.71828182845904523536;\n";
    out += bindings.vertex
               ? "#define texture(t, uv) game_texel((t).SampleLevel(t##_sampler, (uv), 0.0), "
                 "t##_srgb)\n"
               : "#define texture(t, uv) game_texel((t).Sample(t##_sampler, (uv)), t##_srgb)\n";
    return out;
}

// Sets Godot's matrices and the places they give for a draw with the model.
std::string spatial_matrices(std::string_view model) {
    return "    MODEL_MATRIX = " + std::string(model) +
           ";\n"
           "    MODEL_NORMAL_MATRIX = (float3x3)game_normal_model;\n"
           "    VIEW_MATRIX = game_view;\n"
           "    INV_VIEW_MATRIX = game_inv_view;\n"
           "    PROJECTION_MATRIX = game_projection;\n"
           "    INV_PROJECTION_MATRIX = game_inv_projection;\n"
           "    MODELVIEW_MATRIX = mul(game_view, MODEL_MATRIX);\n"
           "    MODELVIEW_NORMAL_MATRIX = mul((float3x3)game_view, MODEL_NORMAL_MATRIX);\n"
           "    VIEWPORT_SIZE = game_viewport.xy;\n"
           "    CAMERA_POSITION_WORLD = mul(game_inv_view, float4(0.0, 0.0, 0.0, 1.0)).xyz;\n"
           "    CAMERA_DIRECTION_WORLD = normalize(mul((float3x3)game_inv_view, "
           "float3(0.0, 0.0, -1.0)));\n"
           "    NODE_POSITION_WORLD = mul(MODEL_MATRIX, float4(0.0, 0.0, 0.0, 1.0)).xyz;\n"
           "    NODE_POSITION_VIEW = mul(game_view, float4(NODE_POSITION_WORLD, 1.0)).xyz;\n";
}

// game_vertex, which mesh.hlsl's and shadow.hlsl's vertex stages and the CPU's module call:
// Godot's built-ins from a corner of the mesh, in its space (or the world's, with
// world_vertex_coords), its vertex(), then the corner in the world. The frame before's model and
// time place it where it was, for the motion prepass.
std::string game_vertex(const render::ShaderSource& source) {
    std::string out =
        "\nstruct GameVertex {\n"
        "    float3 world;\n"
        "    float3 normal;\n"
        "    float3 tangent;\n"
        "    float3 binormal;\n"
        "    float2 uv;\n"
        "    float2 uv2;\n"
        "    float4 color;\n"
        "};\n\n"
        "GameVertex game_vertex(float3 position, float3 normal, float4 tangent, float2 uv,\n"
        "                       float2 uv2, float4 color, bool previous) {\n";
    out += load_params(source, "game_constants", "");
    out += "    if (previous) {\n"
           "        TIME = game_constants[" +
           std::to_string(render::kShaderTimeConstant) +
           "].y;\n"
           "    }\n";
    out += spatial_matrices("previous ? game_previous_model : game_model");
    out += "    VERTEX = position;\n"
           "    NORMAL = normal;\n"
           "    TANGENT = tangent.xyz;\n"
           "    BINORMAL = normalize(cross(normal, tangent.xyz) * tangent.w);\n";
    if (source.world_vertex_coords) {
        out += "    VERTEX = mul(MODEL_MATRIX, float4(VERTEX, 1.0)).xyz;\n"
               "    NORMAL = mul(MODEL_NORMAL_MATRIX, NORMAL);\n"
               "    TANGENT = mul(MODEL_NORMAL_MATRIX, TANGENT);\n"
               "    BINORMAL = mul(MODEL_NORMAL_MATRIX, BINORMAL);\n";
    }
    out += "    UV = uv;\n"
           "    UV2 = uv2;\n"
           "    COLOR = color;\n"
           "    vertex();\n"
           "    GameVertex corner;\n";
    if (source.world_vertex_coords) {
        out += "    corner.world = VERTEX;\n"
               "    corner.normal = NORMAL;\n"
               "    corner.tangent = TANGENT;\n"
               "    corner.binormal = BINORMAL;\n";
    } else {
        out += "    corner.world = mul(MODEL_MATRIX, float4(VERTEX, 1.0)).xyz;\n"
               "    corner.normal = mul(MODEL_NORMAL_MATRIX, NORMAL);\n"
               "    corner.tangent = mul(MODEL_NORMAL_MATRIX, TANGENT);\n"
               "    corner.binormal = mul(MODEL_NORMAL_MATRIX, BINORMAL);\n";
    }
    out += "    corner.uv = UV;\n"
           "    corner.uv2 = UV2;\n"
           "    corner.color = COLOR;\n"
           "    return corner;\n"
           "}\n";
    return out;
}

// game_surface, which mesh.hlsl's fragment stage and the CPU's module call: Godot's built-ins
// from a pixel's values in the world, its fragment(), then the surface it gives, the shading
// normal back in the world (render::cpu_spatial).
std::string game_surface(const render::ShaderSource& source, const Scan& found) {
    std::string out = "\nstruct GameSurface {\n"
                      "    float4 albedo;\n"
                      "    float4 material;\n"
                      "    float4 emission;\n"
                      "    float4 normal;\n"
                      "};\n\n"
                      "GameSurface game_surface(float3 world, float3 normal, float3 tangent,\n"
                      "                         float3 binormal, float2 uv, float2 uv2, "
                      "float4 color,\n"
                      "                         float4 frag_coord, bool front, bool flip) {\n";
    out += load_params(source, "game_constants", "");
    out += spatial_matrices("game_model");
    out += "    VERTEX = mul(game_view, float4(world, 1.0)).xyz;\n"
           "    NORMAL = normalize(mul((float3x3)game_view, flip ? -normal : normal));\n"
           "    TANGENT = normalize(mul((float3x3)game_view, tangent));\n"
           "    BINORMAL = normalize(mul((float3x3)game_view, binormal));\n"
           "    VIEW = normalize(-VERTEX);\n"
           "    UV = uv;\n"
           "    UV2 = uv2;\n"
           "    COLOR = color;\n"
           "    float4 clip = mul(game_projection, float4(VERTEX, 1.0));\n"
           "    FRAGCOORD = float4(frag_coord.xy, clip.z / clip.w, 1.0);\n"
           "    SCREEN_UV = FRAGCOORD.xy / VIEWPORT_SIZE;\n"
           "    FRONT_FACING = front;\n"
           "    ALBEDO = (float3)1.0;\n"
           "    ALPHA = 1.0;\n"
           "    METALLIC = 0.0;\n"
           "    ROUGHNESS = 1.0;\n"
           "    SPECULAR = 0.5;\n"
           "    EMISSION = (float3)0.0;\n"
           "    AO = 1.0;\n"
           "    AO_LIGHT_AFFECT = 0.0;\n"
           "    NORMAL_MAP = float3(0.5, 0.5, 1.0);\n"
           "    NORMAL_MAP_DEPTH = 1.0;\n"
           "    ALPHA_SCISSOR_THRESHOLD = 0.5;\n";
    if (found.fragment) {
        out += "    fragment();\n";
    }
    out += "    float3 shading = NORMAL;\n";
    if (found.normal_map) {
        // As Godot's scene shaders bend it, with the unnormalized tangent frame.
        out += "    float3 mapped = NORMAL_MAP;\n"
               "    mapped.xy = mapped.xy * 2.0 - 1.0;\n"
               "    mapped.z = sqrt(max(0.0, 1.0 - dot(mapped.xy, mapped.xy)));\n"
               "    shading = normalize(lerp(NORMAL, TANGENT * mapped.x + BINORMAL * mapped.y + "
               "NORMAL * mapped.z, NORMAL_MAP_DEPTH));\n";
    }
    out += "    GameSurface surface_out;\n"
           "    surface_out.albedo = float4(ALBEDO, ALPHA);\n"
           "    surface_out.material = float4(METALLIC, ROUGHNESS, SPECULAR, AO);\n"
           "    surface_out.emission = float4(EMISSION, AO_LIGHT_AFFECT);\n"
           "    surface_out.normal = float4(normalize(mul((float3x3)game_inv_view, shading)), "
           "ALPHA_SCISSOR_THRESHOLD);\n"
           "    return surface_out;\n"
           "}\n";
    return out;
}

std::string_view shadow_hlsl() {
    return {reinterpret_cast<const char*>(detail::kShadowHlsl), detail::kShadowHlsl_size};
}

// A stage's code around the shader's: for the vertex stage (bindings.vertex) or the fragment
// stage of a mesh pass or a shadow pass.
Parts spatial_parts(const render::ShaderSource& source, const Scan& found, MeshStage stage,
                    MeshTarget target, bool vertex) {
    Parts parts;
    parts.before = "#define GAME_SHADER\n";
    if (found.vertex) {
        parts.before += "#define GAME_VERTEX\n";
    }
    switch (stage) {
    case MeshStage::Compatibility:
    case MeshStage::Shadow:
        break;
    case MeshStage::Clustered:
        parts.before += "#define CLUSTERED\n";
        break;
    case MeshStage::NormalRoughness:
        parts.before += "#define NORMAL_ROUGHNESS\n";
        break;
    case MeshStage::Motion:
        parts.before += "#define NORMAL_ROUGHNESS\n#define MOTION_VECTORS\n";
        break;
    }
    if (target == MeshTarget::Msl) {
        parts.before += "#define SLASH_MSL\n";
    } else if (target == MeshTarget::Wgsl) {
        parts.before += "#define SLASH_WGSL\n";
    }
    parts.before += "\n// The game's shader, as a spatial material's (tools/shader_compiler).\n";
    const SpatialBindings* bindings = &kMeshFragmentBindings;
    if (vertex) {
        bindings = target == MeshTarget::Wgsl ? &kWgslVertexBindings : &kVertexBindings;
    } else if (stage == MeshStage::Shadow) {
        bindings = &kShadowFragmentBindings;
    }
    parts.before += spatial_declarations(source, *bindings);
    if (found.vertex) {
        parts.after = game_vertex(source);
    }
    parts.after += game_surface(source, found) + "\n";
    if (stage == MeshStage::Shadow) {
        parts.after += shadow_hlsl();
    } else {
        parts.after += mesh_hlsl();
    }
    return parts;
}

Parts cpu_spatial_parts(const render::ShaderSource& source, const Scan& found) {
    Parts parts;
    parts.before = spatial_declarations(source, kCpuFragmentBindings);
    parts.after = game_surface(source, found);
    parts.after += "\nstruct CpuSurfaceInput {\n"
                   "    [[vk::location(0)]] float4 world : TEXCOORD0;\n"
                   "    [[vk::location(1)]] float4 normal : TEXCOORD1;\n"
                   "    [[vk::location(2)]] float4 tangent : TEXCOORD2;\n"
                   "    [[vk::location(3)]] float4 binormal : TEXCOORD3;\n"
                   "    [[vk::location(4)]] float4 uv : TEXCOORD4;\n"
                   "    [[vk::location(5)]] float4 color : TEXCOORD5;\n"
                   "    [[vk::location(6)]] float4 frag_coord : TEXCOORD6;\n"
                   "};\n\n"
                   "struct CpuSurface {\n"
                   "    [[vk::location(0)]] float4 albedo : SV_Target0;\n"
                   "    [[vk::location(1)]] float4 material : SV_Target1;\n"
                   "    [[vk::location(2)]] float4 emission : SV_Target2;\n"
                   "    [[vk::location(3)]] float4 normal : SV_Target3;\n"
                   "};\n\n"
                   "CpuSurface cpu_fragment(CpuSurfaceInput input) {\n"
                   "    GameSurface found = game_surface(input.world.xyz, input.normal.xyz, "
                   "input.tangent.xyz,\n"
                   "        input.binormal.xyz, input.uv.xy, input.uv.zw, input.color, "
                   "input.frag_coord,\n"
                   "        input.normal.w != 0.0, input.world.w != 0.0);\n"
                   "    CpuSurface output;\n"
                   "    output.albedo = found.albedo;\n"
                   "    output.material = found.material;\n"
                   "    output.emission = found.emission;\n"
                   "    output.normal = found.normal;\n"
                   "    return output;\n"
                   "}\n";
    return parts;
}

// The CPU's module for a spatial shader's vertex() (render::cpu_spatial's corners).
Parts cpu_spatial_vertex_parts(const render::ShaderSource& source) {
    Parts parts;
    parts.before = spatial_declarations(source, kCpuVertexBindings);
    parts.after = game_vertex(source);
    parts.after += "\nstruct CpuCornerInput {\n"
                   "    [[vk::location(0)]] float4 position : TEXCOORD0;\n"
                   "    [[vk::location(1)]] float4 normal : TEXCOORD1;\n"
                   "    [[vk::location(2)]] float4 tangent : TEXCOORD2;\n"
                   "    [[vk::location(3)]] float4 uv : TEXCOORD3;\n"
                   "    [[vk::location(4)]] float4 color : TEXCOORD4;\n"
                   "};\n\n"
                   "struct CpuCorner {\n"
                   "    [[vk::location(0)]] float4 world : TEXCOORD0;\n"
                   "    [[vk::location(1)]] float4 normal : TEXCOORD1;\n"
                   "    [[vk::location(2)]] float4 tangent : TEXCOORD2;\n"
                   "    [[vk::location(3)]] float4 binormal : TEXCOORD3;\n"
                   "    [[vk::location(4)]] float4 uv : TEXCOORD4;\n"
                   "    [[vk::location(5)]] float4 color : TEXCOORD5;\n"
                   "};\n\n"
                   "CpuCorner cpu_vertex(CpuCornerInput input) {\n"
                   "    GameVertex moved = game_vertex(input.position.xyz, input.normal.xyz, "
                   "input.tangent,\n"
                   "        input.uv.xy, input.uv.zw, input.color, input.position.w != 0.0);\n"
                   "    CpuCorner output;\n"
                   "    output.world = float4(moved.world, 1.0);\n"
                   "    output.normal = float4(moved.normal, 0.0);\n"
                   "    output.tangent = float4(moved.tangent, 0.0);\n"
                   "    output.binormal = float4(moved.binormal, 0.0);\n"
                   "    output.uv = float4(moved.uv, moved.uv2);\n"
                   "    output.color = moved.color;\n"
                   "    return output;\n"
                   "}\n";
    return parts;
}

// glslang's info log as errors, each "<path>:<line>: message".
void read_log(std::string_view log, const std::string& path, std::vector<std::string>& errors) {
    std::istringstream lines{std::string(log)};
    std::string line;
    while (std::getline(lines, line)) {
        constexpr std::string_view kError = "ERROR: ";
        if (!line.starts_with(kError)) {
            continue;
        }
        std::string rest = line.substr(kError.size());
        if (rest.find("compilation terminated") != std::string::npos ||
            rest.find("compilation errors") != std::string::npos) {
            continue;
        }
        while (!rest.empty() && rest.back() == ' ') {
            rest.pop_back();
        }
        std::string error;
        if (rest.starts_with(std::string(kWrapperName) + ":")) {
            const std::size_t message = rest.find(": ", kWrapperName.size() + 1);
            error = path + ": ";
            error += message == std::string::npos ? rest : rest.substr(message + 2);
            error += " (in the code around the shader: fragment() and vertex() take nothing and "
                     "return void, and the built-in names are taken)";
        } else if (rest.starts_with(path + ":")) {
            error = rest;
        } else {
            error = path + ": ";
            error += rest;
        }
        // One error a line: glslang's next ones on it follow from the first.
        const std::size_t line_end = error.find(": ");
        const std::string_view where = std::string_view(error).substr(0, line_end);
        bool seen = false;
        for (const std::string& known : errors) {
            seen = seen || std::string_view(known).substr(0, known.find(": ")) == where;
        }
        if (!seen) {
            errors.push_back(std::move(error));
        }
    }
}

void initialize_glslang() {
    static std::once_flag once;
    std::call_once(once, [] { glslang::InitializeProcess(); });
}

// How a stage's SPIR-V binds textures: combined with their samplers, as Vulkan, Metal and OpenGL
// ES take them, or apart, as WGSL has them, with each sampler 8 bindings after its texture (SDL's
// bind groups, as the WebGPU device lays them out).
enum class Samplers : std::uint8_t { Combined, Apart };

// The code between parts compiled to SPIR-V for a stage, or nothing with errors added.
std::vector<std::uint32_t> to_spirv(const Parts& parts, const render::ShaderSource& source,
                                    EShLanguage stage, const char* entry,
                                    std::vector<std::string>& errors,
                                    Samplers samplers = Samplers::Combined,
                                    std::uint32_t sampler_shift = 8) {
    initialize_glslang();
    glslang::TShader shader(stage);
    const std::array<const char*, 3> strings{parts.before.c_str(), source.code.c_str(),
                                             parts.after.c_str()};
    const std::array<int, 3> lengths{static_cast<int>(parts.before.size()),
                                     static_cast<int>(source.code.size()),
                                     static_cast<int>(parts.after.size())};
    const std::string wrapper(kWrapperName);
    const std::array<const char*, 3> names{wrapper.c_str(), source.path.c_str(), wrapper.c_str()};
    shader.setStringsWithLengthsAndNames(strings.data(), lengths.data(), names.data(), 3);
    shader.setEntryPoint(entry);
    shader.setEnvInput(glslang::EShSourceHlsl, stage, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);
    if (samplers == Samplers::Combined) {
        shader.setTextureSamplerTransformMode(EShTexSampTransUpgradeTextureRemoveSampler);
    } else {
        shader.setShiftBinding(glslang::EResSampler, sampler_shift);
    }
    const auto messages =
        // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange): glslang's flags combine.
        static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules | EShMsgReadHlsl);
    if (!shader.parse(GetDefaultResources(), 100, false, messages)) {
        read_log(shader.getInfoLog(), source.path, errors);
        return {};
    }
    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(messages)) {
        read_log(program.getInfoLog(), source.path, errors);
        return {};
    }
    // Moves the samplers to their own bindings, as glslangValidator's --shift-sampler-binding.
    if (samplers == Samplers::Apart && !program.mapIO()) {
        read_log(program.getInfoLog(), source.path, errors);
        return {};
    }
    std::vector<std::uint32_t> spirv;
    glslang::SpvOptions options;
    options.disableOptimizer = true;
    glslang::GlslangToSpv(*program.getIntermediate(stage), spirv, &options);
    return spirv;
}

std::string to_msl(const std::vector<std::uint32_t>& spirv) {
    spirv_cross::CompilerMSL compiler(spirv);
    auto options = compiler.get_msl_options();
    options.msl_version = spirv_cross::CompilerMSL::Options::make_msl_version(2, 1);
    // Metal's texture, sampler and buffer indexes are the HLSL registers, as SDL binds them.
    options.enable_decoration_binding = true;
    compiler.set_msl_options(options);
    return compiler.compile();
}

// Names the GL device binds textures by, for each binding (gl_device.cpp).
constexpr std::array<std::string_view, 8> kSamplerNames{"texture0",    "texture1",   "texture2",
                                                        "texture3",    "normal_map", "specular_map",
                                                        "light_atlas", "shadow_map"};

// GLSL ES 3.00 can't place varyings by location, so both stages name them alike to link: the
// canvas shader's, and the mesh shader's (cmake/compile_shaders.cmake names them so).
constexpr std::array<std::string_view, 3> kCanvasVaryings{"v_uv", "v_color", "v_world"};
constexpr std::array<std::string_view, 7> kMeshVaryings{
    "v_world", "v_normal", "v_tangent", "v_binormal", "v_uv", "v_color", "v_uv2"};

// The mesh shader's textures by the names the GL device binds by (gl_device.cpp), in binding
// order: a spatial shader's texture params take the first five's slots.
constexpr std::array<std::string_view, 14> kMeshSamplerNames{
    "albedo_texture",   "metallic_texture", "roughness_texture", "emission_texture",
    "normal_texture",   "ground_texture_2", "ground_texture_3",  "ground_normal_0",
    "ground_normal_1",  "ground_normal_2",  "ground_normal_3",   "directional_atlas",
    "positional_atlas", "radiance_map"};

// With fixup_clipspace, a vertex stage maps its depth from 0..1 to OpenGL's -1..1 at the end, as
// the mesh shaders' committed GLSL does (compile_shaders.cmake's FIXUP_CLIPSPACE).
std::string to_glsl(const std::vector<std::uint32_t>& spirv, bool vertex,
                    std::span<const std::string_view> varying_names = kCanvasVaryings,
                    std::span<const std::string_view> sampler_names = kSamplerNames,
                    bool fixup_clipspace = false) {
    spirv_cross::CompilerGLSL compiler(spirv);
    auto options = compiler.get_common_options();
    options.es = true;
    options.version = 300;
    options.vertex.fixup_clipspace = vertex && fixup_clipspace;
    options.fragment.default_float_precision = spirv_cross::CompilerGLSL::Options::Highp;
    options.fragment.default_int_precision = spirv_cross::CompilerGLSL::Options::Highp;
    compiler.set_common_options(options);
    const spirv_cross::ShaderResources resources = compiler.get_shader_resources();
    for (const auto& variable : vertex ? resources.stage_outputs : resources.stage_inputs) {
        if (compiler.has_decoration(variable.id, spv::DecorationLocation)) {
            const std::uint32_t location =
                compiler.get_decoration(variable.id, spv::DecorationLocation);
            if (location < varying_names.size()) {
                compiler.set_name(variable.id, std::string(varying_names[location]));
            }
        }
    }
    for (const auto& image : resources.sampled_images) {
        const std::uint32_t binding = compiler.get_decoration(image.id, spv::DecorationBinding);
        if (binding < sampler_names.size()) {
            compiler.set_name(image.id, std::string(sampler_names[binding]));
        }
    }
    return compiler.compile();
}

void initialize_tint() {
    static std::once_flag once;
    std::call_once(once, [] { tint::Initialize(); });
}

// SPIR-V with its samplers apart as WGSL, through Tint, or nothing with an error added.
std::string to_wgsl(const std::vector<std::uint32_t>& spirv, const std::string& path,
                    std::vector<std::string>& errors) {
    initialize_tint();
    auto module = tint::spirv::reader::ReadIR(spirv);
    if (module != tint::Success) {
        errors.push_back(path + ": Tint can't read it: " + module.Failure().reason);
        return {};
    }
    // HLSL, GLSL and MSL take derivatives anywhere, after a discard too; WGSL refuses them in
    // control flow that can differ between pixels unless told not to check.
    tint::wgsl::writer::Options options;
    options.allow_non_uniform_derivatives = true;
    auto wgsl = tint::wgsl::writer::WgslFromIR(module.Get(), options);
    if (wgsl != tint::Success) {
        errors.push_back(path + ": Tint can't write it as WGSL: " + wgsl.Failure().reason);
        return {};
    }
    return wgsl->wgsl;
}

// The code of parts around a shader's, as one HLSL file.
std::string joined(const Parts& parts, const render::ShaderSource& source) {
    return parts.before + "#line 1 \"" + source.path + "\"\n" + source.code + "\n#line 1 \"" +
           std::string(kWrapperName) + "\"\n" + parts.after;
}

// Every error as one message, one a line.
Error compile_errors(const std::vector<std::string>& errors) {
    std::string message;
    for (const std::string& error : errors) {
        message += (message.empty() ? "" : "\n") + error;
    }
    return make_error(ErrorCode::InvalidArgument, message);
}

// The mesh shader's samplers sit 16 bindings after its textures in WGSL, as it has more than 8.
constexpr std::uint32_t kMeshSamplerShift = 16;

// A spatial shader's stage for every backend into stage, OpenGL's only with glsl; nothing, with
// errors added, where it doesn't compile.
void compile_mesh_stage(const render::ShaderSource& source, const Scan& found, MeshStage which,
                        bool vertex, render::ShaderStage& stage, bool glsl,
                        std::vector<std::string>& errors) {
    const EShLanguage language = vertex ? EShLangVertex : EShLangFragment;
    const char* entry = vertex ? "vs_main" : "fs_main";
    const Parts parts = spatial_parts(source, found, which, MeshTarget::Other, vertex);
    stage.spirv = to_spirv(parts, source, language, entry, errors);
    if (!errors.empty()) {
        return;
    }
    // Forward+'s clustered stage reads storage buffers, which Metal and WebGPU number apart.
    const bool apart = which == MeshStage::Clustered && !vertex;
    std::vector<std::uint32_t> msl_spirv;
    if (apart) {
        msl_spirv = to_spirv(spatial_parts(source, found, which, MeshTarget::Msl, vertex), source,
                             language, entry, errors);
    }
    const std::vector<std::uint32_t> wgsl_spirv =
        to_spirv(spatial_parts(source, found, which,
                               apart || vertex ? MeshTarget::Wgsl : MeshTarget::Other, vertex),
                 source, language, entry, errors, Samplers::Apart, kMeshSamplerShift);
    if (!errors.empty()) {
        return;
    }
    stage.wgsl = to_wgsl(wgsl_spirv, source.path, errors);
    if (!errors.empty()) {
        return;
    }
    try {
        stage.msl = to_msl(apart ? msl_spirv : stage.spirv);
        if (glsl) {
            stage.glsl = to_glsl(stage.spirv, vertex, kMeshVaryings, kMeshSamplerNames, true);
        }
    } catch (const std::exception& error) {
        errors.push_back(source.path + ": SPIRV-Cross can't translate it: " + error.what());
        return;
    }
    stage.hlsl = joined(parts, source);
}

Result<render::CompiledShader> compile_spatial(const render::ShaderSource& source) {
    const Scan found = scan(source.code);
    render::CompiledShader out;
    out.type = render::ShaderType::Spatial;
    out.normal_map_used = found.normal_map;
    out.alpha_used = found.alpha;
    out.alpha_scissor_used = found.alpha_scissor;
    out.vertex_used = found.vertex;
    std::vector<std::string> errors;
    const auto stage = [&](MeshStage which, bool vertex, render::ShaderStage& into, bool glsl) {
        if (errors.empty()) {
            compile_mesh_stage(source, found, which, vertex, into, glsl, errors);
        }
    };
    render::ShaderStage compatibility;
    stage(MeshStage::Compatibility, false, compatibility, true);
    stage(MeshStage::Clustered, false, out.clustered, false);
    stage(MeshStage::NormalRoughness, false, out.normal_roughness, false);
    stage(MeshStage::Motion, false, out.motion, false);
    render::ShaderStage vertex;
    if (found.vertex) {
        stage(MeshStage::Compatibility, true, vertex, true);
        stage(MeshStage::Motion, true, out.motion_vertex, false);
    }
    // Shadows run the shader where it moves the corners or cuts holes.
    if (out.shadows_used()) {
        stage(MeshStage::Shadow, true, out.shadow_vertex, true);
        stage(MeshStage::Shadow, false, out.shadow, true);
    }
    if (errors.empty()) {
        out.cpu_fragment = to_spirv(cpu_spatial_parts(source, found), source, EShLangFragment,
                                    "cpu_fragment", errors);
    }
    if (errors.empty() && found.vertex) {
        out.cpu_vertex =
            to_spirv(cpu_spatial_vertex_parts(source), source, EShLangVertex, "cpu_vertex", errors);
    }
    if (!errors.empty()) {
        return compile_errors(errors);
    }
    out.spirv_fragment = std::move(compatibility.spirv);
    out.msl_fragment = std::move(compatibility.msl);
    out.glsl_fragment = std::move(compatibility.glsl);
    out.wgsl_fragment = std::move(compatibility.wgsl);
    out.hlsl = std::move(compatibility.hlsl);
    out.spirv_vertex = std::move(vertex.spirv);
    out.msl_vertex = std::move(vertex.msl);
    out.glsl_vertex = std::move(vertex.glsl);
    out.wgsl_vertex = std::move(vertex.wgsl);
    out.hlsl_vertex = std::move(vertex.hlsl);
    for (const auto* module : {&out.cpu_vertex, &out.cpu_fragment}) {
        if (module->empty()) {
            continue;
        }
        if (auto loaded = render::ShaderVm::load(*module); !loaded) {
            return make_error(ErrorCode::InvalidArgument,
                              source.path + ": the CPU can't run it: " + loaded.error().message);
        }
    }
    return out;
}

} // namespace

Wrapped wrap(const render::ShaderSource& source) {
    const Scan found = scan(source.code);
    const auto join = [&](const Parts& parts) { return joined(parts, source); };
    Wrapped out;
    if (source.type == render::ShaderType::Spatial) {
        out.gpu =
            join(spatial_parts(source, found, MeshStage::Compatibility, MeshTarget::Other, false));
        if (found.vertex) {
            out.cpu_vertex = join(cpu_spatial_vertex_parts(source));
        }
        out.cpu_fragment = join(cpu_spatial_parts(source, found));
        return out;
    }
    out.gpu = join(gpu_parts(source, found));
    if (found.vertex) {
        out.cpu_vertex = join(cpu_vertex_parts(source));
    }
    out.cpu_fragment = join(cpu_fragment_parts(source, found));
    return out;
}

Result<render::CompiledShader> compile(const render::ShaderSource& source) {
    if (source.type == render::ShaderType::Spatial) {
        return compile_spatial(source);
    }
    const Scan found = scan(source.code);
    render::CompiledShader out;
    out.normal_map_used = found.normal_map;
    out.specular_used = found.specular;
    std::vector<std::string> errors;
    const Parts gpu = gpu_parts(source, found);
    out.spirv_vertex = to_spirv(gpu, source, EShLangVertex, "vs_main", errors);
    if (errors.empty()) {
        out.spirv_fragment = to_spirv(gpu, source, EShLangFragment, "fs_main", errors);
    }
    // WebGPU's: the same stages with their samplers apart.
    std::vector<std::uint32_t> wgsl_vertex;
    std::vector<std::uint32_t> wgsl_fragment;
    if (errors.empty()) {
        wgsl_vertex = to_spirv(gpu, source, EShLangVertex, "vs_main", errors, Samplers::Apart);
    }
    if (errors.empty()) {
        wgsl_fragment = to_spirv(gpu, source, EShLangFragment, "fs_main", errors, Samplers::Apart);
    }
    if (errors.empty()) {
        out.wgsl_vertex = to_wgsl(wgsl_vertex, source.path, errors);
    }
    if (errors.empty()) {
        out.wgsl_fragment = to_wgsl(wgsl_fragment, source.path, errors);
    }
    if (errors.empty() && found.vertex) {
        out.cpu_vertex =
            to_spirv(cpu_vertex_parts(source), source, EShLangVertex, "cpu_vertex", errors);
    }
    if (errors.empty()) {
        out.cpu_fragment = to_spirv(cpu_fragment_parts(source, found), source, EShLangFragment,
                                    "cpu_fragment", errors);
    }
    if (!errors.empty()) {
        return compile_errors(errors);
    }
    try {
        out.msl_vertex = to_msl(out.spirv_vertex);
        out.msl_fragment = to_msl(out.spirv_fragment);
        out.glsl_vertex = to_glsl(out.spirv_vertex, true);
        out.glsl_fragment = to_glsl(out.spirv_fragment, false);
    } catch (const std::exception& error) {
        return make_error(ErrorCode::InvalidArgument,
                          source.path + ": SPIRV-Cross can't translate it: " + error.what());
    }
    out.hlsl = wrap(source).gpu;
    // The CPU must run what the GPUs do, or frame captures and material.eval would differ.
    for (const auto* module : {&out.cpu_vertex, &out.cpu_fragment}) {
        if (module->empty()) {
            continue;
        }
        if (auto loaded = render::ShaderVm::load(*module); !loaded) {
            return make_error(ErrorCode::InvalidArgument,
                              source.path + ": the CPU can't run it: " + loaded.error().message);
        }
    }
    return out;
}

void install() {
    render::set_shader_compiler(compile);
}

} // namespace slash::shader_compiler
