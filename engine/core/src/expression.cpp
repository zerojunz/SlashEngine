// Godot's Expression (core/math/expression.cpp): a tokenizer, a parser that binds operators by
// Godot's priorities, and a tree of nodes that runs over JSON values as Godot's runs over
// Variants, with Variant's operator rules for the types JSON has.

#include "slash/core/expression.hpp"

#include "slash/core/text.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <memory>
#include <numbers>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace slash {
namespace {

enum class Op : std::uint8_t {
    Pow,
    BitNot,
    Neg,
    Pos,
    Mul,
    Div,
    Mod,
    Add,
    Sub,
    Shl,
    Shr,
    BitAnd,
    BitXor,
    BitOr,
    Lt,
    Le,
    Gt,
    Ge,
    Eq,
    Ne,
    Not,
    And,
    Or,
};

std::string_view op_text(Op op) {
    static constexpr std::array<std::string_view, 23> kText{
        "**", "~", "-", "+",  "*", "/",  "%",  "+",  "-",   "<<",  ">>", "&",
        "^",  "|", "<", "<=", ">", ">=", "==", "!=", "not", "and", "or"};
    return kText.at(static_cast<std::size_t>(op));
}

enum class Function : std::uint8_t {
    Abs,
    Sign,
    Min,
    Max,
    Clamp,
    Floor,
    Ceil,
    Round,
    Sqrt,
    Pow,
    Lerp,
    Fmod,
    IsEqualApprox,
    IsZeroApprox,
    Int,
    Float,
    Bool,
};

struct FunctionInfo {
    std::string_view name;
    Function function;
    int min_args;
    int max_args; // -1: any number from min_args
};

constexpr std::array<FunctionInfo, 17> kFunctions{{
    {"abs", Function::Abs, 1, 1},
    {"sign", Function::Sign, 1, 1},
    {"min", Function::Min, 2, -1},
    {"max", Function::Max, 2, -1},
    {"clamp", Function::Clamp, 3, 3},
    {"floor", Function::Floor, 1, 1},
    {"ceil", Function::Ceil, 1, 1},
    {"round", Function::Round, 1, 1},
    {"sqrt", Function::Sqrt, 1, 1},
    {"pow", Function::Pow, 2, 2},
    {"lerp", Function::Lerp, 3, 3},
    {"fmod", Function::Fmod, 2, 2},
    {"is_equal_approx", Function::IsEqualApprox, 2, 2},
    {"is_zero_approx", Function::IsZeroApprox, 1, 1},
    {"int", Function::Int, 1, 1},
    {"float", Function::Float, 1, 1},
    {"bool", Function::Bool, 1, 1},
}};

constexpr double kCmpEpsilon = 0.00001; // Godot's CMP_EPSILON

} // namespace

struct Expression::Node {
    enum class Kind : std::uint8_t { Constant, Name, Unary, Binary, Call };
    Kind kind = Kind::Constant;
    Op op = Op::Add;
    Function function = Function::Abs;
    json::Value value;
    std::string name;
    std::vector<std::unique_ptr<Node>> children;
};

namespace {

using Node = Expression::Node;

enum class TokenKind : std::uint8_t { End, Constant, Name, Op, LParen, RParen, Comma };

struct Token {
    TokenKind kind = TokenKind::End;
    std::size_t at = 0; // its first character, from 0
    std::string text;   // a name's
    json::Value value;  // a constant's
    Op op = Op::Add;    // an operator's; "-" and "+" are Sub and Add until the parser knows
};

Error error_at(std::size_t at, std::string_view message) {
    return make_error(ErrorCode::InvalidArgument,
                      "at character " + std::to_string(at + 1) + ": " + std::string(message));
}

bool is_name_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

// Godot's Expression::_get_token, for the tokens expressions over values use.
class Tokenizer {
public:
    explicit Tokenizer(std::string_view text) : text_(text) {}

    Result<std::vector<Token>> run() {
        std::vector<Token> out;
        while (true) {
            auto token = next();
            if (!token) {
                return token.error();
            }
            const bool end = token.value().kind == TokenKind::End;
            out.push_back(std::move(token).value());
            if (end) {
                return out;
            }
        }
    }

private:
    [[nodiscard]] char peek(std::size_t ahead = 0) const {
        return pos_ + ahead < text_.size() ? text_[pos_ + ahead] : '\0';
    }

