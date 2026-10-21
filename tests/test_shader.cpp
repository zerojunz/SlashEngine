#include "devices.hpp"

#include "slash/canvas/material.hpp"
#include "slash/core/json.hpp"
#include "slash/core/result.hpp"
#include "slash/platform/file.hpp"
#include "slash/render/device.hpp"
#include "slash/render/image.hpp"
#include "slash/render/shader.hpp"
#include "slash/shader_compiler/shader_compiler.hpp"

#include <doctest/doctest.h>

#if defined(_WIN32)
// Windows' HLSL compiler, which the Direct3D 12 device compiles a game's shaders with.
#include "../engine/render/src/hlsl_compiler.hpp"
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace canvas = slash::canvas;
namespace json = slash::json;
namespace render = slash::render;
namespace shader_compiler = slash::shader_compiler;
using slash::test::drawing_devices;

namespace {

using Color = std::array<float, 4>;
using Rgba = std::array<std::uint8_t, 4>;

render::CompiledShader compiled(const render::ShaderSource& source) {
    auto result = shader_compiler::compile(source);
    REQUIRE_MESSAGE(result.ok(), (result.ok() ? "" : result.error().message));
    return std::move(result).value();
}

std::string error_of(const render::ShaderSource& source) {
    auto result = shader_compiler::compile(source);
    return result.ok() ? "(compiled)" : result.error().message;
}

json::Value parse(std::string_view text) {
    auto parsed = json::parse(text);
    REQUIRE(parsed.ok());
    return std::move(parsed).value();
}

// Each error in a material document, as "path: message".
std::vector<std::string> errors(std::string_view text) {
    std::vector<std::string> out;
    for (const json::ValidationError& error : canvas::validate_material(parse(text))) {
        out.push_back(error.path + ": " + error.message);
    }
    return out;
}

std::string first_error(std::string_view text) {
    const std::vector<std::string> all = errors(text);
    return all.empty() ? "(valid)" : all.front();
}

canvas::CanvasMaterial material(std::string_view text) {
    auto parsed = canvas::parse_material(parse(text));
    REQUIRE_MESSAGE(parsed.ok(), (parsed.ok() ? "" : parsed.error().message));
    return std::move(parsed).value();
}

// Reads files from a map, as a project folder would.
slash::world::FileReader reader(const std::map<std::string, std::string, std::less<>>& files) {
    return [&files](std::string_view path) -> slash::Result<std::string> {
        const auto it = files.find(path);
        if (it == files.end()) {
            return slash::make_error(slash::ErrorCode::NotFound, "no file " + std::string(path));
        }
        return it->second;
    };
}

bool near(const Color& a, const Color& b) {
    constexpr float kTolerance = 0.001F;
    for (std::size_t i = 0; i < 4; ++i) {
        if (a.at(i) < b.at(i) - kTolerance || a.at(i) > b.at(i) + kTolerance) {
            return false;
        }
    }
    return true;
}

// The lowercase names canvas.hlsl declares where a shader's code can see them: its globals,
// functions and uniform buffers' fields.
std::vector<std::string> canvas_globals() {
    auto text = slash::platform::read_file(std::string(SLASH_TEST_DATA_DIR) +
                                           "/../../engine/render/shaders/canvas.hlsl");
    REQUIRE(text.ok());
    const std::regex declaration(
        R"(^\s*(?:static const )?[A-Za-z_][A-Za-z0-9_<>]*\s+([a-z_][a-z0-9_]*)\s*[\[;:=(])");
    std::vector<std::string> names;
    std::vector<char> blocks; // 'c' a uniform buffer, 's' a struct, 'f' anything else
    std::istringstream lines(text.value());
    std::string line;
    while (std::getline(lines, line)) {
        line = line.substr(0, line.find("//"));
        std::smatch match;
        if ((blocks.empty() || blocks.back() == 'c') &&
            std::regex_search(line, match, declaration)) {
            names.push_back(match[1]);
        }
        for (const char c : line) {
            if (c == '{') {
                const auto start = line.find_first_not_of(' ');
                const std::string_view head =
                    start == std::string::npos ? "" : std::string_view(line).substr(start);
                blocks.push_back(head.starts_with("cbuffer")  ? 'c'
                                 : head.starts_with("struct") ? 's'
                                                              : 'f');
            } else if (c == '}') {
                REQUIRE_FALSE(blocks.empty());
                blocks.pop_back();
            }
        }
    }
    return names;
}

// "" when every pixel of got is within 3 of want in each channel, but for a few along edges,
// where the devices' positions can differ in the last bits; else the first that differ.
std::string images_match(const render::Image& got, const render::Image& want) {
    REQUIRE(got.width == want.width);
    REQUIRE(got.height == want.height);
    std::string first;
    std::size_t differ = 0;
    for (std::uint32_t y = 0; y < got.height; ++y) {
        for (std::uint32_t x = 0; x < got.width; ++x) {
            const std::size_t at = ((std::size_t{y} * got.width) + x) * 4;
            bool close = true;
            for (std::size_t channel = 0; channel < 4; ++channel) {
                close = close && std::abs(int{got.pixels[at + channel]} -
                                          int{want.pixels[at + channel]}) <= 3;
            }
            if (!close) {
                ++differ;
                if (first.empty()) {
                    first = "(" + std::to_string(x) + ", " + std::to_string(y) + ") is " +
                            std::to_string(got.pixels[at]) + "," +
                            std::to_string(got.pixels[at + 1]) + "," +
                            std::to_string(got.pixels[at + 2]) + "," +
                            std::to_string(got.pixels[at + 3]) + ", expected " +
                            std::to_string(want.pixels[at]) + "," +
                            std::to_string(want.pixels[at + 1]) + "," +
                            std::to_string(want.pixels[at + 2]) + "," +
                            std::to_string(want.pixels[at + 3]);
                }
            }
        }
    }
    constexpr std::size_t kEdgePixels = 8;
    return differ <= kEdgePixels ? "" : std::to_string(differ) + " pixels differ; " + first;
}

} // namespace

