// Live texture graphs: a graph's outputs as a game shader's fragment(), worked out per pixel as it
// draws instead of baked. Each pass of the compiled program becomes its own copy of
// shaders/texture_graph_ops.hlsl (the compute pass's ops, which the shader tools' compiler finds
// by name), with the pass's op, size and inputs as constants, so the shader compiler keeps only
// that op's code, and its inputs are the earlier passes' functions, called at the pixels they
// read. Settings that follow live parameters are found by compiling the graph again with each
// parameter moved, and must move with it in a straight line.

#include "slash/core/json.hpp"
#include "slash/procedural/texture_graph.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace slash::procedural {
namespace {

// Appends the pieces to out one at a time, with no strings made between them.
void append(std::string& out, std::initializer_list<std::string_view> pieces) {
    for (const std::string_view piece : pieces) {
        out += piece;
    }
}

using render::GraphOp;

// The steps a parameter moves by when the graph is compiled again: small, so settings with
// ranges stay inside them, and a power of two, so values move exactly.
constexpr float kProbeStep = 1.0F / 64.0F;
// How far a setting may stray from a straight line through the probes, against its size.
constexpr float kLinearTolerance = 1e-3F;

// A float as an HLSL literal that reads back as the same float.
std::string literal(float value) {
    std::string text = json::write(json::Value(static_cast<double>(value)), json::Style::Compact);
    if (text.find_first_of(".eE") == std::string::npos) {
        text += ".0";
    }
    return text;
}

// One component of a live parameter, and the HLSL that reads it.
struct LiveComponent {
    std::string param;
    std::size_t component = 0;
    std::string read;  // "lava_speed", "tint.y" or "TIME"
    float base = 0.0F; // the parameter's value in the graph
};

// A setting that follows live parameters: its value at the graph's values, plus each
// component's slope times how far the material moved it.
struct LiveSetting {
    float base = 0.0F;
    std::vector<std::pair<std::size_t, float>> slopes; // LiveComponent index, slope
};

// How far floats of these sizes can be apart from rounding alone, a few steps of the largest.
float rounding(float a, float b, float c) {
    return 16.0F * std::numeric_limits<float>::epsilon() *
           std::max({std::abs(a), std::abs(b), std::abs(c)});
}

std::string setting_code(const LiveSetting& setting, const std::vector<LiveComponent>& live) {
    std::string code = literal(setting.base);
    for (const auto& [index, slope] : setting.slopes) {
        const LiveComponent& component = live.at(index);
        code += " + " + literal(slope) + " * (" + component.read + " - " + literal(component.base) +
                ")";
    }
    return code;
}

bool same_shape(const render::TextureGraphProgram& a, const render::TextureGraphProgram& b) {
    if (a.width != b.width || a.height != b.height || a.tiling != b.tiling ||
        a.passes.size() != b.passes.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.passes.size(); ++i) {
        if (a.passes[i].op != b.passes[i].op || a.passes[i].out != b.passes[i].out ||
            a.passes[i].in != b.passes[i].in) {
            return false;
        }
    }
    return true;
}

// The graph compiled with one live parameter component moved by step.
Result<render::TextureGraphProgram> moved(const json::Value& doc, const GraphReader& reader,
                                          const json::Value& values,
                                          const std::vector<LiveComponent>& live,
                                          const std::vector<float>& steps) {
    json::Object overrides;
    for (std::size_t i = 0; i < live.size(); ++i) {
        if (steps[i] == 0.0F) {
            continue;
        }
        const LiveComponent& component = live[i];
        auto [it, added] = overrides.try_emplace(component.param, *values.find(component.param));
        json::Value& value = it->second;
        if (value.is_array()) {
            value.as_array().at(component.component) =
                static_cast<double>(component.base + steps[i]);
        } else {
            value = static_cast<double>(component.base + steps[i]);
        }
    }
    auto compiled = compile_texture_graph(doc, reader, overrides);
    if (!compiled) {
        return compiled.error();
    }
    return std::move(compiled.value().program);
}

// Evaluations of passes one evaluation of each pass makes, its own included: a pass reads its
// inputs at the pixels its op taps, and each tap is the input's pass worked out again.
std::vector<double> costs(const render::TextureGraphProgram& program,
                          const std::vector<std::array<int, 4>>& writers) {
    std::vector<double> cost(program.passes.size(), 1.0);
    for (std::size_t p = 0; p < program.passes.size(); ++p) {
        const render::GraphPass& pass = program.passes[p];
        const auto& params = pass.params;
        std::array<double, 4> taps{};
        switch (pass.op) {
        case GraphOp::Noise:
            cost[p] = params[25] > 0.5F ? 2.0 : 1.0;
            break;
        case GraphOp::Blur:
            taps[0] = 2.0 * std::clamp(std::trunc(params[0]), 0.0F, 256.0F) + 1.0;
            break;
        case GraphOp::Sharpen:
        case GraphOp::CurvatureFromHeight:
            taps[0] = 5.0;
            break;
        case GraphOp::Warp:
            taps[0] = 4.0;
            taps[1] = std::trunc(params[1]) == 1.0F ? 1.0 : 4.0;
            break;
        case GraphOp::Transform:
        case GraphOp::NormalFromHeight:
            taps[0] = 4.0;
            break;
        case GraphOp::EdgeDetect:
            taps[0] = 8.0;
            break;
        case GraphOp::AoFromHeight:
            taps[0] = 1.0 + 8.0 * std::clamp(std::trunc(params[0]), 1.0F, 64.0F);
            break;
        default:
            taps = {1.0, 1.0, 1.0, 1.0};
            break;
        }
        for (std::size_t k = 0; k < 4; ++k) {
            if (writers[p][k] >= 0) {
                cost[p] += taps[k] * cost[static_cast<std::size_t>(writers[p][k])];
            }
        }
    }
    return cost;
}

// What a spatial shader does with an output, by its name.
struct SpatialUse {
    std::string_view name;
    std::string_view code; // with "c" the output's color
};
constexpr std::array<SpatialUse, 9> kSpatialUses{{
    {"albedo", "ALBEDO = tg_linear(c.rgb);"},
    {"color", "ALBEDO = tg_linear(c.rgb);"},
    {"normal", "NORMAL_MAP = c.rgb;"},
    {"roughness", "ROUGHNESS = c.r;"},
    {"metallic", "METALLIC = c.r;"},
    {"ao", "AO = c.r;"},
    {"orm", "AO = c.r;\n    ROUGHNESS = c.g;\n    METALLIC = c.b;"},
    {"emission", "EMISSION = tg_linear(c.rgb);"},
    {"alpha", "ALPHA = c.r;"},
}};

const SpatialUse* spatial_use(std::string_view name) {
    for (const SpatialUse& use : kSpatialUses) {
        if (use.name == name) {
            return &use;
        }
    }
    return nullptr;
}

} // namespace