    Result<Token> next() {
        while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\t' ||
                                       text_[pos_] == '\n' || text_[pos_] == '\r')) {
            ++pos_;
        }
        Token token;
        token.at = pos_;
        if (pos_ >= text_.size()) {
            return token;
        }
        const char c = text_[pos_];
        if (is_digit(c) || (c == '.' && is_digit(peek(1)))) {
            return number(token);
        }
        if (c == '"' || c == '\'') {
            return string(token);
        }
        if (is_name_start(c)) {
            return word(token);
        }
        const auto op = [&](Op which, std::size_t length) {
            token.kind = TokenKind::Op;
            token.op = which;
            pos_ += length;
            return token;
        };
        switch (c) {
        case '(':
            token.kind = TokenKind::LParen;
            ++pos_;
            return token;
        case ')':
            token.kind = TokenKind::RParen;
            ++pos_;
            return token;
        case ',':
            token.kind = TokenKind::Comma;
            ++pos_;
            return token;
        case '*':
            return peek(1) == '*' ? op(Op::Pow, 2) : op(Op::Mul, 1);
        case '/':
            return op(Op::Div, 1);
        case '%':
            return op(Op::Mod, 1);
        case '+':
            return op(Op::Add, 1);
        case '-':
            return op(Op::Sub, 1);
        case '~':
            return op(Op::BitNot, 1);
        case '^':
            return op(Op::BitXor, 1);
        case '<':
            if (peek(1) == '<') {
                return op(Op::Shl, 2);
            }
            return peek(1) == '=' ? op(Op::Le, 2) : op(Op::Lt, 1);
        case '>':
            if (peek(1) == '>') {
                return op(Op::Shr, 2);
            }
            return peek(1) == '=' ? op(Op::Ge, 2) : op(Op::Gt, 1);
        case '=':
            if (peek(1) == '=') {
                return op(Op::Eq, 2);
            }
            return error_at(pos_, "expected \"==\"; an expression can't assign");
        case '!':
            return peek(1) == '=' ? op(Op::Ne, 2) : op(Op::Not, 1);
        case '&':
            return peek(1) == '&' ? op(Op::And, 2) : op(Op::BitAnd, 1);
        case '|':
            return peek(1) == '|' ? op(Op::Or, 2) : op(Op::BitOr, 1);
        case '.':
            return error_at(pos_, "an expression reads names, not their members or methods");
        default:
            return error_at(pos_, "unexpected character '" + std::string(1, c) + "'");
        }
    }

    // Godot's numbers: decimal, "0x" hexadecimal and "0b" binary, with "_" between digits; a
    // "." or an exponent makes a float.
    Result<Token> number(Token& token) {
        token.kind = TokenKind::Constant;
        const std::size_t begin = pos_;
        int base = 10;
        if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'X')) {
            base = 16;
            pos_ += 2;
        } else if (peek() == '0' && (peek(1) == 'b' || peek(1) == 'B')) {
            base = 2;
            pos_ += 2;
        }
        std::string digits;
        bool is_float = false;
        const auto digit_of = [&](char c) {
            if (base == 16) {
                return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
            }
            return base == 2 ? (c == '0' || c == '1') : is_digit(c);
        };
        while (pos_ < text_.size()) {
            const char c = text_[pos_];
            if (digit_of(c)) {
                digits += c;
            } else if (c == '_' && !digits.empty()) {
                // A separator, as Godot allows.
            } else if (base == 10 && c == '.' && !is_float &&
                       digits.find_first_of("eE") == std::string::npos) {
                is_float = true;
                digits += c;
            } else if (base == 10 && (c == 'e' || c == 'E') &&
                       digits.find_first_of("eE") == std::string::npos) {
                is_float = true;
                digits += c;
                if (peek(1) == '+' || peek(1) == '-') {
                    digits += peek(1);
                    ++pos_;
                }
            } else {
                break;
            }
            ++pos_;
        }
        if (pos_ < text_.size() && is_name_start(text_[pos_])) {
            return error_at(pos_, "a number runs into a name");
        }
        if (digits.empty()) {
            return error_at(begin, "expected digits after the number's prefix");
        }
        if (is_float) {
            const std::optional<double> value = parse_decimal(digits);
            if (!value) {
                return error_at(begin, "\"" + std::string(text_.substr(begin, pos_ - begin)) +
                                           "\" isn't a number");
            }
            token.value = *value;
            return token;
        }
        std::uint64_t value = 0;
        const auto [end, failure] =
            std::from_chars(digits.data(), digits.data() + digits.size(), value, base);
        if (failure != std::errc() || end != digits.data() + digits.size() ||
            (base == 10 &&
             value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))) {
            return error_at(begin, "the integer is too big; write it as a float");
        }
        token.value = static_cast<std::int64_t>(value); // hexadecimal and binary wrap, as Godot's
        return token;
    }

    Result<Token> string(Token& token) {
        token.kind = TokenKind::Constant;
        const char quote = text_[pos_++];
        std::string out;
        while (true) {
            if (pos_ >= text_.size()) {
                return error_at(token.at, "the string has no closing quote");
            }
            const char c = text_[pos_++];
            if (c == quote) {
                break;
            }
            if (c != '\\') {
                out += c;
                continue;
            }
            if (pos_ >= text_.size()) {
                return error_at(token.at, "the string has no closing quote");
            }
            const char escaped = text_[pos_++];
            switch (escaped) {
            case 'n':
                out += '\n';
                break;
            case 't':
                out += '\t';
                break;
            case 'r':
                out += '\r';
                break;
            case '\\':
            case '"':
            case '\'':
                out += escaped;
                break;
            default:
                return error_at(pos_ - 2, "unknown escape \"\\" + std::string(1, escaped) + "\"");
            }
        }
        token.value = std::move(out);
        return token;
    }

    Result<Token> word(Token& token) {
        const std::size_t begin = pos_;
        while (pos_ < text_.size() && (is_name_start(text_[pos_]) || is_digit(text_[pos_]))) {
            ++pos_;
        }
        const std::string_view word = text_.substr(begin, pos_ - begin);
        const auto constant = [&](json::Value value) {
            token.kind = TokenKind::Constant;
            token.value = std::move(value);
            return token;
        };
        const auto op = [&](Op which) {
            token.kind = TokenKind::Op;
            token.op = which;
            return token;
        };
        if (word == "true" || word == "false") {
            return constant(word == "true");
        }
        if (word == "null") {
            return constant(nullptr);
        }
        if (word == "PI") {
            return constant(std::numbers::pi);
        }
        if (word == "TAU") {
            return constant(2.0 * std::numbers::pi);
        }
        if (word == "INF") {
            return constant(std::numeric_limits<double>::infinity());
        }
        if (word == "NAN") {
            return constant(std::numeric_limits<double>::quiet_NaN());
        }
        if (word == "and") {
            return op(Op::And);
        }
        if (word == "or") {
            return op(Op::Or);
        }
        if (word == "not") {
            return op(Op::Not);
        }
        if (word == "in" || word == "self") {
            return error_at(begin, "\"" + std::string(word) + "\" isn't supported");
        }
        token.kind = TokenKind::Name;
        token.text = std::string(word);
        return token;
    }

    std::string_view text_;
    std::size_t pos_ = 0;
};