// ---- Compiling ----

TEST_CASE("a game's shader compiles for every backend and the CPU") {
    const render::ShaderSource source{.path = "shaders/sway.hlsl",
                                      .code = R"(// Sways and tints.
void vertex() {
    VERTEX.x += offset * UV.y;
}

void fragment() {
    COLOR = texture(TEXTURE, UV) * tint * texture(ramp, UV);
}
)",
                                      .params = {{.name = "offset", .components = 1, .slot = 0},
                                                 {.name = "ramp", .components = 0, .slot = 1},
                                                 {.name = "tint", .components = 4, .slot = 1}}};
    const render::CompiledShader shader = compiled(source);
    CHECK_FALSE(shader.spirv_vertex.empty());
    CHECK_FALSE(shader.spirv_fragment.empty());
    CHECK(shader.spirv_vertex.front() == 0x07230203U); // SPIR-V's magic number
    CHECK(shader.msl_vertex.find("vs_main") != std::string::npos);
    CHECK(shader.msl_fragment.find("fs_main") != std::string::npos);
    CHECK(shader.glsl_vertex.starts_with("#version 300 es"));
    CHECK(shader.glsl_fragment.find("precision highp float;") != std::string::npos);
    // The GL device binds textures and varyings by these names.
    CHECK(shader.glsl_fragment.find("texture1") != std::string::npos);
    CHECK(shader.glsl_vertex.find("v_uv") != std::string::npos);
    // WebGPU's WGSL keeps SDL's bind groups, with each sampler 8 bindings after its texture.
    CHECK(shader.wgsl_vertex.find("@vertex") != std::string::npos);
    CHECK(shader.wgsl_vertex.find("@group(1u) @binding(1u)") != std::string::npos);
    CHECK(shader.wgsl_fragment.find("@fragment") != std::string::npos);
    CHECK(shader.wgsl_fragment.find("@group(2u) @binding(1u) var") != std::string::npos);
    CHECK(shader.wgsl_fragment.find("@group(2u) @binding(9u) var") != std::string::npos);
    // Direct3D 12's HLSL names the game's file in its line directives.
    CHECK(shader.hlsl.find("#line 1 \"shaders/sway.hlsl\"") != std::string::npos);
    CHECK_FALSE(shader.cpu_vertex.empty());
    CHECK_FALSE(shader.cpu_fragment.empty());
    CHECK(render::ShaderVm::load(shader.cpu_vertex).ok());
    CHECK(render::ShaderVm::load(shader.cpu_fragment).ok());
    CHECK_FALSE(shader.normal_map_used);
    CHECK_FALSE(shader.specular_used);

    // With no vertex(), the CPU has no vertex module; NORMAL_MAP or SPECULAR_SHININESS anywhere
    // makes lights use them.
    const render::CompiledShader lit =
        compiled({.path = "lit.hlsl",
                  .code = "void fragment() {\n    NORMAL_MAP = float3(1.0, 0.5, 0.5);\n"
                          "    SPECULAR_SHININESS.a = 0.5;\n}\n",
                  .params = {}});
    CHECK(lit.cpu_vertex.empty());
    CHECK(lit.normal_map_used);
    CHECK(lit.specular_used);
    // Neither function draws the texture times the vertex color; comments and strings don't
    // count.
    const render::CompiledShader plain = compiled(
        {.path = "plain.hlsl", .code = "// void fragment() { NORMAL_MAP = 1.0; }\n", .params = {}});
    CHECK(plain.cpu_vertex.empty());
    CHECK_FALSE(plain.normal_map_used);
}

