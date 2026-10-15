#include "slash/render/material.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace slash::render {
namespace {

using Float4 = std::array<float, 4>;

constexpr std::array<std::string_view, 5> kBlendModes = {"mix", "add", "sub", "mul",
                                                         "premul_alpha"};

Error bad_instruction(std::size_t index, const std::string& message) {
    return make_error(ErrorCode::InvalidArgument,
                      "instruction " + std::to_string(index) + " " + message);
}

Float4 splat(float value) {
    return {value, value, value, value};
}

// Applies f to each component.
template <typename F>
Float4 each(const Float4& a, F f) {
    return {f(a[0]), f(a[1]), f(a[2]), f(a[3])};
}

template <typename F>
Float4 each(const Float4& a, const Float4& b, F f) {
    return {f(a[0], b[0]), f(a[1], b[1]), f(a[2], b[2]), f(a[3], b[3])};
}

template <typename F>
Float4 each(const Float4& a, const Float4& b, const Float4& c, F f) {
    return {f(a[0], b[0], c[0]), f(a[1], b[1], c[1]), f(a[2], b[2], c[2]), f(a[3], b[3], c[3])};
}

// The GPU's saturate, which turns NaN into 0.
float saturate(float x) {
    if (x > 0.0F) {
        return x < 1.0F ? x : 1.0F;
    }
    return 0.0F;
}

float dot(const Float4& a, const Float4& b, std::uint32_t count) {
    float sum = 0.0F;
    for (std::uint32_t i = 0; i < std::min(count, 4U); ++i) {
        sum += a.at(i) * b.at(i);
    }
    return sum;
}

Float4 rgb_to_hsv(const Float4& c) {
    const Float4 k{0.0F, -1.0F / 3.0F, 2.0F / 3.0F, -1.0F};
    const Float4 p = c[1] < c[2] ? Float4{c[2], c[1], k[3], k[2]} : Float4{c[1], c[2], k[0], k[1]};
    const Float4 q = c[0] < p[0] ? Float4{p[0], p[1], p[3], c[0]} : Float4{c[0], p[1], p[2], p[0]};
    const float d = q[0] - std::min(q[3], q[1]);
    constexpr float kE = 1.0e-10F;
    return {std::abs(q[2] + ((q[3] - q[1]) / ((6.0F * d) + kE))), d / (q[0] + kE), q[0], 0.0F};
}

Float4 hsv_to_rgb(const Float4& c) {
    const std::array<float, 3> k{1.0F, 2.0F / 3.0F, 1.0F / 3.0F};
    Float4 rgb{};
    for (std::size_t i = 0; i < 3; ++i) {
        const float shifted = c[0] + k.at(i);
        const float p = std::abs(((shifted - std::floor(shifted)) * 6.0F) - 3.0F);
        rgb.at(i) = (1.0F + ((saturate(p - 1.0F) - 1.0F) * c[1])) * c[2];
    }
    return rgb;
}

} // namespace

std::string_view blend_mode_name(BlendMode mode) {
    return kBlendModes.at(static_cast<std::size_t>(mode));
}

std::optional<BlendMode> parse_blend_mode(std::string_view name) {
    for (std::size_t i = 0; i < kBlendModes.size(); ++i) {
        if (kBlendModes.at(i) == name) {
            return static_cast<BlendMode>(i);
        }
    }
    return std::nullopt;
}

int op_arity(Op op) {
    switch (op) {
    case Op::Add:
    case Op::Sub:
    case Op::Mul:
    case Op::Div:
    case Op::Mod:
    case Op::Pow:
    case Op::Min:
    case Op::Max:
    case Op::Step:
    case Op::Atan2:
    case Op::Dot:
    case Op::Distance:
    case Op::Merge:
        return 2;
    case Op::Mix:
    case Op::Clamp:
    case Op::Smoothstep:
        return 3;
    default:
        return 1;
    }
}

