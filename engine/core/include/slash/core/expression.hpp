#pragma once

#include "slash/core/json.hpp"
#include "slash/core/result.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Expressions as Godot's Expression class parses and runs them: "speed > 0.1 and not grounded".
// Values are JSON values: null, booleans, integers (whole numbers, which divide as integers, as
// Godot's ints do), floats and strings. Names are read from the caller when an expression runs,
// as Godot reads its base object's properties.
//
// Operators, tightest first, as Godot's: "**"; "~"; unary "-" and "+"; "*", "/" and "%"; "+" and
// "-"; "<<" and ">>"; "&"; "^"; "|"; "<", "<=", ">", ">=", "==" and "!="; "not" (or "!"); "and"
// (or "&&"); "or" (or "||"). Operators of a level go left to right. Constants are true, false,
// null, PI, TAU, INF and NAN; functions are Godot's abs, sign, min, max, clamp, floor, ceil,
// round, sqrt, pow, lerp, fmod, is_equal_approx and is_zero_approx, and int, float and bool.
namespace slash {

class Expression {
public:
    struct Node;

    // An empty expression, which runs to null.
    Expression() = default;

    // The expression text writes, or its first error, which names the character it is at:
    // "at character 7: expected ')'".
    [[nodiscard]] static Result<Expression> parse(std::string_view text);

    [[nodiscard]] bool empty() const { return root_ == nullptr; }
    [[nodiscard]] const std::string& text() const { return text_; }
    // The names it reads, each once, in the order they first appear.
    [[nodiscard]] const std::vector<std::string>& names() const { return names_; }

    // Its value, reading each name through lookup; an error for a name lookup has no value for,
    // an operator its values don't take (true < 1), or a division by zero, as Godot's
    // has_execute_failed. Every operand is worked out, as Godot's are: "and" and "or" don't
    // stop early.
    using Lookup = std::function<std::optional<json::Value>(std::string_view name)>;
    [[nodiscard]] Result<json::Value> execute(const Lookup& lookup) const;

private:
    std::string text_;
    std::vector<std::string> names_;
    std::shared_ptr<const Node> root_;
};

// Whether a value counts as true, as Godot's Variant::booleanize: false, null, 0, 0.0 and ""
// don't.
[[nodiscard]] bool truthy(const json::Value& value);

} // namespace slash