TEST_CASE("shader errors name the game's file and line") {
    CHECK(error_of({.path = "shaders/bad.hlsl",
                    .code = "void fragment() {\n    COLOR = colr;\n}\n",
                    .params = {}}) == "shaders/bad.hlsl:2: 'colr' : unknown variable");
    // A function that doesn't fit the code around it says so.
    const std::string wrong = error_of(
        {.path = "shaders/bad.hlsl", .code = "void fragment(float4 c) {\n}\n", .params = {}});
    CHECK(wrong.starts_with("shaders/bad.hlsl: "));
    CHECK(wrong.find("fragment() and vertex() take nothing and return void") != std::string::npos);
    // A param the code uses wrongly.
    CHECK(error_of({.path = "a.hlsl",
                    .code = "void fragment() {\n\n    COLOR = amount.xyzw;\n}\n",
                    .params = {{.name = "amount", .components = 1, .slot = 0}}})
              .starts_with("a.hlsl:3: "));
}

TEST_CASE("render::compile_shader keeps compiles by source") {
    REQUIRE(render::has_shader_compiler());
    const render::ShaderSource source{
        .path = "kept.hlsl", .code = "void fragment() { COLOR.rgb *= 0.5; }\n", .params = {}};
    auto first = render::compile_shader(source);
    auto again = render::compile_shader(source);
    REQUIRE(first.ok());
    REQUIRE(again.ok());
    CHECK(first.value() == again.value());
    render::ShaderSource changed = source;
    changed.code += "\n";
    auto other = render::compile_shader(changed);
    REQUIRE(other.ok());
    CHECK(other.value() != first.value());
}

TEST_CASE("param names the code around a shader takes are reserved, and the rest compile") {
    const std::vector<std::string> globals = canvas_globals();
    CHECK(globals.size() > 20);
    for (const std::string& name : globals) {
        CAPTURE(name);
        CHECK(render::shader_name_reserved(name));
    }
    for (const char* name :
         {"float", "float3", "float4x4", "uint2", "half", "bool4", "return", "sampler", "lerp",
          "length", "smoothstep", "texture", "pixel", "gl_position", "a__b"}) {
        CAPTURE(name);
        CHECK(render::shader_name_reserved(name));
    }
    for (const char* name : {"amount", "edge_color", "speed", "float5", "floaty", "noise_map"}) {
        CAPTURE(name);
        CHECK_FALSE(render::shader_name_reserved(name));
    }

    // Every other lowercase name in the code around a shader compiles as a param. Params go 31
    // to a shader, the most it takes.
    const shader_compiler::Wrapped wrapped =
        shader_compiler::wrap({.path = "probe.hlsl", .code = "", .params = {}});
    std::set<std::string> free;
    for (const std::string* code : {&wrapped.gpu, &wrapped.cpu_fragment}) {
        std::string word;
        for (const char c : *code + " ") {
            if (std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_') {
                word += c;
                continue;
            }
            const bool lowercase = !word.empty() && std::ranges::all_of(word, [](char k) {
                return (k >= 'a' && k <= 'z') || (k >= '0' && k <= '9') || k == '_';
            });
            if (lowercase && (word[0] < '0' || word[0] > '9') &&
                !render::shader_name_reserved(word)) {
                free.insert(word);
            }
            word.clear();
        }
    }
    CHECK(free.size() > 20);
    std::vector<std::string> names(free.begin(), free.end());
    for (std::size_t first = 0; first < names.size(); first += render::kShaderTimeConstant) {
        render::ShaderSource source{.path = "probe.hlsl", .code = {}, .params = {}};
        std::string sum = "0.0";
        for (std::size_t i = first; i < std::min(names.size(), first + render::kShaderTimeConstant);
             ++i) {
            source.params.push_back(
                {.name = names[i], .components = 1, .slot = static_cast<std::uint32_t>(i - first)});
            sum += " + " + names[i];
        }
        source.code = "void fragment() {\n    COLOR.r += (" + sum + ") * 0.0;\n}\n";
        CAPTURE(source.code);
        CHECK(error_of(source) == "(compiled)");
    }
}

