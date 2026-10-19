// ShaderVm: SPIR-V run on the CPU, for the software device and material.eval. Modules are
// decoded once into instructions over registers; a run executes them for up to four lanes at a
// time, each register holding one 32-bit word per lane. SPIR-V forbids recursion, so each id
// has one register for the whole run and a call needs only a return address.
//
// Lanes run together while they agree on every branch, which is what derivatives need. When they
// branch apart, each goes on alone to the end of the run, with derivatives of 0 from then on: a
// GPU's derivatives are undefined there anyway.

#include "slash/render/shader.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace slash::render {
namespace {

// SPIR-V's opcodes, storage classes and decorations used here, from the specification's binary
// section.
namespace spv {

enum Op : std::uint16_t {
    Nop = 0,
    Undef = 1,
    SourceContinued = 2,
    Source = 3,
    SourceExtension = 4,
    Name = 5,
    MemberName = 6,
    String = 7,
    Line = 8,
    Extension = 10,
    ExtInstImport = 11,
    ExtInst = 12,
    MemoryModel = 14,
    EntryPoint = 15,
    ExecutionMode = 16,
    Capability = 17,
    TypeVoid = 19,
    TypeBool = 20,
    TypeInt = 21,
    TypeFloat = 22,
    TypeVector = 23,
    TypeMatrix = 24,
    TypeImage = 25,
    TypeSampler = 26,
    TypeSampledImage = 27,
    TypeArray = 28,
    TypeRuntimeArray = 29,
    TypeStruct = 30,
    TypePointer = 32,
    TypeFunction = 33,
    ConstantTrue = 41,
    ConstantFalse = 42,
    Constant = 43,
    ConstantComposite = 44,
    ConstantNull = 46,
    SpecConstantTrue = 48,
    SpecConstantFalse = 49,
    SpecConstant = 50,
    SpecConstantComposite = 51,
    Function = 54,
    FunctionParameter = 55,
    FunctionEnd = 56,
    FunctionCall = 57,
    Variable = 59,
    Load = 61,
    Store = 62,
    CopyMemory = 63,
    AccessChain = 65,
    InBoundsAccessChain = 66,
    Decorate = 71,
    MemberDecorate = 72,
    VectorExtractDynamic = 77,
    VectorInsertDynamic = 78,
    VectorShuffle = 79,
    CompositeConstruct = 80,
    CompositeExtract = 81,
    CompositeInsert = 82,
    CopyObject = 83,
    Transpose = 84,
    SampledImage = 86,
    ImageSampleImplicitLod = 87,
    ImageSampleExplicitLod = 88,
    ImageFetch = 95,
    Image = 100,
    ImageQuerySizeLod = 103,
    ImageQuerySize = 104,
    ImageQueryLevels = 106,
    ConvertFToU = 109,
    ConvertFToS = 110,
    ConvertSToF = 111,
    ConvertUToF = 112,
    Bitcast = 124,
    SNegate = 126,
    FNegate = 127,
    IAdd = 128,
    FAdd = 129,
    ISub = 130,
    FSub = 131,
    IMul = 132,
    FMul = 133,
    UDiv = 134,
    SDiv = 135,
    FDiv = 136,
    UMod = 137,
    SRem = 138,
    SMod = 139,
    FRem = 140,
    FMod = 141,
    VectorTimesScalar = 142,
    MatrixTimesScalar = 143,
    VectorTimesMatrix = 144,
    MatrixTimesVector = 145,
    MatrixTimesMatrix = 146,
    OuterProduct = 147,
    Dot = 148,
    Any = 154,
    All = 155,
    IsNan = 156,
    IsInf = 157,
    LogicalEqual = 164,
    LogicalNotEqual = 165,
    LogicalOr = 166,
    LogicalAnd = 167,
    LogicalNot = 168,
    Select = 169,
    IEqual = 170,
    INotEqual = 171,
    UGreaterThan = 172,
    SGreaterThan = 173,
    UGreaterThanEqual = 174,
    SGreaterThanEqual = 175,
    ULessThan = 176,
    SLessThan = 177,
    ULessThanEqual = 178,
    SLessThanEqual = 179,
    FOrdEqual = 180,
    FUnordEqual = 181,
    FOrdNotEqual = 182,
    FUnordNotEqual = 183,
    FOrdLessThan = 184,
    FUnordLessThan = 185,
    FOrdGreaterThan = 186,
    FUnordGreaterThan = 187,
    FOrdLessThanEqual = 188,
    FUnordLessThanEqual = 189,
    FOrdGreaterThanEqual = 190,
    FUnordGreaterThanEqual = 191,
    ShiftRightLogical = 194,
    ShiftRightArithmetic = 195,
    ShiftLeftLogical = 196,
    BitwiseOr = 197,
    BitwiseXor = 198,
    BitwiseAnd = 199,
    Not = 200,
    BitFieldInsert = 201,
    BitFieldSExtract = 202,
    BitFieldUExtract = 203,
    BitReverse = 204,
    BitCount = 205,
    DPdx = 207,
    DPdy = 208,
    Fwidth = 209,
    DPdxFine = 210,
    DPdyFine = 211,
    FwidthFine = 212,
    DPdxCoarse = 213,
    DPdyCoarse = 214,
    FwidthCoarse = 215,
    Phi = 245,
    LoopMerge = 246,
    SelectionMerge = 247,
    Label = 248,
    Branch = 249,
    BranchConditional = 250,
    Switch = 251,
    Kill = 252,
    Return = 253,
    ReturnValue = 254,
    Unreachable = 255,
    NoLine = 317,
    ModuleProcessed = 330,
    ExecutionModeId = 331,
    DecorateId = 332,
    CopyLogical = 400,
    TerminateInvocation = 4416,
    DemoteToHelperInvocation = 5380,
    IsHelperInvocation = 5381,
    DecorateString = 5632,
    MemberDecorateString = 5633,
};

enum Storage : std::uint8_t {
    UniformConstant = 0,
    Input = 1,
    Uniform = 2,
    Output = 3,
    Private = 6,
    FunctionStorage = 7,
};

enum Decoration : std::uint8_t {
    RowMajor = 4,
    ColMajor = 5,
    ArrayStride = 6,
    MatrixStride = 7,
    BuiltIn = 11,
    Location = 30,
    Binding = 33,
    Offset = 35,
};

// GLSL.std.450's instructions.
enum Glsl : std::uint8_t {
    Round = 1,
    RoundEven = 2,
    Trunc = 3,
    FAbs = 4,
    SAbs = 5,
    FSign = 6,
    SSign = 7,
    Floor = 8,
    Ceil = 9,
    Fract = 10,
    Radians = 11,
    Degrees = 12,
    Sin = 13,
    Cos = 14,
    Tan = 15,
    Asin = 16,
    Acos = 17,
    Atan = 18,
    Sinh = 19,
    Cosh = 20,
    Tanh = 21,
    Asinh = 22,
    Acosh = 23,
    Atanh = 24,
    Atan2 = 25,
    Pow = 26,
    Exp = 27,
    Log = 28,
    Exp2 = 29,
    Log2 = 30,
    Sqrt = 31,
    InverseSqrt = 32,
    Determinant = 33,
    MatrixInverse = 34,
    Modf = 35,
    ModfStruct = 36,
    FMin = 37,
    UMin = 38,
    SMin = 39,
    FMax = 40,
    UMax = 41,
    SMax = 42,
    FClamp = 43,
    UClamp = 44,
    SClamp = 45,
    FMix = 46,
    Step = 48,
    SmoothStep = 49,
    Fma = 50,
    Frexp = 51,
    FrexpStruct = 52,
    Ldexp = 53,
    PackSnorm4x8 = 54,
    PackUnorm4x8 = 55,
    PackSnorm2x16 = 56,
    PackUnorm2x16 = 57,
    PackHalf2x16 = 58,
    UnpackSnorm2x16 = 60,
    UnpackUnorm2x16 = 61,
    UnpackHalf2x16 = 62,
    UnpackSnorm4x8 = 63,
    UnpackUnorm4x8 = 64,
    Length = 66,
    Distance = 67,
    Cross = 68,
    Normalize = 69,
    FaceForward = 70,
    Reflect = 71,
    Refract = 72,
    FindILsb = 73,
    FindSMsb = 74,
    FindUMsb = 75,
    NMin = 79,
    NMax = 80,
    NClamp = 81,
};

// Image operand bits.
constexpr std::uint32_t kBias = 0x1;
constexpr std::uint32_t kLod = 0x2;
constexpr std::uint32_t kGrad = 0x4;
constexpr std::uint32_t kConstOffset = 0x8;
constexpr std::uint32_t kOffset = 0x10;
constexpr std::uint32_t kConstOffsets = 0x20;
constexpr std::uint32_t kSample = 0x40;
constexpr std::uint32_t kMinLod = 0x80;

} // namespace spv

constexpr std::uint32_t kMagic = 0x07230203;
constexpr std::uint8_t kAllLanes = 0xF;
constexpr std::uint32_t kSpareWords = 16;

enum class Kind : std::uint8_t {
    None,
    Void,
    Bool,
    Int,
    Float,
    Vector,
    Matrix,
    Array,
    Struct,
    Pointer,
    Function,
    Image,
    Sampler,
    SampledImage,
};

struct TypeInfo {
    Kind kind = Kind::None;
    bool is_signed = false;
    std::uint32_t element = 0; // vector, matrix and array elements; a pointer's pointee
    std::uint32_t count = 0;   // components, columns or array length
    std::uint32_t words = 0;   // 32-bit words in a value, every component flattened
    std::uint32_t storage = 0; // a pointer's storage class
    std::vector<std::uint32_t> members;
    std::vector<std::uint32_t> member_words; // where each member starts among the words
};

struct MemberLayout {
    std::optional<std::uint32_t> offset;
    std::uint32_t matrix_stride = 0;
    bool row_major = false;
};

struct Decorations {
    std::optional<std::uint32_t> location;
    std::optional<std::uint32_t> binding;
    std::optional<std::uint32_t> builtin;
    std::uint32_t array_stride = 0;
    std::vector<MemberLayout> members;
};

struct VariableInfo {
    std::uint32_t storage = 0;
    std::uint32_t type = 0; // the pointee
    std::uint32_t words = 0;
    std::uint32_t base = 0; // in memory, for private, function, input and output variables
    std::uint32_t binding = 0;
    std::optional<std::uint32_t> location;
    std::uint32_t initializer = 0;
    std::vector<std::uint32_t> byte_offsets; // uniform buffers: each word's byte offset
};

struct Inst {
    std::uint16_t op = 0;
    std::uint16_t count = 0;  // operands
    std::uint32_t first = 0;  // in operands
    std::uint32_t result = 0; // its result's id, or 0
    std::uint32_t words = 0;  // its result's words
};

struct FunctionInfo {
    std::uint32_t first_label = 0;
    std::vector<std::uint32_t> params;
};

// A pointer is a variable and a word in it, kept in one register word.
constexpr std::uint32_t kPointerShift = 16;
constexpr std::uint32_t kPointerMask = 0xFFFF;

Error bad(const std::string& message) {
    return make_error(ErrorCode::InvalidArgument, message);
}

std::string literal_string(std::span<const std::uint32_t> words) {
    std::string out;
    for (const std::uint32_t word : words) {
        for (std::uint32_t i = 0; i < 4; ++i) {
            const auto c = static_cast<char>((word >> (8U * i)) & 0xFFU);
            if (c == '\0') {
                return out;
            }
            out.push_back(c);
        }
    }
    return out;
}

// A value's bits as another type of the same size. std::bit_cast would do, but the glibc 2.31
// sysroot Linux exports build against has libstdc++ 10, which lacks it.
template <typename To, typename From>
To same_bits(From value) {
    static_assert(sizeof(To) == sizeof(From));
    To out{};
    std::memcpy(&out, &value, sizeof(out));
    return out;
}

float to_float(std::uint32_t bits) {
    return same_bits<float>(bits);
}

std::uint32_t from_float(float value) {
    return same_bits<std::uint32_t>(value);
}

std::int32_t to_int(std::uint32_t bits) {
    return same_bits<std::int32_t>(bits);
}

std::uint32_t from_int(std::int32_t value) {
    return same_bits<std::uint32_t>(value);
}

// float to int as Direct3D converts: toward 0, saturating, NaN to 0.
std::int32_t float_to_int(float value) {
    if (std::isnan(value)) {
        return 0;
    }
    if (value >= 2147483647.0F) {
        return std::numeric_limits<std::int32_t>::max();
    }
    if (value <= -2147483648.0F) {
        return std::numeric_limits<std::int32_t>::min();
    }
    return static_cast<std::int32_t>(value);
}

std::uint32_t float_to_uint(float value) {
    if (std::isnan(value) || value <= 0.0F) {
        return 0;
    }
    if (value >= 4294967295.0F) {
        return std::numeric_limits<std::uint32_t>::max();
    }
    return static_cast<std::uint32_t>(value);
}

// IEEE half precision, rounded to nearest even, as GPUs pack it.
std::uint32_t to_half(float value) {
    const std::uint32_t bits = from_float(value);
    const std::uint32_t sign = (bits >> 16U) & 0x8000U;
    const std::uint32_t exponent = (bits >> 23U) & 0xFFU;
    std::uint32_t mantissa = bits & 0x7FFFFFU;
    if (exponent == 0xFFU) {
        return sign | 0x7C00U | (mantissa != 0 ? 0x200U : 0U);
    }
    const int e = static_cast<int>(exponent) - 127 + 15;
    if (e >= 31) {
        return sign | 0x7C00U;
    }
    if (e <= 0) {
        if (e < -10) {
            return sign;
        }
        mantissa |= 0x800000U;
        const auto shift = static_cast<std::uint32_t>(14 - e);
        std::uint32_t half = mantissa >> shift;
        const std::uint32_t rest = mantissa & ((1U << shift) - 1U);
        const std::uint32_t halfway = 1U << (shift - 1U);
        if (rest > halfway || (rest == halfway && (half & 1U) != 0)) {
            ++half;
        }
        return sign | half;
    }
    std::uint32_t half = (static_cast<std::uint32_t>(e) << 10U) | (mantissa >> 13U);
    const std::uint32_t rest = mantissa & 0x1FFFU;
    if (rest > 0x1000U || (rest == 0x1000U && (half & 1U) != 0)) {
        ++half;
    }
    return sign | half;
}

float from_half(std::uint32_t half) {
    const std::uint32_t sign = (half & 0x8000U) << 16U;
    const std::uint32_t exponent = (half >> 10U) & 0x1FU;
    std::uint32_t mantissa = half & 0x3FFU;
    if (exponent == 0) {
        if (mantissa == 0) {
            return to_float(sign);
        }
        int e = -1;
        do {
            ++e;
            mantissa <<= 1U;
        } while ((mantissa & 0x400U) == 0);
        mantissa &= 0x3FFU;
        return to_float(sign | (static_cast<std::uint32_t>(127 - 15 - e) << 23U) |
                        (mantissa << 13U));
    }
    if (exponent == 0x1FU) {
        return to_float(sign | 0x7F800000U | (mantissa << 13U));
    }
    return to_float(sign | ((exponent - 15U + 127U) << 23U) | (mantissa << 13U));
}

float determinant2(float a, float b, float c, float d) {
    return (a * d) - (b * c);
}

// A column-major n x n matrix's determinant, by cofactors.
float determinant(const std::vector<float>& m, std::uint32_t n) {
    const auto at = [&](std::uint32_t column, std::uint32_t row) {
        return m.at((column * n) + row);
    };
    if (n == 2) {
        return determinant2(at(0, 0), at(1, 0), at(0, 1), at(1, 1));
    }
    float total = 0.0F;
    for (std::uint32_t column = 0; column < n; ++column) {
        std::vector<float> minor;
        for (std::uint32_t c = 0; c < n; ++c) {
            if (c == column) {
                continue;
            }
            for (std::uint32_t r = 1; r < n; ++r) {
                minor.push_back(at(c, r));
            }
        }
        const float cofactor = determinant(minor, n - 1) * at(column, 0);
        total += (column % 2 == 0) ? cofactor : -cofactor;
    }
    return total;
}

} // namespace

