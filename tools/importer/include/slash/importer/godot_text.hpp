#pragma once

#include "slash/core/result.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Godot's text formats: the values of its VariantParser, the sections of .tscn and .tres files
// (resource_format_text.cpp) and of project.godot (ProjectSettings::_load_settings_text).
namespace slash::importer::godot {

// One value as Godot writes it. Constructors such as Vector2(1, 2), ExtResource("1_abc"),
// NodePath("a/b") and Object(InputEventKey, ...) are calls: their name and arguments.
struct Value {
    enum class Kind : std::uint8_t {
        Nil,
        Bool,
        Int,
        Float,
        String,
        StringName,
        Array,
        Dictionary,
        Call
    };

    struct Entry;

    Kind kind = Kind::Nil;
    bool boolean = false;
    std::int64_t integer = 0;
    double real = 0.0;
    // A String or StringName's text, or a call's name.
    std::string text;
    // An array's items, or a call's arguments. Object(Type, ...) keeps Type as its first item.
    std::vector<Value> items;
    // A dictionary's entries in file order, or an Object call's properties.
    std::vector<Entry> entries;

    [[nodiscard]] bool is_number() const noexcept {
        return kind == Kind::Int || kind == Kind::Float;
    }
    [[nodiscard]] bool is_string() const noexcept {
        return kind == Kind::String || kind == Kind::StringName;
    }
    [[nodiscard]] bool is_call(std::string_view name) const noexcept {
        return kind == Kind::Call && text == name;
    }
    // The number, from an Int or a Float; 0 otherwise.
    [[nodiscard]] double number() const noexcept;
    // A dictionary entry, or an Object call's property, by its string key.
    [[nodiscard]] const Value* get(std::string_view key) const;
    // The first argument of a call such as ExtResource("1_abc") or NodePath("a"), when it is a
    // string.
    [[nodiscard]] std::optional<std::string> string_argument() const;
    // The numbers of a call such as Vector2(1, 2), Color(1, 0, 0, 1) or
    // PackedVector2Array(0, 0, 1, 1), or of an array of numbers.
    [[nodiscard]] std::optional<std::vector<double>> numbers() const;
};

struct Value::Entry {
    Value key;
    Value value;
};

// Parses one value, as written after a property's '='.
[[nodiscard]] Result<Value> parse_value(std::string_view text);

// A section of a text resource: its tag ("gd_scene", "ext_resource", "sub_resource", "node",
// "connection", "editable", "resource", or a project.godot section such as "application", ""
// before the first one), the fields in its brackets and the properties under it, in file order.
struct Section {
    std::string tag;
    std::vector<std::pair<std::string, Value>> fields;
    std::vector<std::pair<std::string, Value>> properties;
    int line = 0; // 1-based line of the tag

    [[nodiscard]] const Value* field(std::string_view name) const;
    [[nodiscard]] const Value* property(std::string_view name) const;
    // A field or property that is a string, or "".
    [[nodiscard]] std::string field_string(std::string_view name) const;
};

// Parses a .tscn or .tres file. Errors name the line.
[[nodiscard]] Result<std::vector<Section>> parse_resource(std::string_view text);

// Parses project.godot, whose tags are plain section names such as [application].
[[nodiscard]] Result<std::vector<Section>> parse_project_settings(std::string_view text);

} // namespace slash::importer::godot