// A shader that takes a derivative after discarding some pixels, as HLSL allows.
render::CompiledShader sway() {
    return compiled({.path = "shaders/sway.hlsl",
                     .code = "void vertex() {\n    VERTEX.x += offset;\n}\n"
                             "void fragment() {\n    if (UV.x > 0.5) {\n        discard;\n    }\n"
                             "    COLOR = texture(ramp, UV) * ddx(UV.x) * TIME;\n}\n",
                     .params = {{.name = "offset", .components = 1, .slot = 0},
                                {.name = "ramp", .components = 0, .slot = 1}}});
}

TEST_CASE("WGSL takes derivatives where HLSL does") {
    // WGSL refuses derivatives in control flow that can differ between pixels, such as after a
    // discard, unless the check is off.
    const render::CompiledShader shader = sway();
    CHECK(shader.wgsl_fragment.find("diagnostic(off, derivative_uniformity)") != std::string::npos);
    CHECK(shader.wgsl_fragment.find("dpdx") != std::string::npos);
}

#if defined(_WIN32)
TEST_CASE("a game's shader compiles with Windows' HLSL compiler") {
    const render::CompiledShader shader = sway();
    for (const auto& [entry, target] : {std::pair{"vs_main", "vs_5_1"}, {"fs_main", "ps_5_1"}}) {
        const auto result = render::detail::compile_hlsl(shader.hlsl, entry, target);
        CHECK_MESSAGE(result.ok(), entry, ": ", (result.ok() ? "" : result.error().message));
    }
}
#endif

// ---- Materials ----