struct ShaderVm::Module {
    std::vector<TypeInfo> types;
    std::vector<Decorations> decorations;
    std::vector<std::uint32_t> type_of; // each id's result type
    std::vector<std::uint32_t> base;    // each value's first register word
    std::vector<std::uint32_t> var_of;  // each variable id's index in variables
    std::vector<std::uint32_t> label_pc;
    std::vector<FunctionInfo> functions; // by function id
    std::vector<VariableInfo> variables{1};
    std::vector<Inst> code;
    std::vector<std::uint32_t> operands;
    std::vector<std::uint32_t> regs;   // word w of value v, lane l: (base[v] + w) * 4 + l
    std::vector<std::uint32_t> memory; // variables, laid out likewise
    std::uint32_t register_words = 0;
    std::uint32_t memory_words = 0;
    std::uint32_t entry = 0;
    std::uint32_t glsl = 0;
    std::vector<std::uint32_t> ignored_sets;

    // A run's state.
    struct Frame {
        std::uint32_t return_pc = 0;
        std::uint32_t block = 0;
        std::uint32_t result = 0;
    };
    struct Thread {
        std::uint8_t mask = 0;
        std::uint32_t pc = 0;
        std::uint32_t block = 0;
        std::vector<Frame> stack;
    };
    Lanes* lanes = nullptr;
    std::span<const std::span<const std::byte>> uniforms;
    const Textures* textures = nullptr;
    std::array<std::uint64_t, kLanes> steps{};
    std::vector<std::uint32_t> phi_ids;
    std::vector<std::uint32_t> phi_values;

    // A value's word in a lane. Modules come from the shader tools, but an id or word out of
    // range reads a spare word rather than past the registers.
    [[nodiscard]] std::uint32_t& reg(std::uint32_t id, std::uint32_t word, std::size_t lane) {
        const std::size_t index =
            ((std::size_t{id < base.size() ? base[id] : register_words} + word) * kLanes) + lane;
        return regs[index < regs.size() ? index : regs.size() - kLanes + lane];
    }
    // A variable's word in a lane, for loads and copies.
    [[nodiscard]] std::uint32_t read(const VariableInfo& variable, std::uint32_t word,
                                     std::size_t lane) const {
        if (variable.words == 0) {
            return 0;
        }
        word = std::min(word, variable.words - 1);
        switch (variable.storage) {
        case spv::Uniform: {
            std::uint32_t bits = 0;
            if (variable.binding < uniforms.size() && word < variable.byte_offsets.size()) {
                const std::span<const std::byte> bytes = uniforms[variable.binding];
                const std::uint32_t byte = variable.byte_offsets[word];
                if (std::size_t{byte} + 4 <= bytes.size()) {
                    std::memcpy(&bits, bytes.data() + byte, 4);
                }
            }
            return bits;
        }
        case spv::UniformConstant:
            return variable.binding;
        default:
            return memory[((std::size_t{variable.base} + word) * kLanes) + lane];
        }
    }
    [[nodiscard]] float f(std::uint32_t id, std::uint32_t word, std::size_t lane) {
        return to_float(reg(id, word, lane));
    }
    [[nodiscard]] std::int32_t s(std::uint32_t id, std::uint32_t word, std::size_t lane) {
        return to_int(reg(id, word, lane));
    }
    [[nodiscard]] std::uint32_t words_of(std::uint32_t id) const {
        return types[type_of[id]].words;
    }

    [[nodiscard]] Result<void> check_id(std::uint32_t id) const {
        if (id >= types.size()) {
            return bad("the module's ids go past its bound");
        }
        return {};
    }

    Result<void> allocate(std::uint32_t id, std::uint32_t type) {
        if (auto checked = check_id(id); !checked) {
            return checked;
        }
        type_of[id] = type;
        base[id] = register_words;
        register_words += std::max<std::uint32_t>(types[type].words, 1);
        return {};
    }

    Result<void> load(std::span<const std::uint32_t> words);
    Result<void> add_type(std::uint16_t op, std::span<const std::uint32_t> ops);
    Result<void> add_constant(std::uint16_t op, std::span<const std::uint32_t> ops);
    Result<void> add_variable(std::span<const std::uint32_t> ops);
    void lay_out(std::uint32_t type, std::uint32_t byte, const MemberLayout& layout,
                 std::vector<std::uint32_t>& out) const;
    [[nodiscard]] Result<std::uint32_t> flat_offset(std::uint32_t& type, std::uint32_t index) const;

    void run(Lanes& run_lanes, std::span<const std::span<const std::byte>> run_uniforms,
             const Textures& run_textures);
    void execute(Thread thread, std::vector<Thread>& pending);
    void enter_block(Thread& thread, std::uint32_t label);
    bool step(Thread& thread, std::vector<Thread>& pending);
    void ext(const Inst& inst, const std::uint32_t* o, std::uint8_t mask);
    void sample(const Inst& inst, const std::uint32_t* o, std::uint8_t mask);
    void derivative(const Inst& inst, const std::uint32_t* o, std::uint8_t mask);
    void load_pointer(std::uint32_t result, std::uint32_t words, std::uint32_t pointer,
                      std::uint8_t mask);
    void store_pointer(std::uint32_t pointer, std::uint32_t value, std::uint32_t words,
                       std::uint8_t mask);
};

namespace {

bool supported_instruction(std::uint16_t op) {
    switch (op) {
    case spv::Undef:
    case spv::ExtInst:
    case spv::FunctionCall:
    case spv::Variable:
    case spv::Load:
    case spv::Store:
    case spv::CopyMemory:
    case spv::AccessChain:
    case spv::InBoundsAccessChain:
    case spv::VectorExtractDynamic:
    case spv::VectorInsertDynamic:
    case spv::VectorShuffle:
    case spv::CompositeConstruct:
    case spv::CompositeExtract:
    case spv::CompositeInsert:
    case spv::CopyObject:
    case spv::CopyLogical:
    case spv::Transpose:
    case spv::SampledImage:
    case spv::ImageSampleImplicitLod:
    case spv::ImageSampleExplicitLod:
    case spv::ImageFetch:
    case spv::Image:
    case spv::ImageQuerySizeLod:
    case spv::ImageQuerySize:
    case spv::ImageQueryLevels:
    case spv::ConvertFToU:
    case spv::ConvertFToS:
    case spv::ConvertSToF:
    case spv::ConvertUToF:
    case spv::Bitcast:
    case spv::SNegate:
    case spv::FNegate:
    case spv::IAdd:
    case spv::FAdd:
    case spv::ISub:
    case spv::FSub:
    case spv::IMul:
    case spv::FMul:
    case spv::UDiv:
    case spv::SDiv:
    case spv::FDiv:
    case spv::UMod:
    case spv::SRem:
    case spv::SMod:
    case spv::FRem:
    case spv::FMod:
    case spv::VectorTimesScalar:
    case spv::MatrixTimesScalar:
    case spv::VectorTimesMatrix:
    case spv::MatrixTimesVector:
    case spv::MatrixTimesMatrix:
    case spv::OuterProduct:
    case spv::Dot:
    case spv::Any:
    case spv::All:
    case spv::IsNan:
    case spv::IsInf:
    case spv::LogicalEqual:
    case spv::LogicalNotEqual:
    case spv::LogicalOr:
    case spv::LogicalAnd:
    case spv::LogicalNot:
    case spv::Select:
    case spv::IEqual:
    case spv::INotEqual:
    case spv::UGreaterThan:
    case spv::SGreaterThan:
    case spv::UGreaterThanEqual:
    case spv::SGreaterThanEqual:
    case spv::ULessThan:
    case spv::SLessThan:
    case spv::ULessThanEqual:
    case spv::SLessThanEqual:
    case spv::FOrdEqual:
    case spv::FUnordEqual:
    case spv::FOrdNotEqual:
    case spv::FUnordNotEqual:
    case spv::FOrdLessThan:
    case spv::FUnordLessThan:
    case spv::FOrdGreaterThan:
    case spv::FUnordGreaterThan:
    case spv::FOrdLessThanEqual:
    case spv::FUnordLessThanEqual:
    case spv::FOrdGreaterThanEqual:
    case spv::FUnordGreaterThanEqual:
    case spv::ShiftRightLogical:
    case spv::ShiftRightArithmetic:
    case spv::ShiftLeftLogical:
    case spv::BitwiseOr:
    case spv::BitwiseXor:
    case spv::BitwiseAnd:
    case spv::Not:
    case spv::BitFieldInsert:
    case spv::BitFieldSExtract:
    case spv::BitFieldUExtract:
    case spv::BitReverse:
    case spv::BitCount:
    case spv::DPdx:
    case spv::DPdy:
    case spv::Fwidth:
    case spv::DPdxFine:
    case spv::DPdyFine:
    case spv::FwidthFine:
    case spv::DPdxCoarse:
    case spv::DPdyCoarse:
    case spv::FwidthCoarse:
    case spv::Phi:
    case spv::LoopMerge:
    case spv::SelectionMerge:
    case spv::Branch:
    case spv::BranchConditional:
    case spv::Switch:
    case spv::Kill:
    case spv::Return:
    case spv::ReturnValue:
    case spv::Unreachable:
    case spv::TerminateInvocation:
    case spv::DemoteToHelperInvocation:
    case spv::IsHelperInvocation:
        return true;
    default:
        return false;
    }
}

// Whether an instruction in a function body has a result type and result id first.
bool has_result(std::uint16_t op) {
    switch (op) {
    case spv::Store:
    case spv::CopyMemory:
    case spv::LoopMerge:
    case spv::SelectionMerge:
    case spv::Branch:
    case spv::BranchConditional:
    case spv::Switch:
    case spv::Kill:
    case spv::Return:
    case spv::ReturnValue:
    case spv::Unreachable:
    case spv::TerminateInvocation:
    case spv::DemoteToHelperInvocation:
        return false;
    default:
        return true;
    }
}

bool is_glsl_supported(std::uint32_t instruction) {
    switch (instruction) {
    case spv::Round:
    case spv::RoundEven:
    case spv::Trunc:
    case spv::FAbs:
    case spv::SAbs:
    case spv::FSign:
    case spv::SSign:
    case spv::Floor:
    case spv::Ceil:
    case spv::Fract:
    case spv::Radians:
    case spv::Degrees:
    case spv::Sin:
    case spv::Cos:
    case spv::Tan:
    case spv::Asin:
    case spv::Acos:
    case spv::Atan:
    case spv::Sinh:
    case spv::Cosh:
    case spv::Tanh:
    case spv::Asinh:
    case spv::Acosh:
    case spv::Atanh:
    case spv::Atan2:
    case spv::Pow:
    case spv::Exp:
    case spv::Log:
    case spv::Exp2:
    case spv::Log2:
    case spv::Sqrt:
    case spv::InverseSqrt:
    case spv::Determinant:
    case spv::MatrixInverse:
    case spv::Modf:
    case spv::ModfStruct:
    case spv::FMin:
    case spv::UMin:
    case spv::SMin:
    case spv::FMax:
    case spv::UMax:
    case spv::SMax:
    case spv::FClamp:
    case spv::UClamp:
    case spv::SClamp:
    case spv::FMix:
    case spv::Step:
    case spv::SmoothStep:
    case spv::Fma:
    case spv::Frexp:
    case spv::FrexpStruct:
    case spv::Ldexp:
    case spv::PackSnorm4x8:
    case spv::PackUnorm4x8:
    case spv::PackSnorm2x16:
    case spv::PackUnorm2x16:
    case spv::PackHalf2x16:
    case spv::UnpackSnorm2x16:
    case spv::UnpackUnorm2x16:
    case spv::UnpackHalf2x16:
    case spv::UnpackSnorm4x8:
    case spv::UnpackUnorm4x8:
    case spv::Length:
    case spv::Distance:
    case spv::Cross:
    case spv::Normalize:
    case spv::FaceForward:
    case spv::Reflect:
    case spv::Refract:
    case spv::FindILsb:
    case spv::FindSMsb:
    case spv::FindUMsb:
    case spv::NMin:
    case spv::NMax:
    case spv::NClamp:
        return true;
    default:
        return false;
    }
}

} // namespace