// Builds the node tree, binding operators by Godot's priorities (Expression::_parse_expression).
class Parser {
public:
    Parser(std::vector<Token> tokens, std::vector<std::string>& names)
        : tokens_(std::move(tokens)), names_(names) {}

    Result<std::unique_ptr<Node>> run() {
        auto root = parse_or();
        if (!root) {
            return root;
        }
        if (current().kind != TokenKind::End) {
            return error_at(current().at, "expected an operator or the end");
        }
        return root;
    }

private:
    [[nodiscard]] const Token& current() const { return tokens_[pos_]; }
    [[nodiscard]] bool is_op(Op op) const {
        return current().kind == TokenKind::Op && current().op == op;
    }

    static std::unique_ptr<Node> binary(Op op, std::unique_ptr<Node> a, std::unique_ptr<Node> b) {
        auto node = std::make_unique<Node>();
        node->kind = Node::Kind::Binary;
        node->op = op;
        node->children.push_back(std::move(a));
        node->children.push_back(std::move(b));
        return node;
    }

    static std::unique_ptr<Node> unary(Op op, std::unique_ptr<Node> a) {
        auto node = std::make_unique<Node>();
        node->kind = Node::Kind::Unary;
        node->op = op;
        node->children.push_back(std::move(a));
        return node;
    }

