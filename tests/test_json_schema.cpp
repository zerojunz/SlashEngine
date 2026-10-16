#include "slash/core/json.hpp"
#include "slash/core/json_schema.hpp"

#include <doctest/doctest.h>

#include <string>
#include <vector>

namespace json = slash::json;

namespace {

json::Value parse(const std::string& text) {
    auto parsed = json::parse(text);
    REQUIRE_MESSAGE(parsed.ok(), (parsed.ok() ? "" : parsed.error().message));
    return parsed.value();
}

std::vector<json::ValidationError> check(const std::string& instance, const std::string& schema) {
    return json::validate(parse(instance), parse(schema));
}

} // namespace

TEST_CASE("type mismatches name the expected and actual types") {
    auto errors = check(R"({"hp": "full"})",
                        R"({"type": "object", "properties": {"hp": {"type": "number"}}})");
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].path == "/hp");
    CHECK(errors[0].message == "expected number, got string");

    errors = check("3", R"({"type": ["string", "null"]})");
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].message == "expected string or null, got integer");

    CHECK(check("2.0", R"({"type": "integer"})").empty());
    CHECK(check("true", "true").empty());
    CHECK(check("true", "false").size() == 1);
}

TEST_CASE("object keywords report missing and unknown properties") {
    const std::string schema = R"({
        "type": "object",
        "required": ["format"],
        "properties": {"format": {"const": 1}, "entities": {"type": "object"}},
        "additionalProperties": false
    })";
    auto errors = check(R"({"entites": {}})", schema);
    REQUIRE(errors.size() == 2);
    CHECK(errors[0].path.empty());
    CHECK(errors[0].message == R"(missing required property "format")");
    CHECK(errors[1].path == "/entites");
    CHECK(errors[1].message == R"(unknown property "entites"; did you mean "entities"?)");

    errors = check(R"({"format": 2})", schema);
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].message == "expected 1, got 2");

    errors = check(R"({"format": 1, "zzz": 0})", schema);
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].message == R"(unknown property "zzz")");
}

TEST_CASE("additionalProperties and propertyNames apply to every extra key") {
    const std::string schema = R"({
        "type": "object",
        "propertyNames": {"maxLength": 3},
        "additionalProperties": {"type": "integer", "minimum": 0}
    })";
    auto errors = check(R"({"a": 1, "b": -1, "long": 2})", schema);
    REQUIRE(errors.size() == 2);
    CHECK(errors[0].path == "/b");
    CHECK(errors[0].message == "must be at least 0, got -1");
    CHECK(errors[1].path == "/long");
    CHECK(errors[1].message == "must be at most 3 characters long");
}

TEST_CASE("array keywords check counts and each item") {
    const std::string schema =
        R"({"type": "array", "items": {"type": "number"}, "minItems": 2, "maxItems": 3})";
    CHECK(check("[1, 2.5]", schema).empty());
    auto errors = check(R"([1, "x", 3, 4])", schema);
    REQUIRE(errors.size() == 2);
    CHECK(errors[0].message == "expected at most 3 items, got 4");
    CHECK(errors[1].path == "/1");

    errors = check(R"(["a", 2])", R"({"prefixItems": [{"type": "string"}], "items": false})");
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].path == "/1");
}

TEST_CASE("number ranges and enums") {
    CHECK(check("5", R"({"minimum": 0, "maximum": 10})").empty());
    CHECK(check("10", R"({"exclusiveMaximum": 10})").size() == 1);
    auto errors = check(R"("up")", R"({"enum": ["left", "right"]})");
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].message == R"(expected one of "left", "right", got "up")");
}

TEST_CASE("$ref resolves definitions in the same schema") {
    const std::string schema = R"({
        "type": "object",
        "additionalProperties": {"$ref": "#/$defs/point"},
        "$defs": {"point": {"type": "array", "items": {"type": "number"}, "maxItems": 2}}
    })";
    CHECK(check(R"({"a": [1, 2]})", schema).empty());
    auto errors = check(R"({"a": [1, 2, 3]})", schema);
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].path == "/a");

    errors = check("1", R"({"$ref": "#/$defs/missing"})");
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].message == R"(schema error: cannot resolve $ref "#/$defs/missing")");

    CHECK(check("1", R"({"$ref": "#"})").size() == 1); // a cycle ends with an error
}

TEST_CASE("combinators") {
    CHECK(check("1", R"({"anyOf": [{"type": "string"}, {"type": "integer"}]})").empty());
    CHECK(check("1.5", R"({"anyOf": [{"type": "string"}, {"type": "integer"}]})").size() == 1);
    // When only one option takes the value's type, its errors say what is wrong.
    const std::string nullable =
        R"({"anyOf": [{"type": "null"}, {"type": "object", "properties": {"a": {"type": "integer"}},
                                         "additionalProperties": false}]})";
    CHECK(check("null", nullable).empty());
    const auto inner = check(R"({"a": "x", "b": 1})", nullable);
    REQUIRE(inner.size() == 2);
    CHECK(inner[0].path == "/a");
    CHECK(inner[0].message == "expected integer, got string");
    CHECK(inner[1].path == "/b");
    REQUIRE(check("5", nullable).size() == 1);
    CHECK(check("5", nullable)[0].message == "does not match any of the allowed schemas");
    CHECK(check("1", R"({"oneOf": [{"type": "number"}, {"type": "integer"}]})").size() == 1);
    CHECK(check("1", R"({"allOf": [{"minimum": 0}, {"maximum": 0}]})").size() == 1);
    CHECK(check("1", R"({"not": {"type": "string"}})").empty());
}

TEST_CASE("an anyOf whose options are told apart by one property reports that option's errors") {
    const std::string shapes = R"({"anyOf": [
        {"type": "object", "required": ["kind"], "additionalProperties": false,
         "properties": {"kind": {"const": "circle"}, "radius": {"type": "number"}}},
        {"type": "object", "required": ["kind"], "additionalProperties": false,
         "properties": {"kind": {"const": "square"}, "side": {"type": "number"}}}]})";
    CHECK(check(R"({"kind": "square", "side": 2})", shapes).empty());
    const auto wrong = check(R"({"kind": "circle", "radius": "big"})", shapes);
    REQUIRE(wrong.size() == 1);
    CHECK(wrong[0].path == "/radius");
    CHECK(wrong[0].message == "expected number, got string");
    const auto unknown = check(R"({"kind": "cirle"})", shapes);
    REQUIRE(unknown.size() == 1);
    CHECK(unknown[0].path == "/kind");
    CHECK(unknown[0].message ==
          R"(expected one of "circle", "square", got "cirle"; did you mean "circle"?)");
    const auto missing = check(R"({"radius": 1})", shapes);
    REQUIRE(missing.size() == 1);
    CHECK(missing[0].message == "missing required property \"kind\"");
    // Where the property is optional, leaving it out falls back to trying every option.
    CHECK(check(R"({"radius": 1})", R"({"anyOf": [
        {"properties": {"kind": {"const": "circle"}}},
        {"properties": {"kind": {"const": "square"}}}]})")
              .empty());
}
