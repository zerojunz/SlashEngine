#include "slash/core/json.hpp"
#include "slash/core/text.hpp"

#include <doctest/doctest.h>

#include <cstdint>
#include <limits>
#include <string>

namespace json = slash::json;

TEST_CASE("parse reads every JSON type") {
    auto parsed =
        json::parse(R"({"flag": true, "count": -3, "big": 18446744073709551615, "ratio": 0.25,)"
                    R"( "name": "slash", "list": [1, null], "nested": {"ok": false}})");
    REQUIRE(parsed.ok());
    const json::Value& value = parsed.value();
    CHECK(value.is_object());
    CHECK(value.size() == 7);
    CHECK(value.find("flag")->as_bool());
    CHECK(value.find("count")->as_int() == -3);
    CHECK(value.find("big")->is_double()); // above INT64_MAX
    CHECK(value.find("ratio")->as_double() == 0.25);
    CHECK(value.find("name")->as_string() == "slash");
    CHECK(value.find("list")->as_array()[1].is_null());
    CHECK_FALSE(value.find("nested")->find("ok")->as_bool());
    CHECK(value.find("missing") == nullptr);
}

TEST_CASE("parse errors give the line and column") {
    auto parsed = json::parse("{\n  \"a\": 1,\n  \"b\" 2\n}");
    REQUIRE_FALSE(parsed.ok());
    CHECK(parsed.error().code == slash::ErrorCode::InvalidArgument);
    CHECK(parsed.error().message.starts_with("line 3, column 7: "));
}

TEST_CASE("parse rejects comments, trailing commas and duplicate keys") {
    CHECK_FALSE(json::parse("[1, 2,]").ok());
    CHECK_FALSE(json::parse("// note\n{}").ok());
    auto duplicate = json::parse(R"({"scene": {"player": 1, "player": 2}})");
    REQUIRE_FALSE(duplicate.ok());
    CHECK(duplicate.error().message == R"(duplicate key "player" in the object at /scene)");
}

TEST_CASE("write produces the canonical layout") {
    json::Value value;
    value["name"] = "player";
    value["position"] = json::Array{0.0, 1.5, -2.0};
    value["alive"] = true;
    value["tags"] = json::Array{};
    value["children"].push_back(json::Object{{"id", 1}});
    value["empty"] = json::Object{};
    CHECK(json::write(value) == R"({
  "alive": true,
  "children": [
    {
      "id": 1
    }
  ],
  "empty": {},
  "name": "player",
  "position": [0.0, 1.5, -2.0],
  "tags": []
})");
    CHECK(json::write(value, json::Style::Compact) ==
          R"({"alive":true,"children":[{"id":1}],"empty":{},"name":"player",)"
          R"("position":[0.0,1.5,-2.0],"tags":[]})");
}

TEST_CASE("write escapes strings and writes non-finite numbers as null") {
    json::Value value = json::Array{"a \"quoted\"\nline", std::numeric_limits<double>::infinity()};
    CHECK(json::write(value) == R"(["a \"quoted\"\nline", null])");
}

TEST_CASE("floats are stored as their shortest decimal") {
    CHECK(json::write(json::Value(0.1F)) == "0.1");
    CHECK(json::write(json::Value(1.0F)) == "1.0");
    CHECK(json::write(json::Value(0.1)) == "0.1");
    CHECK(static_cast<float>(json::Value(0.3F).as_double()) == 0.3F);
}

TEST_CASE("written JSON parses back to an equal value") {
    auto first = json::parse(R"({"b": [1, 2.5, "x", {"c": null}], "a": -7, "u": "é\u0000"})");
    REQUIRE(first.ok());
    auto second = json::parse(json::write(first.value()));
    REQUIRE(second.ok());
    CHECK(first.value() == second.value());
    CHECK(second.value().find("u")->as_string() == std::string("\xc3\xa9\0", 3));
}

TEST_CASE("numbers compare by value") {
    CHECK(json::Value(1) == json::Value(1.0));
    CHECK_FALSE(json::Value(1) == json::Value(1.5));
    CHECK_FALSE(json::Value(1) == json::Value("1"));
    CHECK(json::Value(2.0).is_integral());
    CHECK_FALSE(json::Value(2.5).is_integral());
    CHECK(json::Value(std::uint8_t{7}).as_int() == 7);
}

TEST_CASE("pointer_escape escapes tilde and slash") {
    CHECK(json::pointer_escape("a/b~c") == "a~1b~0c");
}

TEST_CASE("base64_decode reads RFC 4648's test vectors, padded or not") {
    CHECK(slash::base64_decode("").value().empty());
    CHECK(slash::base64_decode("Zg==").value() == "f");
    CHECK(slash::base64_decode("Zm8=").value() == "fo");
    CHECK(slash::base64_decode("Zm9v").value() == "foo");
    CHECK(slash::base64_decode("Zm9vYg").value() == "foob");
    CHECK(slash::base64_decode("Zm9vYmE=").value() == "fooba");
    CHECK(slash::base64_decode("Zm9vYmFy").value() == "foobar");
    const std::string bytes{"\x00\xff\x10\x80", 4};
    CHECK(slash::base64_decode(slash::base64_encode(bytes)).value() == bytes);
    CHECK(!slash::base64_decode("Zm9v!").has_value());
    CHECK(!slash::base64_decode("Z").has_value());
}

TEST_CASE("base64_encode writes RFC 4648's test vectors") {
    CHECK(slash::base64_encode("").empty());
    CHECK(slash::base64_encode("f") == "Zg==");
    CHECK(slash::base64_encode("fo") == "Zm8=");
    CHECK(slash::base64_encode("foo") == "Zm9v");
    CHECK(slash::base64_encode("foob") == "Zm9vYg==");
    CHECK(slash::base64_encode("fooba") == "Zm9vYmE=");
    CHECK(slash::base64_encode("foobar") == "Zm9vYmFy");
    CHECK(slash::base64_encode(std::string("\xff\xfe\x00\x3e\x3f", 5)) == "//4APj8=");
}