    // One level of left-to-right binary operators over the next level's operands.
    template <typename Next>
    Result<std::unique_ptr<Node>> level(std::initializer_list<Op> ops, Next next) {
        auto left = (this->*next)();
        if (!left) {
            return left;
        }
        std::unique_ptr<Node> out = std::move(left).value();
        while (current().kind == TokenKind::Op &&
               std::ranges::find(ops, current().op) != ops.end()) {
            const Op op = current().op;
            ++pos_;
            auto right = (this->*next)();
            if (!right) {
                return right;
            }
            out = binary(op, std::move(out), std::move(right).value());
        }
        return out;
    }

    Result<std::unique_ptr<Node>> parse_or() { return level({Op::Or}, &Parser::parse_and); }
    Result<std::unique_ptr<Node>> parse_and() { return level({Op::And}, &Parser::parse_not); }

    Result<std::unique_ptr<Node>> parse_not() {
        if (is_op(Op::Not)) {
            ++pos_;
            auto operand = parse_not();
            if (!operand) {
                return operand;
            }
            return unary(Op::Not, std::move(operand).value());
        }
        return parse_compare();
    }

    Result<std::unique_ptr<Node>> parse_compare() {
        return level({Op::Lt, Op::Le, Op::Gt, Op::Ge, Op::Eq, Op::Ne}, &Parser::parse_bit_or);
    }
    Result<std::unique_ptr<Node>> parse_bit_or() {
        return level({Op::BitOr}, &Parser::parse_bit_xor);
    }
    Result<std::unique_ptr<Node>> parse_bit_xor() {
        return level({Op::BitXor}, &Parser::parse_bit_and);
    }
    Result<std::unique_ptr<Node>> parse_bit_and() {
        return level({Op::BitAnd}, &Parser::parse_shift);
    }
    Result<std::unique_ptr<Node>> parse_shift() {
        return level({Op::Shl, Op::Shr}, &Parser::parse_sum);
    }
    Result<std::unique_ptr<Node>> parse_sum() {
        return level({Op::Add, Op::Sub}, &Parser::parse_product);
    }
    Result<std::unique_ptr<Node>> parse_product() {
        return level({Op::Mul, Op::Div, Op::Mod}, &Parser::parse_sign);
    }

    Result<std::unique_ptr<Node>> parse_sign() {
        if (is_op(Op::Sub) || is_op(Op::Add)) {
            const Op op = current().op == Op::Sub ? Op::Neg : Op::Pos;
            ++pos_;
            auto operand = parse_sign();
            if (!operand) {
                return operand;
            }
            return unary(op, std::move(operand).value());
        }
        return parse_bit_not();
    }

    Result<std::unique_ptr<Node>> parse_bit_not() {
        if (is_op(Op::BitNot)) {
            ++pos_;
            auto operand = parse_bit_not();
            if (!operand) {
                return operand;
            }
            return unary(Op::BitNot, std::move(operand).value());
        }
        return parse_power();
    }

    Result<std::unique_ptr<Node>> parse_power() { return level({Op::Pow}, &Parser::parse_operand); }

    Result<std::unique_ptr<Node>> parse_operand() {
        const Token& token = current();
        switch (token.kind) {
        case TokenKind::Constant: {
            auto node = std::make_unique<Node>();
            node->value = token.value;
            ++pos_;
            return node;
        }
        case TokenKind::LParen: {
            ++pos_;
            auto inner = parse_or();
            if (!inner) {
                return inner;
            }
            if (current().kind != TokenKind::RParen) {
                return error_at(current().at, "expected ')'");
            }
            ++pos_;
            return inner;
        }
        case TokenKind::Name:
            return name();
        case TokenKind::Op:
            if (token.op == Op::Not) {
                // "a == not b": "not" takes what follows as its operand.
                return parse_not();
            }
            if (token.op == Op::Sub || token.op == Op::Add || token.op == Op::BitNot) {
                return parse_sign();
            }
            return error_at(token.at,
                            "expected a value before \"" + std::string(op_text(token.op)) + "\"");
        default:
            return error_at(token.at, "expected a value");
        }
    }

