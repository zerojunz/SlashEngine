#include "slash/core/expression.hpp"
#include "slash/core/json.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace json = slash::json;
using slash::Expression;

namespace {

// The value text runs to, reading names from values; an error's message as a string.
json::Value run(std::string_view text, const std::map<std::string, json::Value>& values = {}) {
    auto parsed = Expression::parse(text);
    REQUIRE_MESSAGE(parsed.ok(), (parsed.ok() ? std::string() : parsed.error().message));
    auto result = parsed.value().execute([&](std::string_view name) -> std::optional<json::Value> {
        const auto it = values.find(std::string(name));
        if (it == values.end()) {
            return std::nullopt;
        }
        return it->second;
    });
    return result ? result.value() : json::Value("error: " + result.error().message);
}

std::string parse_error(std::string_view text) {
    auto parsed = Expression::parse(text);
    REQUIRE(!parsed.ok());
    return parsed.error().message;
}

} // namespace

TEST_CASE("expressions bind operators as Godot's Expression does") {
    CHECK(run("1 + 2 * 3") == json::Value(7));
    CHECK(run("(1 + 2) * 3") == json::Value(9));
    CHECK(run("2 ** 3 ** 2") == json::Value(64)); // left to right, as Godot's
    CHECK(run("-2 ** 2") == json::Value(-4));     // ** binds tighter than unary minus
    CHECK(run("10 - 4 - 3") == json::Value(3));
    CHECK(run("1 << 4 | 1") == json::Value(17));
    CHECK(run("6 & 3 ^ 1") == json::Value(3)); // & before ^
    CHECK(run("not 1 == 2") == json::Value(true));
    CHECK(run("!true or true") == json::Value(true));
    CHECK(run("true or false and false") == json::Value(true));
    CHECK(run("1 < 2 == true") == json::Value(true));
    CHECK(run("3 == not false") == json::Value("error: \"==\" doesn't take an integer and a "
                                               "boolean"));
    CHECK(run("~5") == json::Value(-6));
    CHECK(run("- -3") == json::Value(3));
    CHECK(run("2 * -3") == json::Value(-6));
}

TEST_CASE("expression values follow Godot's Variant rules for numbers, booleans and strings") {
    // Integers divide and take remainders as integers; floats as floats.
    CHECK(run("7 / 2") == json::Value(3));
    CHECK(run("-7 / 2") == json::Value(-3));
    CHECK(run("-7 % 3") == json::Value(-1));
    CHECK(run("7 / 2.0") == json::Value(3.5));
    CHECK(run("7.5 % 2") == json::Value("error: \"%\" takes integers; fmod() takes floats"));
    CHECK(run("1 / 0") == json::Value("error: division by zero"));
    CHECK(std::isinf(run("1.0 / 0").as_double()));
    CHECK(run("2 ** -1") == json::Value(0));
    CHECK(run("2.0 ** -1") == json::Value(0.5));
    CHECK(run("9223372036854775807 + 1") == json::Value(INT64_MIN)); // wraps
    // Numbers compare by value, whatever their kind; other kinds compare only with their own.
    CHECK(run("1 == 1.0") == json::Value(true));
    CHECK(run("\"b\" > \"a\"") == json::Value(true));
    CHECK(run("'a' + \"b\" == 'ab'") == json::Value(true));
    CHECK(run("false < true") == json::Value(true));
    CHECK(run("true < 1") == json::Value("error: \"<\" doesn't take a boolean and an integer"));
    CHECK(run("null == null") == json::Value(true));
    CHECK(run("x != null", {{"x", 0}}) == json::Value(true));
    CHECK(run("NAN == NAN") == json::Value(false));
    CHECK(run("NAN != NAN") == json::Value(true));
    // "and" and "or" take any values by whether they count as true.
    CHECK(run("1 and 'x'") == json::Value(true));
    CHECK(run("0.0 or ''") == json::Value(false));
    CHECK(run("0x1F + 0b101 + 1_000") == json::Value(1036));
    CHECK(run("1e3 + .5") == json::Value(1000.5));
    CHECK(run("PI > 3.14 and TAU < 6.3") == json::Value(true));
}

