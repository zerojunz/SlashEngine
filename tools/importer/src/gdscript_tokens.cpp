#include "godot_binary.hpp"

#include "slash/core/json.hpp"
#include "slash/importer/godot_pack.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <numeric>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace slash::importer::godot {
namespace {

// GDScriptTokenizer::Token::Type as of tokenizer version 101.
enum Token : std::uint8_t {
    kEmpty,
    kAnnotation,
    kIdentifier,
    kLiteral,
    kLess,
    kLessEqual,
    kGreater,
    kGreaterEqual,
    kEqualEqual,
    kBangEqual,
    kAnd,
    kOr,
    kNot,
    kAmpersandAmpersand,
    kPipePipe,
    kBang,
    kAmpersand,
    kPipe,
    kTilde,
    kCaret,
    kLessLess,
    kGreaterGreater,
    kPlus,
    kMinus,
    kStar,
    kStarStar,
    kSlash,
    kPercent,
    kEqual,
    kPlusEqual,
    kMinusEqual,
    kStarEqual,
    kStarStarEqual,
    kSlashEqual,
    kPercentEqual,
    kLessLessEqual,
    kGreaterGreaterEqual,
    kAmpersandEqual,
    kPipeEqual,
    kCaretEqual,
    kIf,
    kElif,
    kElse,
    kFor,
    kWhile,
    kBreak,
    kContinue,
    kPass,
    kReturn,
    kMatch,
    kWhen,
    kAs,
    kAssert,
    kAwait,
    kBreakpoint,
    kClass,
    kClassName,
    kConst,
    kEnum,
    kExtends,
    kFunc,
    kIn,
    kIs,
    kNamespace,
    kPreload,
    kSelf,
    kSignal,
    kStatic,
    kSuper,
    kTrait,
    kVar,
    kVoid,
    kYield,
    kBracketOpen,
    kBracketClose,
    kBraceOpen,
    kBraceClose,
    kParenthesisOpen,
    kParenthesisClose,
    kComma,
    kSemicolon,
    kPeriod,
    kPeriodPeriod,
    kPeriodPeriodPeriod,
    kColon,
    kDollar,
    kForwardArrow,
    kUnderscore,
    kNewline,
    kIndent,
    kDedent,
    kConstPi,
    kConstTau,
    kConstInf,
    kConstNan,
    kVcsConflictMarker,
    kBacktick,
    kQuestionMark,
    kError,
    kEof,
    kTokenMax,
};

// The text of each token type, as GDScriptTokenizer's token_names; "" for those with none.
constexpr std::array<const char*, kTokenMax> kNames{
    "",           "",      "",       "",        "<",      "<=",    ">",          ">=",
    "==",         "!=",    "and",    "or",      "not",    "&&",    "||",         "!",
    "&",          "|",     "~",      "^",       "<<",     ">>",    "+",          "-",
    "*",          "**",    "/",      "%",       "=",      "+=",    "-=",         "*=",
    "**=",        "/=",    "%=",     "<<=",     ">>=",    "&=",    "|=",         "^=",
    "if",         "elif",  "else",   "for",     "while",  "break", "continue",   "pass",
    "return",     "match", "when",   "as",      "assert", "await", "breakpoint", "class",
    "class_name", "const", "enum",   "extends", "func",   "in",    "is",         "namespace",
    "preload",    "self",  "signal", "static",  "super",  "trait", "var",        "void",
    "yield",      "[",     "]",      "{",       "}",      "(",     ")",          ",",
    ";",          ".",     "..",     "...",     ":",      "$",     "->",         "_",
    "",           "",      "",       "PI",      "TAU",    "INF",   "NAN",        "",
    "`",          "?",     "",       "",
};

constexpr std::uint32_t kTokenizerVersion = 101;
// Godot 4.3 and 4.4's version: no "..." token, and an unused number before the token count.
constexpr std::uint32_t kTokenizerVersion43 = 100;
constexpr std::uint32_t kTokenByteMask = 0x80;
constexpr std::uint32_t kTokenBits = 8;
constexpr std::uint32_t kTokenMask = (1U << (kTokenBits - 1)) - 1;

struct Scanned {
    std::uint8_t type = kEmpty;
    std::uint32_t index = 0; // into the identifiers or constants
    std::uint32_t line = 1;
};

std::string quoted(std::string_view text, std::string_view prefix) {
    std::string out(prefix);
    out += '"';
    for (const char c : text) {
        switch (c) {
        case '\\':
            out += "\\\\";
            break;
        case '"':
            out += "\\\"";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\t':
            out += "\\t";
            break;
        case '\r':
            out += "\\r";
            break;
        default:
            out += c;
            break;
        }
    }
    return out + '"';
}

std::string number_text(double number) {
    if (std::isnan(number)) {
        return "NAN";
    }
    if (std::isinf(number)) {
        return number > 0 ? "INF" : "-INF";
    }
    std::string text = json::write(json::Value(number), json::Style::Compact);
    if (text.find_first_of(".eE") == std::string::npos) {
        text += ".0";
    }
    return text;
}

// A constant as GDScript source writes it.
std::string literal_text(const Value& value) {
    switch (value.kind) {
    case Value::Kind::Nil:
        return "null";
    case Value::Kind::Bool:
        return value.boolean ? "true" : "false";
    case Value::Kind::Int:
        return std::to_string(value.integer);
    case Value::Kind::Float:
        return number_text(value.real);
    case Value::Kind::String:
        // A string of several lines stays on its lines, so later tokens keep their numbers.
        if (value.text.find('\n') != std::string::npos &&
            value.text.find(R"(""")") == std::string::npos && value.text.back() != '"' &&
            value.text.back() != '\\') {
            std::string body;
            for (const char c : value.text) {
                body += c == '\\' ? std::string("\\\\") : std::string(1, c);
            }
            return R"(""")" + body + R"(""")";
        }
        return quoted(value.text, "");
    case Value::Kind::StringName:
        return quoted(value.text, "&");
    case Value::Kind::Call:
        if (value.text == "NodePath") {
            return quoted(value.string_argument().value_or(""), "^");
        }
        return write_value(value);
    case Value::Kind::Array:
    case Value::Kind::Dictionary:
        return write_value(value);
    }
    return "null";
}

// Tokens after which a '-', '+', '%' or '...' is unary, and a '(' or '[' starts a group.
bool ends_operand(std::uint8_t type) {
    switch (type) {
    case kIdentifier:
    case kLiteral:
    case kParenthesisClose:
    case kBracketClose:
    case kBraceClose:
    case kSelf:
    case kSuper:
    case kConstPi:
    case kConstTau:
    case kConstInf:
    case kConstNan:
    case kUnderscore:
        return true;
    default:
        return false;
    }
}

bool calls_with(std::uint8_t type) {
    return type == kIdentifier || type == kAnnotation || type == kParenthesisClose ||
           type == kBracketClose || type == kPreload || type == kSuper || type == kAssert ||
           type == kFunc || type == kLiteral || type == kAwait || type == kYield;
}

} // namespace

Result<std::string> detokenize_gdscript(std::string_view bytes, int tab_size) {
    ByteReader header(bytes);
    if (header.bytes(4) != "GDSC") {
        return make_error(ErrorCode::InvalidArgument, "not a GDScript token file");
    }
    const std::uint32_t version = header.u32();
    if (version != kTokenizerVersion && version != kTokenizerVersion43) {
        return make_error(ErrorCode::InvalidArgument,
                          "GDScript tokens of version " + std::to_string(version) +
                              "; versions 100 (Godot 4.3 and 4.4) and 101 (Godot 4.5) are read");
    }
    const std::uint32_t size = header.u32();
    std::string contents;
    if (size == 0) {
        contents = std::string(bytes.substr(12));
    } else {
        auto plain = zstd_decompress(bytes.substr(12), size);
        if (!plain) {
            return plain.error();
        }
        contents = std::move(plain.value());
    }

    ByteReader reader(contents);
    const std::uint32_t identifier_count = reader.u32();
    const std::uint32_t constant_count = reader.u32();
    const std::uint32_t line_count = reader.u32();
    if (version == kTokenizerVersion43) {
        reader.skip(4); // unused
    }
    const std::uint32_t token_count = reader.u32();
    std::vector<std::string> identifiers;
    for (std::uint32_t i = 0; i < identifier_count && reader.ok(); ++i) {
        const std::uint32_t length = reader.u32();
        std::string text;
        for (std::uint32_t j = 0; j < length && reader.ok(); ++j) {
            std::uint32_t code = 0;
            for (std::uint32_t k = 0; k < 4; ++k) {
                code |= static_cast<std::uint32_t>(reader.u8() ^ 0xB6U) << (8U * k);
            }
            // UTF-32 to UTF-8.
            if (code < 0x80) {
                text += static_cast<char>(code);
            } else if (code < 0x800) {
                text += static_cast<char>(0xC0 | (code >> 6U));
                text += static_cast<char>(0x80 | (code & 0x3FU));
            } else if (code < 0x10000) {
                text += static_cast<char>(0xE0 | (code >> 12U));
                text += static_cast<char>(0x80 | ((code >> 6U) & 0x3FU));
                text += static_cast<char>(0x80 | (code & 0x3FU));
            } else {
                text += static_cast<char>(0xF0 | (code >> 18U));
                text += static_cast<char>(0x80 | ((code >> 12U) & 0x3FU));
                text += static_cast<char>(0x80 | ((code >> 6U) & 0x3FU));
                text += static_cast<char>(0x80 | (code & 0x3FU));
            }
        }
        identifiers.push_back(std::move(text));
    }
    std::vector<Value> constants;
    for (std::uint32_t i = 0; i < constant_count && reader.ok(); ++i) {
        auto value = decode_variant(reader);
        if (!value) {
            return value.error();
        }
        constants.push_back(std::move(value.value()));
    }
    std::map<std::uint32_t, std::uint32_t> columns;
    for (std::uint32_t i = 0; i < line_count; ++i) {
        reader.skip(8); // token index and line, which each token carries too
    }
    for (std::uint32_t i = 0; i < line_count && reader.ok(); ++i) {
        const std::uint32_t token = reader.u32();
        columns[token] = reader.u32();
    }
    std::vector<Scanned> tokens;
    for (std::uint32_t i = 0; i < token_count && reader.ok(); ++i) {
        std::uint32_t raw = reader.u8();
        if ((raw & kTokenByteMask) != 0) {
            for (std::uint32_t k = 1; k < 4; ++k) {
                raw |= static_cast<std::uint32_t>(reader.u8()) << (8U * k);
            }
        }
        Scanned token;
        token.type = static_cast<std::uint8_t>(raw & kTokenMask);
        if (version == kTokenizerVersion43 && token.type >= kPeriodPeriodPeriod) {
            ++token.type; // version 100 has no "..."
        }
        token.index = raw >> kTokenBits;
        token.line = reader.u32();
        if (token.type >= kTokenMax) {
            return make_error(ErrorCode::InvalidArgument,
                              "unknown GDScript token " + std::to_string(token.type));
        }
        tokens.push_back(token);
    }
    if (!reader.ok()) {
        return make_error(ErrorCode::InvalidArgument, "GDScript tokens are cut short");
    }

    const auto text_of = [&](const Scanned& token) -> std::string {
        switch (token.type) {
        case kIdentifier:
        case kAnnotation:
            return token.index < identifiers.size() ? identifiers[token.index] : "_";
        case kLiteral:
            return token.index < constants.size() ? literal_text(constants[token.index]) : "null";
        default:
            return kNames[token.type];
        }
    };

    std::string out;
    std::uint32_t line = 1;
    // With a tab counted as one column, as a space is, a file whose indents are all multiples of
    // two or more was indented with spaces, and any other with tabs.
    std::uint32_t divisor = 0;
    for (const auto& [token, column] : columns) {
        if (column > 1) {
            divisor = std::gcd(divisor, column - 1);
        }
    }
    const bool spaces = divisor >= 2;
    std::uint8_t previous = kEmpty; // on this line
    bool node_path = false;         // inside $Path/To/Node or %Unique
    std::string indentation;        // of the last line that kept its column
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        const Scanned& token = tokens[i];
        if (token.type == kError || token.type == kEof) {
            continue;
        }
        const bool new_line = i == 0 || token.line > tokens[i - 1].line;
        if (new_line) {
            const auto column = columns.find(static_cast<std::uint32_t>(i));
            if (i > 0) {
                // A later line with no column, not reached by a string's own lines, was
                // continued after a backslash.
                if (column == columns.end() && line < token.line) {
                    out += " \\";
                }
                out += '\n';
                ++line;
            }
            while (line < token.line) {
                out += '\n';
                ++line;
            }
            if (column != columns.end()) {
                const std::uint32_t indent = column->second > 0 ? column->second - 1 : 0;
                if (tab_size > 1) {
                    indentation = std::string(indent / static_cast<std::uint32_t>(tab_size), '\t') +
                                  std::string(indent % static_cast<std::uint32_t>(tab_size), ' ');
                } else {
                    indentation = std::string(indent, spaces ? ' ' : '\t');
                }
                out += indentation;
            } else if (i > 0) {
                // A continued line, one level in.
                out += indentation + (tab_size == 1 && spaces ? "    " : "\t");
            }
            previous = kEmpty;
            node_path = false;
        }
        const std::uint8_t type = token.type;
        const std::uint8_t next = i + 1 < tokens.size() && tokens[i + 1].line == token.line
                                      ? tokens[i + 1].type
                                      : static_cast<std::uint8_t>(kEmpty);
        // An operator after an operand is binary, on the same line or the one after.
        const bool unary_position =
            new_line ? i == 0 || !ends_operand(tokens[i - 1].type) : !ends_operand(previous);
        bool space = previous != kEmpty;
        if (node_path && (type == kIdentifier || type == kSlash || type == kPercent ||
                          (type == kLiteral && previous == kDollar))) {
            space = false;
        } else {
            node_path = false;
            if (previous == kParenthesisOpen || previous == kBracketOpen ||
                previous == kBraceOpen || previous == kPeriod || previous == kDollar ||
                previous == kBang || previous == kTilde) {
                space = false;
            }
            if (type == kParenthesisClose || type == kBracketClose || type == kBraceClose ||
                type == kComma || type == kPeriod || type == kSemicolon) {
                space = false;
            }
            if (type == kColon) {
                // "x := 1" keeps its space; "var x: int" and "{"a": 1}" don't.
                space = space && next == kEqual;
            }
            if (type == kEqual && previous == kColon) {
                space = false;
            }
            if (type == kParenthesisOpen && calls_with(previous)) {
                space = false;
            }
            if (type == kBracketOpen && ends_operand(previous) && previous != kUnderscore) {
                space = false;
            }
        }
        if (space) {
            out += ' ';
        }
        std::string text = text_of(token);
        line += static_cast<std::uint32_t>(std::ranges::count(text, '\n'));
        out += text;
        // A unary operator holds on to its operand.
        if ((type == kMinus || type == kPlus || type == kPeriodPeriodPeriod) && unary_position) {
            previous = kParenthesisOpen;
        } else if (type == kPercent && unary_position) {
            previous = kDollar;
            node_path = true;
        } else {
            previous = type;
            node_path = node_path || type == kDollar;
        }
    }
    out += '\n';
    return out;
}

namespace {

// --- GDScriptTokenizerText, in the multiline mode GDScriptTokenizerBuffer scans with ---

std::u32string utf32(std::string_view text) {
    std::u32string out;
    for (std::size_t i = 0; i < text.size();) {
        const auto byte = static_cast<std::uint8_t>(text[i]);
        std::size_t length = 1;
        char32_t code = byte;
        if (byte >= 0xF0) {
            length = 4;
            code = byte & 0x07U;
        } else if (byte >= 0xE0) {
            length = 3;
            code = byte & 0x0FU;
        } else if (byte >= 0xC0) {
            length = 2;
            code = byte & 0x1FU;
        }
        for (std::size_t k = 1; k < length && i + k < text.size(); ++k) {
            code = (code << 6U) | (static_cast<std::uint8_t>(text[i + k]) & 0x3FU);
        }
        out += code;
        i += length;
    }
    return out;
}

std::string utf8(std::u32string_view text) {
    std::string out;
    for (const char32_t code : text) {
        if (code < 0x80) {
            out += static_cast<char>(code);
        } else if (code < 0x800) {
            out += static_cast<char>(0xC0 | (code >> 6U));
            out += static_cast<char>(0x80 | (code & 0x3FU));
        } else if (code < 0x10000) {
            out += static_cast<char>(0xE0 | (code >> 12U));
            out += static_cast<char>(0x80 | ((code >> 6U) & 0x3FU));
            out += static_cast<char>(0x80 | (code & 0x3FU));
        } else {
            out += static_cast<char>(0xF0 | (code >> 18U));
            out += static_cast<char>(0x80 | ((code >> 12U) & 0x3FU));
            out += static_cast<char>(0x80 | ((code >> 6U) & 0x3FU));
            out += static_cast<char>(0x80 | (code & 0x3FU));
        }
    }
    return out;
}

bool is_digit(char32_t c) {
    return c >= '0' && c <= '9';
}

bool is_hex_digit(char32_t c) {
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

bool is_binary_digit(char32_t c) {
    return c == '0' || c == '1';
}

char32_t hex_value(char32_t digit) {
    return is_digit(digit) ? digit - '0' : (digit | 0x20U) - 'a' + 10;
}

bool is_unicode_space(char32_t c) {
    return c == 0xA0 || c == 0x1680 || (c >= 0x2000 && c <= 0x200F) || c == 0x2028 || c == 0x2029 ||
           c == 0x202F || c == 0x205F || c == 0x3000 || c == 0xFEFF ||
           (c >= 0x202A && c <= 0x202E) || (c >= 0x2066 && c <= 0x2069);
}

// ASCII letters and '_', and any character past ASCII that isn't a space: Godot checks those
// against Unicode's identifier tables, which valid scripts already satisfy.
bool is_identifier_start(char32_t c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
           (c >= 0x80 && !is_unicode_space(c));
}

bool is_identifier_continue(char32_t c) {
    return is_identifier_start(c) || is_digit(c);
}

constexpr std::array<std::pair<std::u32string_view, Token>, 41> kKeywords = {{
    {U"as", kAs},
    {U"and", kAnd},
    {U"assert", kAssert},
    {U"await", kAwait},
    {U"break", kBreak},
    {U"breakpoint", kBreakpoint},
    {U"class", kClass},
    {U"class_name", kClassName},
    {U"const", kConst},
    {U"continue", kContinue},
    {U"elif", kElif},
    {U"else", kElse},
    {U"enum", kEnum},
    {U"extends", kExtends},
    {U"for", kFor},
    {U"func", kFunc},
    {U"if", kIf},
    {U"in", kIn},
    {U"is", kIs},
    {U"match", kMatch},
    {U"namespace", kNamespace},
    {U"not", kNot},
    {U"or", kOr},
    {U"pass", kPass},
    {U"preload", kPreload},
    {U"return", kReturn},
    {U"self", kSelf},
    {U"signal", kSignal},
    {U"static", kStatic},
    {U"super", kSuper},
    {U"trait", kTrait},
    {U"var", kVar},
    {U"void", kVoid},
    {U"while", kWhile},
    {U"when", kWhen},
    {U"yield", kYield},
    {U"INF", kConstInf},
    {U"NAN", kConstNan},
    {U"PI", kConstPi},
    {U"TAU", kConstTau},
    {U"_", kUnderscore},
}};

// Token::can_precede_bin_op: after these a '+' or '-' before a digit is an operator, not a
// number's sign.
bool can_precede_binary_operator(std::uint8_t type) {
    switch (type) {
    case kIdentifier:
    case kLiteral:
    case kSelf:
    case kBracketClose:
    case kBraceClose:
    case kParenthesisClose:
    case kConstPi:
    case kConstTau:
    case kConstInf:
    case kConstNan:
        return true;
    default:
        return false;
    }
}

struct TextToken {
    std::uint8_t type = kEmpty;
    std::u32string identifier; // an identifier's or annotation's text
    Value literal;
    int start_line = 1;
    int start_column = 1;
    int end_line = 1;
};

class Tokenizer {
public:
    Tokenizer(std::string_view source, int tab_size)
        : source_(utf32(source)), tab_size_(tab_size) {}

    // The next token, or one of type kEof at the end.
    Result<TextToken> scan() {
        skip_whitespace();
        pending_newline_ = false;
        start_ = current_;
        start_line_ = line_;
        start_column_ = column_;
        if (at_end()) {
            return make(kEof);
        }
        const char32_t c = advance();
        if (c == '\\') {
            // A line continued after a backslash.
            if (peek() == '\r') {
                advance();
            }
            if (peek() != '\n') {
                return fail(R"(expected a new line after "\")");
            }
            advance();
            newline(false);
            line_continuation_ = true;
            skip_whitespace();
            continuation_lines_.push_back(line_);
            return scan();
        }
        line_continuation_ = false;
        if (is_digit(c)) {
            return number();
        }
        if (c == 'r' && (peek() == '"' || peek() == '\'')) {
            return string();
        }
        if (is_identifier_start(c)) {
            return identifier();
        }
        return symbol(c);
    }

    [[nodiscard]] const std::vector<int>& continuation_lines() const { return continuation_lines_; }

private:
    Result<TextToken> symbol(char32_t c) {
        switch (c) {
        case '"':
        case '\'':
            return string();
        case '@':
            return annotation();
        case '~':
            return make(kTilde);
        case ',':
            return make(kComma);
        case ':':
            return make(kColon);
        case ';':
            return make(kSemicolon);
        case '$':
            return make(kDollar);
        case '?':
            return make(kQuestionMark);
        case '`':
            return make(kBacktick);
        case '(':
            return make(kParenthesisOpen);
        case '[':
            return make(kBracketOpen);
        case '{':
            return make(kBraceOpen);
        case ')':
            return make(kParenthesisClose);
        case ']':
            return make(kBracketClose);
        case '}':
            return make(kBraceClose);
        case '!':
            return pick('=', kBangEqual, kBang);
        case '.':
            if (peek() == '.') {
                advance();
                return pick('.', kPeriodPeriodPeriod, kPeriodPeriod);
            }
            if (is_digit(peek())) {
                return number();
            }
            return make(kPeriod);
        case '+':
        case '-':
            if (peek() == '=') {
                advance();
                return make(c == '+' ? kPlusEqual : kMinusEqual);
            }
            if (is_digit(peek()) && !can_precede_binary_operator(last_type_)) {
                return number();
            }
            if (c == '-' && peek() == '>') {
                advance();
                return make(kForwardArrow);
            }
            return make(c == '+' ? kPlus : kMinus);
        case '*':
            if (peek() == '=') {
                advance();
                return make(kStarEqual);
            }
            if (peek() == '*') {
                advance();
                return pick('=', kStarStarEqual, kStarStar);
            }
            return make(kStar);
        case '/':
            return pick('=', kSlashEqual, kSlash);
        case '%':
            return pick('=', kPercentEqual, kPercent);
        case '^':
            if (peek() == '"' || peek() == '\'') {
                return string();
            }
            return pick('=', kCaretEqual, kCaret);
        case '&':
            if (peek() == '&') {
                advance();
                return make(kAmpersandAmpersand);
            }
            if (peek() == '"' || peek() == '\'') {
                return string();
            }
            return pick('=', kAmpersandEqual, kAmpersand);
        case '|':
            if (peek() == '|') {
                advance();
                return make(kPipePipe);
            }
            return pick('=', kPipeEqual, kPipe);
        case '=':
            if (peek() == '=') {
                return conflict_marker('=', kEqualEqual);
            }
            return make(kEqual);
        case '<':
        case '>': {
            const bool less = c == '<';
            if (peek() == '=') {
                advance();
                return make(less ? kLessEqual : kGreaterEqual);
            }
            if (peek() == c) {
                if (peek(1) == '=') {
                    advance();
                    advance();
                    return make(less ? kLessLessEqual : kGreaterGreaterEqual);
                }
                return conflict_marker(c, less ? kLessLess : kGreaterGreater);
            }
            return make(less ? kLess : kGreater);
        }
        default:
            return fail("unexpected character U+" + hex(c));
        }
    }

    [[nodiscard]] bool at_end() const { return current_ >= source_.size(); }

    [[nodiscard]] char32_t peek(std::size_t offset = 0) const {
        return current_ + offset < source_.size() ? source_[current_ + offset] : U'\0';
    }

    char32_t advance() {
        if (at_end()) {
            return U'\0';
        }
        ++current_;
        ++column_;
        if (at_end()) {
            newline(true); // as if the file ended in one
        }
        return source_[current_ - 1];
    }

    void newline(bool make_token) {
        if (make_token && !pending_newline_ && !line_continuation_) {
            pending_newline_ = true;
            last_type_ = kNewline;
        }
        ++line_;
        column_ = 1;
    }

    // Skips the blank lines, comment lines and indentation at the start of a line.
    void check_indent() {
        while (!at_end()) {
            const char32_t first = peek();
            if (first != ' ' && first != '\t' && first != '\r' && first != '\n' && first != '#') {
                return;
            }
            if (peek() == '\r') {
                advance();
            }
            if (peek() == '\n') {
                advance();
                newline(false);
                continue;
            }
            while (!at_end() && (peek() == ' ' || peek() == '\t')) {
                if (peek() == '\t') {
                    column_ += tab_size_ - 1;
                }
                advance();
            }
            if (at_end()) {
                return;
            }
            if (peek() == '\r') {
                advance();
            }
            if (peek() == '\n') {
                advance();
                newline(false);
                continue;
            }
            if (peek() != '#') {
                return;
            }
            while (peek() != '\n' && !at_end()) {
                advance();
            }
            if (at_end()) {
                return;
            }
            advance();
            newline(false);
        }
    }

    void skip_whitespace() {
        if (column_ == 1) {
            check_indent();
            return;
        }
        while (true) {
            switch (peek()) {
            case ' ':
            case '\r':
                advance();
                break;
            case '\t':
                advance();
                column_ += tab_size_ - 1;
                break;
            case '\n':
                advance();
                newline(true);
                check_indent();
                break;
            case '#':
                while (peek() != '\n' && !at_end()) {
                    advance();
                }
                if (at_end()) {
                    return;
                }
                advance();
                newline(true);
                check_indent();
                break;
            default:
                return;
            }
        }
    }

    TextToken make(std::uint8_t type) {
        TextToken token;
        token.type = type;
        token.start_line = start_line_;
        token.start_column = start_column_;
        token.end_line = line_;
        last_type_ = type;
        return token;
    }

    TextToken make_literal(Value literal) {
        TextToken token = make(kLiteral);
        token.literal = std::move(literal);
        return token;
    }

    // The two-character token when the next character is `second`, else the one-character one.
    TextToken pick(char32_t second, std::uint8_t both, std::uint8_t one) {
        if (peek() == second) {
            advance();
            return make(both);
        }
        return make(one);
    }

    // check_vcs_marker: seven or more of a character mark a merge conflict.
    TextToken conflict_marker(char32_t c, std::uint8_t pair) {
        std::size_t count = 2;
        while (peek(count - 1) == c) {
            ++count;
        }
        constexpr std::size_t kMarker = 7;
        if (count >= kMarker) {
            for (std::size_t i = 1; i < count; ++i) {
                advance();
            }
            return make(kVcsConflictMarker);
        }
        advance();
        return make(pair);
    }

    // An error at `line`, by default the line being read.
    [[nodiscard]] Error fail(const std::string& message, int line = 0) const {
        return make_error(ErrorCode::InvalidArgument,
                          "line " + std::to_string(line > 0 ? line : line_) + ": " + message);
    }

    static std::string hex(char32_t c) {
        static constexpr std::string_view kDigits = "0123456789ABCDEF";
        std::string out;
        for (std::uint32_t shift = 16; shift > 0; shift -= 4) {
            out += kDigits[(static_cast<std::uint32_t>(c) >> (shift - 4)) & 0xFU];
        }
        return out;
    }

    Result<TextToken> annotation() {
        if (!is_identifier_start(peek())) {
            return fail("expected an annotation's name after \"@\"");
        }
        while (is_identifier_continue(peek())) {
            advance();
        }
        TextToken token = make(kAnnotation);
        token.identifier = source_.substr(start_, current_ - start_);
        return token;
    }

    Result<TextToken> identifier() {
        while (is_identifier_continue(peek())) {
            advance();
        }
        const std::u32string_view name(source_.data() + start_, current_ - start_);
        for (const auto& [keyword, type] : kKeywords) {
            if (keyword == name) {
                return make(type);
            }
        }
        if (name == U"true" || name == U"false") {
            Value value;
            value.kind = Value::Kind::Bool;
            value.boolean = name == U"true";
            return make_literal(std::move(value));
        }
        if (name == U"null") {
            return make_literal(Value());
        }
        TextToken token = make(kIdentifier);
        token.identifier = std::u32string(name);
        return token;
    }

    // A number's digits, with '_' between them for reading.
    void digits(bool (*is_digit_of)(char32_t)) {
        while (is_digit_of(peek()) || peek() == '_') {
            advance();
        }
    }

    Result<TextToken> number() {
        int base = 10;
        bool decimal = false;
        bool exponent = false;
        const char32_t first = source_[start_];
        if ((first == '+' || first == '-') && peek() == '0') {
            advance();
        }
        const char32_t last = source_[current_ - 1];
        if (last == '.') {
            decimal = true;
        } else if (last == '0' && (peek() == 'x' || peek() == 'X')) {
            base = 16;
            advance();
        } else if (last == '0' && (peek() == 'b' || peek() == 'B')) {
            base = 2;
            advance();
        }
        if (base == 16) {
            digits(is_hex_digit);
        } else {
            digits(base == 2 ? is_binary_digit : is_digit);
        }
        // A ".." after the digits is a range, not a decimal point.
        if (base == 10 && !decimal && peek() == '.' && peek(1) != '.') {
            decimal = true;
            advance();
            digits(is_digit);
        }
        if (base == 10 && (peek() == 'e' || peek() == 'E')) {
            exponent = true;
            advance();
            if (peek() == '+' || peek() == '-') {
                advance();
            }
            if (!is_digit(peek())) {
                return fail("expected an exponent after \"e\"");
            }
            digits(is_digit);
        }
        if (is_identifier_continue(peek()) || (peek() == '.' && peek(1) != '.')) {
            return fail("invalid number");
        }
        std::string text;
        for (std::size_t i = start_; i < current_; ++i) {
            if (source_[i] != '_') {
                text += static_cast<char>(source_[i]);
            }
        }
        Value value;
        value.kind = Value::Kind::Int;
        if (base != 10) {
            std::uint64_t magnitude = 0;
            for (std::size_t i = text.find_first_of("xXbB") + 1; i < text.size(); ++i) {
                magnitude = (magnitude * static_cast<std::uint64_t>(base)) +
                            hex_value(static_cast<char32_t>(text[i]));
            }
            value.integer = static_cast<std::int64_t>(magnitude);
            if (text.starts_with('-')) {
                value.integer = -value.integer;
            }
        } else if (decimal || exponent) {
            value.kind = Value::Kind::Float;
            value.real = std::strtod(text.c_str(), nullptr);
        } else {
            value.integer = std::strtoll(text.c_str(), nullptr, 10);
        }
        return make_literal(std::move(value));
    }

    // One escape after a backslash into `result`, or false with `error` set.
    bool escape(std::u32string& result, char32_t& lead, std::string& error) {
        const char32_t code = advance();
        char32_t escaped = 0;
        switch (code) {
        case 'a':
            escaped = '\a';
            break;
        case 'b':
            escaped = '\b';
            break;
        case 'f':
            escaped = '\f';
            break;
        case 'n':
            escaped = '\n';
            break;
        case 'r':
            escaped = '\r';
            break;
        case 't':
            escaped = '\t';
            break;
        case 'v':
            escaped = '\v';
            break;
        case '\'':
        case '"':
        case '\\':
            escaped = code;
            break;
        case 'u':
        case 'U':
            for (int i = 0; i < (code == 'U' ? 6 : 4); ++i) {
                if (!is_hex_digit(peek())) {
                    error = "invalid hexadecimal digit in a Unicode escape";
                    return false;
                }
                escaped = (escaped << 4U) | hex_value(advance());
            }
            break;
        case '\n':
            newline(false); // an escaped new line is left out
            return true;
        default:
            error = "invalid escape in a string";
            return false;
        }
        // A UTF-16 surrogate pair makes one character.
        if ((escaped & 0xFFFFFC00U) == 0xD800) {
            lead = escaped;
            return true;
        }
        if ((escaped & 0xFFFFFC00U) == 0xDC00 && lead != 0) {
            escaped = 0x10000 + ((lead - 0xD800) << 10U) + (escaped - 0xDC00);
        }
        lead = 0;
        result += escaped;
        return true;
    }

    Result<TextToken> string() {
        enum class Kind : std::uint8_t { Regular, Name, NodePath };
        bool raw = false;
        Kind kind = Kind::Regular;
        const char32_t prefix = source_[current_ - 1];
        if (prefix == 'r' || prefix == '&' || prefix == '^') {
            raw = prefix == 'r';
            if (prefix == '&') {
                kind = Kind::Name;
            } else if (prefix == '^') {
                kind = Kind::NodePath;
            }
            advance();
        }
        const char32_t quote = source_[current_ - 1];
        bool multiline = false;
        if (peek() == quote && peek(1) == quote) {
            multiline = true;
            advance();
            advance();
        }
        std::u32string result;
        char32_t lead = 0;
        while (true) {
            if (at_end()) {
                return fail("unterminated string", start_line_); // where it starts
            }
            const char32_t c = peek();
            if (c == '\\') {
                advance();
                if (at_end()) {
                    return fail("unterminated string", start_line_); // where it starts
                }
                if (raw) {
                    // A raw string keeps its backslashes; one only stops the quote or backslash
                    // after it from ending the string.
                    result += '\\';
                    if (peek() == quote || peek() == '\\') {
                        result += advance();
                    }
                    continue;
                }
                if (std::string error; !escape(result, lead, error)) {
                    return fail(error);
                }
            } else if (c == quote) {
                advance();
                if (!multiline) {
                    break;
                }
                if (peek() == quote && peek(1) == quote) {
                    advance();
                    advance();
                    break;
                }
                result += quote;
            } else {
                result += c;
                advance();
                if (c == '\n') {
                    newline(false);
                }
            }
        }
        Value text;
        text.kind = kind == Kind::Name ? Value::Kind::StringName : Value::Kind::String;
        text.text = utf8(result);
        if (kind != Kind::NodePath) {
            return make_literal(std::move(text));
        }
        Value path;
        path.kind = Value::Kind::Call;
        path.text = "NodePath";
        path.items.push_back(std::move(text));
        return make_literal(std::move(path));
    }

    std::u32string source_;
    int tab_size_ = 4;
    std::size_t current_ = 0;
    std::size_t start_ = 0;
    int line_ = 1;
    int column_ = 1;
    int start_line_ = 1;
    int start_column_ = 1;
    bool pending_newline_ = false;
    bool line_continuation_ = false;
    std::uint8_t last_type_ = kEmpty;
    std::vector<int> continuation_lines_;
};

} // namespace

Result<std::string> tokenize_gdscript(std::string_view source, const TokenOptions& options) {
    if (options.version != kTokenizerVersion && options.version != kTokenizerVersion43) {
        return make_error(ErrorCode::InvalidArgument, "GDScript token version " +
                                                          std::to_string(options.version) +
                                                          "; versions 100 and 101 are written");
    }
    Tokenizer tokenizer(source, std::max(options.tab_size, 1));
    std::vector<std::u32string> identifiers;
    std::map<std::u32string, std::uint32_t> identifier_ids;
    std::vector<Value> constants;
    std::map<std::string, std::uint32_t> constant_ids;
    // Each token that starts a later line than the last one ended on keeps its line and column.
    struct Position {
        std::uint32_t token;
        int line;
        int column;
    };
    std::vector<Position> positions;
    ByteWriter tokens;
    std::uint32_t count = 0;
    int last_line = 0;
    while (true) {
        auto token = tokenizer.scan();
        if (!token) {
            return token.error();
        }
        const TextToken& scanned = token.value();
        if (scanned.type == kEof) {
            break;
        }
        std::uint32_t index = 0;
        if (scanned.type == kIdentifier || scanned.type == kAnnotation) {
            const auto [it, added] = identifier_ids.try_emplace(
                scanned.identifier, static_cast<std::uint32_t>(identifiers.size()));
            if (added) {
                identifiers.push_back(scanned.identifier);
            }
            index = it->second;
        } else if (scanned.type == kLiteral) {
            // Constants are listed once, as a Variant-keyed map compares them: by type and value.
            std::string key = std::to_string(static_cast<int>(scanned.literal.kind)) +
                              write_value(scanned.literal);
            const auto [it, added] = constant_ids.try_emplace(
                std::move(key), static_cast<std::uint32_t>(constants.size()));
            if (added) {
                constants.push_back(scanned.literal);
            }
            index = it->second;
        }
        std::uint32_t type = scanned.type;
        if (options.version == kTokenizerVersion43 && type >= kPeriodPeriodPeriod) {
            if (type == kPeriodPeriodPeriod) {
                return make_error(ErrorCode::InvalidArgument,
                                  "line " + std::to_string(scanned.start_line) +
                                      ": Godot 4.3 and 4.4 have no \"...\"");
            }
            --type;
        }
        if (count > 0 && scanned.start_line > last_line) {
            positions.push_back({count, scanned.start_line, scanned.start_column});
        }
        last_line = scanned.end_line;
        tokens.u32(type | (index << kTokenBits) | kTokenByteMask);
        tokens.u32(static_cast<std::uint32_t>(scanned.start_line));
        ++count;
    }
    // A line continued after a backslash keeps no position: its indentation means nothing.
    for (const int line : tokenizer.continuation_lines()) {
        std::erase_if(positions, [&](const Position& position) { return position.line == line; });
    }

    ByteWriter contents;
    contents.u32(static_cast<std::uint32_t>(identifiers.size()));
    contents.u32(static_cast<std::uint32_t>(constants.size()));
    contents.u32(static_cast<std::uint32_t>(positions.size()));
    if (options.version == kTokenizerVersion43) {
        contents.u32(0); // unused
    }
    contents.u32(count);
    for (const std::u32string& identifier : identifiers) {
        contents.u32(static_cast<std::uint32_t>(identifier.size()));
        for (const char32_t code : identifier) {
            for (std::uint32_t k = 0; k < 4; ++k) {
                contents.u8(((code >> (8U * k)) & 0xFFU) ^ 0xB6U);
            }
        }
    }
    for (const Value& constant : constants) {
        auto encoded = encode_variant(constant);
        if (!encoded) {
            return encoded.error();
        }
        contents.bytes(encoded.value());
    }
    for (const Position& position : positions) {
        contents.u32(position.token);
        contents.u32(static_cast<std::uint32_t>(position.line));
    }
    for (const Position& position : positions) {
        contents.u32(position.token);
        contents.u32(static_cast<std::uint32_t>(position.column));
    }
    contents.bytes(tokens.take());

    ByteWriter out;
    out.bytes("GDSC");
    out.u32(options.version);
    const std::string body = contents.take();
    if (options.compress) {
        out.u32(static_cast<std::uint32_t>(body.size()));
        out.bytes(zstd_store(body));
    } else {
        out.u32(0);
        out.bytes(body);
    }
    return out.take();
}

} // namespace slash::importer::godot