    Result<std::unique_ptr<Node>> name() {
        const Token& token = current();
        ++pos_;
        if (current().kind != TokenKind::LParen) {
            auto node = std::make_unique<Node>();
            node->kind = Node::Kind::Name;
            node->name = token.text;
            if (std::ranges::find(names_, token.text) == names_.end()) {
                names_.push_back(token.text);
            }
            return node;
        }
        const FunctionInfo* info = nullptr;
        for (const FunctionInfo& function : kFunctions) {
            if (function.name == token.text) {
                info = &function;
                break;
            }
        }
        if (info == nullptr) {
            std::string message = "there is no function \"" + token.text + "\"";
            std::vector<std::string_view> known;
            known.reserve(kFunctions.size());
            for (const FunctionInfo& function : kFunctions) {
                known.push_back(function.name);
            }
            if (const std::string_view hint = closest_match(token.text, known); !hint.empty()) {
                message += "; did you mean \"" + std::string(hint) + "\"?";
            }
            return error_at(token.at, message);
        }
        ++pos_; // (
        auto node = std::make_unique<Node>();
        node->kind = Node::Kind::Call;
        node->function = info->function;
        if (current().kind != TokenKind::RParen) {
            while (true) {
                auto argument = parse_or();
                if (!argument) {
                    return argument;
                }
                node->children.push_back(std::move(argument).value());
                if (current().kind == TokenKind::Comma) {
                    ++pos_;
                    continue;
                }
                if (current().kind != TokenKind::RParen) {
                    return error_at(current().at, "expected ',' or ')'");
                }
                break;
            }
        }
        ++pos_; // )
        const auto count = static_cast<int>(node->children.size());
        if (count < info->min_args || (info->max_args >= 0 && count > info->max_args)) {
            std::string expected = std::to_string(info->min_args);
            if (info->max_args < 0) {
                expected += " or more";
            } else if (info->max_args != info->min_args) {
                expected += " to " + std::to_string(info->max_args);
            }
            return error_at(
                token.at,
                std::string(info->name) + "() takes " + expected +
                    (info->min_args == 1 && info->max_args == 1 ? " argument" : " arguments"));
        }
        return node;
    }

    std::vector<Token> tokens_;
    std::size_t pos_ = 0;
    std::vector<std::string>& names_;
};

std::string_view type_name(const json::Value& value) {
    switch (value.type()) {
    case json::Type::Null:
        return "null";
    case json::Type::Bool:
        return "a boolean";
    case json::Type::Int:
        return "an integer";
    case json::Type::Double:
        return "a float";
    case json::Type::String:
        return "a string";
    default:
        return "a list or object";
    }
}

Error operand_error(Op op, const json::Value& a, const json::Value* b) {
    std::string message = "\"" + std::string(op_text(op)) + "\" doesn't take ";
    message += type_name(a);
    if (b != nullptr) {
        message += " and ";
        message += type_name(*b);
    }
    return make_error(ErrorCode::InvalidArgument, message);
}

Error function_error(std::string_view function, const json::Value& value) {
    return make_error(ErrorCode::InvalidArgument,
                      std::string(function) + "() doesn't take " + std::string(type_name(value)));
}

// Integers wrap as Godot's do, without overflowing in C++.
std::int64_t wrap(std::uint64_t value) {
    return static_cast<std::int64_t>(value);
}
std::uint64_t bits(std::int64_t value) {
    return static_cast<std::uint64_t>(value);
}

// x to a whole power by squaring, the same on every platform.
double power_by_squaring(double x, std::int64_t n) {
    const bool invert = n < 0;
    std::uint64_t e = invert ? bits(-(n + 1)) + 1 : bits(n);
    double result = 1.0;
    while (e != 0) {
        if ((e & 1U) != 0) {
            result *= x;
        }
        x *= x;
        e >>= 1U;
    }
    return invert ? 1.0 / result : result;
}