TEST_CASE("a shader material gives its params slots in name order and the time the last constant") {
    const canvas::CanvasMaterial dissolve = material(R"({
      "format": 1,
      "light_mode": "unshaded",
      "params": {
        "amount": {"type": "float", "value": 0.25, "min": 0, "max": 1},
        "edge": {"type": "color", "value": [1, 0.5, 0]},
        "noise_map": {"type": "texture", "value": "noise.png", "repeat": true},
        "offset": {"type": "vec2", "value": [3, 4]},
        "ramp": {"type": "texture"}
      },
      "shader": "shaders/dissolve.hlsl"
    })");
    CHECK(dissolve.shader == "shaders/dissolve.hlsl");
    CHECK(dissolve.light_mode == render::LightMode::Unshaded);
    CHECK(dissolve.program.code.empty());
    REQUIRE(dissolve.program.constants.size() == render::cpu_shader::kConstants);
    CHECK(dissolve.time_constant == render::kShaderTimeConstant);
    CHECK(dissolve.params.at("amount").slot == 0U);
    CHECK(dissolve.params.at("edge").slot == 1U);
    CHECK(dissolve.params.at("offset").slot == 2U);
    CHECK(dissolve.params.at("noise_map").slot == 1U);
    CHECK(dissolve.params.at("ramp").slot == 2U);
    CHECK(dissolve.program.constants[0] == Color{0.25F, 0.25F, 0.25F, 0.25F});
    CHECK(dissolve.program.constants[1] == Color{1.0F, 0.5F, 0.0F, 1.0F});
    CHECK(dissolve.program.constants[2] == Color{3.0F, 4.0F, 0.0F, 0.0F});
    CHECK(dissolve.compiled == nullptr);

    const render::ShaderSource source = canvas::shader_source(dissolve, "code");
    CHECK(source.path == "shaders/dissolve.hlsl");
    CHECK(source.code == "code");
    REQUIRE(source.params.size() == 5);
    CHECK(source.params[0] ==
          render::ShaderSource::Param{.name = "amount", .components = 1, .slot = 0});
    CHECK(source.params[1] ==
          render::ShaderSource::Param{.name = "edge", .components = 4, .slot = 1});
    CHECK(source.params[2] ==
          render::ShaderSource::Param{.name = "noise_map", .components = 0, .slot = 1});
    CHECK(source.params[3] ==
          render::ShaderSource::Param{.name = "offset", .components = 2, .slot = 2});

    // A sprite's params and the time go in their constants, and its textures in their slots.
    auto instance = canvas::instantiate(dissolve, {{"amount", json::Value(0.75)}}, 2.5);
    REQUIRE(instance.ok());
    CHECK(instance.value().shader == "shaders/dissolve.hlsl");
    CHECK(instance.value().program.constants[0] == Color{0.75F, 0.75F, 0.75F, 0.75F});
    CHECK(instance.value().program.constants[render::kShaderTimeConstant][0] == 2.5F);
    CHECK(instance.value().textures[0].path == "noise.png");
    CHECK(instance.value().textures[0].repeat);
    CHECK(instance.value().textures[1].path.empty());
}

TEST_CASE("shader material mistakes point at the bad value") {
    const std::string shader = R"("shader": "shaders/a.hlsl")";
    CHECK(first_error(R"({"format": 1, "shader": "shaders/a.glsl"})") ==
          "/shader: expected an .hlsl file's path inside the project folder, separated by '/', "
          "with no \".\" or \"..\" parts");
    CHECK(first_error(R"({"format": 1, "shader": "../a.hlsl"})").starts_with("/shader: "));
    CHECK(first_error(R"({"format": 1, "color": "color", )" + shader + "}") ==
          "/color: a material with a shader has no color; the shader colors the pixels");
    CHECK(first_error(R"({"format": 1, "nodes": {"a": {"op": "mul", "args": [1, 2]}}, )" + shader +
                      "}") ==
          "/nodes: a material with a shader has no nodes; the shader colors the pixels");
    CHECK(first_error(R"({"format": 1, "params": {"length": {"type": "float"}}, )" + shader +
                      "}") == "/params/length: \"length\" is taken in shaders, by HLSL or the "
                              "code around the shader; name the param something else");
    // A texture's sampler is named after it.
    CHECK(first_error(R"({"format": 1, "params": {"normal": {"type": "texture"}}, )" + shader + "}")
              .starts_with("/params/normal: \"normal\" is taken"));
    CHECK(first_error(R"({"format": 1, "params": {"normal": {"type": "float"}}, )" + shader +
                      "}") == "(valid)");
    std::string many = R"({"format": 1, "params": {)";
    for (int i = 0; i < 32; ++i) {
        many += (i == 0 ? "" : ", ") + std::string("\"p") + (i < 10 ? "0" : "") +
                std::to_string(i) + R"(": {"type": "float"})";
    }
    CHECK(first_error(many + "}, " + shader + "}") ==
          "/params/p31: a shader reads at most 31 number params");
    CHECK(
        first_error(R"({"format": 1, "params": {"a": {"type": "texture"}, "b": {"type": "texture"},
        "c": {"type": "texture"}, "d": {"type": "texture"}}, )" +
                    shader + "}") == "/params/d: a material samples at most 3 textures of its "
                                     "own, besides the sprite's");
    CHECK(first_error(R"({"format": 1, "compiled": {"cpu_fragment": ""}})") ==
          "/compiled: only a material with a shader has compiled code");
    CHECK(first_error(R"({"format": 1, "compiled": {"cpu_fragment": "%%"}, )" + shader + "}") ==
          "/compiled/cpu_fragment: expected SPIR-V in base64");
}