Result<void> check_program(const MaterialProgram& program) {
    if (program.code.size() > kMaxInstructions) {
        return make_error(ErrorCode::InvalidArgument,
                          "a material program has at most " + std::to_string(kMaxInstructions) +
                              " instructions, not " + std::to_string(program.code.size()));
    }
    if (program.constants.size() > kMaxConstants) {
        return make_error(ErrorCode::InvalidArgument,
                          "a material program has at most " + std::to_string(kMaxConstants) +
                              " constants, not " + std::to_string(program.constants.size()));
    }
    std::array<bool, kRegisterCount> written{};
    for (std::uint32_t r = 0; r < kFirstFreeRegister; ++r) {
        written.at(r) = true;
    }
    for (std::size_t i = 0; i < program.code.size(); ++i) {
        const Instruction& instruction = program.code[i];
        const std::uint32_t op_number = instruction.head & 0xFFU;
        const std::uint32_t destination = (instruction.head >> 8U) & 0xFFU;
        const std::uint32_t aux = (instruction.head >> 16U) & 0xFFU;
        if (op_number >= kOpCount) {
            return bad_instruction(i, "has no op " + std::to_string(op_number));
        }
        const auto op = static_cast<Op>(op_number);
        const std::array<std::uint32_t, 3> operands{instruction.a, instruction.b, instruction.c};
        for (int n = 0; n < op_arity(op); ++n) {
            const std::uint32_t index = operands.at(static_cast<std::size_t>(n)) & 0xFFU;
            if (index < kConstantOperand) {
                if (!written.at(index)) {
                    return bad_instruction(i, "reads register " + std::to_string(index) +
                                                  " before anything writes it");
                }
            } else if (index - kConstantOperand >= program.constants.size()) {
                return bad_instruction(
                    i, "reads constant " + std::to_string(index - kConstantOperand) +
                           ", but the program has " + std::to_string(program.constants.size()));
            }
        }
        if (destination >= kRegisterCount) {
            return bad_instruction(i, "writes register " + std::to_string(destination) +
                                          "; registers are 0 to 31");
        }
        const bool counts =
            op == Op::Dot || op == Op::Length || op == Op::Distance || op == Op::Normalize;
        if (counts && (aux < 1 || aux > 4)) {
            return bad_instruction(i, "uses " + std::to_string(aux) + " components; use 1 to 4");
        }
        if (op == Op::Sample && aux >= kMaterialTextureSlots) {
            return bad_instruction(i, "samples texture slot " + std::to_string(aux) +
                                          "; slots are 0 to 3");
        }
        if (op == Op::Merge && aux > 15) {
            return bad_instruction(i, "merges with mask " + std::to_string(aux) +
                                          "; masks are 0 to 15");
        }
        written.at(destination) = true;
    }
    return {};
}