double power(double x, double y) {
    if (y == std::trunc(y) && std::abs(y) <= 1024.0) {
        return power_by_squaring(x, static_cast<std::int64_t>(y));
    }
    return std::pow(x, y);
}

Result<json::Value> integer_power(std::int64_t a, std::int64_t b) {
    if (b < 0) {
        if (a == 0) {
            return make_error(ErrorCode::InvalidArgument, "division by zero");
        }
        if (a == 1) {
            return json::Value(std::int64_t{1});
        }
        if (a == -1) {
            return json::Value(std::int64_t{(b % 2) == 0 ? 1 : -1});
        }
        return json::Value(std::int64_t{0});
    }
    std::uint64_t result = 1;
    std::uint64_t base = bits(a);
    auto e = bits(b);
    while (e != 0) {
        if ((e & 1U) != 0) {
            result *= base;
        }
        base *= base;
        e >>= 1U;
    }
    return json::Value(wrap(result));
}

// -1, 0 or 1 as a is below, at or above b.
template <typename T>
int three_way(T a, T b) {
    return static_cast<int>(a > b) - static_cast<int>(a < b);
}

int compare_numbers(const json::Value& a, const json::Value& b, bool& unordered) {
    unordered = false;
    if (a.is_int() && b.is_int()) {
        return three_way(a.as_int(), b.as_int());
    }
    const double x = a.as_double();
    const double y = b.as_double();
    if (std::isnan(x) || std::isnan(y)) {
        unordered = true;
        return 0;
    }
    return three_way(x, y);
}

Result<json::Value> compare(Op op, const json::Value& a, const json::Value& b) {
    const bool equality = op == Op::Eq || op == Op::Ne;
    if (equality && (a.is_null() || b.is_null())) {
        const bool same = a.is_null() && b.is_null();
        return json::Value(op == Op::Eq ? same : !same);
    }
    int order = 0;
    bool unordered = false;
    if (a.is_number() && b.is_number()) {
        order = compare_numbers(a, b, unordered);
    } else if (a.is_string() && b.is_string()) {
        order = three_way(a.as_string().compare(b.as_string()), 0);
    } else if (a.is_bool() && b.is_bool()) {
        order = static_cast<int>(a.as_bool()) - static_cast<int>(b.as_bool());
    } else {
        return operand_error(op, a, &b);
    }
    switch (op) {
    case Op::Lt:
        return json::Value(!unordered && order < 0);
    case Op::Le:
        return json::Value(!unordered && order <= 0);
    case Op::Gt:
        return json::Value(!unordered && order > 0);
    case Op::Ge:
        return json::Value(!unordered && order >= 0);
    case Op::Eq:
        return json::Value(!unordered && order == 0);
    default:
        return json::Value(unordered || order != 0);
    }
}

Result<json::Value> arithmetic(Op op, const json::Value& a, const json::Value& b) {
    if (op == Op::Add && a.is_string() && b.is_string()) {
        return json::Value(a.as_string() + b.as_string());
    }
    if (!a.is_number() || !b.is_number()) {
        return operand_error(op, a, &b);
    }
    if (a.is_int() && b.is_int()) {
        const std::int64_t x = a.as_int();
        const std::int64_t y = b.as_int();
        switch (op) {
        case Op::Add:
            return json::Value(wrap(bits(x) + bits(y)));
        case Op::Sub:
            return json::Value(wrap(bits(x) - bits(y)));
        case Op::Mul:
            return json::Value(wrap(bits(x) * bits(y)));
        case Op::Div:
        case Op::Mod:
            if (y == 0) {
                return make_error(ErrorCode::InvalidArgument, "division by zero");
            }
            if (y == -1) { // INT64_MIN / -1 would overflow
                return json::Value(op == Op::Div ? wrap(0 - bits(x)) : std::int64_t{0});
            }
            return json::Value(op == Op::Div ? x / y : x % y);
        case Op::Pow:
            return integer_power(x, y);
        default:
            break;
        }
    }
    if (op == Op::Mod) {
        return make_error(ErrorCode::InvalidArgument, "\"%\" takes integers; fmod() takes floats");
    }
    const double x = a.as_double();
    const double y = b.as_double();
    switch (op) {
    case Op::Add:
        return json::Value(x + y);
    case Op::Sub:
        return json::Value(x - y);
    case Op::Mul:
        return json::Value(x * y);
    case Op::Div:
        return json::Value(x / y);
    default:
        return json::Value(power(x, y));
    }
}