TEST_CASE("check_material_shader compiles a material's shader and reports each error at /shader") {
    std::map<std::string, std::string, std::less<>> files{
        {"shaders/good.hlsl", "void fragment() {\n    COLOR *= tint;\n}\n"},
        {"shaders/bad.hlsl", "void fragment() {\n    COLOR *= tnt;\n}\n"}};
    const auto check = [&](std::string_view file) {
        std::vector<std::string> out;
        for (const json::ValidationError& error : canvas::check_material_shader(
                 parse(R"({"format": 1, "params": {"tint": {"type": "color"}}, "shader": ")" +
                       std::string(file) + "\"}"),
                 reader(files))) {
            out.push_back(error.path + ": " + error.message);
        }
        return out;
    };
    CHECK(check("shaders/good.hlsl").empty());
    CHECK(check("shaders/bad.hlsl") ==
          std::vector<std::string>{"/shader: shaders/bad.hlsl:2: 'tnt' : unknown variable"});
    CHECK(check("shaders/gone.hlsl") ==
          std::vector<std::string>{"/shader: no file shaders/gone.hlsl"});
    // Graph materials have no shader to check.
    CHECK(canvas::check_material_shader(parse(R"({"format": 1})"), reader(files)).empty());
}

TEST_CASE("compiled shaders go into material files with the backends asked for, and come back") {
    const render::CompiledShader shader =
        compiled({.path = "shaders/a.hlsl",
                  .code = "void vertex() { VERTEX.y += 1.0; }\n"
                          "void fragment() { NORMAL_MAP = float3(0.5, 0.5, 1.0); }\n",
                  .params = {}});
    json::Value doc = parse(R"({"format": 1, "shader": "shaders/a.hlsl"})");
    doc["compiled"] = canvas::compiled_shader_json(shader);
    CHECK(canvas::validate_material(doc).empty());
    const canvas::CanvasMaterial everything = material(json::write(doc));
    REQUIRE(everything.compiled != nullptr);
    CHECK(*everything.compiled == shader);
    // The file's code is used as it is: there is nothing to read or compile.
    const std::map<std::string, std::string, std::less<>> none;
    auto found = canvas::compile_material_shader(everything, reader(none));
    REQUIRE(found.ok());
    CHECK(found.value() == everything.compiled);

    // Metal only: the CPU's modules are always kept.
    doc["compiled"] = canvas::compiled_shader_json(
        shader, {.spirv = false, .msl = true, .glsl = false, .hlsl = false});
    CHECK(doc.find("compiled")->find("spirv_fragment") == nullptr);
    CHECK(doc.find("compiled")->find("hlsl") == nullptr);
    CHECK(doc.find("compiled")->find("normal_map_used")->as_bool());
    const canvas::CanvasMaterial metal = material(json::write(doc));
    REQUIRE(metal.compiled != nullptr);
    CHECK(metal.compiled->spirv_fragment.empty());
    CHECK(metal.compiled->glsl_fragment.empty());
    CHECK(metal.compiled->msl_fragment == shader.msl_fragment);
    CHECK(metal.compiled->cpu_vertex == shader.cpu_vertex);
    CHECK(metal.compiled->cpu_fragment == shader.cpu_fragment);
    CHECK(metal.compiled->normal_map_used);
}

