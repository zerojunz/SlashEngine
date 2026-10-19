#pragma once

#include "slash/core/result.hpp"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// A JSON document model shared by scenes, schemas and (later) the command API.
//
// Objects keep their keys sorted, so writing a value always produces the same text. That is the
// canonical form scenes are stored in: sorted keys, two-space indent, arrays of plain values on
// one line, e.g.
//
//   {
//     "name": "player",
//     "position": [0.0, 1.5, 0.0]
//   }
namespace slash::json {

enum class Type : std::uint8_t { Null, Bool, Int, Double, String, Array, Object };

// The JSON Schema name of a type: "null", "boolean", "integer", "number", "string", "array" or
// "object".
[[nodiscard]] std::string_view to_string(Type type) noexcept;

class Value;
using Array = std::vector<Value>;
using Object = std::map<std::string, Value, std::less<>>;

class Value {
public:
    Value() noexcept = default;
    Value(std::nullptr_t) noexcept {}
    Value(bool value) : data_(value) {}

    template <std::integral T>
        requires(!std::same_as<T, bool>)
    Value(T value) {
        if constexpr (std::unsigned_integral<T> && sizeof(T) >= sizeof(std::int64_t)) {
            // Integers above INT64_MAX are kept as the nearest double.
            if (value > static_cast<T>(std::numeric_limits<std::int64_t>::max())) {
                data_ = static_cast<double>(value);
                return;
            }
        }
        data_ = static_cast<std::int64_t>(value);
    }

    Value(double value) : data_(value) {}

    // Stores the shortest decimal that reads back as the same float, so 0.1F is written as 0.1
    // rather than 0.10000000149011612.
    Value(float value);

    Value(const char* text) : data_(std::string(text)) {}
    Value(std::string_view text) : data_(std::string(text)) {}
    Value(std::string text) : data_(std::move(text)) {}
    Value(Array items) : data_(std::move(items)) {}
    Value(Object members) : data_(std::move(members)) {}

    [[nodiscard]] Type type() const noexcept { return static_cast<Type>(data_.index()); }
    [[nodiscard]] bool is_null() const noexcept { return type() == Type::Null; }
    [[nodiscard]] bool is_bool() const noexcept { return type() == Type::Bool; }
    [[nodiscard]] bool is_int() const noexcept { return type() == Type::Int; }
    [[nodiscard]] bool is_double() const noexcept { return type() == Type::Double; }
    [[nodiscard]] bool is_number() const noexcept { return is_int() || is_double(); }
    [[nodiscard]] bool is_string() const noexcept { return type() == Type::String; }
    [[nodiscard]] bool is_array() const noexcept { return type() == Type::Array; }
    [[nodiscard]] bool is_object() const noexcept { return type() == Type::Object; }

    // True for numbers with no fractional part, which JSON Schema counts as integers (1 and 1.0).
    [[nodiscard]] bool is_integral() const noexcept;

    // Typed accessors. Each asserts that the value has that type; as_double() accepts integers.
    [[nodiscard]] bool as_bool() const;
    [[nodiscard]] std::int64_t as_int() const;
    [[nodiscard]] double as_double() const;
    [[nodiscard]] const std::string& as_string() const;
    [[nodiscard]] const Array& as_array() const;
    [[nodiscard]] Array& as_array();
    [[nodiscard]] const Object& as_object() const;
    [[nodiscard]] Object& as_object();

    // The member named key, or nullptr if this is not an object or has no such member.
    [[nodiscard]] const Value* find(std::string_view key) const;
    [[nodiscard]] Value* find(std::string_view key);

    // Returns the member named key, inserting null if it is missing. A null value becomes an
    // empty object first.
    Value& operator[](std::string_view key);

    // Appends to an array. A null value becomes an empty array first.
    void push_back(Value item);

    // The number of items in an array or members in an object, otherwise 0.
    [[nodiscard]] std::size_t size() const noexcept;

    // Structural equality; numbers compare by value, so 1 == 1.0.
    friend bool operator==(const Value& a, const Value& b);

private:
    std::variant<std::nullptr_t, bool, std::int64_t, double, std::string, Array, Object> data_;
};

// Parses strict JSON (RFC 8259). Errors give the line and column, e.g.
// "line 3, column 14: unexpected character, expected a comma or a closing bracket".
// Duplicate keys in an object are an error.
[[nodiscard]] Result<Value> parse(std::string_view text);

enum class Style : std::uint8_t {
    Pretty,  // the canonical layout described above
    Compact, // no whitespace, for messages and one-line output
};

// Writes JSON text without a trailing newline. Non-finite numbers are written as null.
[[nodiscard]] std::string write(const Value& value, Style style = Style::Pretty);

// Escapes a key for use as one segment of a JSON Pointer (RFC 6901): "~" becomes "~0" and "/"
// becomes "~1".
[[nodiscard]] std::string pointer_escape(std::string_view key);

// The key a JSON Pointer's first segment names, unescaped: "armor" for "/armor/0". "" for "".
[[nodiscard]] std::string pointer_first_key(std::string_view pointer);

} // namespace slash::json