Result<json::Value> bitwise(Op op, const json::Value& a, const json::Value& b) {
    if (!a.is_int() || !b.is_int()) {
        return operand_error(op, a, &b);
    }
    const std::int64_t x = a.as_int();
    const std::int64_t y = b.as_int();
    switch (op) {
    case Op::Shl:
    case Op::Shr:
        if (y < 0 || y > 63) {
            return make_error(ErrorCode::InvalidArgument,
                              "a shift goes by 0 to 63 bits, not " + std::to_string(y));
        }
        return json::Value(op == Op::Shl ? wrap(bits(x) << static_cast<unsigned>(y))
                                         : x >> static_cast<unsigned>(y));
    case Op::BitAnd:
        return json::Value(x & y);
    case Op::BitXor:
        return json::Value(x ^ y);
    default:
        return json::Value(x | y);
    }
}

Result<json::Value> apply(Op op, const json::Value& a, const json::Value& b) {
    switch (op) {
    case Op::And:
        return json::Value(truthy(a) && truthy(b));
    case Op::Or:
        return json::Value(truthy(a) || truthy(b));
    case Op::Lt:
    case Op::Le:
    case Op::Gt:
    case Op::Ge:
    case Op::Eq:
    case Op::Ne:
        return compare(op, a, b);
    case Op::Shl:
    case Op::Shr:
    case Op::BitAnd:
    case Op::BitXor:
    case Op::BitOr:
        return bitwise(op, a, b);
    default:
        return arithmetic(op, a, b);
    }
}

Result<json::Value> apply(Op op, const json::Value& a) {
    switch (op) {
    case Op::Not:
        return json::Value(!truthy(a));
    case Op::Neg:
        if (a.is_int()) {
            return json::Value(wrap(0 - bits(a.as_int())));
        }
        if (a.is_double()) {
            return json::Value(-a.as_double());
        }
        break;
    case Op::Pos:
        if (a.is_number()) {
            return a;
        }
        break;
    default: // BitNot
        if (a.is_int()) {
            return json::Value(~a.as_int());
        }
        break;
    }
    return operand_error(op, a, nullptr);
}

