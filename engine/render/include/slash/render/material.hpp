#pragma once

#include "slash/core/result.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <vector>

// Material programs: what the material shader (shaders/material.hlsl) runs for each pixel. A
// canvas material's node graph compiles to a short list of instructions over float4 registers,
// so materials need no shader compiler at run time and draw the same on every backend.
// run_program runs one on the CPU, the same way, for checks and tests.
//
// Registers 0 to 31 start as: 0 the texture coordinate (xy), 1 the vertex color, 2 the pixel's
// position in the target (xy, in pixels from the top left), 3 one over the size of the draw's
// texture (xy). The rest start undefined, so a program writes them before reading them. Operand
// indexes 32 to 63 read the program's constants. The program's color is register 0 at the end.
//
// A float is kept in every component of its register, so one op works on floats and vectors
// alike and a float mixes with a vector as if it were one of the same size.
namespace slash::render {

// How a draw's colors combine with what is under them, as Godot's canvas blend modes.
enum class BlendMode : std::uint8_t {
    Mix,         // ordinary alpha blending, the default
    Add,         // adds the color, times its alpha: glows, fire and light
    Sub,         // subtracts it, times its alpha
    Mul,         // multiplies what is under it by the color: shadows and tints
    PremulAlpha, // like Mix, for colors already multiplied by their alpha
};

// "mix", "add", "sub", "mul" or "premul_alpha".
[[nodiscard]] std::string_view blend_mode_name(BlendMode mode);
// The mode with this name, or nullopt.
[[nodiscard]] std::optional<BlendMode> parse_blend_mode(std::string_view name);

// The material shader's operations. a, b and c are the instruction's operands, read as float4
// values; ops on vectors use only the first aux components (1 to 4). The numbers are part of
// shaders/material.hlsl, so they never change.
enum class Op : std::uint8_t {
    Mov = 0,         // a
    Add = 1,         // a + b
    Sub = 2,         // a - b
    Mul = 3,         // a * b
    Div = 4,         // a / b
    Mod = 5,         // a - b * floor(a / b), as GLSL's mod
    Pow = 6,         // a to the power b; a below 0 counts as 0
    Min = 7,         // min(a, b)
    Max = 8,         // max(a, b)
    Step = 9,        // 1 where b >= a, else 0
    Atan2 = 10,      // the angle of (b, a): atan2(y = a, x = b)
    Abs = 11,        // |a|
    Sign = 12,       // -1, 0 or 1
    Floor = 13,      // rounds down
    Ceil = 14,       // rounds up
    Round = 15,      // rounds to the nearest whole number, halves up
    Fract = 16,      // a - floor(a)
    Sqrt = 17,       // square root
    Sin = 18,        // sine, of radians
    Cos = 19,        // cosine
    Tan = 20,        // tangent
    Asin = 21,       // arcsine
    Acos = 22,       // arccosine
    Atan = 23,       // arctangent
    Exp = 24,        // e to the power a
    Log = 25,        // natural logarithm
    Saturate = 26,   // a clamped to 0..1
    OneMinus = 27,   // 1 - a
    Negate = 28,     // -a
    Mix = 29,        // a + (b - a) * c
    Clamp = 30,      // min(max(a, b), c)
    Smoothstep = 31, // Hermite step from 0 at a to 1 at b, at c
    Dot = 32,        // dot product of the first aux components, in every component
    Length = 33,     // length of the first aux components of a, in every component
    Distance = 34,   // length(a - b), likewise
    Normalize = 35,  // a scaled to length 1 over the first aux components; 0 stays 0
    Merge = 36,      // component i from b where bit i of aux is set, else from a
    Sample = 37,     // texture slot aux (0 to 3) at the texture coordinate a.xy
    RgbToHsv = 38,   // a.rgb as hue, saturation and value, each 0..1
    HsvToRgb = 39,   // a.xyz, hue, saturation and value, as rgb
};
inline constexpr std::uint32_t kOpCount = 40;

// The operands an op reads: 1 to 3.
[[nodiscard]] int op_arity(Op op);

inline constexpr std::size_t kMaxInstructions = 64;
inline constexpr std::size_t kMaxConstants = 32;
inline constexpr std::uint32_t kRegisterCount = 32;
// Registers the shader fills before the program runs.
inline constexpr std::uint32_t kUvRegister = 0;
inline constexpr std::uint32_t kColorRegister = 1;
inline constexpr std::uint32_t kFragCoordRegister = 2;
inline constexpr std::uint32_t kTexelRegister = 3;
inline constexpr std::uint32_t kFirstFreeRegister = 4;
// Operand indexes from here on read constants.
inline constexpr std::uint32_t kConstantOperand = 32;
// Texture slot 0 is the draw's own texture; a material can bind three more.
inline constexpr std::size_t kMaterialTextureSlots = 4;

// Picks the source component for each of the four components: 0 x, 1 y, 2 z, 3 w.
using Swizzle = std::array<std::uint8_t, 4>;
inline constexpr Swizzle kIdentitySwizzle{0, 1, 2, 3};

// An operand: a register (0 to 31) or constant (32 to 63), and the components it reads.
[[nodiscard]] constexpr std::uint32_t operand(std::uint32_t index,
                                              const Swizzle& swizzle = kIdentitySwizzle) {
    return (index & 0xFFU) | (std::uint32_t{swizzle[0]} << 8U) |
           (std::uint32_t{swizzle[1]} << 10U) | (std::uint32_t{swizzle[2]} << 12U) |
           (std::uint32_t{swizzle[3]} << 14U);
}

// One instruction, laid out as the shader reads it: a uint4 of the op, destination register and
// aux, then three operands. Operands an op doesn't read are ignored.
struct Instruction {
    std::uint32_t head = 0; // op | destination << 8 | aux << 16
    std::uint32_t a = 0;
    std::uint32_t b = 0;
    std::uint32_t c = 0;