Result<void> ShaderVm::Module::add_type(std::uint16_t op, std::span<const std::uint32_t> ops) {
    const std::uint32_t id = ops[0];
    if (auto checked = check_id(id); !checked) {
        return checked;
    }
    TypeInfo& type = types[id];
    const auto need = [&](std::size_t count) -> Result<void> {
        if (ops.size() < count) {
            return bad("a type declaration is cut short");
        }
        return {};
    };
    const auto element = [&](std::uint32_t element_id) -> Result<const TypeInfo*> {
        if (auto checked = check_id(element_id); !checked) {
            return checked.error();
        }
        if (types[element_id].kind == Kind::None) {
            return bad("a type uses type " + std::to_string(element_id) + " before it is declared");
        }
        return &types[element_id];
    };
    switch (op) {
    case spv::TypeVoid:
        type.kind = Kind::Void;
        break;
    case spv::TypeBool:
        type.kind = Kind::Bool;
        type.words = 1;
        break;
    case spv::TypeInt:
        if (auto ok = need(3); !ok) {
            return ok;
        }
        if (ops[1] != 32) {
            return bad("the shader uses " + std::to_string(ops[1]) +
                       "-bit integers; the CPU runs 32-bit ones only");
        }
        type.kind = Kind::Int;
        type.is_signed = ops[2] != 0;
        type.words = 1;
        break;
    case spv::TypeFloat:
        if (auto ok = need(2); !ok) {
            return ok;
        }
        if (ops[1] != 32) {
            return bad("the shader uses " + std::to_string(ops[1]) +
                       "-bit floats; the CPU runs 32-bit ones only");
        }
        type.kind = Kind::Float;
        type.words = 1;
        break;
    case spv::TypeVector:
    case spv::TypeMatrix:
    case spv::TypeArray: {
        if (auto ok = need(3); !ok) {
            return ok;
        }
        auto inner = element(ops[1]);
        if (!inner) {
            return inner.error();
        }
        std::uint32_t count = ops[2];
        if (op == spv::TypeArray) {
            // The length is a constant's id.
            if (ops[2] >= base.size() || type_of[ops[2]] == 0) {
                return bad("an array's length isn't a constant");
            }
            count = regs[std::size_t{base[ops[2]]} * kLanes];
        }
        type.kind = Kind::Array;
        if (op == spv::TypeVector) {
            type.kind = Kind::Vector;
        } else if (op == spv::TypeMatrix) {
            type.kind = Kind::Matrix;
        }
        type.element = ops[1];
        type.count = count;
        type.words = inner.value()->words * count;
        break;
    }
    case spv::TypeRuntimeArray:
        return bad("the shader uses a runtime-sized array, which needs a storage buffer");
    case spv::TypeStruct: {
        type.kind = Kind::Struct;
        std::uint32_t words = 0;
        for (std::size_t i = 1; i < ops.size(); ++i) {
            auto inner = element(ops[i]);
            if (!inner) {
                return inner.error();
            }
            type.members.push_back(ops[i]);
            type.member_words.push_back(words);
            words += inner.value()->words;
        }
        type.words = words;
        break;
    }
    case spv::TypePointer:
        if (auto ok = need(3); !ok) {
            return ok;
        }
        type.kind = Kind::Pointer;
        type.storage = ops[1];
        type.element = ops[2];
        type.words = 1;
        break;
    case spv::TypeFunction:
        type.kind = Kind::Function;
        break;
    case spv::TypeImage:
        if (auto ok = need(8); !ok) {
            return ok;
        }
        // Dim 1 is 2D; arrayed, multisampled and storage images aren't sampled here.
        if (ops[2] != 1 || ops[4] != 0 || ops[5] != 0 || ops[6] == 2) {
            return bad("the shader uses a texture that isn't a plain Texture2D");
        }
        type.kind = Kind::Image;
        type.words = 1;
        break;
    case spv::TypeSampler:
        type.kind = Kind::Sampler;
        type.words = 1;
        break;
    case spv::TypeSampledImage:
        type.kind = Kind::SampledImage;
        type.words = 1;
        break;
    default:
        return bad("the shader uses a type the CPU can't run (SPIR-V opcode " + std::to_string(op) +
                   ")");
    }
    return {};
}

Result<void> ShaderVm::Module::add_constant(std::uint16_t op, std::span<const std::uint32_t> ops) {
    if (ops.size() < 2) {
        return bad("a constant is cut short");
    }
    const std::uint32_t type = ops[0];
    const std::uint32_t id = ops[1];
    if (type >= types.size() || types[type].kind == Kind::None) {
        return bad("a constant has an unknown type");
    }
    if (auto made = allocate(id, type); !made) {
        return made;
    }
    const std::uint32_t words = types[type].words;
    std::vector<std::uint32_t> values;
    switch (op) {
    case spv::ConstantTrue:
    case spv::SpecConstantTrue:
        values = {1};
        break;
    case spv::ConstantFalse:
    case spv::SpecConstantFalse:
        values = {0};
        break;
    case spv::Constant:
    case spv::SpecConstant:
        if (ops.size() < 3) {
            return bad("a constant has no value");
        }
        values = {ops[2]};
        break;
    case spv::ConstantComposite:
    case spv::SpecConstantComposite:
        for (std::size_t i = 2; i < ops.size(); ++i) {
            const std::uint32_t part = ops[i];
            if (part >= base.size() || type_of[part] == 0) {
                return bad("a composite constant names an unknown constant");
            }
            for (std::uint32_t w = 0; w < words_of(part); ++w) {
                values.push_back(regs[((std::size_t{base[part]} + w) * kLanes)]);
            }
        }
        break;
    default: // ConstantNull and Undef
        break;
    }
    values.resize(words, 0);
    regs.resize(std::size_t{register_words} * kLanes, 0);
    for (std::uint32_t w = 0; w < words; ++w) {
        for (std::size_t lane = 0; lane < kLanes; ++lane) {
            reg(id, w, lane) = values[w];
        }
    }
    return {};
}

void ShaderVm::Module::lay_out(std::uint32_t type, std::uint32_t byte, const MemberLayout& layout,
                               std::vector<std::uint32_t>& out) const {
    const TypeInfo& info = types[type];
    switch (info.kind) {
    case Kind::Vector:
        for (std::uint32_t i = 0; i < info.count; ++i) {
            out.push_back(byte + (4 * i));
        }
        break;
    case Kind::Matrix: {
        const std::uint32_t rows = types[info.element].count;
        const std::uint32_t stride = layout.matrix_stride != 0 ? layout.matrix_stride : 16;
        for (std::uint32_t column = 0; column < info.count; ++column) {
            for (std::uint32_t row = 0; row < rows; ++row) {
                out.push_back(byte + (layout.row_major ? (row * stride) + (column * 4)
                                                       : (column * stride) + (row * 4)));
            }
        }
        break;
    }
    case Kind::Array: {
        const std::uint32_t stride = decorations[type].array_stride != 0
                                         ? decorations[type].array_stride
                                         : types[info.element].words * 4;
        for (std::uint32_t i = 0; i < info.count; ++i) {
            lay_out(info.element, byte + (i * stride), layout, out);
        }
        break;
    }
    case Kind::Struct: {
        const std::vector<MemberLayout>& members = decorations[type].members;
        std::uint32_t packed = 0;
        for (std::size_t m = 0; m < info.members.size(); ++m) {
            const MemberLayout member = m < members.size() ? members[m] : MemberLayout{};
            const std::uint32_t at = member.offset.value_or(packed);
            lay_out(info.members[m], byte + at, member, out);
            packed = at + (types[info.members[m]].words * 4);
        }
        break;
    }
    default:
        out.push_back(byte);
        break;
    }
}

Result<void> ShaderVm::Module::add_variable(std::span<const std::uint32_t> ops) {
    if (ops.size() < 3) {
        return bad("a variable is cut short");
    }
    const std::uint32_t pointer = ops[0];
    const std::uint32_t id = ops[1];
    if (pointer >= types.size() || types[pointer].kind != Kind::Pointer) {
        return bad("a variable's type isn't a pointer");
    }
    if (auto made = allocate(id, pointer); !made) {
        return made;
    }
    VariableInfo variable;
    variable.storage = ops[2];
    variable.type = types[pointer].element;
    variable.words = types[variable.type].words;
    variable.initializer = ops.size() > 3 ? ops[3] : 0;
    if (variable.words > kPointerMask) {
        return bad("a variable is too big for the CPU to run");
    }
    const Decorations& decorated = decorations[id];
    variable.location = decorated.location;
    variable.binding = decorated.binding.value_or(0);
    if (decorated.builtin) {
        return bad("the shader reads or writes built-in " + std::to_string(*decorated.builtin) +
                   ", which the CPU doesn't provide");
    }
    switch (variable.storage) {
    case spv::Private:
    case spv::FunctionStorage:
    case spv::Input:
    case spv::Output:
        variable.base = memory_words;
        memory_words += variable.words;
        if ((variable.storage == spv::Input || variable.storage == spv::Output) &&
            (!variable.location || variable.words > 4 ||
             *variable.location >= ShaderVm::kLocations)) {
            return bad("the shader's inputs and outputs need locations 0 to 7, of 4 words "
                       "at most");
        }
        break;
    case spv::Uniform:
        lay_out(variable.type, 0, {}, variable.byte_offsets);
        break;
    case spv::UniformConstant: {
        const Kind kind = types[variable.type].kind;
        if (kind != Kind::Image && kind != Kind::SampledImage && kind != Kind::Sampler) {
            return bad("the shader has an array of textures or another resource the CPU can't "
                       "bind");
        }
        break;
    }
    default:
        return bad("the shader uses storage class " + std::to_string(variable.storage) +
                   ", which the CPU can't run (only inputs, outputs, uniform buffers, textures "
                   "and private and function variables)");
    }
    var_of[id] = static_cast<std::uint32_t>(variables.size());
    variables.push_back(std::move(variable));
    if (variables.size() > kPointerMask) {
        return bad("the shader has too many variables for the CPU to run");
    }
    return {};
}

Result<std::uint32_t> ShaderVm::Module::flat_offset(std::uint32_t& type,
                                                    std::uint32_t index) const {
    const TypeInfo& info = types[type];
    switch (info.kind) {
    case Kind::Struct:
        if (index >= info.members.size()) {
            return bad("a member index is out of range");
        }
        type = info.members[index];
        return info.member_words[index];
    case Kind::Vector:
    case Kind::Matrix:
    case Kind::Array: {
        const std::uint32_t clamped = std::min(index, info.count - 1);
        type = info.element;
        return clamped * types[info.element].words;
    }
    default:
        return bad("an index goes into a value that isn't a composite");
    }
}