Float4 run_program(const MaterialProgram& program, const PixelInput& input,
                   const TextureSampler& sample) {
    std::array<Float4, kRegisterCount> registers{};
    registers.at(kUvRegister) = {input.uv[0], input.uv[1], 0.0F, 0.0F};
    registers.at(kColorRegister) = input.color;
    registers.at(kFragCoordRegister) = {input.frag_coord[0], input.frag_coord[1], 0.0F, 0.0F};
    registers.at(kTexelRegister) = {input.texel[0], input.texel[1], 0.0F, 0.0F};
    const auto fetch = [&](std::uint32_t operand) {
        const std::uint32_t index = operand & 0xFFU;
        const Float4& value = index < kConstantOperand
                                  ? registers.at(index)
                                  : program.constants.at(index - kConstantOperand);
        Float4 picked{};
        for (std::uint32_t i = 0; i < 4; ++i) {
            picked.at(i) = value.at((operand >> (8U + (2U * i))) & 3U);
        }
        return picked;
    };
    for (const Instruction& instruction : program.code) {
        const auto op = static_cast<Op>(instruction.head & 0xFFU);
        const std::uint32_t aux = (instruction.head >> 16U) & 0xFFU;
        const Float4 a = fetch(instruction.a);
        const Float4 b = op_arity(op) > 1 ? fetch(instruction.b) : Float4{};
        const Float4 c = op_arity(op) > 2 ? fetch(instruction.c) : Float4{};
        Float4 out = a;
        switch (op) {
        case Op::Mov:
            break;
        case Op::Add:
            out = each(a, b, [](float x, float y) { return x + y; });
            break;
        case Op::Sub:
            out = each(a, b, [](float x, float y) { return x - y; });
            break;
        case Op::Mul:
            out = each(a, b, [](float x, float y) { return x * y; });
            break;
        case Op::Div:
            out = each(a, b, [](float x, float y) { return x / y; });
            break;
        case Op::Mod:
            out = each(a, b, [](float x, float y) { return x - (y * std::floor(x / y)); });
            break;
        case Op::Pow:
            out = each(a, b, [](float x, float y) { return std::pow(std::max(x, 0.0F), y); });
            break;
        case Op::Min:
            out = each(a, b, [](float x, float y) { return std::min(x, y); });
            break;
        case Op::Max:
            out = each(a, b, [](float x, float y) { return std::max(x, y); });
            break;
        case Op::Step:
            out = each(a, b, [](float edge, float x) { return x >= edge ? 1.0F : 0.0F; });
            break;
        case Op::Atan2:
            out = each(a, b, [](float y, float x) { return std::atan2(y, x); });
            break;
        case Op::Abs:
            out = each(a, [](float x) { return std::abs(x); });
            break;
        case Op::Sign:
            out = each(a, [](float x) {
                if (x > 0.0F) {
                    return 1.0F;
                }
                return x < 0.0F ? -1.0F : 0.0F;
            });
            break;
        case Op::Floor:
            out = each(a, [](float x) { return std::floor(x); });
            break;
        case Op::Ceil:
            out = each(a, [](float x) { return std::ceil(x); });
            break;
        case Op::Round:
            out = each(a, [](float x) { return std::floor(x + 0.5F); });
            break;
        case Op::Fract:
            out = each(a, [](float x) { return x - std::floor(x); });
            break;
        case Op::Sqrt:
            out = each(a, [](float x) { return std::sqrt(x); });
            break;
        case Op::Sin:
            out = each(a, [](float x) { return std::sin(x); });
            break;
        case Op::Cos:
            out = each(a, [](float x) { return std::cos(x); });
            break;
        case Op::Tan:
            out = each(a, [](float x) { return std::tan(x); });
            break;
        case Op::Asin:
            out = each(a, [](float x) { return std::asin(x); });
            break;
        case Op::Acos:
            out = each(a, [](float x) { return std::acos(x); });
            break;
        case Op::Atan:
            out = each(a, [](float x) { return std::atan(x); });
            break;
        case Op::Exp:
            out = each(a, [](float x) { return std::exp(x); });
            break;
        case Op::Log:
            out = each(a, [](float x) { return std::log(x); });
            break;
        case Op::Saturate:
            out = each(a, saturate);
            break;
        case Op::OneMinus:
            out = each(a, [](float x) { return 1.0F - x; });
            break;
        case Op::Negate:
            out = each(a, [](float x) { return -x; });
            break;
        case Op::Mix:
            out = each(a, b, c, [](float x, float y, float t) { return x + ((y - x) * t); });
            break;
        case Op::Clamp:
            out = each(a, b, c,
                       [](float x, float lo, float hi) { return std::min(std::max(x, lo), hi); });
            break;
        case Op::Smoothstep:
            out = each(a, b, c, [](float edge0, float edge1, float x) {
                const float t = saturate((x - edge0) / (edge1 - edge0));
                return t * t * (3.0F - (2.0F * t));
            });
            break;
        case Op::Dot:
            out = splat(dot(a, b, aux));
            break;
        case Op::Length:
            out = splat(std::sqrt(dot(a, a, aux)));
            break;
        case Op::Distance: {
            const Float4 d = each(a, b, [](float x, float y) { return x - y; });
            out = splat(std::sqrt(dot(d, d, aux)));
            break;
        }
        case Op::Normalize: {
            const float size = std::sqrt(dot(a, a, aux));
            out = splat(0.0F);
            for (std::uint32_t i = 0; i < std::min(aux, 4U) && size > 0.0F; ++i) {
                out.at(i) = a.at(i) / size;
            }
            break;
        }
        case Op::Merge:
            for (std::uint32_t i = 0; i < 4; ++i) {
                out.at(i) = ((aux >> i) & 1U) != 0 ? b.at(i) : a.at(i);
            }
            break;
        case Op::Sample:
            out = sample(aux, a[0], a[1]);
            break;
        case Op::RgbToHsv:
            out = rgb_to_hsv(a);
            break;
        case Op::HsvToRgb:
            out = hsv_to_rgb(a);
            break;
        }
        registers.at((instruction.head >> 8U) & 31U) = out;
    }
    return registers.at(kUvRegister);
}

} // namespace slash::render