Result<std::string> texture_graph_shader(const json::Value& doc, const GraphReader& reader,
                                         render::ShaderType type,
                                         std::span<const render::ShaderSource::Param> params) {
    if (type == render::ShaderType::Compositor) {
        return make_error(ErrorCode::InvalidArgument,
                          "a texture graph draws canvas_item and spatial materials, not compositor "
                          "effects");
    }
    auto described = describe_texture_graph(doc, reader);
    if (!described) {
        return described.error();
    }
    for (const auto& [name, node] : described.value().find("nodes")->as_object()) {
        if (node.find("op")->as_string() == render::graph_op_name(GraphOp::Image)) {
            return make_error(ErrorCode::InvalidArgument,
                              "a live graph can't read image files; bake it, or make the image "
                              "from nodes");
        }
    }
    auto compiled = compile_texture_graph(doc, reader);
    if (!compiled) {
        return compiled.error();
    }
    const CompiledGraph& graph = compiled.value();
    const render::TextureGraphProgram& program = graph.program;

    // The parameters the material moves: its number params named as the graph's, and "time"
    // as the shader's TIME unless the material has a param of that name.
    json::Value values = json::Object{};
    const json::Value* listed = described.value().find("parameters");
    for (const auto& [name, item] : listed->as_object()) {
        if (const json::Value* value = item.find("value"); value != nullptr) {
            values[name] = *value;
        }
    }
    std::vector<LiveComponent> live;
    const auto add_live = [&](const std::string& name, std::string_view read,
                              std::uint32_t components) -> Result<void> {
        const json::Value* value = values.find(name);
        if (value == nullptr) {
            return {};
        }
        const std::size_t count = value->is_array() ? value->as_array().size() : 1;
        if (count != components) {
            return make_error(
                ErrorCode::InvalidArgument,
                "the material's param \"" + name + "\" has " + std::to_string(components) +
                    " numbers, but the graph's parameter has " + std::to_string(count));
        }
        for (std::size_t c = 0; c < count; ++c) {
            const json::Value& component = value->is_array() ? value->as_array()[c] : *value;
            std::string read_as(read);
            if (count > 1) {
                read_as += std::string(".") + "xyzw"[c];
            }
            live.push_back(
                {name, c, std::move(read_as), static_cast<float>(component.as_double())});
        }
        return {};
    };
    bool time_declared = false;
    for (const render::ShaderSource::Param& param : params) {
        time_declared = time_declared || param.name == "time";
        if (param.components == 0) {
            continue;
        }
        if (auto added = add_live(param.name, param.name, param.components); !added) {
            return added.error();
        }
    }
    if (!time_declared) {
        if (auto added = add_live("time", "TIME", 1); !added) {
            return added.error();
        }
    }

    // Each setting's slope for each live component, from compiles with it moved one and two
    // steps. A move that leaves the setting's range is tried the other way.
    std::vector<std::vector<LiveSetting>> settings(program.passes.size());
    for (std::size_t p = 0; p < program.passes.size(); ++p) {
        for (const float param : program.passes[p].params) {
            settings[p].push_back({param, {}});
        }
    }
    const auto shape_error = [](const LiveComponent& component) {
        return make_error(ErrorCode::InvalidArgument,
                          "parameter \"" + component.param +
                              "\" changes which nodes run or how they connect, so it can't "
                              "move as the material draws");
    };
    std::vector<float> combined(live.size(), 0.0F);
    for (std::size_t i = 0; i < live.size(); ++i) {
        std::optional<render::TextureGraphProgram> one;
        std::optional<render::TextureGraphProgram> two;
        float step = kProbeStep;
        for (const float direction : {1.0F, -1.0F}) {
            std::vector<float> steps(live.size(), 0.0F);
            steps[i] = direction * kProbeStep;
            auto first = moved(doc, reader, values, live, steps);
            steps[i] *= 2.0F;
            auto second = moved(doc, reader, values, live, steps);
            if (first && second) {
                one = std::move(first).value();
                two = std::move(second).value();
                step = direction * kProbeStep;
                break;
            }
        }
        if (!one) {
            return make_error(ErrorCode::InvalidArgument,
                              "parameter \"" + live[i].param +
                                  "\" can't move from its value as the material draws");
        }
        if (!same_shape(program, *one) || !same_shape(program, *two)) {
            return shape_error(live[i]);
        }
        combined[i] = step;
        for (std::size_t p = 0; p < program.passes.size(); ++p) {
            for (std::size_t j = 0; j < render::kGraphParams; ++j) {
                const float base = program.passes[p].params[j];
                const float d1 = one->passes[p].params[j] - base;
                const float d2 = two->passes[p].params[j] - base;
                if (d1 == 0.0F && d2 == 0.0F) {
                    continue;
                }
                if (std::abs(d2 - 2.0F * d1) >
                    (kLinearTolerance * std::max(std::abs(d1), std::abs(d2))) +
                        rounding(base, one->passes[p].params[j], two->passes[p].params[j])) {
                    return make_error(
                        ErrorCode::InvalidArgument,
                        "parameter \"" + live[i].param + "\" moves a setting of a \"" +
                            std::string(render::graph_op_name(program.passes[p].op)) +
                            "\" node other than in a straight line, so it can't move as the "
                            "material draws");
                }
                settings[p][j].slopes.emplace_back(i, d1 / step);
            }
        }
    }
    // Parameters that only work together, as one times another, show when all move at once.
    if (live.size() > 1) {
        auto together = moved(doc, reader, values, live, combined);
        if (together) {
            if (!same_shape(program, together.value())) {
                return shape_error(live.front());
            }
            for (std::size_t p = 0; p < program.passes.size(); ++p) {
                for (std::size_t j = 0; j < render::kGraphParams; ++j) {
                    float expected = program.passes[p].params[j];
                    for (const auto& [index, slope] : settings[p][j].slopes) {
                        expected += slope * combined[index];
                    }
                    const float actual = together.value().passes[p].params[j];
                    const float moved_by = std::abs(actual - program.passes[p].params[j]);
                    if (std::abs(actual - expected) >
                        (kLinearTolerance * moved_by) +
                            rounding(program.passes[p].params[j], actual, expected)) {
                        return make_error(
                            ErrorCode::InvalidArgument,
                            "parameters move a setting of a \"" +
                                std::string(render::graph_op_name(program.passes[p].op)) +
                                "\" node together other than by adding, so they can't move as "
                                "the material draws");
                    }
                }
            }
        }
    }

    // The outputs drawn, and the passes they need.
    struct Drawn {
        const GraphOutput* output = nullptr;
        std::string code; // for spatial outputs
        int pass = -1;
    };
    std::vector<Drawn> drawn;
    const auto writer_before = [&](std::uint32_t slot, std::size_t before) {
        for (std::size_t q = before; q-- > 0;) {
            if (program.passes[q].out == slot) {
                return static_cast<int>(q);
            }
        }
        return -1;
    };
    for (const GraphOutput& output : graph.outputs) {
        if (type == render::ShaderType::Canvas && !drawn.empty()) {
            break;
        }
        std::string code;
        if (type == render::ShaderType::Spatial) {
            const SpatialUse* use = spatial_use(output.name);
            if (use == nullptr) {
                continue;
            }
            code = use->code;
        }
        drawn.push_back(
            {&output, std::move(code), writer_before(output.slot, program.passes.size())});
    }
    if (drawn.empty()) {
        return make_error(ErrorCode::InvalidArgument,
                          "a spatial material's graph needs an output named albedo, color, "
                          "normal, roughness, metallic, ao, orm, emission or alpha");
    }
    std::vector<std::array<int, 4>> writers(program.passes.size(), {-1, -1, -1, -1});
    for (std::size_t p = 0; p < program.passes.size(); ++p) {
        for (std::size_t k = 0; k < render::kGraphInputs; ++k) {
            if (program.passes[p].in[k] != render::kNoGraphSlot) {
                writers[p][k] = writer_before(program.passes[p].in[k], p);
            }
        }
    }
    std::vector<bool> needed(program.passes.size(), false);
    for (const Drawn& each : drawn) {
        needed[static_cast<std::size_t>(each.pass)] = true;
    }
    for (std::size_t p = program.passes.size(); p-- > 0;) {
        if (!needed[p]) {
            continue;
        }
        for (const int writer : writers[p]) {
            if (writer >= 0) {
                needed[static_cast<std::size_t>(writer)] = true;
            }
        }
    }
    const std::vector<double> cost = costs(program, writers);
    for (const Drawn& each : drawn) {
        // Four texels a pixel, filtered.
        const double per_pixel = 4.0 * cost[static_cast<std::size_t>(each.pass)];
        if (per_pixel > static_cast<double>(kMaxLiveGraphCost)) {
            return make_error(ErrorCode::InvalidArgument,
                              "output \"" + each.output->name + "\" works out " +
                                  std::to_string(static_cast<std::int64_t>(per_pixel)) +
                                  " node pixels for each pixel drawn, more than " +
                                  std::to_string(kMaxLiveGraphCost) +
                                  "; bake it, or blur less before it");
        }
    }

    // The kinds of noise, fractal and domain warp the graph's noise uses, so the shader holds
    // only their code (FastNoiseLite's FNL_USED_*); all of them where a parameter moves a kind.
    unsigned used_noise = 0;
    unsigned used_fractal = 0;
    unsigned used_warp = 0;
    bool every_kind = false;
    for (std::size_t p = 0; p < program.passes.size(); ++p) {
        if (!needed[p] || program.passes[p].op != GraphOp::Noise) {
            continue;
        }
        const auto kind = [&](std::size_t index) {
            if (!settings[p][index].slopes.empty()) {
                every_kind = true;
            }
            return static_cast<unsigned>(std::clamp(program.passes[p].params[index], 0.0F, 7.0F));
        };
        used_noise |= 1U << kind(0);
        used_fractal |= 1U << kind(3);
        if (program.passes[p].params[12] > 0.5F || !settings[p][12].slopes.empty()) {
            used_warp |= 1U << kind(13);
            // The domain warp fractal types: none, progressive, independent.
            constexpr std::array<unsigned, 3> kWarpFractals{0, 4, 5};
            used_fractal |= 1U << kWarpFractals.at(std::min(kind(16), 2U));
        }
    }
    std::string out = "// A texture graph, worked out as it draws (texture_graph_shader).\n"
                      "#define FNL_STATIC_TABLES\n";
    if (!every_kind) {
        out += "#define FNL_USED_NOISE " + std::to_string(used_noise) +
               "\n#define FNL_USED_FRACTAL " + std::to_string(used_fractal) +
               "\n#define FNL_USED_WARP " + std::to_string(used_warp) + "\n";
    }
    out += "#include \"texture_graph_common.hlsl\"\n\n";
    const std::string width = std::to_string(program.width);
    const std::string height = std::to_string(program.height);
    for (std::size_t p = 0; p < program.passes.size(); ++p) {
        if (!needed[p]) {
            continue;
        }
        const render::GraphPass& pass = program.passes[p];
        const std::string prefix = "tg_p" + std::to_string(p) + "_";
        std::uint32_t mask = 0;
        for (std::size_t k = 0; k < render::kGraphInputs; ++k) {
            if (writers[p][k] >= 0) {
                mask |= 1U << k;
            }
        }
        out += "// Pass " + std::to_string(p) + ": " + std::string(render::graph_op_name(pass.op)) +
               "\n";
        out += "float " + prefix + "setting(uint i) {\n    switch (i) {\n";
        for (std::size_t j = 0; j < render::kGraphParams; ++j) {
            const LiveSetting& setting = settings[p][j];
            if (setting.base == 0.0F && setting.slopes.empty()) {
                continue;
            }
            out += "    case " + std::to_string(j) + "u: return " + setting_code(setting, live) +
                   ";\n";
        }
        out += "    default: return 0.0;\n    }\n}\n";
        out += "float4 " + prefix + "input(uint k, int x, int y) {\n";
        for (std::size_t k = 0; k < render::kGraphInputs; ++k) {
            if (writers[p][k] >= 0) {
                out += "    if (k == " + std::to_string(k) + "u) {\n        return tg_p" +
                       std::to_string(writers[p][k]) + "_run_pixel(x, y);\n    }\n";
            }
        }
        out += "    return float4(1.0, 1.0, 1.0, 1.0);\n}\n";
        out += "#define TG(name) " + prefix + "##name\n";
        out += "#define TG_OP " + std::to_string(static_cast<int>(pass.op)) + "\n";
        out += "#define TG_OP_VALUE " + std::to_string(static_cast<int>(pass.op)) + "u\n";
        out += "#define TG_PARAM(tg_i) " + prefix + "setting(tg_i)\n";
        append(out, {"#define TG_WIDTH ", width, "\n#define TG_HEIGHT ", height, "\n"});
        out += std::string("#define TG_TILING ") + (program.tiling ? "true" : "false") + "\n";
        out +=
            "#define TG_HAS_INPUT(tg_k) (((" + std::to_string(mask) + "u >> (tg_k)) & 1u) != 0u)\n";
        out += "#define TG_INPUT(tg_k, tg_x, tg_y) " + prefix + "input(tg_k, tg_x, tg_y)\n";
        out += "#include \"texture_graph_ops.hlsl\"\n";
        out += "#undef TG\n#undef TG_OP\n#undef TG_OP_VALUE\n#undef TG_PARAM\n#undef TG_WIDTH\n"
               "#undef TG_HEIGHT\n#undef TG_TILING\n#undef TG_HAS_INPUT\n#undef TG_INPUT\n\n";
    }

    // Each drawn output filtered as a texture of the graph's size is, each texel clamped to 0-1
    // as a baked texel is.
    const std::string wrap = program.tiling ? "tg_wrap_int(v, n)" : "clamp(v, 0, n - 1)";
    out += "int tg_wrap(int v, int n) {\n    return " + wrap + ";\n}\n";
    out += "float3 tg_linear(float3 c) {\n"
           "    // A polynomial approximation of sRGB to linear.\n"
           "    return c * (c * (c * 0.305306011 + 0.682171111) + 0.012522878);\n}\n";
    for (std::size_t o = 0; o < drawn.size(); ++o) {
        const Drawn& each = drawn[o];
        const std::string run = "tg_p" + std::to_string(each.pass) + "_run_pixel";
        const std::string texel = each.output->gray
                                      ? "float4(saturate(" + run + "(x, y).x).xxx, 1.0)"
                                      : "saturate(" + run + "(x, y))";
        const std::string index = std::to_string(o);
        append(out, {"float4 tg_texel_", index, "(int x, int y) {\n    x = tg_wrap(x, ", width,
                     ");\n    y = tg_wrap(y, ", height, ");\n    return ", texel, ";\n}\n"});
        // The four texels come from one call site in a loop the compiler keeps, so the shader
        // holds the graph once rather than four times.
        append(out, {"float4 tg_output_", index, "(float2 uv) {\n    float fx = uv.x * ", width,
                     ".0 - 0.5;\n    float fy = uv.y * ", height,
                     ".0 - 0.5;\n"
                     "    float x0 = floor(fx);\n"
                     "    float y0 = floor(fy);\n"
                     "    float tx = fx - x0;\n"
                     "    float ty = fy - y0;\n"
                     "    int x = (int)x0;\n"
                     "    int y = (int)y0;\n"
                     "    float4 a = float4(0.0, 0.0, 0.0, 0.0);\n"
                     "    float4 b = a;\n"
                     "    float4 c = a;\n"
                     "    float4 d = a;\n"
                     "    [loop] for (uint i = 0u; i < 4u; i++) {\n"
                     "        float4 t = tg_texel_",
                     index,
                     "(x + (int)(i & 1u), y + (int)(i >> 1u));\n"
                     "        if (i == 0u) {\n            a = t;\n        } else if (i == 1u) {\n"
                     "            b = t;\n        } else if (i == 2u) {\n            c = t;\n"
                     "        } else {\n            d = t;\n        }\n    }\n"
                     "    return lerp(lerp(a, b, tx), lerp(c, d, tx), ty);\n}\n"});
    }
    out += "\nvoid fragment() {\n";
    if (type == render::ShaderType::Canvas) {
        out += "    COLOR = tg_output_0(UV) * COLOR;\n";
    } else {
        for (std::size_t o = 0; o < drawn.size(); ++o) {
            out += "    {\n        float4 c = tg_output_" + std::to_string(o) + "(UV);\n        ";
            std::string code = drawn[o].code;
            for (std::size_t at = code.find("\n    "); at != std::string::npos;
                 at = code.find("\n    ", at + 9)) {
                code.replace(at, 5, "\n        ");
            }
            append(out, {code, "\n    }\n"});
        }
    }
    out += "}\n";
    return out;
}

} // namespace slash::procedural