Result<void> ShaderVm::Module::load(std::span<const std::uint32_t> words) {
    if (words.size() < 5 || words[0] != kMagic) {
        return bad("not a SPIR-V module");
    }
    const std::uint32_t bound = words[3];
    if (bound == 0 || bound > (1U << 22)) {
        return bad("the module's id bound is out of range");
    }
    types.resize(bound);
    decorations.resize(bound);
    type_of.assign(bound, 0);
    base.assign(bound, 0);
    var_of.assign(bound, 0);
    label_pc.assign(bound, 0);
    functions.resize(bound);
    std::uint32_t function = 0;
    std::size_t at = 5;
    while (at < words.size()) {
        const std::uint32_t head = words[at];
        const std::uint32_t count = head >> 16U;
        const auto op = static_cast<std::uint16_t>(head & 0xFFFFU);
        if (count == 0 || at + count > words.size()) {
            return bad("the module has a malformed instruction");
        }
        const std::span<const std::uint32_t> ops = words.subspan(at + 1, count - 1);
        at += count;
        const auto need = [&](std::size_t n) -> Result<void> {
            if (ops.size() < n) {
                return bad("an instruction (SPIR-V opcode " + std::to_string(op) +
                           ") is cut short");
            }
            return {};
        };
        switch (op) {
        case spv::Nop:
        case spv::SourceContinued:
        case spv::Source:
        case spv::SourceExtension:
        case spv::Name:
        case spv::MemberName:
        case spv::String:
        case spv::Line:
        case spv::NoLine:
        case spv::Extension:
        case spv::MemoryModel:
        case spv::ExecutionMode:
        case spv::ExecutionModeId:
        case spv::Capability:
        case spv::ModuleProcessed:
        case spv::DecorateId:
        case spv::DecorateString:
        case spv::MemberDecorateString:
            continue;
        case spv::ExtInstImport: {
            if (auto ok = need(1); !ok) {
                return ok;
            }
            const std::string name = literal_string(ops.subspan(1));
            if (name == "GLSL.std.450") {
                glsl = ops[0];
            } else if (name.starts_with("NonSemantic.")) {
                ignored_sets.push_back(ops[0]);
            } else {
                return bad("the shader uses the extended instructions " + name +
                           ", which the CPU can't run");
            }
            continue;
        }
        case spv::EntryPoint:
            if (auto ok = need(2); !ok) {
                return ok;
            }
            // Vertex is 0 and Fragment 4.
            if (ops[0] != 0 && ops[0] != 4) {
                return bad("the CPU runs vertex and fragment entry points only");
            }
            if (entry == 0) {
                entry = ops[1];
            }
            continue;
        case spv::Decorate: {
            if (auto ok = need(2); !ok) {
                return ok;
            }
            if (auto checked = check_id(ops[0]); !checked) {
                return checked;
            }
            Decorations& d = decorations[ops[0]];
            const std::uint32_t value = ops.size() > 2 ? ops[2] : 0;
            switch (ops[1]) {
            case spv::Location:
                d.location = value;
                break;
            case spv::Binding:
                d.binding = value;
                break;
            case spv::BuiltIn:
                d.builtin = value;
                break;
            case spv::ArrayStride:
                d.array_stride = value;
                break;
            default:
                break;
            }
            continue;
        }
        case spv::MemberDecorate: {
            if (auto ok = need(3); !ok) {
                return ok;
            }
            if (auto checked = check_id(ops[0]); !checked) {
                return checked;
            }
            std::vector<MemberLayout>& members = decorations[ops[0]].members;
            if (ops[1] > 4096) {
                return bad("a member index is out of range");
            }
            if (members.size() <= ops[1]) {
                members.resize(ops[1] + 1);
            }
            MemberLayout& member = members[ops[1]];
            const std::uint32_t value = ops.size() > 3 ? ops[3] : 0;
            switch (ops[2]) {
            case spv::Offset:
                member.offset = value;
                break;
            case spv::MatrixStride:
                member.matrix_stride = value;
                break;
            case spv::RowMajor:
                member.row_major = true;
                break;
            case spv::ColMajor:
                member.row_major = false;
                break;
            case spv::BuiltIn:
                return bad("the shader reads or writes built-in " + std::to_string(value) +
                           ", which the CPU doesn't provide");
            default:
                break;
            }
            continue;
        }
        case spv::TypeVoid:
        case spv::TypeBool:
        case spv::TypeInt:
        case spv::TypeFloat:
        case spv::TypeVector:
        case spv::TypeMatrix:
        case spv::TypeImage:
        case spv::TypeSampler:
        case spv::TypeSampledImage:
        case spv::TypeArray:
        case spv::TypeRuntimeArray:
        case spv::TypeStruct:
        case spv::TypePointer:
        case spv::TypeFunction:
            if (auto ok = need(1); !ok) {
                return ok;
            }
            if (auto added = add_type(op, ops); !added) {
                return added;
            }
            continue;
        case spv::ConstantTrue:
        case spv::ConstantFalse:
        case spv::Constant:
        case spv::ConstantComposite:
        case spv::ConstantNull:
        case spv::SpecConstantTrue:
        case spv::SpecConstantFalse:
        case spv::SpecConstant:
        case spv::SpecConstantComposite:
            if (auto added = add_constant(op, ops); !added) {
                return added;
            }
            continue;
        case spv::Variable:
            if (function == 0) {
                if (auto added = add_variable(ops); !added) {
                    return added;
                }
                continue;
            }
            // A function's variable: made like a private one, set when the instruction runs.
            if (auto ok = need(3); !ok) {
                return ok;
            }
            if (ops[2] != spv::FunctionStorage) {
                return bad("a variable in a function has to be a function variable");
            }
            if (auto added = add_variable(ops); !added) {
                return added;
            }
            break;
        case spv::Undef:
            if (function == 0) {
                if (auto added = add_constant(op, ops); !added) {
                    return added;
                }
                continue;
            }
            break;
        case spv::Function:
            if (auto ok = need(2); !ok) {
                return ok;
            }
            if (auto checked = check_id(ops[1]); !checked) {
                return checked;
            }
            function = ops[1];
            type_of[function] = ops[0];
            continue;
        case spv::FunctionParameter:
            if (auto ok = need(2); !ok) {
                return ok;
            }
            if (function == 0) {
                return bad("a function parameter is outside a function");
            }
            if (auto made = allocate(ops[1], ops[0]); !made) {
                return made;
            }
            functions[function].params.push_back(ops[1]);
            continue;
        case spv::FunctionEnd:
            function = 0;
            continue;
        case spv::Label:
            if (auto ok = need(1); !ok) {
                return ok;
            }
            if (auto checked = check_id(ops[0]); !checked) {
                return checked;
            }
            if (function == 0) {
                return bad("a label is outside a function");
            }
            if (functions[function].first_label == 0) {
                functions[function].first_label = ops[0];
            }
            label_pc[ops[0]] = static_cast<std::uint32_t>(code.size());
            continue;
        default:
            break;
        }
        if (function == 0) {
            return bad("the module has SPIR-V opcode " + std::to_string(op) +
                       " outside a function, which the CPU can't run");
        }
        if (!supported_instruction(op)) {
            return bad("the shader uses SPIR-V opcode " + std::to_string(op) +
                       ", which the CPU can't run");
        }
        Inst inst;
        inst.op = op;
        inst.count = static_cast<std::uint16_t>(ops.size());
        inst.first = static_cast<std::uint32_t>(operands.size());
        if (has_result(op)) {
            if (auto ok = need(2); !ok) {
                return ok;
            }
            if (op == spv::ExtInst) {
                if (auto ok = need(4); !ok) {
                    return ok;
                }
                if (std::find(ignored_sets.begin(), ignored_sets.end(), ops[2]) !=
                    ignored_sets.end()) {
                    continue; // debug information
                }
                if (ops[2] != glsl || !is_glsl_supported(ops[3])) {
                    return bad("the shader uses GLSL.std.450 instruction " +
                               std::to_string(ops[3]) + ", which the CPU can't run");
                }
            }
            inst.result = ops[1];
            if (op != spv::Variable) {
                if (ops[0] >= types.size() || types[ops[0]].kind == Kind::None) {
                    return bad("an instruction has an unknown result type");
                }
                if (auto made = allocate(ops[1], ops[0]); !made) {
                    return made;
                }
            }
            inst.words = types[type_of[ops[1]]].words;
        }
        operands.insert(operands.end(), ops.begin(), ops.end());
        code.push_back(inst);
    }
    if (entry == 0 || entry >= functions.size() || functions[entry].first_label == 0) {
        return bad("the module has no entry point");
    }
    // A few spare words past the last value, which ids and words out of range read instead.
    regs.resize((std::size_t{register_words} + kSpareWords) * kLanes, 0);
    memory.resize(std::size_t{memory_words} * kLanes, 0);
    for (const FunctionInfo& info : functions) {
        if (info.first_label != 0 && label_pc[info.first_label] >= code.size()) {
            return bad("a function has no instructions");
        }
    }
    return {};
}

void ShaderVm::Module::enter_block(Thread& thread, std::uint32_t label) {
    const std::uint32_t from = label < label_pc.size() ? thread.block : 0;
    thread.block = label;
    thread.pc = label < label_pc.size() ? label_pc[label] : static_cast<std::uint32_t>(code.size());
    // Phis read their values all at once, then write them.
    phi_ids.clear();
    phi_values.clear();
    std::uint32_t pc = thread.pc;
    while (pc < code.size() && code[pc].op == spv::Phi) {
        const Inst& inst = code[pc];
        const std::uint32_t* o = &operands[inst.first];
        for (std::uint32_t i = 2; i + 1 < inst.count; i += 2) {
            if (o[i + 1] != from) {
                continue;
            }
            phi_ids.push_back(inst.result);
            for (std::uint32_t w = 0; w < inst.words; ++w) {
                for (std::size_t lane = 0; lane < kLanes; ++lane) {
                    phi_values.push_back(reg(o[i], w, lane));
                }
            }
            break;
        }
        ++pc;
    }
    std::size_t next = 0;
    for (const std::uint32_t id : phi_ids) {
        for (std::uint32_t w = 0; w < words_of(id); ++w) {
            for (std::size_t lane = 0; lane < kLanes; ++lane) {
                const std::uint32_t value = phi_values[next++];
                if ((thread.mask & (1U << lane)) != 0) {
                    reg(id, w, lane) = value;
                }
            }
        }
    }
    thread.pc = pc;
}

void ShaderVm::Module::load_pointer(std::uint32_t result, std::uint32_t words,
                                    std::uint32_t pointer, std::uint8_t mask) {
    for (std::size_t lane = 0; lane < kLanes; ++lane) {
        if ((mask & (1U << lane)) == 0) {
            continue;
        }
        const std::uint32_t value = reg(pointer, 0, lane);
        const std::uint32_t index = value >> kPointerShift;
        const std::uint32_t offset = value & kPointerMask;
        for (std::uint32_t w = 0; w < words; ++w) {
            reg(result, w, lane) = index == 0 || index >= variables.size()
                                       ? 0
                                       : read(variables[index], offset + w, lane);
        }
    }
}

void ShaderVm::Module::store_pointer(std::uint32_t pointer, std::uint32_t value,
                                     std::uint32_t words, std::uint8_t mask) {
    for (std::size_t lane = 0; lane < kLanes; ++lane) {
        if ((mask & (1U << lane)) == 0) {
            continue;
        }
        const std::uint32_t target = reg(pointer, 0, lane);
        const std::uint32_t index = target >> kPointerShift;
        const std::uint32_t offset = target & kPointerMask;
        if (index == 0 || index >= variables.size()) {
            continue;
        }
        const VariableInfo& variable = variables[index];
        if (variable.storage == spv::Uniform || variable.storage == spv::UniformConstant ||
            variable.storage == spv::Input) {
            continue;
        }
        for (std::uint32_t w = 0; w < words && offset + w < variable.words; ++w) {
            memory[((std::size_t{variable.base} + offset + w) * kLanes) + lane] =
                reg(value, w, lane);
        }
    }
}

