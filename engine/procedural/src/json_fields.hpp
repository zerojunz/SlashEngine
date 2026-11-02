#pragma once

// Schema fields and readers the procedural files share: each field optional with Godot's default,
// enums as snake_case names.

#include "slash/core/json.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/core/result.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace slash::procedural::detail {

template <typename E>
struct EnumName {
    E value;
    std::string_view name;
};

template <typename E>
[[nodiscard]] std::string_view name_of(std::span<const EnumName<E>> names, E value) {
    for (const EnumName<E>& entry : names) {
        if (entry.value == value) {
            return entry.name;
        }
    }
    return names.front().name;
}

template <typename E, std::size_t N>
[[nodiscard]] json::Value enum_field(const std::array<EnumName<E>, N>& names, E fallback,
                                     std::string_view description) {
    json::Value field;
    field["type"] = "string";
    json::Array values;
    for (const EnumName<E>& entry : names) {
        values.emplace_back(entry.name);
    }
    field["enum"] = std::move(values);
    field["default"] = name_of(std::span<const EnumName<E>>(names), fallback);
    field["description"] = description;
    return field;
}

[[nodiscard]] inline json::Value number_field(float fallback, std::string_view description) {
    json::Value field;
    field["type"] = "number";
    field["default"] = fallback;
    field["description"] = description;
    return field;
}

[[nodiscard]] inline json::Value number_field(float fallback, std::string_view description,
                                              double minimum, double maximum) {
    json::Value field = number_field(fallback, description);
    field["minimum"] = minimum;
    field["maximum"] = maximum;
    return field;
}

[[nodiscard]] inline json::Value integer_field(std::int64_t fallback,
                                               std::string_view description) {
    json::Value field;
    field["type"] = "integer";
    field["default"] = fallback;
    field["description"] = description;
    return field;
}

[[nodiscard]] inline json::Value integer_field(std::int64_t fallback, std::string_view description,
                                               std::int64_t minimum, std::int64_t maximum) {
    json::Value field = integer_field(fallback, description);
    field["minimum"] = minimum;
    field["maximum"] = maximum;
    return field;
}

[[nodiscard]] inline json::Value boolean_field(bool fallback, std::string_view description) {
    json::Value field;
    field["type"] = "boolean";
    field["default"] = fallback;
    field["description"] = description;
    return field;
}

// The first error as a Result error: "<pointer>: <message>", or "root: ..." at the top.
[[nodiscard]] inline Error first_error(const std::vector<json::ValidationError>& errors) {
    const json::ValidationError& first = errors.front();
    return make_error(ErrorCode::InvalidArgument,
                      (first.path.empty() ? std::string("root") : first.path) + ": " +
                          first.message);
}

// Readers for documents the schema passed: each leaves the value alone when the key is missing.
inline void read(const json::Value& doc, std::string_view key, float& out) {
    if (const json::Value* value = doc.find(key); value != nullptr) {
        out = static_cast<float>(value->as_double());
    }
}

inline void read(const json::Value& doc, std::string_view key, int& out) {
    if (const json::Value* value = doc.find(key); value != nullptr) {
        out = static_cast<int>(value->as_int());
    }
}

inline void read(const json::Value& doc, std::string_view key, bool& out) {
    if (const json::Value* value = doc.find(key); value != nullptr) {
        out = value->as_bool();
    }
}

template <typename E, std::size_t N>
void read(const json::Value& doc, std::string_view key, const std::array<EnumName<E>, N>& names,
          E& out) {
    if (const json::Value* value = doc.find(key); value != nullptr) {
        for (const EnumName<E>& entry : names) {
            if (entry.name == value->as_string()) {
                out = entry.value;
            }
        }
    }
}

// Writers that leave out values at their defaults.
template <typename T>
void write(json::Value& out, std::string_view key, T value, T fallback) {
    if (value != fallback) {
        out[key] = value;
    }
}

template <typename E, std::size_t N>
void write(json::Value& out, std::string_view key, const std::array<EnumName<E>, N>& names, E value,
           E fallback) {
    if (value != fallback) {
        out[key] = name_of(std::span<const EnumName<E>>(names), value);
    }
}

} // namespace slash::procedural::detail
