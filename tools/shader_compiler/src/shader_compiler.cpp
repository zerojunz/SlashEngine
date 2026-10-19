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
#include <spirv_glsl.hpp>
#include <spirv_msl.hpp>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace slash::shader_compiler {

namespace detail {
extern const unsigned char kCanvasHlsl[];
extern const std::size_t kCanvasHlsl_size;
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
        }
    }
    return out;
}

std::string_view canvas_hlsl() {
    return {reinterpret_cast<const char*>(detail::kCanvasHlsl), detail::kCanvasHlsl_size};
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

// Statements that read the params and the time from an array of constants, and the texel size.
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
    out += "    TEXTURE_PIXEL_SIZE = " + std::string(texel) + ".xy;\n";
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

// The code between parts compiled to SPIR-V for a stage, or nothing with errors added.
std::vector<std::uint32_t> to_spirv(const Parts& parts, const render::ShaderSource& source,
                                    EShLanguage stage, const char* entry,
                                    std::vector<std::string>& errors) {
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
    shader.setTextureSamplerTransformMode(EShTexSampTransUpgradeTextureRemoveSampler);
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

std::string to_glsl(const std::vector<std::uint32_t>& spirv, bool vertex) {
    spirv_cross::CompilerGLSL compiler(spirv);
    auto options = compiler.get_common_options();
    options.es = true;
    options.version = 300;
    options.fragment.default_float_precision = spirv_cross::CompilerGLSL::Options::Highp;
    options.fragment.default_int_precision = spirv_cross::CompilerGLSL::Options::Highp;
    compiler.set_common_options(options);
    const spirv_cross::ShaderResources resources = compiler.get_shader_resources();
    // GLSL ES 3.00 can't place varyings by location, so both stages name them alike to link.
    constexpr std::array<std::string_view, 3> kVaryings{"v_uv", "v_color", "v_world"};
    for (const auto& variable : vertex ? resources.stage_outputs : resources.stage_inputs) {
        if (compiler.has_decoration(variable.id, spv::DecorationLocation)) {
            const std::uint32_t location =
                compiler.get_decoration(variable.id, spv::DecorationLocation);
            if (location < kVaryings.size()) {
                compiler.set_name(variable.id, std::string(kVaryings.at(location)));
            }
        }
    }
    for (const auto& image : resources.sampled_images) {
        const std::uint32_t binding = compiler.get_decoration(image.id, spv::DecorationBinding);
        if (binding < kSamplerNames.size()) {
            compiler.set_name(image.id, std::string(kSamplerNames.at(binding)));
        }
    }
    return compiler.compile();
}

} // namespace

Wrapped wrap(const render::ShaderSource& source) {
    const Scan found = scan(source.code);
    const auto join = [&](const Parts& parts) {
        return parts.before + "#line 1 \"" + source.path + "\"\n" + source.code + "\n#line 1 \"" +
               std::string(kWrapperName) + "\"\n" + parts.after;
    };
    Wrapped out;
    out.gpu = join(gpu_parts(source, found));
    if (found.vertex) {
        out.cpu_vertex = join(cpu_vertex_parts(source));
    }
    out.cpu_fragment = join(cpu_fragment_parts(source, found));
    return out;
}

Result<render::CompiledShader> compile(const render::ShaderSource& source) {
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
    if (errors.empty() && found.vertex) {
        out.cpu_vertex =
            to_spirv(cpu_vertex_parts(source), source, EShLangVertex, "cpu_vertex", errors);
    }
    if (errors.empty()) {
        out.cpu_fragment = to_spirv(cpu_fragment_parts(source, found), source, EShLangFragment,
                                    "cpu_fragment", errors);
    }
    if (!errors.empty()) {
        std::string message;
        for (const std::string& error : errors) {
            message += (message.empty() ? "" : "\n") + error;
        }
        return make_error(ErrorCode::InvalidArgument, message);
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