TEST_CASE("evaluate runs a shader's fragment() with its params, textures and discards") {
    std::map<std::string, std::string, std::less<>> files{
        {"shaders/cut.hlsl", R"(// Drops the right half; the left is the ramp times tint, plus
// the time in blue.
void fragment() {
    if (UV.x > cut) {
        discard;
    }
    COLOR = texture(ramp, UV) * tint * COLOR;
    COLOR.b = TIME * 0.1 + TEXTURE_PIXEL_SIZE.x;
}
)"}};
    canvas::CanvasMaterial cut = material(R"({
      "format": 1,
      "params": {
        "cut": {"type": "float", "value": 0.75},
        "ramp": {"type": "texture", "value": "ramp.png", "filter": "nearest"},
        "tint": {"type": "color", "value": [1, 0.5, 1, 1]}
      },
      "shader": "shaders/cut.hlsl"
    })");
    auto instance = canvas::instantiate(cut, {}, 2.0);
    REQUIRE(instance.ok());
    // A shader that isn't compiled can't run.
    const std::array<std::array<float, 2>, 3> uvs{{{0.1F, 0.5F}, {0.6F, 0.5F}, {0.9F, 0.5F}}};
    const render::Image ramp{.width = 2, .height = 1, .pixels = {255, 255, 0, 255, 0, 255, 0, 255}};
    const render::Image sprite{
        .width = 4, .height = 4, .pixels = std::vector<std::uint8_t>(64, 255)};
    const canvas::ImageSource images =
        [&](const std::string& path) -> slash::Result<const render::Image*> {
        return path == "ramp.png" ? &ramp : &sprite;
    };
    const canvas::PixelInputs inputs{.color = {1.0F, 1.0F, 1.0F, 0.5F}, .frag_coord = {}};
    auto missing = canvas::evaluate(instance.value(), "sprite.png", render::Filter::Nearest, inputs,
                                    uvs, images);
    REQUIRE_FALSE(missing.ok());
    CHECK(missing.error().message == "shaders/cut.hlsl: the shader isn't compiled");

    auto shader = canvas::compile_material_shader(cut, reader(files));
    REQUIRE_MESSAGE(shader.ok(), (shader.ok() ? "" : shader.error().message));
    instance.value().compiled = shader.value();
    auto colors = canvas::evaluate(instance.value(), "sprite.png", render::Filter::Nearest, inputs,
                                   uvs, images);
    REQUIRE_MESSAGE(colors.ok(), (colors.ok() ? "" : colors.error().message));
    REQUIRE(colors.value().size() == 3);
    // Yellow, then green, from the ramp; the time is 2 and the sprite 4 wide.
    CHECK(near(colors.value()[0], {1.0F, 0.5F, 0.45F, 0.5F}));
    CHECK(near(colors.value()[1], {0.0F, 0.5F, 0.45F, 0.5F}));
    CHECK(colors.value()[2] == Color{});
    // A sprite's param moves the cut.
    auto wide = canvas::instantiate(cut, {{"cut", json::Value(1.0)}}, 2.0);
    REQUIRE(wide.ok());
    wide.value().compiled = shader.value();
    colors =
        canvas::evaluate(wide.value(), "sprite.png", render::Filter::Nearest, inputs, uvs, images);
    REQUIRE(colors.ok());
    CHECK(near(colors.value()[2], {0.0F, 0.5F, 0.45F, 0.5F}));
}

// ---- Devices ----