void ShaderVm::Module::derivative(const Inst& inst, const std::uint32_t* o, std::uint8_t mask) {
    const std::uint32_t result = o[1];
    const std::uint32_t value = o[2];
    const bool quad = mask == kAllLanes && lanes->quad;
    const bool coarse =
        inst.op == spv::DPdxCoarse || inst.op == spv::DPdyCoarse || inst.op == spv::FwidthCoarse;
    for (std::uint32_t w = 0; w < inst.words; ++w) {
        std::array<float, kLanes> v{};
        for (std::size_t lane = 0; lane < kLanes; ++lane) {
            v[lane] = f(value, w, lane);
        }
        std::array<float, kLanes> dx{};
        std::array<float, kLanes> dy{};
        if (quad) {
            const float top = v[1] - v[0];
            const float bottom = coarse ? top : v[3] - v[2];
            const float left = v[2] - v[0];
            const float right = coarse ? left : v[3] - v[1];
            dx = {top, top, bottom, bottom};
            dy = {left, right, left, right};
        }
        for (std::size_t lane = 0; lane < kLanes; ++lane) {
            if ((mask & (1U << lane)) == 0) {
                continue;
            }
            float out = 0.0F;
            switch (inst.op) {
            case spv::DPdx:
            case spv::DPdxFine:
            case spv::DPdxCoarse:
                out = dx[lane];
                break;
            case spv::DPdy:
            case spv::DPdyFine:
            case spv::DPdyCoarse:
                out = dy[lane];
                break;
            default:
                out = std::abs(dx[lane]) + std::abs(dy[lane]);
                break;
            }
            reg(result, w, lane) = from_float(out);
        }
    }
}

void ShaderVm::Module::sample(const Inst& inst, const std::uint32_t* o, std::uint8_t mask) {
    const std::uint32_t result = o[1];
    const std::uint32_t image = o[2];
    const std::uint32_t coordinate = o[3];
    // Image operands: a mask, then its operands in bit order.
    std::uint32_t offset_id = 0;
    bool const_offset = false;
    if (inst.count > 4) {
        const std::uint32_t bits = o[4];
        std::uint32_t next = 5;
        for (const std::uint32_t bit :
             {spv::kBias, spv::kLod, spv::kGrad, spv::kConstOffset, spv::kOffset,
              spv::kConstOffsets, spv::kSample, spv::kMinLod}) {
            if ((bits & bit) == 0) {
                continue;
            }
            if (bit == spv::kConstOffset || bit == spv::kOffset) {
                offset_id = next < inst.count ? o[next] : 0;
                const_offset = true;
            }
            next += bit == spv::kGrad ? 2 : 1;
        }
    }
    for (std::size_t lane = 0; lane < kLanes; ++lane) {
        if ((mask & (1U << lane)) == 0) {
            continue;
        }
        const std::uint32_t binding = reg(image, 0, lane);
        std::array<std::int32_t, 2> offset{};
        if (const_offset && offset_id != 0) {
            offset = {s(offset_id, 0, lane), s(offset_id, 1, lane)};
        }
        ShaderVm::Float4 texel{};
        if (inst.op == spv::ImageFetch) {
            if (textures->fetch) {
                texel = textures->fetch(binding, s(coordinate, 0, lane) + offset[0],
                                        s(coordinate, 1, lane) + offset[1]);
            }
        } else if (textures->sample) {
            float u = f(coordinate, 0, lane);
            float v = f(coordinate, 1, lane);
            if ((offset[0] != 0 || offset[1] != 0) && textures->size) {
                const auto size = textures->size(binding);
                u += static_cast<float>(offset[0]) / static_cast<float>(std::max(size[0], 1U));
                v += static_cast<float>(offset[1]) / static_cast<float>(std::max(size[1], 1U));
            }
            texel = textures->sample(binding, u, v);
        }
        for (std::uint32_t w = 0; w < inst.words && w < 4; ++w) {
            reg(result, w, lane) = from_float(texel[w]);
        }
    }
}

void ShaderVm::Module::ext(const Inst& inst, const std::uint32_t* o, std::uint8_t mask) {
    const std::uint32_t result = o[1];
    const std::uint32_t instruction = o[3];
    const std::uint32_t* args = o + 4;
    const std::uint32_t words = inst.words;
    const auto each_lane = [&](auto&& body) {
        for (std::size_t lane = 0; lane < kLanes; ++lane) {
            if ((mask & (1U << lane)) != 0) {
                body(lane);
            }
        }
    };
    const auto unary = [&](auto&& op) {
        each_lane([&](std::size_t lane) {
            for (std::uint32_t w = 0; w < words; ++w) {
                reg(result, w, lane) = op(reg(args[0], w, lane));
            }
        });
    };
    const auto unary_f = [&](auto&& op) {
        unary([&](std::uint32_t x) { return from_float(op(to_float(x))); });
    };
    const auto binary = [&](auto&& op) {
        each_lane([&](std::size_t lane) {
            for (std::uint32_t w = 0; w < words; ++w) {
                reg(result, w, lane) = op(reg(args[0], w, lane), reg(args[1], w, lane));
            }
        });
    };
    const auto binary_f = [&](auto&& op) {
        binary([&](std::uint32_t x, std::uint32_t y) {
            return from_float(op(to_float(x), to_float(y)));
        });
    };
    const auto ternary = [&](auto&& op) {
        each_lane([&](std::size_t lane) {
            for (std::uint32_t w = 0; w < words; ++w) {
                reg(result, w, lane) =
                    op(reg(args[0], w, lane), reg(args[1], w, lane), reg(args[2], w, lane));
            }
        });
    };
    const auto ternary_f = [&](auto&& op) {
        ternary([&](std::uint32_t x, std::uint32_t y, std::uint32_t z) {
            return from_float(op(to_float(x), to_float(y), to_float(z)));
        });
    };
    // The length of a vector argument, in lane.
    const auto length = [&](std::uint32_t id, std::size_t lane) {
        float sum = 0.0F;
        for (std::uint32_t w = 0; w < words_of(id); ++w) {
            const float x = f(id, w, lane);
            sum += x * x;
        }
        return std::sqrt(sum);
    };
    const auto dot = [&](std::uint32_t a, std::uint32_t b, std::size_t lane) {
        float sum = 0.0F;
        for (std::uint32_t w = 0; w < words_of(a); ++w) {
            sum += f(a, w, lane) * f(b, w, lane);
        }
        return sum;
    };
    switch (instruction) {
    case spv::Round:
        unary_f([](float x) { return std::round(x); });
        break;
    case spv::RoundEven:
        unary_f([](float x) { return std::nearbyint(x); });
        break;
    case spv::Trunc:
        unary_f([](float x) { return std::trunc(x); });
        break;
    case spv::FAbs:
        unary_f([](float x) { return std::abs(x); });
        break;
    case spv::SAbs:
        unary([](std::uint32_t x) {
            const std::int32_t v = to_int(x);
            return v < 0 ? from_int(static_cast<std::int32_t>(0U - x)) : x;
        });
        break;
    case spv::FSign:
        unary_f([](float x) {
            if (x > 0.0F) {
                return 1.0F;
            }
            return x < 0.0F ? -1.0F : 0.0F;
        });
        break;
    case spv::SSign:
        unary([](std::uint32_t x) {
            const std::int32_t v = to_int(x);
            return from_int(static_cast<std::int32_t>(v > 0) - static_cast<std::int32_t>(v < 0));
        });
        break;
    case spv::Floor:
        unary_f([](float x) { return std::floor(x); });
        break;
    case spv::Ceil:
        unary_f([](float x) { return std::ceil(x); });
        break;
    case spv::Fract:
        unary_f([](float x) { return x - std::floor(x); });
        break;
    case spv::Radians:
        unary_f([](float x) { return x * 0.017453292519943295F; });
        break;
    case spv::Degrees:
        unary_f([](float x) { return x * 57.29577951308232F; });
        break;
    case spv::Sin:
        unary_f([](float x) { return std::sin(x); });
        break;
    case spv::Cos:
        unary_f([](float x) { return std::cos(x); });
        break;
    case spv::Tan:
        unary_f([](float x) { return std::tan(x); });
        break;
    case spv::Asin:
        unary_f([](float x) { return std::asin(x); });
        break;
    case spv::Acos:
        unary_f([](float x) { return std::acos(x); });
        break;
    case spv::Atan:
        unary_f([](float x) { return std::atan(x); });
        break;
    case spv::Sinh:
        unary_f([](float x) { return std::sinh(x); });
        break;
    case spv::Cosh:
        unary_f([](float x) { return std::cosh(x); });
        break;
    case spv::Tanh:
        unary_f([](float x) { return std::tanh(x); });
        break;
    case spv::Asinh:
        unary_f([](float x) { return std::asinh(x); });
        break;
    case spv::Acosh:
        unary_f([](float x) { return std::acosh(x); });
        break;
    case spv::Atanh:
        unary_f([](float x) { return std::atanh(x); });
        break;
    case spv::Atan2:
        binary_f([](float y, float x) { return std::atan2(y, x); });
        break;
    case spv::Pow:
        binary_f([](float x, float y) { return std::pow(x, y); });
        break;
    case spv::Exp:
        unary_f([](float x) { return std::exp(x); });
        break;
    case spv::Log:
        unary_f([](float x) { return std::log(x); });
        break;
    case spv::Exp2:
        unary_f([](float x) { return std::exp2(x); });
        break;
    case spv::Log2:
        unary_f([](float x) { return std::log2(x); });
        break;
    case spv::Sqrt:
        unary_f([](float x) { return std::sqrt(x); });
        break;
    case spv::InverseSqrt:
        unary_f([](float x) { return 1.0F / std::sqrt(x); });
        break;
    case spv::Determinant: {
        const TypeInfo& matrix = types[type_of[args[0]]];
        each_lane([&](std::size_t lane) {
            std::vector<float> m(matrix.words);
            for (std::uint32_t w = 0; w < matrix.words; ++w) {
                m[w] = f(args[0], w, lane);
            }
            reg(result, 0, lane) = from_float(determinant(m, matrix.count));
        });
        break;
    }
    case spv::MatrixInverse: {
        const std::uint32_t n = types[type_of[args[0]]].count;
        each_lane([&](std::size_t lane) {
            std::vector<float> m(std::size_t{n} * n);
            for (std::uint32_t w = 0; w < n * n; ++w) {
                m[w] = f(args[0], w, lane);
            }
            const float det = determinant(m, n);
            // The adjugate over the determinant: the element in row i, column j is the cofactor
            // of row j, column i.
            for (std::uint32_t j = 0; j < n; ++j) {
                for (std::uint32_t i = 0; i < n; ++i) {
                    std::vector<float> minor;
                    for (std::uint32_t c = 0; c < n; ++c) {
                        for (std::uint32_t r = 0; r < n; ++r) {
                            if (c != i && r != j) {
                                minor.push_back(m[(c * n) + r]);
                            }
                        }
                    }
                    float cofactor = n == 2 ? minor[0] : determinant(minor, n - 1);
                    if ((i + j) % 2 != 0) {
                        cofactor = -cofactor;
                    }
                    reg(result, (j * n) + i, lane) = from_float(cofactor / det);
                }
            }
        });
        break;
    }
    case spv::Modf:
    case spv::ModfStruct: {
        const std::uint32_t parts = instruction == spv::Modf ? words : words / 2;
        each_lane([&](std::size_t lane) {
            for (std::uint32_t w = 0; w < parts; ++w) {
                const float x = f(args[0], w, lane);
                const float whole = std::trunc(x);
                reg(result, w, lane) = from_float(x - whole);
                if (instruction == spv::ModfStruct) {
                    reg(result, parts + w, lane) = from_float(whole);
                } else {
                    // The whole part goes through the pointer argument.
                    const std::uint32_t target = reg(args[1], 0, lane);
                    const std::uint32_t index = target >> kPointerShift;
                    if (index != 0 && index < variables.size()) {
                        const VariableInfo& variable = variables[index];
                        const std::uint32_t word = (target & kPointerMask) + w;
                        if (word < variable.words && variable.storage != spv::Uniform) {
                            memory[((std::size_t{variable.base} + word) * kLanes) + lane] =
                                from_float(whole);
                        }
                    }
                }
            }
        });
        break;
    }
    case spv::FMin:
    case spv::NMin:
        binary_f([](float x, float y) { return std::fmin(x, y); });
        break;
    case spv::UMin:
        binary([](std::uint32_t x, std::uint32_t y) { return std::min(x, y); });
        break;
    case spv::SMin:
        binary([](std::uint32_t x, std::uint32_t y) {
            return from_int(std::min(to_int(x), to_int(y)));
        });
        break;
    case spv::FMax:
    case spv::NMax:
        binary_f([](float x, float y) { return std::fmax(x, y); });
        break;
    case spv::UMax:
        binary([](std::uint32_t x, std::uint32_t y) { return std::max(x, y); });
        break;
    case spv::SMax:
        binary([](std::uint32_t x, std::uint32_t y) {
            return from_int(std::max(to_int(x), to_int(y)));
        });
        break;
    case spv::FClamp:
    case spv::NClamp:
        ternary_f([](float x, float lo, float hi) { return std::fmin(std::fmax(x, lo), hi); });
        break;
    case spv::UClamp:
        ternary([](std::uint32_t x, std::uint32_t lo, std::uint32_t hi) {
            return std::min(std::max(x, lo), hi);
        });
        break;
    case spv::SClamp:
        ternary([](std::uint32_t x, std::uint32_t lo, std::uint32_t hi) {
            return from_int(std::min(std::max(to_int(x), to_int(lo)), to_int(hi)));
        });
        break;
    case spv::FMix:
        ternary_f([](float x, float y, float a) { return x + ((y - x) * a); });
        break;
    case spv::Step:
        binary_f([](float edge, float x) { return x < edge ? 0.0F : 1.0F; });
        break;
    case spv::SmoothStep:
        ternary_f([](float edge0, float edge1, float x) {
            const float t = std::fmin(std::fmax((x - edge0) / (edge1 - edge0), 0.0F), 1.0F);
            return t * t * (3.0F - (2.0F * t));
        });
        break;
    case spv::Fma:
        ternary_f([](float a, float b, float c) { return (a * b) + c; });
        break;
    case spv::Frexp:
    case spv::FrexpStruct: {
        const std::uint32_t parts = instruction == spv::Frexp ? words : words / 2;
        each_lane([&](std::size_t lane) {
            for (std::uint32_t w = 0; w < parts; ++w) {
                int exponent = 0;
                const float mantissa = std::frexp(f(args[0], w, lane), &exponent);
                reg(result, w, lane) = from_float(mantissa);
                if (instruction == spv::FrexpStruct) {
                    reg(result, parts + w, lane) = from_int(exponent);
                } else {
                    const std::uint32_t target = reg(args[1], 0, lane);
                    const std::uint32_t index = target >> kPointerShift;
                    if (index != 0 && index < variables.size()) {
                        const VariableInfo& variable = variables[index];
                        const std::uint32_t word = (target & kPointerMask) + w;
                        if (word < variable.words && variable.storage != spv::Uniform) {
                            memory[((std::size_t{variable.base} + word) * kLanes) + lane] =
                                from_int(exponent);
                        }
                    }
                }
            }
        });
        break;
    }
    case spv::Ldexp:
        binary([](std::uint32_t x, std::uint32_t e) {
            return from_float(std::ldexp(to_float(x), to_int(e)));
        });
        break;
    case spv::PackHalf2x16:
        each_lane([&](std::size_t lane) {
            reg(result, 0, lane) =
                to_half(f(args[0], 0, lane)) | (to_half(f(args[0], 1, lane)) << 16U);
        });
        break;
    case spv::UnpackHalf2x16:
        each_lane([&](std::size_t lane) {
            const std::uint32_t packed = reg(args[0], 0, lane);
            reg(result, 0, lane) = from_float(from_half(packed & 0xFFFFU));
            reg(result, 1, lane) = from_float(from_half(packed >> 16U));
        });
        break;
    case spv::PackUnorm4x8:
    case spv::PackSnorm4x8:
    case spv::PackUnorm2x16:
    case spv::PackSnorm2x16: {
        const bool four = instruction == spv::PackUnorm4x8 || instruction == spv::PackSnorm4x8;
        const bool is_signed =
            instruction == spv::PackSnorm4x8 || instruction == spv::PackSnorm2x16;
        const std::uint32_t parts = four ? 4 : 2;
        const std::uint32_t bits = four ? 8 : 16;
        const auto scale = static_cast<float>((1U << (is_signed ? bits - 1 : bits)) - 1U);
        each_lane([&](std::size_t lane) {
            std::uint32_t packed = 0;
            for (std::uint32_t i = 0; i < parts; ++i) {
                const float x = f(args[0], i, lane);
                const float clamped = is_signed ? std::fmin(std::fmax(x, -1.0F), 1.0F)
                                                : std::fmin(std::fmax(x, 0.0F), 1.0F);
                const auto value = static_cast<std::int32_t>(std::round(clamped * scale));
                packed |= (static_cast<std::uint32_t>(value) & ((1U << bits) - 1U)) << (i * bits);
            }
            reg(result, 0, lane) = packed;
        });
        break;
    }
    case spv::UnpackUnorm4x8:
    case spv::UnpackSnorm4x8:
    case spv::UnpackUnorm2x16:
    case spv::UnpackSnorm2x16: {
        const bool four = instruction == spv::UnpackUnorm4x8 || instruction == spv::UnpackSnorm4x8;
        const bool is_signed =
            instruction == spv::UnpackSnorm4x8 || instruction == spv::UnpackSnorm2x16;
        const std::uint32_t parts = four ? 4 : 2;
        const std::uint32_t bits = four ? 8 : 16;
        const auto scale = static_cast<float>((1U << (is_signed ? bits - 1 : bits)) - 1U);
        each_lane([&](std::size_t lane) {
            const std::uint32_t packed = reg(args[0], 0, lane);
            for (std::uint32_t i = 0; i < parts; ++i) {
                const std::uint32_t raw = (packed >> (i * bits)) & ((1U << bits) - 1U);
                float value = 0.0F;
                if (is_signed) {
                    const std::int32_t sign_extended =
                        (raw & (1U << (bits - 1))) != 0
                            ? static_cast<std::int32_t>(raw) - static_cast<std::int32_t>(1U << bits)
                            : static_cast<std::int32_t>(raw);
                    value = std::fmax(static_cast<float>(sign_extended) / scale, -1.0F);
                } else {
                    value = static_cast<float>(raw) / scale;
                }
                reg(result, i, lane) = from_float(value);
            }
        });
        break;
    }
    case spv::Length:
        each_lane(
            [&](std::size_t lane) { reg(result, 0, lane) = from_float(length(args[0], lane)); });
        break;
    case spv::Distance:
        each_lane([&](std::size_t lane) {
            float sum = 0.0F;
            for (std::uint32_t w = 0; w < words_of(args[0]); ++w) {
                const float d = f(args[0], w, lane) - f(args[1], w, lane);
                sum += d * d;
            }
            reg(result, 0, lane) = from_float(std::sqrt(sum));
        });
        break;
    case spv::Cross:
        each_lane([&](std::size_t lane) {
            const float ax = f(args[0], 0, lane);
            const float ay = f(args[0], 1, lane);
            const float az = f(args[0], 2, lane);
            const float bx = f(args[1], 0, lane);
            const float by = f(args[1], 1, lane);
            const float bz = f(args[1], 2, lane);
            reg(result, 0, lane) = from_float((ay * bz) - (by * az));
            reg(result, 1, lane) = from_float((az * bx) - (bz * ax));
            reg(result, 2, lane) = from_float((ax * by) - (bx * ay));
        });
        break;
    case spv::Normalize:
        each_lane([&](std::size_t lane) {
            const float size = length(args[0], lane);
            for (std::uint32_t w = 0; w < words; ++w) {
                reg(result, w, lane) = from_float(f(args[0], w, lane) / size);
            }
        });
        break;
    case spv::FaceForward:
        each_lane([&](std::size_t lane) {
            const bool flip = dot(args[2], args[1], lane) >= 0.0F;
            for (std::uint32_t w = 0; w < words; ++w) {
                const float n = f(args[0], w, lane);
                reg(result, w, lane) = from_float(flip ? -n : n);
            }
        });
        break;
    case spv::Reflect:
        each_lane([&](std::size_t lane) {
            const float d = dot(args[1], args[0], lane);
            for (std::uint32_t w = 0; w < words; ++w) {
                reg(result, w, lane) =
                    from_float(f(args[0], w, lane) - (2.0F * d * f(args[1], w, lane)));
            }
        });
        break;
    case spv::Refract:
        each_lane([&](std::size_t lane) {
            const float d = dot(args[1], args[0], lane);
            const float eta = f(args[2], 0, lane);
            const float k = 1.0F - (eta * eta * (1.0F - (d * d)));
            for (std::uint32_t w = 0; w < words; ++w) {
                const float out = k < 0.0F ? 0.0F
                                           : (eta * f(args[0], w, lane)) -
                                                 (((eta * d) + std::sqrt(k)) * f(args[1], w, lane));
                reg(result, w, lane) = from_float(out);
            }
        });
        break;
    case spv::FindILsb:
        unary([](std::uint32_t x) {
            return x == 0 ? 0xFFFFFFFFU : static_cast<std::uint32_t>(std::countr_zero(x));
        });
        break;
    case spv::FindUMsb:
        unary([](std::uint32_t x) {
            return x == 0 ? 0xFFFFFFFFU : static_cast<std::uint32_t>(31 - std::countl_zero(x));
        });
        break;
    case spv::FindSMsb:
        unary([](std::uint32_t x) {
            const std::uint32_t v = to_int(x) < 0 ? ~x : x;
            return v == 0 ? 0xFFFFFFFFU : static_cast<std::uint32_t>(31 - std::countl_zero(v));
        });
        break;
    default:
        break;
    }
}