    bool operator==(const Instruction&) const = default;
};
static_assert(sizeof(Instruction) == 16);

[[nodiscard]] constexpr Instruction instruction(Op op, std::uint32_t destination, std::uint32_t a,
                                                std::uint32_t b = 0, std::uint32_t c = 0,
                                                std::uint32_t aux = 0) {
    return {.head = static_cast<std::uint32_t>(op) | ((destination & 0xFFU) << 8U) |
                    ((aux & 0xFFU) << 16U),
            .a = a,
            .b = b,
            .c = c};
}

struct MaterialProgram {
    std::vector<Instruction> code;
    std::vector<std::array<float, 4>> constants;

    bool operator==(const MaterialProgram&) const = default;
};

// Whether a program is one the shader can run: at most kMaxInstructions instructions and
// kMaxConstants constants, known ops, registers written before they are read, and operands,
// texture slots and component counts in range. The error names the instruction. Registers below
// filled start written; a program without textures (a particle graph's) may not sample.
[[nodiscard]] Result<void> check_program(const MaterialProgram& program,
                                         std::uint32_t filled = kFirstFreeRegister,
                                         bool textures = true);

// What a pixel starts with: see the registers above.
struct PixelInput {
    std::array<float, 2> uv{};
    std::array<float, 4> color{1.0F, 1.0F, 1.0F, 1.0F};
    std::array<float, 2> frag_coord{};
    std::array<float, 2> texel{};
};

// Samples texture slot 0 to 3 at a texture coordinate, as the GPU would.
using TextureSampler = std::function<std::array<float, 4>(std::uint32_t slot, float u, float v)>;

// Runs a program that passes check_program for one pixel, as the material shader does, and
// returns its color before blending. Floating-point results may differ from a GPU's in the last
// bits.
[[nodiscard]] std::array<float, 4>
run_program(const MaterialProgram& program, const PixelInput& input, const TextureSampler& sample);

using Registers = std::array<std::array<float, 4>, kRegisterCount>;

// Runs a program that passes check_program on registers filled as its kind needs. A program that
// samples needs sample. With portable, sin, cos, tan and their inverses, exp, log and pow give the
// same bits on every platform, as particles need; otherwise they are the C library's.
void run_instructions(const MaterialProgram& program, Registers& registers,
                      const TextureSampler* sample, bool portable);

} // namespace slash::render