TEST_CASE("expressions read names from the caller and call Godot's math functions") {
    const std::map<std::string, json::Value> values{
        {"speed", 2.5}, {"grounded", true}, {"jumps", 2}, {"name", "mira"}};
    CHECK(run("speed > 0.1 and grounded", values) == json::Value(true));
    CHECK(run("jumps * 2 + speed", values) == json::Value(6.5));
    CHECK(run("name == 'mira'", values) == json::Value(true));
    CHECK(run("missing > 1", values) == json::Value("error: nothing is named \"missing\""));
    CHECK(run("abs(-3)") == json::Value(3));
    CHECK(run("abs(-3.5)") == json::Value(3.5));
    CHECK(run("sign(-2.0)") == json::Value(-1.0));
    CHECK(run("min(3, 1.5, 2)") == json::Value(1.5));
    CHECK(run("max(3, 1, 2)") == json::Value(3));
    CHECK(run("clamp(5, 0, 3)") == json::Value(3));
    CHECK(run("clamp(-0.5, 0.0, 1.0)") == json::Value(0.0));
    CHECK(run("floor(2.7) + ceil(2.2) + round(2.5)") == json::Value(8.0));
    CHECK(run("sqrt(16)") == json::Value(4.0));
    CHECK(run("pow(2, 10)") == json::Value(1024.0));
    CHECK(run("lerp(0, 10, 0.25)") == json::Value(2.5));
    CHECK(run("fmod(7.5, 2)") == json::Value(1.5));
    CHECK(run("is_equal_approx(0.1 + 0.2, 0.3)") == json::Value(true));
    CHECK(run("is_zero_approx(0.000001)") == json::Value(true));
    CHECK(run("int(2.9) + int(true)") == json::Value(3));
    CHECK(run("float(3) / 2") == json::Value(1.5));
    CHECK(run("bool('') or bool(2)") == json::Value(true));
    CHECK(run("abs(true)") == json::Value("error: abs() doesn't take a boolean"));

    const auto parsed = Expression::parse("speed > limit or (grounded and speed > 1)");
    REQUIRE(parsed.ok());
    CHECK(parsed.value().names() == std::vector<std::string>{"speed", "limit", "grounded"});
    CHECK(parsed.value().text() == "speed > limit or (grounded and speed > 1)");
    CHECK(Expression().empty());
    CHECK(Expression().execute({}).value().is_null());
    CHECK(slash::truthy(json::Value(0.5)));
    CHECK(!slash::truthy(json::Value()));
}

TEST_CASE("expression errors say where in the text they are") {
    CHECK(parse_error("speed >") == "at character 8: expected a value");
    CHECK(parse_error("(1 + 2") == "at character 7: expected ')'");
    CHECK(parse_error("speed = 1") ==
          "at character 7: expected \"==\"; an expression can't assign");
    CHECK(parse_error("1 2") == "at character 3: expected an operator or the end");
    CHECK(parse_error("absf(1)") ==
          "at character 1: there is no function \"absf\"; did you mean \"abs\"?");
    CHECK(parse_error("clamp(1, 2)") == "at character 1: clamp() takes 3 arguments");
    CHECK(parse_error("min(1)") == "at character 1: min() takes 2 or more arguments");
    CHECK(parse_error("'open") == "at character 1: the string has no closing quote");
    CHECK(parse_error("12abc") == "at character 3: a number runs into a name");
    CHECK(parse_error("a in b") == "at character 3: \"in\" isn't supported");
    CHECK(parse_error("speed # 2") == "at character 7: unexpected character '#'");
    CHECK(parse_error("velocity.x > 0") ==
          "at character 9: an expression reads names, not their members or methods");
    CHECK(parse_error("* 2") == "at character 1: expected a value before \"*\"");
    CHECK(parse_error("99999999999999999999") ==
          "at character 1: the integer is too big; write it as a float");
}