bool ShaderVm::Module::step(Thread& thread, std::vector<Thread>& pending) {
    const Inst& inst = code[thread.pc];
    const std::uint32_t* o = &operands[inst.first];
    const std::uint8_t mask = thread.mask;
    const std::uint32_t words = inst.words;
    const std::uint32_t result = inst.result;
    const auto each_lane = [&](auto&& body) {
        for (std::size_t lane = 0; lane < kLanes; ++lane) {
            if ((mask & (1U << lane)) != 0) {
                body(lane);
            }
        }
    };
    // Component-wise ops over the result's words.
    const auto unary = [&](auto&& op) {
        each_lane([&](std::size_t lane) {
            for (std::uint32_t w = 0; w < words; ++w) {
                reg(result, w, lane) = op(reg(o[2], w, lane));
            }
        });
    };
    const auto binary = [&](auto&& op) {
        each_lane([&](std::size_t lane) {
            for (std::uint32_t w = 0; w < words; ++w) {
                reg(result, w, lane) = op(reg(o[2], w, lane), reg(o[3], w, lane));
            }
        });
    };
    const auto binary_f = [&](auto&& op) {
        binary([&](std::uint32_t x, std::uint32_t y) {
            return from_float(op(to_float(x), to_float(y)));
        });
    };
    const auto compare_f = [&](auto&& op) {
        binary([&](std::uint32_t x, std::uint32_t y) {
            return op(to_float(x), to_float(y)) ? 1U : 0U;
        });
    };
    const auto compare_i = [&](auto&& op) {
        binary([&](std::uint32_t x, std::uint32_t y) { return op(x, y) ? 1U : 0U; });
    };
    const auto unordered = [](float x, float y) { return std::isnan(x) || std::isnan(y); };
    ++thread.pc;
    switch (inst.op) {
    case spv::Undef:
        each_lane([&](std::size_t lane) {
            for (std::uint32_t w = 0; w < words; ++w) {
                reg(result, w, lane) = 0;
            }
        });
        break;
    case spv::Variable: {
        // A function variable: points at its storage, and starts with its initializer.
        const std::uint32_t index = var_of[result];
        const VariableInfo& variable = variables[index];
        each_lane([&](std::size_t lane) {
            reg(result, 0, lane) = index << kPointerShift;
            if (variable.initializer != 0) {
                for (std::uint32_t w = 0; w < variable.words; ++w) {
                    memory[((std::size_t{variable.base} + w) * kLanes) + lane] =
                        reg(variable.initializer, w, lane);
                }
            }
        });
        break;
    }
    case spv::Load:
        load_pointer(result, words, o[2], mask);
        break;
    case spv::Store:
        store_pointer(o[0], o[1], words_of(o[1]), mask);
        break;
    case spv::CopyMemory: {
        const std::uint32_t pointee = types[type_of[o[0]]].element;
        const std::uint32_t count = types[pointee].words;
        // Through a scratch register: the source's value, then the store.
        std::vector<std::uint32_t> scratch(std::size_t{count} * kLanes);
        each_lane([&](std::size_t lane) {
            const std::uint32_t source = reg(o[1], 0, lane);
            const std::uint32_t index = source >> kPointerShift;
            if (index == 0 || index >= variables.size()) {
                return;
            }
            for (std::uint32_t w = 0; w < count; ++w) {
                scratch[(w * kLanes) + lane] =
                    read(variables[index], (source & kPointerMask) + w, lane);
            }
            const std::uint32_t target = reg(o[0], 0, lane);
            const std::uint32_t to_index = target >> kPointerShift;
            if (to_index == 0 || to_index >= variables.size()) {
                return;
            }
            const VariableInfo& to = variables[to_index];
            if (to.storage == spv::Uniform || to.storage == spv::UniformConstant ||
                to.storage == spv::Input) {
                return;
            }
            for (std::uint32_t w = 0; w < count && (target & kPointerMask) + w < to.words; ++w) {
                memory[((std::size_t{to.base} + (target & kPointerMask) + w) * kLanes) + lane] =
                    scratch[(w * kLanes) + lane];
            }
        });
        break;
    }
    case spv::AccessChain:
    case spv::InBoundsAccessChain:
        each_lane([&](std::size_t lane) {
            std::uint32_t type = types[type_of[o[2]]].element;
            const std::uint32_t pointer = reg(o[2], 0, lane);
            std::uint32_t offset = pointer & kPointerMask;
            for (std::uint32_t i = 3; i < inst.count; ++i) {
                const std::uint32_t index = reg(o[i], 0, lane);
                auto added = flat_offset(type, index);
                offset += added ? added.value() : 0;
            }
            reg(result, 0, lane) = (pointer & ~kPointerMask) | (offset & kPointerMask);
        });
        break;
    case spv::FunctionCall: {
        const std::uint32_t callee = o[2];
        const FunctionInfo& info = functions[callee];
        for (std::size_t i = 0; i < info.params.size() && 3 + i < inst.count; ++i) {
            const std::uint32_t param = info.params[i];
            const std::uint32_t arg = o[3 + i];
            each_lane([&](std::size_t lane) {
                for (std::uint32_t w = 0; w < words_of(param); ++w) {
                    reg(param, w, lane) = reg(arg, w, lane);
                }
            });
        }
        thread.stack.push_back({.return_pc = thread.pc, .block = thread.block, .result = result});
        enter_block(thread, info.first_label);
        break;
    }
    case spv::Return:
    case spv::ReturnValue: {
        if (thread.stack.empty()) {
            return false;
        }
        const Frame frame = thread.stack.back();
        thread.stack.pop_back();
        if (inst.op == spv::ReturnValue && frame.result != 0) {
            each_lane([&](std::size_t lane) {
                for (std::uint32_t w = 0; w < words_of(frame.result); ++w) {
                    reg(frame.result, w, lane) = reg(o[0], w, lane);
                }
            });
        }
        thread.pc = frame.return_pc;
        thread.block = frame.block;
        break;
    }
    case spv::Branch:
        enter_block(thread, o[0]);
        break;
    case spv::BranchConditional:
    case spv::Switch: {
        std::array<std::uint32_t, kLanes> targets{};
        each_lane([&](std::size_t lane) {
            const std::uint32_t value = reg(o[0], 0, lane);
            if (inst.op == spv::BranchConditional) {
                targets[lane] = value != 0 ? o[1] : o[2];
                return;
            }
            targets[lane] = o[1];
            for (std::uint32_t i = 2; i + 1 < inst.count; i += 2) {
                if (o[i] == value) {
                    targets[lane] = o[i + 1];
                    break;
                }
            }
        });
        std::optional<std::uint32_t> shared;
        bool apart = false;
        each_lane([&](std::size_t lane) {
            if (!shared) {
                shared = targets[lane];
            } else if (*shared != targets[lane]) {
                apart = true;
            }
        });
        if (!apart) {
            enter_block(thread, shared.value_or(o[1]));
            break;
        }
        // The lanes branch apart: each goes on alone.
        each_lane([&](std::size_t lane) {
            Thread alone = thread;
            alone.mask = static_cast<std::uint8_t>(1U << lane);
            enter_block(alone, targets[lane]);
            pending.push_back(std::move(alone));
        });
        return false;
    }
    case spv::Phi:
    case spv::LoopMerge:
    case spv::SelectionMerge:
        break;
    case spv::Kill:
    case spv::TerminateInvocation:
        each_lane([&](std::size_t lane) { lanes->discarded[lane] = true; });
        return false;
    case spv::DemoteToHelperInvocation:
        each_lane([&](std::size_t lane) { lanes->discarded[lane] = true; });
        break;
    case spv::IsHelperInvocation:
        each_lane([&](std::size_t lane) {
            reg(result, 0, lane) = lanes->discarded[lane] || lane >= lanes->count ? 1U : 0U;
        });
        break;
    case spv::Unreachable:
        return false;
    case spv::ExtInst:
        ext(inst, o, mask);
        break;
    case spv::CompositeConstruct:
        each_lane([&](std::size_t lane) {
            std::uint32_t w = 0;
            for (std::uint32_t i = 2; i < inst.count && w < words; ++i) {
                for (std::uint32_t k = 0; k < words_of(o[i]) && w < words; ++k) {
                    reg(result, w++, lane) = reg(o[i], k, lane);
                }
            }
        });
        break;
    case spv::CompositeExtract: {
        std::uint32_t type = type_of[o[2]];
        std::uint32_t offset = 0;
        for (std::uint32_t i = 3; i < inst.count; ++i) {
            auto added = flat_offset(type, o[i]);
            offset += added ? added.value() : 0;
        }
        each_lane([&](std::size_t lane) {
            for (std::uint32_t w = 0; w < words; ++w) {
                reg(result, w, lane) = reg(o[2], offset + w, lane);
            }
        });
        break;
    }
    case spv::CompositeInsert: {
        std::uint32_t type = type_of[o[3]];
        std::uint32_t offset = 0;
        for (std::uint32_t i = 4; i < inst.count; ++i) {
            auto added = flat_offset(type, o[i]);
            offset += added ? added.value() : 0;
        }
        const std::uint32_t part = words_of(o[2]);
        each_lane([&](std::size_t lane) {
            for (std::uint32_t w = 0; w < words; ++w) {
                reg(result, w, lane) = reg(o[3], w, lane);
            }
            for (std::uint32_t w = 0; w < part && offset + w < words; ++w) {
                reg(result, offset + w, lane) = reg(o[2], w, lane);
            }
        });
        break;
    }
    case spv::VectorExtractDynamic:
        each_lane([&](std::size_t lane) {
            const std::uint32_t index = std::min(reg(o[3], 0, lane), words_of(o[2]) - 1);
            reg(result, 0, lane) = reg(o[2], index, lane);
        });
        break;
    case spv::VectorInsertDynamic:
        each_lane([&](std::size_t lane) {
            for (std::uint32_t w = 0; w < words; ++w) {
                reg(result, w, lane) = reg(o[2], w, lane);
            }
            const std::uint32_t index = reg(o[4], 0, lane);
            if (index < words) {
                reg(result, index, lane) = reg(o[3], 0, lane);
            }
        });
        break;
    case spv::VectorShuffle: {
        const std::uint32_t first = words_of(o[2]);
        each_lane([&](std::size_t lane) {
            std::array<std::uint32_t, 4> out{};
            for (std::uint32_t w = 0; w < words && w < 4; ++w) {
                const std::uint32_t pick = o[4 + w];
                if (pick == 0xFFFFFFFFU) {
                    out[w] = 0;
                } else if (pick < first) {
                    out[w] = reg(o[2], pick, lane);
                } else {
                    out[w] = reg(o[3], std::min(pick - first, words_of(o[3]) - 1), lane);
                }
            }
            for (std::uint32_t w = 0; w < words && w < 4; ++w) {
                reg(result, w, lane) = out[w];
            }
        });
        break;
    }
    case spv::CopyObject:
    case spv::CopyLogical:
    case spv::Bitcast:
        unary([](std::uint32_t x) { return x; });
        break;
    case spv::Transpose: {
        const TypeInfo& out = types[type_of[result]];
        const std::uint32_t rows = types[out.element].count;
        const std::uint32_t columns = out.count;
        each_lane([&](std::size_t lane) {
            std::vector<std::uint32_t> copy(words);
            for (std::uint32_t c = 0; c < columns; ++c) {
                for (std::uint32_t r = 0; r < rows; ++r) {
                    copy[(c * rows) + r] = reg(o[2], (r * columns) + c, lane);
                }
            }
            for (std::uint32_t w = 0; w < words; ++w) {
                reg(result, w, lane) = copy[w];
            }
        });
        break;
    }
    case spv::SampledImage:
    case spv::Image:
        each_lane([&](std::size_t lane) { reg(result, 0, lane) = reg(o[2], 0, lane); });
        break;
    case spv::ImageSampleImplicitLod:
    case spv::ImageSampleExplicitLod:
    case spv::ImageFetch:
        sample(inst, o, mask);
        break;
    case spv::ImageQuerySizeLod:
    case spv::ImageQuerySize:
        each_lane([&](std::size_t lane) {
            const auto size = textures->size ? textures->size(reg(o[2], 0, lane))
                                             : std::array<std::uint32_t, 2>{1, 1};
            for (std::uint32_t w = 0; w < words && w < 2; ++w) {
                reg(result, w, lane) = size.at(w);
            }
        });
        break;
    case spv::ImageQueryLevels:
        each_lane([&](std::size_t lane) { reg(result, 0, lane) = 1; });
        break;
    case spv::ConvertFToU:
        unary([](std::uint32_t x) { return float_to_uint(to_float(x)); });
        break;
    case spv::ConvertFToS:
        unary([](std::uint32_t x) { return from_int(float_to_int(to_float(x))); });
        break;
    case spv::ConvertSToF:
        unary([](std::uint32_t x) { return from_float(static_cast<float>(to_int(x))); });
        break;
    case spv::ConvertUToF:
        unary([](std::uint32_t x) { return from_float(static_cast<float>(x)); });
        break;
    case spv::SNegate:
        unary([](std::uint32_t x) { return 0U - x; });
        break;
    case spv::FNegate:
        unary([](std::uint32_t x) { return from_float(-to_float(x)); });
        break;
    case spv::IAdd:
        binary([](std::uint32_t x, std::uint32_t y) { return x + y; });
        break;
    case spv::ISub:
        binary([](std::uint32_t x, std::uint32_t y) { return x - y; });
        break;
    case spv::IMul:
        binary([](std::uint32_t x, std::uint32_t y) { return x * y; });
        break;
    case spv::UDiv:
        binary([](std::uint32_t x, std::uint32_t y) { return y == 0 ? 0xFFFFFFFFU : x / y; });
        break;
    case spv::UMod:
        binary([](std::uint32_t x, std::uint32_t y) { return y == 0 ? 0xFFFFFFFFU : x % y; });
        break;
    case spv::SDiv:
    case spv::SRem:
    case spv::SMod: {
        const std::uint16_t op = inst.op;
        binary([op](std::uint32_t x, std::uint32_t y) {
            const std::int64_t a = to_int(x);
            const std::int64_t b = to_int(y);
            if (b == 0) {
                return 0U;
            }
            std::int64_t out = op == spv::SDiv ? a / b : a % b;
            if (op == spv::SMod && out != 0 && ((out < 0) != (b < 0))) {
                out += b;
            }
            return static_cast<std::uint32_t>(out);
        });
        break;
    }
    case spv::FAdd:
        binary_f([](float x, float y) { return x + y; });
        break;
    case spv::FSub:
        binary_f([](float x, float y) { return x - y; });
        break;
    case spv::FMul:
        binary_f([](float x, float y) { return x * y; });
        break;
    case spv::FDiv:
        binary_f([](float x, float y) { return x / y; });
        break;
    case spv::FRem:
        binary_f([](float x, float y) { return std::fmod(x, y); });
        break;
    case spv::FMod:
        binary_f([](float x, float y) { return x - (y * std::floor(x / y)); });
        break;
    case spv::VectorTimesScalar:
    case spv::MatrixTimesScalar:
        each_lane([&](std::size_t lane) {
            const float scalar = f(o[3], 0, lane);
            for (std::uint32_t w = 0; w < words; ++w) {
                reg(result, w, lane) = from_float(f(o[2], w, lane) * scalar);
            }
        });
        break;
    case spv::MatrixTimesVector: {
        const TypeInfo& matrix = types[type_of[o[2]]];
        const std::uint32_t rows = types[matrix.element].count;
        each_lane([&](std::size_t lane) {
            for (std::uint32_t r = 0; r < rows; ++r) {
                float sum = 0.0F;
                for (std::uint32_t c = 0; c < matrix.count; ++c) {
                    sum += f(o[2], (c * rows) + r, lane) * f(o[3], c, lane);
                }
                reg(result, r, lane) = from_float(sum);
            }
        });
        break;
    }
    case spv::VectorTimesMatrix: {
        const TypeInfo& matrix = types[type_of[o[3]]];
        const std::uint32_t rows = types[matrix.element].count;
        each_lane([&](std::size_t lane) {
            for (std::uint32_t c = 0; c < matrix.count; ++c) {
                float sum = 0.0F;
                for (std::uint32_t r = 0; r < rows; ++r) {
                    sum += f(o[2], r, lane) * f(o[3], (c * rows) + r, lane);
                }
                reg(result, c, lane) = from_float(sum);
            }
        });
        break;
    }
    case spv::MatrixTimesMatrix: {
        const TypeInfo& left = types[type_of[o[2]]];
        const TypeInfo& right = types[type_of[o[3]]];
        const std::uint32_t rows = types[left.element].count;
        const std::uint32_t inner = left.count;
        each_lane([&](std::size_t lane) {
            std::vector<std::uint32_t> out(words);
            for (std::uint32_t c = 0; c < right.count; ++c) {
                for (std::uint32_t r = 0; r < rows; ++r) {
                    float sum = 0.0F;
                    for (std::uint32_t k = 0; k < inner; ++k) {
                        sum += f(o[2], (k * rows) + r, lane) * f(o[3], (c * inner) + k, lane);
                    }
                    out[(c * rows) + r] = from_float(sum);
                }
            }
            for (std::uint32_t w = 0; w < words; ++w) {
                reg(result, w, lane) = out[w];
            }
        });
        break;
    }
    case spv::OuterProduct: {
        const std::uint32_t rows = words_of(o[2]);
        const std::uint32_t columns = words_of(o[3]);
        each_lane([&](std::size_t lane) {
            for (std::uint32_t c = 0; c < columns; ++c) {
                for (std::uint32_t r = 0; r < rows; ++r) {
                    reg(result, (c * rows) + r, lane) =
                        from_float(f(o[2], r, lane) * f(o[3], c, lane));
                }
            }
        });
        break;
    }
    case spv::Dot:
        each_lane([&](std::size_t lane) {
            float sum = 0.0F;
            for (std::uint32_t w = 0; w < words_of(o[2]); ++w) {
                sum += f(o[2], w, lane) * f(o[3], w, lane);
            }
            reg(result, 0, lane) = from_float(sum);
        });
        break;
    case spv::Any:
    case spv::All: {
        const bool any = inst.op == spv::Any;
        each_lane([&](std::size_t lane) {
            bool out = !any;
            for (std::uint32_t w = 0; w < words_of(o[2]); ++w) {
                const bool set = reg(o[2], w, lane) != 0;
                out = any ? (out || set) : (out && set);
            }
            reg(result, 0, lane) = out ? 1U : 0U;
        });
        break;
    }
    case spv::IsNan:
        unary([](std::uint32_t x) { return std::isnan(to_float(x)) ? 1U : 0U; });
        break;
    case spv::IsInf:
        unary([](std::uint32_t x) { return std::isinf(to_float(x)) ? 1U : 0U; });
        break;
    case spv::LogicalEqual:
        compare_i([](std::uint32_t x, std::uint32_t y) { return (x != 0) == (y != 0); });
        break;
    case spv::LogicalNotEqual:
        compare_i([](std::uint32_t x, std::uint32_t y) { return (x != 0) != (y != 0); });
        break;
    case spv::LogicalOr:
        compare_i([](std::uint32_t x, std::uint32_t y) { return x != 0 || y != 0; });
        break;
    case spv::LogicalAnd:
        compare_i([](std::uint32_t x, std::uint32_t y) { return x != 0 && y != 0; });
        break;
    case spv::LogicalNot:
        unary([](std::uint32_t x) { return x != 0 ? 0U : 1U; });
        break;
    case spv::Select: {
        const std::uint32_t condition_words = words_of(o[2]);
        each_lane([&](std::size_t lane) {
            for (std::uint32_t w = 0; w < words; ++w) {
                const bool pick = reg(o[2], condition_words == 1 ? 0 : w, lane) != 0;
                reg(result, w, lane) = pick ? reg(o[3], w, lane) : reg(o[4], w, lane);
            }
        });
        break;
    }
    case spv::IEqual:
        compare_i([](std::uint32_t x, std::uint32_t y) { return x == y; });
        break;
    case spv::INotEqual:
        compare_i([](std::uint32_t x, std::uint32_t y) { return x != y; });
        break;
    case spv::UGreaterThan:
        compare_i([](std::uint32_t x, std::uint32_t y) { return x > y; });
        break;
    case spv::UGreaterThanEqual:
        compare_i([](std::uint32_t x, std::uint32_t y) { return x >= y; });
        break;
    case spv::ULessThan:
        compare_i([](std::uint32_t x, std::uint32_t y) { return x < y; });
        break;
    case spv::ULessThanEqual:
        compare_i([](std::uint32_t x, std::uint32_t y) { return x <= y; });
        break;
    case spv::SGreaterThan:
        compare_i([](std::uint32_t x, std::uint32_t y) { return to_int(x) > to_int(y); });
        break;
    case spv::SGreaterThanEqual:
        compare_i([](std::uint32_t x, std::uint32_t y) { return to_int(x) >= to_int(y); });
        break;
    case spv::SLessThan:
        compare_i([](std::uint32_t x, std::uint32_t y) { return to_int(x) < to_int(y); });
        break;
    case spv::SLessThanEqual:
        compare_i([](std::uint32_t x, std::uint32_t y) { return to_int(x) <= to_int(y); });
        break;
    case spv::FOrdEqual:
        compare_f([](float x, float y) { return x == y; });
        break;
    case spv::FUnordEqual:
        compare_f([&](float x, float y) { return unordered(x, y) || x == y; });
        break;
    case spv::FOrdNotEqual:
        compare_f([&](float x, float y) { return !unordered(x, y) && x != y; });
        break;
    case spv::FUnordNotEqual:
        compare_f([](float x, float y) { return !(x == y); });
        break;
    case spv::FOrdLessThan:
        compare_f([](float x, float y) { return x < y; });
        break;
    case spv::FUnordLessThan:
        compare_f([&](float x, float y) { return unordered(x, y) || x < y; });
        break;
    case spv::FOrdGreaterThan:
        compare_f([](float x, float y) { return x > y; });
        break;
    case spv::FUnordGreaterThan:
        compare_f([&](float x, float y) { return unordered(x, y) || x > y; });
        break;
    case spv::FOrdLessThanEqual:
        compare_f([](float x, float y) { return x <= y; });
        break;
    case spv::FUnordLessThanEqual:
        compare_f([&](float x, float y) { return unordered(x, y) || x <= y; });
        break;
    case spv::FOrdGreaterThanEqual:
        compare_f([](float x, float y) { return x >= y; });
        break;
    case spv::FUnordGreaterThanEqual:
        compare_f([&](float x, float y) { return unordered(x, y) || x >= y; });
        break;
    case spv::ShiftRightLogical:
        binary([](std::uint32_t x, std::uint32_t y) { return x >> (y & 31U); });
        break;
    case spv::ShiftRightArithmetic:
        binary([](std::uint32_t x, std::uint32_t y) { return from_int(to_int(x) >> (y & 31U)); });
        break;
    case spv::ShiftLeftLogical:
        binary([](std::uint32_t x, std::uint32_t y) { return x << (y & 31U); });
        break;
    case spv::BitwiseOr:
        binary([](std::uint32_t x, std::uint32_t y) { return x | y; });
        break;
    case spv::BitwiseXor:
        binary([](std::uint32_t x, std::uint32_t y) { return x ^ y; });
        break;
    case spv::BitwiseAnd:
        binary([](std::uint32_t x, std::uint32_t y) { return x & y; });
        break;
    case spv::Not:
        unary([](std::uint32_t x) { return ~x; });
        break;
    case spv::BitCount:
        unary([](std::uint32_t x) { return static_cast<std::uint32_t>(std::popcount(x)); });
        break;
    case spv::BitReverse:
        unary([](std::uint32_t x) {
            std::uint32_t out = 0;
            for (std::uint32_t i = 0; i < 32; ++i) {
                out |= ((x >> i) & 1U) << (31U - i);
            }
            return out;
        });
        break;
    case spv::BitFieldInsert:
        each_lane([&](std::size_t lane) {
            const std::uint32_t offset = reg(o[4], 0, lane) & 31U;
            const std::uint32_t count = std::min(reg(o[5], 0, lane), 32U - offset);
            const std::uint32_t field =
                count >= 32 ? 0xFFFFFFFFU : (((1U << count) - 1U) << offset);
            for (std::uint32_t w = 0; w < words; ++w) {
                reg(result, w, lane) =
                    (reg(o[2], w, lane) & ~field) | ((reg(o[3], w, lane) << offset) & field);
            }
        });
        break;
    case spv::BitFieldSExtract:
    case spv::BitFieldUExtract: {
        const bool is_signed = inst.op == spv::BitFieldSExtract;
        each_lane([&](std::size_t lane) {
            const std::uint32_t offset = reg(o[3], 0, lane) & 31U;
            const std::uint32_t count = std::min(reg(o[4], 0, lane), 32U - offset);
            for (std::uint32_t w = 0; w < words; ++w) {
                if (count == 0) {
                    reg(result, w, lane) = 0;
                    continue;
                }
                std::uint32_t value = reg(o[2], w, lane) >> offset;
                if (count < 32) {
                    value &= (1U << count) - 1U;
                    if (is_signed && (value & (1U << (count - 1U))) != 0) {
                        value |= ~((1U << count) - 1U);
                    }
                }
                reg(result, w, lane) = value;
            }
        });
        break;
    }
    case spv::DPdx:
    case spv::DPdy:
    case spv::Fwidth:
    case spv::DPdxFine:
    case spv::DPdyFine:
    case spv::FwidthFine:
    case spv::DPdxCoarse:
    case spv::DPdyCoarse:
    case spv::FwidthCoarse:
        derivative(inst, o, mask);
        break;
    default:
        break;
    }
    return true;
}