Result<json::Value> call(Function function, const std::vector<json::Value>& args) {
    const std::string_view name =
        std::ranges::find(kFunctions, function, &FunctionInfo::function)->name;
    for (const json::Value& arg : args) {
        if (function != Function::Bool && function != Function::Int &&
            function != Function::Float && !arg.is_number()) {
            return function_error(name, arg);
        }
    }
    const json::Value& x = args.front();
    switch (function) {
    case Function::Abs:
        if (x.is_int()) {
            return json::Value(x.as_int() < 0 ? wrap(0 - bits(x.as_int())) : x.as_int());
        }
        return json::Value(std::abs(x.as_double()));
    case Function::Sign:
        if (x.is_int()) {
            return json::Value(std::int64_t{three_way(x.as_int(), std::int64_t{0})});
        }
        return json::Value(static_cast<double>(three_way(x.as_double(), 0.0)));
    case Function::Min:
    case Function::Max: {
        const json::Value* best = &x;
        for (std::size_t i = 1; i < args.size(); ++i) {
            bool unordered = false;
            const int order = compare_numbers(args[i], *best, unordered);
            if (!unordered && (function == Function::Min ? order < 0 : order > 0)) {
                best = &args[i];
            }
        }
        return *best;
    }
    case Function::Clamp:
        if (x.is_int() && args[1].is_int() && args[2].is_int()) {
            return json::Value(std::max(args[1].as_int(), std::min(x.as_int(), args[2].as_int())));
        }
        return json::Value(
            std::max(args[1].as_double(), std::min(x.as_double(), args[2].as_double())));
    case Function::Floor:
        return x.is_int() ? x : json::Value(std::floor(x.as_double()));
    case Function::Ceil:
        return x.is_int() ? x : json::Value(std::ceil(x.as_double()));
    case Function::Round:
        return x.is_int() ? x : json::Value(std::round(x.as_double()));
    case Function::Sqrt:
        return json::Value(std::sqrt(x.as_double()));
    case Function::Pow:
        return json::Value(power(x.as_double(), args[1].as_double()));
    case Function::Lerp:
        return json::Value(x.as_double() +
                           ((args[1].as_double() - x.as_double()) * args[2].as_double()));
    case Function::Fmod:
        return json::Value(std::fmod(x.as_double(), args[1].as_double()));
    case Function::IsEqualApprox: {
        // Godot's Math::is_equal_approx.
        const double a = x.as_double();
        const double b = args[1].as_double();
        if (a == b) {
            return json::Value(true);
        }
        const double tolerance = std::max(kCmpEpsilon * std::abs(a), kCmpEpsilon);
        return json::Value(std::abs(a - b) < tolerance);
    }
    case Function::IsZeroApprox:
        return json::Value(std::abs(x.as_double()) < kCmpEpsilon);
    case Function::Int:
        if (x.is_bool()) {
            return json::Value(std::int64_t{x.as_bool() ? 1 : 0});
        }
        if (x.is_int()) {
            return x;
        }
        if (x.is_double() && std::isfinite(x.as_double()) && std::abs(x.as_double()) < 9.2e18) {
            return json::Value(static_cast<std::int64_t>(x.as_double()));
        }
        return function_error(name, x);
    case Function::Float:
        if (x.is_bool()) {
            return json::Value(x.as_bool() ? 1.0 : 0.0);
        }
        if (x.is_number()) {
            return json::Value(x.as_double());
        }
        return function_error(name, x);
    default: // Bool
        return json::Value(truthy(x));
    }
}

Result<json::Value> run(const Node& node, const Expression::Lookup& lookup) {
    switch (node.kind) {
    case Node::Kind::Constant:
        return node.value;
    case Node::Kind::Name: {
        std::optional<json::Value> value = lookup ? lookup(node.name) : std::nullopt;
        if (!value) {
            return make_error(ErrorCode::NotFound, "nothing is named \"" + node.name + "\"");
        }
        return std::move(*value);
    }
    case Node::Kind::Unary: {
        auto a = run(*node.children[0], lookup);
        if (!a) {
            return a;
        }
        return apply(node.op, a.value());
    }
    case Node::Kind::Binary: {
        auto a = run(*node.children[0], lookup);
        if (!a) {
            return a;
        }
        auto b = run(*node.children[1], lookup);
        if (!b) {
            return b;
        }
        return apply(node.op, a.value(), b.value());
    }
    default: {
        std::vector<json::Value> args;
        args.reserve(node.children.size());
        for (const auto& child : node.children) {
            auto value = run(*child, lookup);
            if (!value) {
                return value;
            }
            args.push_back(std::move(value).value());
        }
        return call(node.function, args);
    }
    }
}

} // namespace

Result<Expression> Expression::parse(std::string_view text) {
    auto tokens = Tokenizer(text).run();
    if (!tokens) {
        return tokens.error();
    }
    Expression out;
    auto root = Parser(std::move(tokens).value(), out.names_).run();
    if (!root) {
        return root.error();
    }
    out.text_ = std::string(text);
    out.root_ = std::shared_ptr<const Node>(std::move(root).value());
    return out;
}

Result<json::Value> Expression::execute(const Lookup& lookup) const {
    if (root_ == nullptr) {
        return json::Value();
    }
    return run(*root_, lookup);
}

bool truthy(const json::Value& value) {
    switch (value.type()) {
    case json::Type::Bool:
        return value.as_bool();
    case json::Type::Int:
        return value.as_int() != 0;
    case json::Type::Double:
        return value.as_double() != 0.0;
    case json::Type::String:
        return !value.as_string().empty();
    case json::Type::Array:
    case json::Type::Object:
        return value.size() != 0;
    default:
        return false;
    }
}

} // namespace slash