TEST_CASE("every device draws a game's shader as the software device does") {
    const render::CompiledShader shader =
        compiled({.path = "shaders/probe.hlsl",
                  .code = R"(// Reads every built-in.
void vertex() {
    VERTEX.x += offset * UV.y; // the bottom leans right
    COLOR.g *= 0.5;
}

float stripe(float x) {
    return step(0.5, frac(x * 4.0));
}

void fragment() {
    if (UV.x > cut) {
        discard;
    }
    float4 shade = texture(ramp, UV * 2.0);
    COLOR = COLOR * shade * gain;
    COLOR.rgb += float3(stripe(FRAGCOORD.x / 32.0) * 0.25, TIME * 0.1,
                        TEXTURE_PIXEL_SIZE.x * 2.0);
}
)",
                  .params = {
                      {.name = "cut", .components = 1, .slot = 0},
                      {.name = "gain", .components = 4, .slot = 1},
                      {.name = "offset", .components = 1, .slot = 2},
                      {.name = "ramp", .components = 0, .slot = 1},
                  }});
    // A checker of two grays, and a ramp from dark to light that repeats.
    std::vector<std::uint8_t> checker;
    for (std::uint32_t y = 0; y < 4; ++y) {
        for (std::uint32_t x = 0; x < 4; ++x) {
            const std::uint8_t gray = ((x + y) % 2 == 0) ? 230 : 160;
            checker.insert(checker.end(), {gray, gray, gray, 255});
        }
    }
    const std::vector<std::uint8_t> ramp{40, 40, 40, 255, 120, 90, 60, 255, 200, 160, 120, 255};

    std::optional<render::Image> software;
    for (const auto& device : drawing_devices()) {
        CAPTURE(render::backend_name(device->backend()));
        auto made = device->create_shader(shader);
        REQUIRE_MESSAGE(made.ok(), (made.ok() ? "" : made.error().message));
        auto target = device->create_texture(
            {.width = 32, .height = 32, .filter = render::Filter::Nearest, .render_target = true});
        auto texture = device->create_texture(
            {.width = 4, .height = 4, .filter = render::Filter::Nearest, .render_target = false},
            checker);
        auto ramp_texture = device->create_texture(
            {.width = 3, .height = 1, .filter = render::Filter::Linear, .render_target = false},
            ramp);
        REQUIRE(target.ok());
        REQUIRE(texture.ok());
        REQUIRE(ramp_texture.ok());
        render::Material material;
        material.shader = made.value();
        material.program.constants.assign(render::cpu_shader::kConstants, {});
        material.program.constants[0] = {0.8F, 0.8F, 0.8F, 0.8F};     // cut
        material.program.constants[1] = {1.2F, 1.0F, 0.9F, 1.0F};     // gain
        material.program.constants[2] = {6.25F, 6.25F, 6.25F, 6.25F}; // offset
        material.program.constants[render::kShaderTimeConstant] = {1.5F, 0.0F, 0.0F, 0.0F};
        material.textures[0] = {.texture = ramp_texture.value(), .repeat = true};
        const std::array<std::uint8_t, 4> tint{255, 200, 220, 255};
        const std::vector<render::Vertex2D> quad{
            {.x = 3.0F, .y = 3.0F, .u = 0.0F, .v = 0.0F, .rgba = tint},
            {.x = 23.0F, .y = 3.0F, .u = 1.0F, .v = 0.0F, .rgba = tint},
            {.x = 3.0F, .y = 29.0F, .u = 0.0F, .v = 1.0F, .rgba = tint},
            {.x = 23.0F, .y = 3.0F, .u = 1.0F, .v = 0.0F, .rgba = tint},
            {.x = 23.0F, .y = 29.0F, .u = 1.0F, .v = 1.0F, .rgba = tint},
            {.x = 3.0F, .y = 29.0F, .u = 0.0F, .v = 1.0F, .rgba = tint}};
        REQUIRE(device
                    ->begin_frame({.target = target.value(),
                                   .clear = {.r = 0.0F, .g = 0.0F, .b = 0.0F, .a = 1.0F},
                                   .view = {}})
                    .ok());
        REQUIRE(device->draw(texture.value(), quad, material).ok());
        REQUIRE(device->end_frame().ok());
        auto image = device->read_texture(target.value());
        REQUIRE(image.ok());
        if (!software) {
            software = image.value();
            // The software device drew it: the top left is the checker times the tint, the
            // ramp and the gain, plus the time; the bottom leans right, and the right is cut.
            const auto at = [&](std::uint32_t x, std::uint32_t y) {
                const std::size_t i = ((std::size_t{y} * 32) + x) * 4;
                return Rgba{software->pixels[i], software->pixels[i + 1], software->pixels[i + 2],
                            software->pixels[i + 3]};
            };
            CHECK(at(1, 1) == Rgba{0, 0, 0, 255});
            CHECK(at(4, 4) != Rgba{0, 0, 0, 255});
            CHECK(at(4, 28) == Rgba{0, 0, 0, 255});  // leaned away
            CHECK(at(24, 28) != Rgba{0, 0, 0, 255}); // leaned in
            CHECK(at(21, 4) == Rgba{0, 0, 0, 255});  // cut
        } else {
            CHECK_MESSAGE(images_match(image.value(), *software).empty(),
                          images_match(image.value(), *software));
        }
        // A shader no longer made can't draw.
        device->destroy_shader(made.value());
        REQUIRE(device->begin_frame({.target = target.value(), .clear = {}, .view = {}}).ok());
        CHECK_FALSE(device->draw(texture.value(), quad, material).ok());
        REQUIRE(device->end_frame().ok());
    }
}