void ShaderVm::Module::execute(Thread thread, std::vector<Thread>& pending) {
    while (thread.pc < code.size()) {
        bool over = false;
        for (std::size_t lane = 0; lane < kLanes; ++lane) {
            if ((thread.mask & (1U << lane)) != 0 && ++steps[lane] > ShaderVm::kMaxSteps) {
                over = true;
            }
        }
        if (over || !step(thread, pending)) {
            return;
        }
    }
}

void ShaderVm::Module::run(Lanes& run_lanes,
                           std::span<const std::span<const std::byte>> run_uniforms,
                           const Textures& run_textures) {
    lanes = &run_lanes;
    uniforms = run_uniforms;
    textures = &run_textures;
    steps = {};
    const std::uint32_t count = std::clamp<std::uint32_t>(run_lanes.count, 1, kLanes);
    run_lanes.discarded = {};
    std::fill(memory.begin(), memory.end(), 0U);
    for (std::size_t index = 1; index < variables.size(); ++index) {
        const VariableInfo& variable = variables[index];
        if (variable.storage == spv::Private && variable.initializer != 0) {
            for (std::uint32_t w = 0; w < variable.words; ++w) {
                for (std::size_t lane = 0; lane < kLanes; ++lane) {
                    memory[((std::size_t{variable.base} + w) * kLanes) + lane] =
                        reg(variable.initializer, w, lane);
                }
            }
        }
        if (variable.storage == spv::Input) {
            for (std::uint32_t w = 0; w < variable.words; ++w) {
                for (std::size_t lane = 0; lane < count; ++lane) {
                    memory[((std::size_t{variable.base} + w) * kLanes) + lane] = from_float(
                        run_lanes.inputs.at(lane).at(variable.location.value_or(0)).at(w));
                }
            }
        }
    }
    // Global variables' registers hold pointers to them.
    for (std::uint32_t id = 0; id < var_of.size(); ++id) {
        if (var_of[id] != 0 && variables[var_of[id]].storage != spv::FunctionStorage) {
            for (std::size_t lane = 0; lane < kLanes; ++lane) {
                reg(id, 0, lane) = var_of[id] << kPointerShift;
            }
        }
    }
    std::vector<Thread> pending;
    Thread first;
    first.mask = static_cast<std::uint8_t>((1U << count) - 1U);
    first.block = 0;
    enter_block(first, functions[entry].first_label);
    pending.push_back(std::move(first));
    while (!pending.empty()) {
        Thread thread = std::move(pending.back());
        pending.pop_back();
        execute(std::move(thread), pending);
    }
    for (std::size_t index = 1; index < variables.size(); ++index) {
        const VariableInfo& variable = variables[index];
        if (variable.storage != spv::Output) {
            continue;
        }
        for (std::size_t lane = 0; lane < count; ++lane) {
            ShaderVm::Float4& out = run_lanes.outputs.at(lane).at(variable.location.value_or(0));
            for (std::uint32_t w = 0; w < variable.words && w < 4; ++w) {
                out.at(w) = to_float(memory[((std::size_t{variable.base} + w) * kLanes) + lane]);
            }
        }
    }
}

ShaderVm::ShaderVm() = default;
ShaderVm::ShaderVm(const ShaderVm& other)
    : module_(other.module_ ? std::make_unique<Module>(*other.module_) : nullptr) {}
ShaderVm& ShaderVm::operator=(const ShaderVm& other) {
    if (this != &other) {
        module_ = other.module_ ? std::make_unique<Module>(*other.module_) : nullptr;
    }
    return *this;
}
ShaderVm::ShaderVm(ShaderVm&&) noexcept = default;
ShaderVm& ShaderVm::operator=(ShaderVm&&) noexcept = default;
ShaderVm::~ShaderVm() = default;

Result<ShaderVm> ShaderVm::load(std::span<const std::uint32_t> words) {
    ShaderVm vm;
    vm.module_ = std::make_unique<Module>();
    if (auto loaded = vm.module_->load(words); !loaded) {
        return loaded.error();
    }
    return vm;
}

void ShaderVm::run(Lanes& lanes, std::span<const std::span<const std::byte>> uniforms,
                   const Textures& textures) {
    if (module_) {
        module_->run(lanes, uniforms, textures);
    }
}

} // namespace slash::render
