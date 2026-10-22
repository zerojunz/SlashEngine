#include "slash/importer/scripts.hpp"

#include "common.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace slash::importer {
namespace {

// --- Text ---------------------------------------------------------------------------------------

bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r';
}

bool is_identifier_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

bool is_digit_char(char c) {
    return c >= '0' && c <= '9';
}

std::string_view trimmed(std::string_view text) {
    while (!text.empty() && is_space(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && is_space(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

// The identifier text starts with, or "".
std::string_view leading_identifier(std::string_view text) {
    std::size_t end = 0;
    while (end < text.size() && is_identifier_char(text[end])) {
        ++end;
    }
    return text.substr(0, end);
}

// Calls visit(i, depth) for each character of text outside string literals, with the depth of
// brackets around it, and returns the depth at the end.
template <typename Visit>
int scan_code(std::string_view text, Visit visit) {
    int depth = 0;
    char quote = '\0';
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (quote != '\0') {
            if (c == '\\') {
                ++i;
            } else if (c == quote) {
                quote = '\0';
            }
            continue;
        }
        if (c == '"' || c == '\'') {
            quote = c;
            continue;
        }
        if (c == '#') {
            break; // a comment
        }
        if (c == '(' || c == '[' || c == '{') {
            ++depth;
        } else if ((c == ')' || c == ']' || c == '}') && depth > 0) {
            --depth;
        }
        if (!visit(i, depth)) {
            break;
        }
    }
    return depth;
}

// text without its comment.
std::string_view without_comment(std::string_view text) {
    std::size_t end = text.size();
    char quote = '\0';
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (quote != '\0') {
            if (c == '\\') {
                ++i;
            } else if (c == quote) {
                quote = '\0';
            }
        } else if (c == '"' || c == '\'') {
            quote = c;
        } else if (c == '#') {
            end = i;
            break;
        }
    }
    return trimmed(text.substr(0, end));
}

// Where the first of chars is outside brackets and strings, or npos.
std::size_t find_top(std::string_view text, std::string_view chars) {
    std::size_t found = std::string_view::npos;
    scan_code(text, [&](std::size_t i, int depth) {
        if (depth == 0 && chars.find(text[i]) != std::string_view::npos) {
            found = i;
            return false;
        }
        return true;
    });
    return found;
}

// text split at each separator outside brackets and strings.
std::vector<std::string_view> split_top(std::string_view text, char separator) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    scan_code(text, [&](std::size_t i, int depth) {
        if (depth == 0 && text[i] == separator) {
            parts.push_back(trimmed(text.substr(start, i - start)));
            start = i + 1;
        }
        return true;
    });
    if (const std::string_view last = trimmed(text.substr(start));
        !last.empty() || !parts.empty()) {
        parts.push_back(last);
    }
    return parts;
}

// The text inside the brackets opening at text[open], or nullopt when they don't close.
std::optional<std::string_view> bracketed(std::string_view text, std::size_t open,
                                          std::size_t* close = nullptr) {
    std::optional<std::string_view> found;
    const std::string_view rest = text.substr(open);
    scan_code(rest, [&](std::size_t i, int depth) {
        if (i > 0 && depth == 0) {
            found = rest.substr(1, i - 1);
            if (close != nullptr) {
                *close = open + i;
            }
            return false;
        }
        return true;
    });
    return found;
}

// text as a C++ string literal.
std::string cpp_string(std::string_view text) {
    std::string out = "\"";
    for (const char c : text) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\t') {
            out += "\\t";
        } else {
            out += c;
        }
    }
    return out + "\"";
}

// text as C++ string literals starting at column, broken after spaces into literals on lines of
// their own, at the same column, so no line passes 100 columns with after characters following
// the last.
std::string cpp_strings(std::string_view text, std::size_t column, std::size_t after) {
    constexpr std::size_t kColumns = 100;
    std::vector<std::string> words;
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t end = text.find(' ', start);
        end = end == std::string_view::npos ? text.size() : end + 1;
        const std::string quoted = cpp_string(text.substr(start, end - start));
        words.push_back(quoted.substr(1, quoted.size() - 2));
        start = end;
    }
    std::string out;
    std::string piece;
    for (std::size_t i = 0; i < words.size(); ++i) {
        const std::size_t tail = i + 1 == words.size() ? after : 0;
        if (!piece.empty() && column + piece.size() + words[i].size() + 2 + tail > kColumns) {
            out += "\"" + piece + "\"\n" + std::string(column, ' ');
            piece.clear();
        }
        piece += words[i];
    }
    return out + "\"" + piece + "\"";
}

// A GDScript string literal's text, or nullopt for anything else. Takes &"..." (a StringName)
// and ^"..." (a NodePath) too.
std::optional<std::string> string_literal(std::string_view text) {
    if (!text.empty() && (text.front() == '&' || text.front() == '^')) {
        text.remove_prefix(1);
    }
    if (text.size() < 2 || (text.front() != '"' && text.front() != '\'') ||
        text.back() != text.front()) {
        return std::nullopt;
    }
    const char quote = text.front();
    std::string out;
    for (std::size_t i = 1; i + 1 < text.size(); ++i) {
        char c = text[i];
        if (c == quote) {
            return std::nullopt; // two literals
        }
        if (c == '\\' && i + 2 < text.size()) {
            c = text[++i];
            switch (c) {
            case 'n':
                c = '\n';
                break;
            case 't':
                c = '\t';
                break;
            default:
                break;
            }
        }
        out += c;
    }
    return out;
}

// --- Names --------------------------------------------------------------------------------------

constexpr auto kKeywords = std::to_array<std::string_view>(
    {"alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand", "bitor", "bool", "break",
     "case", "catch", "char", "char8_t", "char16_t", "char32_t", "class", "compl", "concept",
     "const", "consteval", "constexpr", "constinit", "const_cast", "continue", "co_await",
     "co_return", "co_yield", "decltype", "default", "delete", "do", "double", "dynamic_cast",
     "else", "enum", "explicit", "export", "extern", "false", "float", "for", "friend", "goto",
     "if", "inline", "int", "long", "mutable", "namespace", "new", "noexcept", "not", "not_eq",
     "nullptr", "operator", "or", "or_eq", "private", "protected", "public", "register",
     "reinterpret_cast", "requires", "return", "short", "signed", "sizeof", "static",
     "static_assert", "static_cast", "struct", "switch", "template", "this", "thread_local",
     "throw", "true", "try", "typedef", "typeid", "typename", "union", "unsigned", "using",
     "virtual", "void", "volatile", "wchar_t", "while", "xor", "xor_eq",
     // Names the skeletons use themselves.
     "std", "slash", "entt", "imported", "World", "run", "NULL"});

// A variable's or function's name in C++: without the leading underscores GDScript marks private
// names with.
std::string member_name(std::string_view name) {
    while (name.size() > 1 && name.front() == '_') {
        name.remove_prefix(1);
    }
    return cpp_identifier(name);
}

// "MAX_HEALTH" -> "kMaxHealth".
std::string constant_name(std::string_view name) {
    std::string out = "k";
    for (const std::string_view part : split_path(name, '_')) {
        if (part.empty()) {
            continue;
        }
        const bool upper = std::ranges::none_of(part, [](char c) { return c >= 'a' && c <= 'z'; });
        for (std::size_t i = 0; i < part.size(); ++i) {
            char c = part[i];
            if (i == 0 && c >= 'a' && c <= 'z') {
                c = static_cast<char>(c - 'a' + 'A');
            } else if (i > 0 && upper && c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c - 'A' + 'a');
            }
            out += c;
        }
    }
    return cpp_identifier(out.size() > 1 ? out : "kValue");
}

// A name not yet in taken, made from base, and now in it.
std::string unique_name(std::string base, std::set<std::string>& taken) {
    while (taken.contains(base)) {
        base += '_';
    }
    taken.insert(base);
    return base;
}

// --- Types and values ---------------------------------------------------------------------------

// A type a GDScript declaration names, as C++.
// Every member has an initializer so designated initializers may skip any of them without
// tripping GCC's -Wmissing-field-initializers.
struct Mapped {
    std::string type{};    // NOLINT(readability-redundant-member-init)
    std::string comment{}; // NOLINT(readability-redundant-member-init): what it was, where the
                           // C++ type doesn't say
    FieldKind kind = FieldKind::Value;
};

bool is_resource_type(std::string_view type) {
    static constexpr auto kNames = std::to_array<std::string_view>(
        {"Resource",        "PackedScene", "Shader",        "Theme",
         "SpriteFrames",    "TileSet",     "Animation",     "AnimationLibrary",
         "Environment",     "World2D",     "Script",        "GDScript",
         "Image",           "BitMap",      "LabelSettings", "AudioBusLayout",
         "PhysicsMaterial", "Translation", "Skin",          "MultiMesh",
         "Texture"});
    if (contains(kNames, type)) {
        return true;
    }
    const auto starts = [&](std::string_view prefix) { return type.starts_with(prefix); };
    const auto ends = [&](std::string_view suffix) { return type.ends_with(suffix); };
    return ends("Texture") || ends("Texture2D") || ends("Texture3D") || starts("AudioStream") ||
           type.find("Material") != std::string_view::npos || starts("Font") || ends("Font") ||
           starts("Curve") || starts("Gradient") || starts("StyleBox") || starts("InputEvent") ||
           ends("Mesh") || ends("Resource") || starts("Noise") ||
           ((ends("Shape2D") || ends("Shape3D")) && !starts("Collision"));
}

// Godot's own types SlashEngine has no field type for.
bool is_other_builtin(std::string_view type) {
    static constexpr auto kNames = std::to_array<std::string_view>(
        {"Transform2D", "Transform3D", "Basis", "Plane", "AABB", "Projection", "Callable", "Signal",
         "RID", "Object", "Rect2i", "Vector3i", "Vector4i"});
    return contains(kNames, type);
}

struct Context {
    const GdscriptOptions* options = nullptr;
    // Constants by their GDScript names: their C++ names and types ("" when they don't
    // translate).
    std::map<std::string, std::pair<std::string, std::string>> constants;
    std::set<std::string> enums;                    // GDScript names of named enums
    std::map<std::string, std::string> enum_values; // "State.IDLE" and "IDLE" -> C++
};

Mapped map_type(std::string_view type, const Context& context) {
    type = trimmed(type);
    const std::string name(type);
    if (type.empty() || type == "Variant") {
        return {.type = "slash::json::Value"};
    }
    if (type == "int") {
        return {.type = "int"};
    }
    if (type == "float") {
        return {.type = "float"};
    }
    if (type == "bool") {
        return {.type = "bool"};
    }
    if (type == "String" || type == "StringName") {
        return {.type = "std::string"};
    }
    if (type == "NodePath") {
        return {.type = "entt::entity", .comment = "NodePath", .kind = FieldKind::Entity};
    }
    if (type == "Vector2") {
        return {.type = "slash::Vec2"};
    }
    if (type == "Vector2i") {
        return {.type = "slash::Vec2i"};
    }
    if (type == "Vector3") {
        return {.type = "slash::Vec3"};
    }
    if (type == "Vector4") {
        return {.type = "slash::Vec4"};
    }
    if (type == "Color") {
        return {.type = "slash::Vec4", .comment = "Color"};
    }
    if (type == "Quaternion") {
        return {.type = "slash::Quat"};
    }
    if (type == "Rect2") {
        return {.type = "slash::Vec4", .comment = "Rect2: x, y, width, height"};
    }
    if (context.enums.contains(name) || type.find('.') != std::string_view::npos) {
        return {.type = "int", .comment = name};
    }
    if (type == "Array" || type == "Dictionary") {
        return {.type = "slash::json::Value", .comment = name};
    }
    if (type.starts_with("Array[") && type.ends_with("]")) {
        const Mapped item = map_type(type.substr(6, type.size() - 7), context);
        return {.type = "std::vector<" + item.type + ">",
                .comment = item.comment.empty() ? "" : name,
                .kind = item.kind == FieldKind::Entity ? FieldKind::Entities : FieldKind::Value};
    }
    if (type.starts_with("Dictionary[") && type.ends_with("]")) {
        const auto parts = split_top(type.substr(11, type.size() - 12), ',');
        if (parts.size() == 2 && (parts[0] == "String" || parts[0] == "StringName")) {
            const Mapped value = map_type(parts[1], context);
            if (value.kind == FieldKind::Value) {
                return {.type = "std::map<std::string, " + value.type + ">",
                        .comment = value.comment.empty() ? "" : name};
            }
        }
        return {.type = "slash::json::Value", .comment = name};
    }
    static const std::map<std::string, std::string, std::less<>> kPacked{
        {"PackedByteArray", "std::vector<int>"},
        {"PackedInt32Array", "std::vector<int>"},
        {"PackedInt64Array", "std::vector<std::int64_t>"},
        {"PackedFloat32Array", "std::vector<float>"},
        {"PackedFloat64Array", "std::vector<double>"},
        {"PackedStringArray", "std::vector<std::string>"},
        {"PackedVector2Array", "std::vector<slash::Vec2>"},
        {"PackedVector3Array", "std::vector<slash::Vec3>"},
        {"PackedVector4Array", "std::vector<slash::Vec4>"},
        {"PackedColorArray", "std::vector<slash::Vec4>"}};
    if (const auto packed = kPacked.find(type); packed != kPacked.end()) {
        return {.type = packed->second, .comment = type == "PackedColorArray" ? name : ""};
    }
    if (is_other_builtin(type)) {
        return {.type = "slash::json::Value", .comment = name};
    }
    if (is_resource_type(type)) {
        return {.type = "std::string", .comment = name + ": its path"};
    }
    // A node class, Godot's or one of the game's: the entity it is.
    return {.type = "entt::entity", .comment = name, .kind = FieldKind::Entity};
}

// A value written in GDScript, as C++.
struct Translated {
    // Its natural C++ type: "" for an empty list or dictionary, "null". Every member has an
    // initializer, as Mapped's do.
    std::string type{};    // NOLINT(readability-redundant-member-init)
    std::string value{};   // NOLINT(readability-redundant-member-init): as a C++ expression
    std::string comment{}; // NOLINT(readability-redundant-member-init)
};

// A number literal: ints as written without '_', floats with an F suffix.
std::optional<Translated> number(std::string_view text) {
    std::string digits;
    std::size_t i = 0;
    if (!text.empty() && (text.front() == '-' || text.front() == '+')) {
        if (text.front() == '-') {
            digits += '-';
        }
        i = 1;
    }
    const std::string_view body = text.substr(i);
    if (body.empty()) {
        return std::nullopt;
    }
    if (body.starts_with("0x") || body.starts_with("0b")) {
        for (std::size_t j = 2; j < body.size(); ++j) {
            const char c = body[j];
            const bool hex = (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
            if (!is_digit_char(c) && (!hex || body[1] != 'x') && c != '_') {
                return std::nullopt;
            }
        }
        digits += body.substr(0, 2);
        for (const char c : body.substr(2)) {
            if (c != '_') {
                digits += c;
            }
        }
        return Translated{.type = "int", .value = digits};
    }
    bool real = false;
    bool exponent = false;
    for (std::size_t j = 0; j < body.size(); ++j) {
        const char c = body[j];
        if (is_digit_char(c)) {
            digits += c;
        } else if (c == '_') {
            continue;
        } else if (c == '.' && !real && !exponent) {
            real = true;
            if (digits.empty() || digits == "-") {
                digits += '0';
            }
            digits += '.';
            if (j + 1 == body.size() || !is_digit_char(body[j + 1])) {
                digits += '0';
            }
        } else if ((c == 'e' || c == 'E') && !exponent && j > 0) {
            exponent = true;
            real = true;
            digits += 'e';
            if (j + 1 < body.size() && (body[j + 1] == '-' || body[j + 1] == '+')) {
                digits += body[++j];
            }
        } else {
            return std::nullopt;
        }
    }
    if (digits.empty() || digits == "-") {
        return std::nullopt;
    }
    if (!real) {
        const std::size_t length = digits.size() - (digits.front() == '-' ? 1 : 0);
        return Translated{.type = length > 9 ? "std::int64_t" : "int", .value = digits};
    }
    if (digits.find('.') == std::string::npos) {
        digits.insert(digits.find('e'), ".0");
    }
    return Translated{.type = "float", .value = digits + "F"};
}

// value as a float expression: "2" -> "2.0F".
std::string as_float(const Translated& value) {
    if (value.type == "int" && !value.value.starts_with("0x") && !value.value.starts_with("0b") &&
        !value.value.starts_with("-0x")) {
        return value.value + ".0F";
    }
    if (value.type == "int" || value.type == "std::int64_t") {
        return "static_cast<float>(" + value.value + ")";
    }
    return value.value;
}

// Godot's named colors that games use most, as Color's constants define them.
std::optional<std::array<float, 4>> named_color(std::string_view name) {
    static const std::map<std::string_view, std::array<float, 4>> kColors{
        {"BLACK", {0.0F, 0.0F, 0.0F, 1.0F}},
        {"WHITE", {1.0F, 1.0F, 1.0F, 1.0F}},
        {"RED", {1.0F, 0.0F, 0.0F, 1.0F}},
        {"GREEN", {0.0F, 1.0F, 0.0F, 1.0F}},
        {"BLUE", {0.0F, 0.0F, 1.0F, 1.0F}},
        {"YELLOW", {1.0F, 1.0F, 0.0F, 1.0F}},
        {"CYAN", {0.0F, 1.0F, 1.0F, 1.0F}},
        {"MAGENTA", {1.0F, 0.0F, 1.0F, 1.0F}},
        {"TRANSPARENT", {1.0F, 1.0F, 1.0F, 0.0F}},
        {"GRAY", {0.745098F, 0.745098F, 0.745098F, 1.0F}},
        {"ORANGE", {1.0F, 0.647059F, 0.0F, 1.0F}},
        {"PURPLE", {0.627451F, 0.12549F, 0.941176F, 1.0F}}};
    const auto found = kColors.find(name);
    return found != kColors.end() ? std::optional(found->second) : std::nullopt;
}

std::string float_text(float value) {
    std::array<char, 32> buffer{};
    std::snprintf(buffer.data(), buffer.size(), "%g", static_cast<double>(value));
    std::string text = buffer.data();
    if (text.find_first_of(".e") == std::string::npos) {
        text += ".0";
    }
    return text + "F";
}

std::string double_text(double value) {
    std::string text = float_text(static_cast<float>(value));
    text.pop_back(); // the F
    return text;
}

std::string vec4_text(const std::array<float, 4>& color) {
    return "slash::Vec4{" + float_text(color[0]) + ", " + float_text(color[1]) + ", " +
           float_text(color[2]) + ", " + float_text(color[3]) + "}";
}

std::optional<std::array<float, 4>> hex_color(std::string_view text) {
    if (text.starts_with('#')) {
        text.remove_prefix(1);
    }
    if (text.size() != 6 && text.size() != 8) {
        return std::nullopt;
    }
    std::array<float, 4> color{0.0F, 0.0F, 0.0F, 1.0F};
    for (std::size_t i = 0; i < text.size() / 2; ++i) {
        int value = 0;
        for (const char c : text.substr(i * 2, 2)) {
            value *= 16;
            if (is_digit_char(c)) {
                value += c - '0';
            } else if (c >= 'a' && c <= 'f') {
                value += c - 'a' + 10;
            } else if (c >= 'A' && c <= 'F') {
                value += c - 'A' + 10;
            } else {
                return std::nullopt;
            }
        }
        color.at(i) = static_cast<float>(value) / 255.0F;
    }
    return color;
}

std::optional<Translated> translate(std::string_view text, const Context& context);

// Vector2(1, 2), Vector2.ZERO and the like.
std::optional<Translated> vector(std::string_view text, const Context& context) {
    struct Kind {
        std::string_view name;
        std::string_view type;
        std::size_t size;
        bool integer;
    };
    static constexpr std::array<Kind, 4> kKinds{{{"Vector2", "slash::Vec2", 2, false},
                                                 {"Vector2i", "slash::Vec2i", 2, true},
                                                 {"Vector3", "slash::Vec3", 3, false},
                                                 {"Vector4", "slash::Vec4", 4, false}}};
    for (const Kind& kind : kKinds) {
        if (!text.starts_with(kind.name)) {
            continue;
        }
        const std::string_view rest = text.substr(kind.name.size());
        const std::string type(kind.type);
        if (rest.starts_with('.')) {
            static const std::map<std::string_view, std::array<int, 3>> kConstants{
                {"ZERO", {0, 0, 0}},     {"ONE", {1, 1, 1}},   {"UP", {0, -1, 0}},
                {"DOWN", {0, 1, 0}},     {"LEFT", {-1, 0, 0}}, {"RIGHT", {1, 0, 0}},
                {"FORWARD", {0, 0, -1}}, {"BACK", {0, 0, 1}}};
            auto found = kConstants.find(rest.substr(1));
            if (found == kConstants.end() || kind.size == 4) {
                return std::nullopt;
            }
            std::array<int, 3> axes = found->second;
            if (kind.size == 3 && (rest == ".UP" || rest == ".DOWN")) {
                axes[1] = -axes[1]; // Vector3's up is +Y
            }
            if (kind.size == 2 && axes[2] != 0) {
                return std::nullopt;
            }
            std::string value = type + "{";
            for (std::size_t i = 0; i < kind.size; ++i) {
                const std::string n = std::to_string(axes.at(i));
                value += (i > 0 ? ", " : "") + (kind.integer ? n : n + ".0F");
            }
            return Translated{.type = type, .value = value + "}"};
        }
        if (!rest.starts_with('(') || !rest.ends_with(')')) {
            continue;
        }
        const auto args = split_top(rest.substr(1, rest.size() - 2), ',');
        if (!args.empty() && args.size() != kind.size) {
            return std::nullopt;
        }
        std::string value = type + "{";
        for (std::size_t i = 0; i < args.size(); ++i) {
            const auto item = translate(args[i], context);
            if (!item || (item->type != "int" && item->type != "float")) {
                return std::nullopt;
            }
            if (kind.integer && item->type != "int") {
                return std::nullopt;
            }
            value += (i > 0 ? ", " : "") + (kind.integer ? item->value : as_float(*item));
        }
        return Translated{.type = type, .value = value + "}"};
    }
    return std::nullopt;
}

// Color(1, 0.5, 0), Color("#ff8000"), Color.RED and the like.
std::optional<Translated> color(std::string_view text, const Context& context) {
    const Translated base{.type = "slash::Vec4", .comment = "Color"};
    if (text.starts_with("Color.") && !text.starts_with("Color.html")) {
        const auto named = named_color(text.substr(6));
        if (!named) {
            return std::nullopt;
        }
        Translated out = base;
        out.value = vec4_text(*named);
        return out;
    }
    std::string_view args_text;
    if (text.starts_with("Color(") && text.ends_with(')')) {
        args_text = text.substr(6, text.size() - 7);
    } else if (text.starts_with("Color.html(") && text.ends_with(')')) {
        args_text = text.substr(11, text.size() - 12);
    } else {
        return std::nullopt;
    }
    const auto args = split_top(args_text, ',');
    if (args.size() == 1) {
        if (const auto hex = string_literal(args[0]); hex) {
            if (const auto parsed = hex_color(*hex); parsed) {
                Translated out = base;
                out.value = vec4_text(*parsed);
                return out;
            }
        }
        return std::nullopt;
    }
    if (args.size() != 3 && args.size() != 4) {
        return std::nullopt;
    }
    std::string value = "slash::Vec4{";
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto item = translate(args[i], context);
        if (!item || (item->type != "int" && item->type != "float")) {
            return std::nullopt;
        }
        value += (i > 0 ? ", " : "") + as_float(*item);
    }
    Translated out = base;
    out.value = value + (args.size() == 3 ? ", 1.0F}" : "}");
    return out;
}

std::optional<Translated> translate(std::string_view text, const Context& context) {
    text = trimmed(text);
    if (text.empty()) {
        return std::nullopt;
    }
    if (auto value = number(text); value) {
        return value;
    }
    if (text == "true" || text == "false") {
        return Translated{.type = "bool", .value = std::string(text)};
    }
    if (text == "null") {
        return Translated{.type = "null"};
    }
    if (text == "[]") {
        return Translated{.type = "[]"};
    }
    if (text == "{}") {
        return Translated{.type = "{}"};
    }
    if (text.front() == '^' || text.front() == '$' || text.front() == '%' ||
        text.starts_with("get_node(")) {
        return Translated{.type = "entt::entity", .value = "entt::null", .comment = "NodePath"};
    }
    if (auto literal = string_literal(text); literal) {
        return Translated{.type = "std::string", .value = cpp_string(*literal)};
    }
    if (text == "PI" || text == "TAU") {
        return Translated{.type = "float", .value = text == "PI" ? "3.14159265F" : "6.28318531F"};
    }
    if (text.starts_with("Vector")) {
        return vector(text, context);
    }
    if (text.starts_with("Color")) {
        return color(text, context);
    }
    for (const std::string_view call : {std::string_view("preload("), std::string_view("load(")}) {
        if (text.starts_with(call) && text.ends_with(')')) {
            const auto path =
                string_literal(text.substr(call.size(), text.size() - call.size() - 1));
            if (!path) {
                return std::nullopt;
            }
            std::string mapped = *path;
            if (context.options != nullptr && context.options->resource_path) {
                mapped = context.options->resource_path(*path);
            }
            return Translated{.type = "std::string",
                              .value = cpp_string(mapped),
                              .comment = std::string(call.substr(0, call.size() - 1))};
        }
    }
    const std::string name(text.front() == '-' ? text.substr(1) : text);
    const std::string_view sign = text.front() == '-' ? "-" : "";
    if (const auto constant = context.constants.find(name); constant != context.constants.end()) {
        const auto& [cpp_name, type] = constant->second;
        if (type.empty() || (!sign.empty() && type != "int" && type != "float")) {
            return std::nullopt;
        }
        return Translated{.type = type, .value = std::string(sign) + cpp_name};
    }
    if (const auto value = context.enum_values.find(name); value != context.enum_values.end()) {
        return Translated{.type = "int", .value = std::string(sign) + value->second};
    }
    return std::nullopt;
}

// A translated value as the initializer of a field of a type: " = 1.0F", "{1.0F, 2.0F}" or "".
std::optional<std::string> initializer(const Translated& value, const std::string& type) {
    if (value.type == "null" || value.type == "[]" || value.type == "{}") {
        if (type == "slash::json::Value") {
            if (value.type == "[]") {
                return std::string(" = slash::json::Array{}");
            }
            if (value.type == "{}") {
                return std::string(" = slash::json::Object{}");
            }
        }
        if (type == "entt::entity") {
            return std::string(" = entt::null");
        }
        return std::string();
    }
    if (type == "std::string" && value.type == "std::string_view") {
        return "{" + value.value + "}";
    }
    if (type == value.type) {
        if (value.value.starts_with("slash::")) {
            return value.value.substr(value.value.find('{'));
        }
        return " = " + value.value;
    }
    if ((type == "float" || type == "double") && (value.type == "int" || value.type == "float")) {
        std::string text = as_float(value);
        if (type == "double" && text.ends_with('F')) {
            text.pop_back();
        }
        return " = " + text;
    }
    if (type == "int" && value.type == "int") {
        return " = " + value.value;
    }
    if (type == "std::int64_t" && value.type == "int") {
        return " = " + value.value;
    }
    if (type == "slash::json::Value" && (value.type == "int" || value.type == "float" ||
                                         value.type == "bool" || value.type == "std::string")) {
        return " = slash::json::Value(" + value.value + ")";
    }
    return std::nullopt;
}

// --- Reading GDScript ---------------------------------------------------------------------------

// A line of the file, with its number from 1.
struct Line {
    int number = 0;
    std::string_view text;
};

std::vector<Line> lines_of(std::string_view text) {
    std::vector<Line> lines;
    std::size_t start = 0;
    int number = 1;
    while (start < text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        std::string_view line = text.substr(start, end - start);
        if (line.ends_with('\r')) {
            line.remove_suffix(1);
        }
        lines.push_back({number++, line});
        start = end + 1;
    }
    return lines;
}

bool indented(std::string_view line) {
    return !line.empty() && (line.front() == ' ' || line.front() == '\t');
}

std::string detab(std::string_view line) {
    std::string out;
    for (const char c : line) {
        if (c == '\t') {
            out += "    ";
        } else {
            out += c;
        }
    }
    return out;
}

// A function's body: the indented lines after its declaration, one indent less.
std::vector<std::string> body_lines(const std::vector<Line>& lines, std::size_t from,
                                    std::size_t& next) {
    std::size_t end = from;
    while (end < lines.size()) {
        const std::string_view text = lines[end].text;
        if (!trimmed(text).empty() && !indented(text) && !text.starts_with('#')) {
            break;
        }
        ++end;
    }
    next = end;
    // Comments and blank lines at the end belong to what follows.
    while (end > from &&
           (trimmed(lines[end - 1].text).empty() || lines[end - 1].text.starts_with('#'))) {
        --end;
    }
    std::vector<std::string> body;
    std::size_t indent = std::string::npos;
    for (std::size_t i = from; i < end; ++i) {
        body.push_back(detab(lines[i].text));
        const std::string& line = body.back();
        if (const std::size_t first = line.find_first_not_of(' ');
            first != std::string::npos && !line.empty() && line[first] != '#') {
            indent = std::min(indent, first);
        }
    }
    for (std::string& line : body) {
        const std::size_t first = line.find_first_not_of(' ');
        line.erase(0, std::min({indent, first, line.size()}));
        while (!line.empty() && line.back() == ' ') {
            line.pop_back();
        }
    }
    return body;
}

// The annotations at the start of a line, such as @export_range(0, 10), and what follows them.
struct Annotated {
    std::vector<std::pair<std::string, std::string>> annotations; // name, arguments
    std::string_view rest;
};

Annotated annotations_of(std::string_view text) {
    Annotated out;
    text = trimmed(text);
    while (text.starts_with('@')) {
        const std::string_view name = leading_identifier(text.substr(1));
        std::string_view rest = text.substr(1 + name.size());
        std::string args;
        if (rest.starts_with('(')) {
            std::size_t close = 0;
            if (const auto inside = bracketed(rest, 0, &close); inside) {
                args = std::string(*inside);
                rest = rest.substr(close + 1);
            }
        }
        out.annotations.emplace_back(std::string(name), std::move(args));
        text = trimmed(rest);
    }
    out.rest = text;
    return out;
}

// A declaration's name, type and value: "speed: float = 2.0", "count := 0", "x = 1", "y".
struct Declared {
    std::string name;
    std::string type;       // as written, or "" when inferred or left out
    std::string value;      // as written, or ""
    bool inferred = false;  // ":="
    bool accessors = false; // a setter or getter follows
};

Declared declared(std::string_view text) {
    Declared out;
    text = trimmed(text);
    out.name = std::string(leading_identifier(text));
    std::string_view rest = trimmed(text.substr(out.name.size()));
    // A setter or getter starts at a ':' after the value, or right after the type.
    const auto take_value = [&](std::string_view value) {
        if (const std::size_t colon = find_top(value, ":"); colon != std::string_view::npos) {
            out.accessors = true;
            value = value.substr(0, colon);
        }
        out.value = std::string(trimmed(value));
    };
    if (rest.starts_with(":=")) {
        out.inferred = true;
        take_value(rest.substr(2));
    } else if (rest.starts_with(':')) {
        rest = trimmed(rest.substr(1));
        const std::size_t end = find_top(rest, "=:");
        out.type = std::string(trimmed(rest.substr(0, end)));
        if (end != std::string_view::npos && rest[end] == '=') {
            take_value(rest.substr(end + 1));
        } else if (end != std::string_view::npos) {
            out.accessors = true;
        }
    } else if (rest.starts_with('=')) {
        take_value(rest.substr(1));
    }
    return out;
}

// The C++ type and initializer of a variable or parameter.
struct Typed {
    Mapped mapped;
    std::string init;         // " = 1.0F", "{...}" or ""
    std::string untranslated; // the value as written, when it didn't translate
};

// The initializer of a variable of a mapped type from its value as written (in GDScript's
// syntax), and that value when it doesn't translate.
Typed typed(Mapped mapped, std::string_view text, const std::optional<Translated>& value,
            const Context& context) {
    Typed out;
    out.mapped = std::move(mapped);
    if (value) {
        if (auto init = initializer(*value, out.mapped.type); init) {
            out.init = std::move(*init);
            if (out.mapped.comment.empty() && !value->comment.empty() &&
                value->comment != "NodePath") {
                out.mapped.comment = value->comment;
            }
            return out;
        }
    }
    // A list for a typed array, item by item.
    const std::string_view list = text;
    if (!value && out.mapped.type.starts_with("std::vector<") && list.starts_with('[') &&
        list.ends_with(']')) {
        const std::string item_type = out.mapped.type.substr(12, out.mapped.type.size() - 13);
        std::vector<std::string> items;
        bool translated = true;
        for (const std::string_view item : split_top(list.substr(1, list.size() - 2), ',')) {
            if (item.empty()) {
                continue;
            }
            const auto item_value = translate(item, context);
            auto init = item_value ? initializer(*item_value, item_type) : std::nullopt;
            if (!init || init->empty()) {
                translated = false;
                break;
            }
            items.push_back(init->starts_with(" = ") ? init->substr(3) : item_type + *init);
        }
        if (translated) {
            out.init = "{" + join(items, ", ") + "}";
            return out;
        }
    }
    if (out.mapped.type == "entt::entity") {
        out.init = " = entt::null";
    }
    if (!text.empty() && !(value && value->type == "entt::entity")) {
        out.untranslated = std::string(text);
    }
    return out;
}

Typed type_of(const Declared& declaration, const Context& context) {
    std::optional<Translated> value;
    if (!declaration.value.empty()) {
        value = translate(declaration.value, context);
    }
    Mapped mapped;
    if (!declaration.type.empty()) {
        mapped = map_type(declaration.type, context);
    } else if (value && !value->type.empty() && value->type != "null" && value->type != "[]" &&
               value->type != "{}") {
        mapped.type = value->type;
        mapped.comment = value->comment;
        if (value->type == "entt::entity") {
            mapped.kind = FieldKind::Entity;
        }
    } else {
        mapped.type = "slash::json::Value";
    }
    return typed(std::move(mapped), declaration.value, value, context);
}

// A parameter's C++ type: strings, lists and JSON by const reference.
std::string parameter_type(const std::string& type) {
    if (type == "std::string" || type.starts_with("std::vector") || type.starts_with("std::map") ||
        type == "slash::json::Value") {
        return "const " + type + "&";
    }
    return type;
}

std::vector<ScriptParam> parameters(std::string_view text, const Context& context) {
    std::vector<ScriptParam> out;
    std::set<std::string> taken{"world", "entity", "self", "data", "tick_seconds"};
    for (std::string_view part : split_top(text, ',')) {
        if (part.empty()) {
            continue;
        }
        ScriptParam param;
        param.source = std::string(part);
        const bool rest = part.starts_with("...");
        if (rest) {
            part.remove_prefix(3);
        }
        const Declared declaration = declared(part);
        param.name = unique_name(member_name(declaration.name), taken);
        const Typed typed = type_of(declaration, context);
        param.type = rest ? "const slash::json::Value&" : parameter_type(typed.mapped.type);
        out.push_back(std::move(param));
    }
    return out;
}

} // namespace

std::string cpp_identifier(std::string_view name) {
    std::string out;
    for (const char c : name) {
        out += is_identifier_char(c) ? c : '_';
    }
    if (out.empty()) {
        return "value";
    }
    if (is_digit_char(out.front())) {
        out.insert(out.begin(), 'n');
    }
    if (contains(kKeywords, std::string_view(out))) {
        out += '_';
    }
    return out;
}

std::string pascal_case(std::string_view name) {
    std::string out;
    bool start = true;
    for (const char c : name) {
        if (c == '_' || c == '-' || c == ' ' || c == '.') {
            start = true;
            continue;
        }
        if (!is_identifier_char(c)) {
            continue;
        }
        out += start && c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
        start = false;
    }
    if (out.empty() || is_digit_char(out.front())) {
        out.insert(0, "Script");
    }
    return cpp_identifier(out);
}

bool is_engine_system(std::string_view name) {
    static constexpr std::array<std::string_view, 15> kSystems{"ai",
                                                               "animated_sprite_2d",
                                                               "animation_player",
                                                               "animation_tree",
                                                               "audio",
                                                               "camera_2d",
                                                               "cpu_particles_2d",
                                                               "gpu_particles_2d",
                                                               "gui",
                                                               "input",
                                                               "parallax_2d",
                                                               "physics2d",
                                                               "physics3d",
                                                               "services",
                                                               "skeleton_modifiers"};
    return contains(kSystems, name);
}

namespace {

// A top-level declaration of a GDScript file: its lines joined, without comments.
struct Declaration {
    std::string code;
    std::string doc;      // its ## comment
    int line = 0;         // where it starts, from 1
    std::size_t next = 0; // the index of the line after it
};

// The file's top-level declarations, and the class's ## comment: the one before its first
// member that a blank line or extends follows.
std::vector<Declaration> declarations_of(const std::vector<Line>& lines, std::string& class_doc) {
    std::vector<Declaration> out;
    std::string doc;
    bool member = false; // a declaration other than extends and class_name seen
    for (std::size_t i = 0; i < lines.size();) {
        const std::string_view line = lines[i].text;
        if (trimmed(line).empty()) {
            if (!member && !doc.empty() && class_doc.empty()) {
                class_doc = doc;
            }
            doc.clear();
            ++i;
            continue;
        }
        if (line.starts_with("##")) {
            const std::string_view words = trimmed(line.substr(2));
            if (!words.empty()) {
                doc += (doc.empty() ? "" : " ") + std::string(words);
            }
            ++i;
            continue;
        }
        if (indented(line) || line.starts_with('#')) {
            ++i;
            continue;
        }
        Declaration declaration{.code = {}, .doc = std::move(doc), .line = lines[i].number};
        doc.clear();
        std::size_t j = i;
        while (j < lines.size()) {
            std::string_view part = without_comment(lines[j].text);
            const bool continued = part.ends_with('\\');
            if (continued) {
                part.remove_suffix(1);
            }
            if (!trimmed(part).empty()) {
                declaration.code +=
                    (declaration.code.empty() ? "" : " ") + std::string(trimmed(part));
            }
            ++j;
            if (!continued &&
                scan_code(declaration.code, [](std::size_t, int) { return true; }) == 0) {
                break;
            }
        }
        declaration.next = j;
        const std::string_view code = declaration.code;
        if (code.starts_with("extends") || code.starts_with("class_name") ||
            code.starts_with('@')) {
            if (!member && !declaration.doc.empty() && class_doc.empty() &&
                !code.starts_with("@export")) {
                class_doc = declaration.doc;
            }
        } else {
            member = true;
        }
        out.push_back(std::move(declaration));
        i = j;
    }
    return out;
}

// Reads `const NAME := value` into a constant, translated when it can be.
ScriptConstant read_constant(std::string_view code, const Context& context) {
    const Declared declaration = declared(code);
    ScriptConstant constant;
    constant.name = constant_name(declaration.name);
    constant.declaration = "const " + std::string(code);
    const auto value = translate(declaration.value, context);
    if (!value || value->type.empty() || value->type == "null" || value->type == "[]" ||
        value->type == "{}" || value->type == "entt::entity") {
        return constant;
    }
    constant.type = value->type;
    constant.value = value->value;
    if (!declaration.type.empty()) {
        const Mapped mapped = map_type(declaration.type, context);
        if (auto init = initializer(*value, mapped.type); init && !init->empty()) {
            constant.type = mapped.type;
            constant.value = init->starts_with(" = ") ? init->substr(3) : mapped.type + *init;
        }
    }
    if (constant.type == "std::string") {
        constant.type = "std::string_view";
    }
    return constant;
}

// Reads `enum Name {A, B = 2}` into an enum, noting its values in the context.
std::optional<ScriptEnum> read_enum(std::string_view code, Context& context) {
    ScriptEnum out;
    out.declaration = "enum " + std::string(code);
    const std::string name(leading_identifier(code));
    const std::string_view rest = trimmed(code.substr(name.size()));
    if (!rest.starts_with('{')) {
        return std::nullopt;
    }
    const auto inside = bracketed(rest, 0);
    if (!inside) {
        return std::nullopt;
    }
    out.name = name.empty() ? std::string() : cpp_identifier(name);
    for (const std::string_view item : split_top(*inside, ',')) {
        const std::string value_name(leading_identifier(item));
        if (value_name.empty()) {
            continue;
        }
        std::string value = constant_name(value_name);
        if (const std::size_t equals = item.find('='); equals != std::string_view::npos) {
            const auto number_value = translate(item.substr(equals + 1), context);
            if (number_value && number_value->type == "int") {
                value += " = " + number_value->value;
            } else {
                value += " /* = " + std::string(trimmed(item.substr(equals + 1))) + " */";
            }
        }
        const std::string cpp = out.name.empty() ? constant_name(value_name)
                                                 : out.name + "::" + constant_name(value_name);
        if (!name.empty()) {
            std::string qualified = name;
            qualified += ".";
            qualified += value_name;
            context.enum_values[qualified] = cpp;
        }
        context.enum_values.emplace(value_name, cpp);
        out.values.push_back(std::move(value));
    }
    return out;
}

using Annotations = std::vector<std::pair<std::string, std::string>>;

// Whether an annotation on a line of its own belongs to the declaration after it.
bool carries(std::string_view annotation) {
    return annotation != "tool" && annotation != "icon" && annotation != "static_unload" &&
           annotation != "export_group" && annotation != "export_subgroup" &&
           annotation != "export_category";
}

// Reads `var name: Type = value` into a field, with what its annotations say.
ScriptField read_variable(const Declared& declaration, const Annotations& annotations,
                          const Context& context) {
    ScriptField field;
    Declared typed_declaration = declaration;
    for (const auto& [name, args] : annotations) {
        if (!name.starts_with("export") || !carries(name)) {
            continue;
        }
        field.exported = true;
        std::string type;
        if (name == "export_range") {
            const auto parts = split_top(args, ',');
            const auto low = parts.size() >= 2 ? number(parts[0]) : std::nullopt;
            const auto high = parts.size() >= 2 ? number(parts[1]) : std::nullopt;
            if (low && high) {
                std::string low_text = low->value;
                std::string high_text = high->value;
                for (std::string* text : {&low_text, &high_text}) {
                    if (text->ends_with('F')) {
                        text->pop_back();
                    }
                }
                field.min = std::stod(low_text);
                field.max = std::stod(high_text);
            }
        } else if (name == "export_node_path") {
            type = "NodePath";
        } else if (name == "export_enum" || name.starts_with("export_flags")) {
            type = "int";
        } else if (name == "export_file" || name == "export_dir" || name == "export_global_file" ||
                   name == "export_global_dir" || name == "export_multiline" ||
                   name == "export_placeholder") {
            type = "String";
        } else if (name == "export_color_no_alpha") {
            type = "Color";
        } else if (name == "export_exp_easing") {
            type = "float";
        }
        if (typed_declaration.type.empty() && !type.empty() &&
            (typed_declaration.value.empty() || !translate(typed_declaration.value, context))) {
            typed_declaration.type = type;
        }
    }
    const Typed typed = type_of(typed_declaration, context);
    field.source_name = declaration.name;
    field.type = typed.mapped.type;
    field.init = typed.init;
    field.kind = typed.mapped.kind;
    field.comment = typed.mapped.comment;
    if (!typed.untranslated.empty()) {
        field.comment += (field.comment.empty() ? "" : "; ") + std::string("= ") +
                         typed.untranslated + " in Godot";
    }
    return field;
}

// Reads `func name(params) -> Type:` and the body after it into a function.
ScriptFunction read_function(std::string_view code, const Declaration& declaration,
                             const std::vector<Line>& lines, const Context& context) {
    ScriptFunction function;
    function.line = declaration.line;
    std::string_view rest = trimmed(code);
    function.source_name = std::string(leading_identifier(rest));
    rest = trimmed(rest.substr(function.source_name.size()));
    if (rest.starts_with('(')) {
        std::size_t close = 0;
        if (const auto inside = bracketed(rest, 0, &close); inside) {
            function.params = parameters(*inside, context);
            rest = trimmed(rest.substr(close + 1));
        }
    }
    if (rest.starts_with("->")) {
        rest = trimmed(rest.substr(2));
        const std::size_t colon = find_top(rest, ":");
        const std::string_view type = trimmed(rest.substr(0, colon));
        if (type != "void") {
            function.returns = map_type(type, context).type;
        }
        rest = colon == std::string_view::npos ? std::string_view() : rest.substr(colon);
    }
    const std::string_view whole = declaration.code;
    const std::size_t colon = find_top(whole, ":");
    function.declaration = std::string(
        trimmed(whole.substr(0, colon == std::string_view::npos ? whole.size() : colon + 1)));
    if (rest.starts_with(':') && !trimmed(rest.substr(1)).empty()) {
        function.body.emplace_back(trimmed(rest.substr(1))); // func f(): return 1
    }
    std::size_t next = 0;
    std::vector<std::string> body = body_lines(lines, declaration.next, next);
    function.body.insert(function.body.end(), body.begin(), body.end());
    return function;
}

} // namespace

Script read_gdscript(std::string_view path, std::string_view text, const GdscriptOptions& options) {
    Script script;
    script.path = std::string(path);
    std::string stem = snake_case(file_stem(path));
    std::ranges::replace(stem, '-', '_');
    script.ns = cpp_identifier(stem);
    script.system = script.ns;
    script.component = pascal_case(file_stem(path));
    const std::vector<Line> lines = lines_of(text);
    const std::vector<Declaration> declarations = declarations_of(lines, script.doc);

    Context context;
    context.options = &options;
    // Names a function can't take: the system's, the signals' helpers', enums' and constants'.
    std::set<std::string> functions_taken{"run"};
    for (const Declaration& declaration : declarations) {
        const std::string_view code = annotations_of(declaration.code).rest;
        if (code.starts_with("enum ")) {
            const std::string_view name = leading_identifier(trimmed(code.substr(5)));
            if (!name.empty()) {
                context.enums.emplace(name);
            }
        } else if (code.starts_with("signal ")) {
            functions_taken.insert("emit_" +
                                   member_name(leading_identifier(trimmed(code.substr(7)))));
        }
    }

    // What the script is, its constants and its enums, which variables' values may name.
    for (const Declaration& declaration : declarations) {
        std::string_view code = annotations_of(declaration.code).rest;
        if (code.starts_with("class_name ")) {
            code = trimmed(code.substr(11));
            script.class_name = std::string(leading_identifier(code));
            code = trimmed(code.substr(script.class_name.size()));
        }
        if (code.starts_with("extends ")) {
            script.extends = std::string(trimmed(code.substr(8)));
            if (const auto quoted = string_literal(script.extends); quoted) {
                script.notes.push_back("It extends the script " + *quoted +
                                       ", whose variables and functions aren't here.");
                script.extends = *quoted;
            }
        } else if (code.starts_with("const ")) {
            ScriptConstant constant = read_constant(trimmed(code.substr(6)), context);
            context.constants[std::string(leading_identifier(trimmed(code.substr(6))))] = {
                constant.name, constant.type};
            functions_taken.insert(constant.name);
            script.constants.push_back(std::move(constant));
        } else if (code.starts_with("enum ") || code.starts_with("enum{")) {
            if (auto read = read_enum(trimmed(code.substr(4)), context); read) {
                functions_taken.insert(read->name);
                script.enums.push_back(std::move(*read));
            }
        }
    }
    if (!script.class_name.empty()) {
        script.component = cpp_identifier(script.class_name);
    }
    functions_taken.insert(script.component);
    script.node = !(script.extends.empty() || script.extends == "RefCounted" ||
                    script.extends == "Object" || is_resource_type(script.extends));

    // Variables, signals, functions, and what the skeleton leaves out.
    std::set<std::string> fields_taken;
    Annotations carried;
    std::string carried_doc;
    for (const Declaration& declaration : declarations) {
        const Annotated annotated = annotations_of(declaration.code);
        if (annotated.rest.empty()) {
            for (const auto& annotation : annotated.annotations) {
                if (carries(annotation.first)) {
                    carried.push_back(annotation);
                    carried_doc = declaration.doc;
                }
            }
            continue;
        }
        Annotations annotations = std::move(carried);
        carried.clear();
        annotations.insert(annotations.end(), annotated.annotations.begin(),
                           annotated.annotations.end());
        const std::string& doc = declaration.doc.empty() ? carried_doc : declaration.doc;
        carried_doc.clear();
        std::string_view code = annotated.rest;
        const bool is_static = code.starts_with("static ");
        if (is_static) {
            code = trimmed(code.substr(7));
        }
        const bool onready = std::ranges::any_of(
            annotations, [](const auto& annotation) { return annotation.first == "onready"; });
        if (code.starts_with("var ")) {
            const Declared variable = declared(code.substr(4));
            if (is_static) {
                script.notes.push_back("static var " + variable.name +
                                       ": keep what every entity shares in one entity's "
                                       "component.");
            } else if (onready) {
                script.notes.push_back("@onready var " + variable.name +
                                       ": find the entity when you need it, with world.find.");
            } else {
                ScriptField field = read_variable(variable, annotations, context);
                field.name = unique_name(member_name(variable.name), fields_taken);
                field.doc = doc;
                if (variable.accessors) {
                    script.notes.push_back(variable.name +
                                           " has a setter or getter: run its code where the "
                                           "field changes.");
                }
                script.fields.push_back(std::move(field));
            }
        } else if (code.starts_with("signal ")) {
            ScriptSignal signal;
            signal.declaration = std::string(code);
            const std::string_view rest = trimmed(code.substr(7));
            signal.name = std::string(leading_identifier(rest));
            if (const std::string_view after = trimmed(rest.substr(signal.name.size()));
                after.starts_with('(')) {
                if (const auto inside = bracketed(after, 0); inside) {
                    signal.params = parameters(*inside, context);
                }
            }
            script.signals.push_back(std::move(signal));
        } else if (code.starts_with("func ")) {
            ScriptFunction function = read_function(code.substr(5), declaration, lines, context);
            function.is_static = is_static;
            function.name = unique_name(member_name(function.source_name), functions_taken);
            script.functions.push_back(std::move(function));
        } else if (code.starts_with("class ")) {
            script.notes.push_back("The inner class " +
                                   std::string(leading_identifier(trimmed(code.substr(6)))) +
                                   ": port it as a struct of its own.");
        }
    }
    return script;
}

std::string_view engine_name(ScriptSource source) {
    return source == ScriptSource::UnityCsharp ? "Unity" : "Godot";
}

namespace {

// --- Reading C# ---------------------------------------------------------------------------------

constexpr std::size_t kNone = std::string_view::npos;

bool is_blank(char c) {
    return is_space(c) || c == '\n';
}

// C# text with its comments, preprocessor lines and the insides of its string and character
// literals made spaces, so the brackets and semicolons in them don't count. As long as text, with
// newlines where it has them.
std::string csharp_code(std::string_view text) {
    std::string out(text);
    const auto blank = [&](std::size_t from, std::size_t to) {
        for (std::size_t j = from; j < to && j < out.size(); ++j) {
            if (out[j] != '\n') {
                out[j] = ' ';
            }
        }
    };
    const auto line_end = [&](std::size_t from) {
        return std::min(text.find('\n', from), text.size());
    };
    bool line_start = true;
    std::size_t i = 0;
    while (i < text.size()) {
        const char c = text[i];
        if (c == '\n') {
            line_start = true;
            ++i;
            continue;
        }
        if (is_space(c)) {
            ++i;
            continue;
        }
        if (line_start && c == '#') {
            const std::size_t end = line_end(i);
            blank(i, end);
            i = end;
            continue;
        }
        line_start = false;
        const std::string_view rest = text.substr(i);
        if (rest.starts_with("//")) {
            const std::size_t end = line_end(i);
            blank(i, end);
            i = end;
            continue;
        }
        if (rest.starts_with("/*")) {
            const std::size_t close = text.find("*/", i + 2);
            const std::size_t end = close == kNone ? text.size() : close + 2;
            blank(i, end);
            i = end;
            continue;
        }
        if (c == '\'') {
            std::size_t j = i + 1;
            while (j < text.size() && text[j] != '\'' && text[j] != '\n') {
                j += text[j] == '\\' ? 2U : 1U;
            }
            j = std::min(j, text.size());
            blank(i + 1, j);
            i = j + 1;
            continue;
        }
        // "...", @"...", $"...", $@"..." and """...""".
        std::size_t quote = i;
        bool verbatim = false;
        while (quote < text.size() && (text[quote] == '@' || text[quote] == '$')) {
            verbatim = verbatim || text[quote] == '@';
            ++quote;
        }
        if (quote >= text.size() || text[quote] != '"') {
            i = std::max(quote, i + 1);
            continue;
        }
        std::size_t run = 0;
        while (quote + run < text.size() && text[quote + run] == '"') {
            ++run;
        }
        if (run >= 3) { // a raw string, closed by as many quotes
            const std::size_t close = text.find(std::string(run, '"'), quote + run);
            const std::size_t end = close == kNone ? text.size() : close;
            blank(quote + run, end);
            i = std::min(end + run, text.size());
            continue;
        }
        std::size_t j = quote + 1;
        while (j < text.size()) {
            const bool doubled =
                verbatim && text[j] == '"' && j + 1 < text.size() && text[j + 1] == '"';
            if (doubled || (!verbatim && text[j] == '\\')) { // "" in @"...", or an escape
                j += 2;
            } else if (text[j] == '"' || (!verbatim && text[j] == '\n')) {
                break;
            } else {
                ++j;
            }
        }
        j = std::min(j, text.size());
        blank(quote + 1, j);
        i = j + 1;
    }
    return out;
}

// Where the bracket opening at code[open] closes, counting only its own kind, or kNone.
std::size_t closing(std::string_view code, std::size_t open) {
    const char opener = code[open];
    char closer = '>';
    if (opener == '{') {
        closer = '}';
    } else if (opener == '(') {
        closer = ')';
    } else if (opener == '[') {
        closer = ']';
    }
    int depth = 0;
    for (std::size_t i = open; i < code.size(); ++i) {
        if (code[i] == opener) {
            ++depth;
        } else if (code[i] == closer && --depth == 0) {
            return i;
        }
    }
    return kNone;
}

// Whether word is at code[at], not part of a longer name.
bool word_at(std::string_view code, std::size_t at, std::string_view word) {
    return code.substr(at).starts_with(word) && (at == 0 || !is_identifier_char(code[at - 1])) &&
           (at + word.size() == code.size() || !is_identifier_char(code[at + word.size()]));
}

// text without the spaces and newlines around it.
std::string_view stripped(std::string_view text) {
    while (!text.empty() && is_blank(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && is_blank(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

std::size_t skip_blanks(std::string_view code, std::size_t at, std::size_t end = kNone) {
    end = std::min(end, code.size());
    while (at < end && is_blank(code[at])) {
        ++at;
    }
    return at;
}

// text on one line: each run of spaces and newlines outside strings one space.
std::string one_line(std::string_view text) {
    std::string out;
    bool space = false;
    char quote = '\0';
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (quote != '\0') {
            out += c;
            if (c == '\\' && i + 1 < text.size()) {
                out += text[++i];
            } else if (c == quote) {
                quote = '\0';
            }
            continue;
        }
        if (is_blank(c)) {
            space = !out.empty();
            continue;
        }
        if (space) {
            out += ' ';
            space = false;
        }
        if (c == '"' || c == '\'') {
            quote = c;
        }
        out += c;
    }
    return out;
}

// Where code has an assignment's '=' outside brackets, or kNone: not ==, =>, !=, <= or >=.
std::size_t assignment_at(std::string_view code) {
    int depth = 0;
    for (std::size_t i = 0; i < code.size(); ++i) {
        const char c = code[i];
        if (c == '(' || c == '[' || c == '{') {
            ++depth;
        } else if (c == ')' || c == ']' || c == '}') {
            --depth;
        } else if (depth == 0 && c == '=') {
            const char before = i > 0 ? code[i - 1] : ' ';
            const char after = i + 1 < code.size() ? code[i + 1] : ' ';
            if (after == '=' || after == '>') {
                ++i;
            } else if (before != '=' && before != '!' && before != '<' && before != '>') {
                return i;
            }
        }
    }
    return kNone;
}

// Where code has "=>" outside brackets, or kNone.
std::size_t arrow_at(std::string_view code) {
    int depth = 0;
    for (std::size_t i = 0; i + 1 < code.size(); ++i) {
        const char c = code[i];
        if (c == '(' || c == '[' || c == '{') {
            ++depth;
        } else if (c == ')' || c == ']' || c == '}') {
            --depth;
        } else if (depth == 0 && c == '=' && code[i + 1] == '>') {
            return i;
        }
    }
    return kNone;
}

// code split at each separator outside brackets, generic types' angle brackets included where
// no '=' comes before them in the piece.
std::vector<std::string_view> split_declarators(std::string_view code, char separator) {
    std::vector<std::string_view> parts;
    int depth = 0;
    int angles = 0;
    bool value = false;
    std::size_t start = 0;
    for (std::size_t i = 0; i < code.size(); ++i) {
        const char c = code[i];
        if (c == '(' || c == '[' || c == '{') {
            ++depth;
        } else if (c == ')' || c == ']' || c == '}') {
            --depth;
        } else if (c == '<' && !value) {
            ++angles;
        } else if (c == '>' && !value && angles > 0) {
            --angles;
        } else if (c == '=' && depth == 0) {
            value = true;
        } else if (c == separator && depth == 0 && angles == 0) {
            parts.push_back(stripped(code.substr(start, i - start)));
            start = i + 1;
            value = false;
        }
    }
    if (const std::string_view last = stripped(code.substr(start)); !last.empty()) {
        parts.push_back(last);
    }
    return parts;
}

// A C# member's name in C++: snake_case, without the m_ and _ that mark private fields or the @
// that lets a keyword be a name. "moveSpeed" -> "move_speed", "_Ready" -> "ready".
std::string csharp_member_name(std::string_view name) {
    if (name.starts_with('@')) {
        name.remove_prefix(1);
    }
    if (name.size() > 2 && (name.starts_with("m_") || name.starts_with("s_"))) {
        name.remove_prefix(2);
    }
    while (name.size() > 1 && name.front() == '_') {
        name.remove_prefix(1);
    }
    return cpp_identifier(snake_case(name));
}

// A C# type's name without namespaces, global:: or nullable marks:
// "System.Collections.Generic.List<UnityEngine.GameObject>?" -> "List<GameObject>".
std::string plain_type(std::string_view type) {
    std::string out;
    std::size_t segment = 0;
    type = stripped(type);
    if (type.starts_with("global::")) {
        type.remove_prefix(8);
    }
    for (const char c : type) {
        if (c == '.') {
            out.resize(segment);
        } else if (c == '?' || is_blank(c)) {
            continue;
        } else {
            out += c;
            if (!is_identifier_char(c)) {
                segment = out.size();
            }
        }
    }
    return out;
}

// The arguments of a generic type: "Dictionary<string,int>" -> {"string", "int"}.
std::vector<std::string_view> generic_args(std::string_view type) {
    const std::size_t open = type.find('<');
    if (open == kNone || !type.ends_with('>')) {
        return {};
    }
    return split_declarators(type.substr(open + 1, type.size() - open - 2), ',');
}

// An attribute of a C# declaration: [SerializeField], [Range(0, 10)], [field: SerializeField].
struct Attribute {
    std::string name;              // without its namespace and the Attribute suffix
    std::vector<std::string> args; // as written
    std::string target;            // "field" in [field: SerializeField], or ""
};

std::vector<Attribute> attributes_of(std::string_view inside) {
    std::vector<Attribute> out;
    std::string_view target;
    if (const std::size_t colon = inside.find(':');
        colon != kNone && colon < inside.find('(') &&
        std::ranges::all_of(stripped(inside.substr(0, colon)), is_identifier_char)) {
        target = stripped(inside.substr(0, colon));
        inside = inside.substr(colon + 1);
    }
    for (const std::string_view item : split_top(inside, ',')) {
        Attribute attribute;
        attribute.target = std::string(target);
        const std::size_t paren = item.find('(');
        attribute.name = plain_type(item.substr(0, paren));
        if (attribute.name.ends_with("Attribute") && attribute.name.size() > 9) {
            attribute.name.resize(attribute.name.size() - 9);
        }
        if (paren != kNone) {
            if (const auto args = bracketed(item, paren); args) {
                for (const std::string_view arg : split_top(*args, ',')) {
                    attribute.args.emplace_back(arg);
                }
            }
        }
        out.push_back(std::move(attribute));
    }
    return out;
}

// The text of the /// comments in text, without their XML tags, on one line.
std::string xml_doc(std::string_view text) {
    std::string out;
    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t end = std::min(text.find('\n', start), text.size());
        std::string_view line = stripped(text.substr(start, end - start));
        start = end + 1;
        if (!line.starts_with("///")) {
            continue;
        }
        line.remove_prefix(3);
        bool tag = false;
        out += ' ';
        for (const char c : line) {
            if (c == '<') {
                tag = true;
            } else if (c == '>') {
                tag = false;
                out += ' ';
            } else if (!tag) {
                out += c;
            }
        }
    }
    std::string doc = one_line(out);
    for (const auto& [entity, character] :
         {std::pair{"&lt;", "<"}, std::pair{"&gt;", ">"}, std::pair{"&amp;", "&"}}) {
        for (std::size_t at = doc.find(entity); at != std::string::npos;
             at = doc.find(entity, at + 1)) {
            doc.replace(at, std::string_view(entity).size(), character);
        }
    }
    return doc;
}

// A number literal of C#, without its type suffix: "2.5f" -> "2.5".
std::string without_suffix(std::string_view text) {
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        out += c;
        const bool starts = is_digit_char(c) && (i == 0 || !is_identifier_char(text[i - 1]));
        if (!starts) {
            continue;
        }
        const bool hex = text.substr(i).starts_with("0x") || text.substr(i).starts_with("0X");
        std::size_t j = i + 1;
        while (j < text.size() &&
               (is_digit_char(text[j]) || text[j] == '.' || text[j] == '_' ||
                (hex && is_identifier_char(text[j]) && text[j] != 'u' && text[j] != 'U' &&
                 text[j] != 'l' && text[j] != 'L') ||
                (!hex && (text[j] == 'e' || text[j] == 'E') && j + 1 < text.size() &&
                 (is_digit_char(text[j + 1]) || text[j + 1] == '-' || text[j + 1] == '+')) ||
                (!hex && (text[j] == '-' || text[j] == '+') &&
                 (text[j - 1] == 'e' || text[j - 1] == 'E')) ||
                (hex && j == i + 1))) {
            out += text[j];
            ++j;
        }
        while (j < text.size() && std::string_view("fFdDmMlLuU").find(text[j]) != kNone) {
            ++j;
        }
        i = j - 1;
    }
    return out;
}

// Unity's named colors, as Color's properties define them.
std::optional<std::array<float, 4>> unity_color(std::string_view name) {
    static const std::map<std::string_view, std::array<float, 4>> kColors{
        {"black", {0.0F, 0.0F, 0.0F, 1.0F}}, {"white", {1.0F, 1.0F, 1.0F, 1.0F}},
        {"red", {1.0F, 0.0F, 0.0F, 1.0F}},   {"green", {0.0F, 1.0F, 0.0F, 1.0F}},
        {"blue", {0.0F, 0.0F, 1.0F, 1.0F}},  {"yellow", {1.0F, 0.921569F, 0.0156863F, 1.0F}},
        {"cyan", {0.0F, 1.0F, 1.0F, 1.0F}},  {"magenta", {1.0F, 0.0F, 1.0F, 1.0F}},
        {"gray", {0.5F, 0.5F, 0.5F, 1.0F}},  {"grey", {0.5F, 0.5F, 0.5F, 1.0F}},
        {"clear", {0.0F, 0.0F, 0.0F, 0.0F}}};
    const auto found = kColors.find(name);
    return found != kColors.end() ? std::optional(found->second) : std::nullopt;
}

// "DarkGreen" -> "DARK_GREEN", as Godot's C# Colors name GDScript's Color constants.
std::string upper_snake(std::string_view name) {
    std::string out = snake_case(name);
    for (char& c : out) {
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
    }
    return out;
}

// A C# value as GDScript writes it, for translate(): "new Vector2(1f, 2f)" -> "Vector2(1, 2)",
// "Vector2.zero" -> "Vector2.ZERO", "new int[] { 1, 2 }" -> "[1, 2]". type is the C# type of
// what it initializes, for new() and new(1, 2).
std::string gdscript_value(std::string_view text, std::string_view type, ScriptSource source) {
    std::string value = one_line(stripped(text));
    if (value.empty()) {
        return value;
    }
    if (value == "string.Empty" || value == "String.Empty") {
        return "\"\"";
    }
    if (value == "default" || value.starts_with("default(")) {
        return "null";
    }
    // A verbatim string, with its doubled quotes.
    if (value.starts_with("@\"") && value.ends_with('"') && value.size() >= 3) {
        std::string out = "\"";
        for (std::size_t i = 2; i + 1 < value.size(); ++i) {
            if (value[i] == '"' && value[i + 1] == '"') {
                out += "\\\"";
                ++i;
            } else if (value[i] == '\\') {
                out += "\\\\";
            } else {
                out += value[i];
            }
        }
        return out + "\"";
    }
    if (value.starts_with('"') || value.starts_with('\'')) {
        return value;
    }
    value = without_suffix(value);
    // Collections: { 1, 2 }, new int[] { 1, 2 }, new List<int> { 1, 2 }, new List<int>().
    if (value.starts_with("new ") || value.starts_with("new(") || value.starts_with('{')) {
        std::string_view rest = value;
        if (rest.starts_with("new")) {
            rest = stripped(rest.substr(3));
        }
        // The first of each follows the type created, whatever brackets come after.
        const std::size_t brace = rest.find('{');
        const std::size_t paren = rest.find('(');
        std::string created = std::string(stripped(rest.substr(0, std::min(brace, paren))));
        if (created.empty()) {
            created = plain_type(type);
        }
        created = plain_type(created);
        const bool list = created.empty() || created.ends_with(']') ||
                          created.starts_with("List<") || created.starts_with("Array<") ||
                          created.starts_with("HashSet<") || created.starts_with("Queue<") ||
                          created.starts_with("Stack<") || created == "Array";
        const bool map = created.starts_with("Dictionary<") || created == "Dictionary";
        if (brace != kNone && (list || map)) {
            const auto inside = bracketed(rest, brace);
            if (!inside || map) {
                return map && inside && stripped(*inside).empty() ? "{}" : std::string();
            }
            std::vector<std::string> items;
            for (const std::string_view item : split_top(*inside, ',')) {
                if (!item.empty()) {
                    items.push_back(gdscript_value(item,
                                                   generic_args(created).empty()
                                                       ? std::string_view()
                                                       : generic_args(created).front(),
                                                   source));
                }
            }
            return "[" + join(items, ", ") + "]";
        }
        if (list || map) {
            return map ? "{}" : "[]";
        }
        if (paren == kNone) {
            return {};
        }
        rest = rest.substr(paren);
        if (created == "Color32") {
            const auto args = split_top(rest.substr(1, rest.size() - 2), ',');
            std::vector<std::string> parts;
            for (const std::string_view arg : args) {
                const auto n = number(arg);
                if (!n) {
                    return {};
                }
                parts.push_back(float_text(static_cast<float>(std::stod(n->value) / 255.0)));
                parts.back().pop_back(); // the F
            }
            return "Color(" + join(parts, ", ") + ")";
        }
        if (created == "Vector2Int") {
            created = "Vector2i";
        }
        if (created == "Vector2I") {
            created = "Vector2i";
        }
        if (rest == "()" && (created.starts_with("Vector") || created == "Color")) {
            if (created == "Color" || created.ends_with('4')) {
                return created + "(0, 0, 0, 0)";
            }
            return created + (created.ends_with('3') ? "(0, 0, 0)" : "(0, 0)");
        }
        return created + std::string(rest);
    }
    // Static members: Vector2.zero, Color.red, Colors.Red, Mathf.PI.
    const std::size_t dot = value.find('.');
    if (dot != kNone && dot + 1 < value.size() &&
        std::ranges::all_of(std::string_view(value).substr(dot + 1), is_identifier_char)) {
        std::string owner = value.substr(0, dot);
        const std::string member = value.substr(dot + 1);
        if (owner == "Mathf" && (member == "PI" || member == "Pi")) {
            return "PI";
        }
        if (owner == "Vector2Int" || owner == "Vector2I") {
            owner = "Vector2i";
        }
        if (source == ScriptSource::UnityCsharp) {
            if (owner == "Color") {
                const auto named = unity_color(member);
                if (!named) {
                    return value;
                }
                std::vector<std::string> parts;
                for (const float channel : *named) {
                    std::string part = float_text(channel);
                    part.pop_back();
                    parts.push_back(part);
                }
                return "Color(" + join(parts, ", ") + ")";
            }
            if (owner.starts_with("Vector")) {
                // Unity's directions as Unity has them, +Y up.
                static const std::map<std::string_view, std::array<int, 3>> kAxes{
                    {"zero", {0, 0, 0}},    {"one", {1, 1, 1}},   {"up", {0, 1, 0}},
                    {"down", {0, -1, 0}},   {"left", {-1, 0, 0}}, {"right", {1, 0, 0}},
                    {"forward", {0, 0, 1}}, {"back", {0, 0, -1}}};
                const auto axes = kAxes.find(member);
                if (axes == kAxes.end()) {
                    return value;
                }
                const std::size_t size = owner.starts_with("Vector3") ? 3 : 2;
                if (size == 2 && axes->second[2] != 0) {
                    return value;
                }
                std::vector<std::string> parts;
                for (std::size_t i = 0; i < size; ++i) {
                    parts.push_back(std::to_string(axes->second.at(i)));
                }
                return owner + "(" + join(parts, ", ") + ")";
            }
            return value;
        }
        if (owner == "Colors") {
            return "Color." + upper_snake(member);
        }
        if (owner.starts_with("Vector")) {
            return owner + "." + upper_snake(member);
        }
        if (owner == "Color" && member.starts_with("FromHtml")) {
            return value;
        }
    }
    if (value.starts_with("Color.FromHtml(")) {
        return "Color.html(" + value.substr(15);
    }
    return value;
}

// Unity's types of assets, which a field holds the path of.
bool is_unity_asset(std::string_view type) {
    static constexpr auto kNames = std::to_array<std::string_view>({"Sprite",
                                                                    "Texture",
                                                                    "Texture2D",
                                                                    "RenderTexture",
                                                                    "Cubemap",
                                                                    "AudioClip",
                                                                    "AudioMixer",
                                                                    "AudioMixerGroup",
                                                                    "AudioMixerSnapshot",
                                                                    "Material",
                                                                    "PhysicsMaterial2D",
                                                                    "PhysicMaterial",
                                                                    "AnimationClip",
                                                                    "Avatar",
                                                                    "RuntimeAnimatorController",
                                                                    "Font",
                                                                    "TMP_FontAsset",
                                                                    "AnimatorOverrideController",
                                                                    "TextAsset",
                                                                    "Shader",
                                                                    "ComputeShader",
                                                                    "Mesh",
                                                                    "TileBase",
                                                                    "Tile",
                                                                    "RuleTile",
                                                                    "AnimatedTile",
                                                                    "ScriptableObject",
                                                                    "VideoClip",
                                                                    "SpriteAtlas",
                                                                    "TMP_SpriteAsset",
                                                                    "InputActionAsset",
                                                                    "InputActionReference",
                                                                    "TimelineAsset"});
    return contains(kNames, type);
}

// Unity's components and objects, which a field holds the entity of.
bool is_unity_object(std::string_view type) {
    static constexpr auto kNames = std::to_array<std::string_view>({"GameObject",
                                                                    "Transform",
                                                                    "RectTransform",
                                                                    "MonoBehaviour",
                                                                    "Behaviour",
                                                                    "Component",
                                                                    "Rigidbody2D",
                                                                    "Collider2D",
                                                                    "BoxCollider2D",
                                                                    "CircleCollider2D",
                                                                    "CapsuleCollider2D",
                                                                    "PolygonCollider2D",
                                                                    "EdgeCollider2D",
                                                                    "CompositeCollider2D",
                                                                    "SpriteRenderer",
                                                                    "Renderer",
                                                                    "Animator",
                                                                    "AudioSource",
                                                                    "AudioListener",
                                                                    "Camera",
                                                                    "Canvas",
                                                                    "CanvasGroup",
                                                                    "Text",
                                                                    "Image",
                                                                    "RawImage",
                                                                    "Button",
                                                                    "Toggle",
                                                                    "Slider",
                                                                    "Scrollbar",
                                                                    "Dropdown",
                                                                    "InputField",
                                                                    "ScrollRect",
                                                                    "TMP_Text",
                                                                    "TextMeshPro",
                                                                    "TextMeshProUGUI",
                                                                    "TMP_InputField",
                                                                    "TMP_Dropdown",
                                                                    "ParticleSystem",
                                                                    "Light2D",
                                                                    "Tilemap",
                                                                    "TilemapRenderer",
                                                                    "Grid",
                                                                    "LineRenderer",
                                                                    "TrailRenderer",
                                                                    "Collision2D",
                                                                    "Joint2D",
                                                                    "HingeJoint2D",
                                                                    "SpringJoint2D"});
    return contains(kNames, type);
}

// Unity's enums that games' fields use most, which scenes save as numbers.
bool is_unity_enum(std::string_view type) {
    static constexpr auto kNames = std::to_array<std::string_view>(
        {"KeyCode", "ForceMode2D", "RigidbodyType2D", "Space", "FilterMode", "TextAnchor",
         "FontStyle", "SpriteMaskInteraction", "CollisionDetectionMode2D",
         "RigidbodyInterpolation2D", "AnimatorUpdateMode", "AudioRolloffMode",
         "TextAlignmentOptions"});
    return contains(kNames, type);
}

class CsharpReader {
public:
    CsharpReader(std::string_view path, std::string_view text, ScriptSource source)
        : text_(text), code_text_(csharp_code(text)), source_(source) {
        script_.source = source;
        script_.path = std::string(path);
        std::string stem = snake_case(file_stem(path));
        std::ranges::replace(stem, '-', '_');
        script_.ns = cpp_identifier(stem);
        script_.system = script_.ns;
        script_.component = pascal_case(file_stem(path));
    }

    Script read() {
        const std::vector<Class> classes = classes_of();
        const Class* main = nullptr;
        for (const Class& candidate : classes) {
            if (candidate.name == file_stem(script_.path)) {
                main = &candidate;
                break;
            }
        }
        for (const Class& candidate : classes) {
            if (main == nullptr ||
                (candidate.depth < main->depth && main->name != file_stem(script_.path))) {
                main = &candidate;
            }
        }
        if (main == nullptr) {
            script_.node = false;
            script_.notes.emplace_back("The file declares no class.");
            return script_;
        }
        script_.class_name = main->name;
        script_.component = cpp_identifier(main->name);
        script_.doc = xml_doc(text_.substr(main->start, main->keyword - main->start));
        for (const std::string& base : main->bases) {
            const bool interface =
                base.size() > 1 && base[0] == 'I' && base[1] >= 'A' && base[1] <= 'Z';
            if (!interface) {
                script_.extends = base;
                break;
            }
        }
        const std::string& extends = script_.extends;
        if (source_ == ScriptSource::UnityCsharp) {
            script_.node = !extends.empty() && extends != "ScriptableObject" &&
                           extends != "Object" && extends != "object";
            if (!extends.empty() && extends != "MonoBehaviour" && extends != "ScriptableObject") {
                script_.notes.push_back("It extends " + extends +
                                        ", whose fields and methods aren't here.");
            }
        } else {
            script_.node =
                !(extends.empty() || extends == "RefCounted" || extends == "GodotObject" ||
                  extends == "Object" || is_resource_type(extends));
        }
        for (const Class& nested : classes) {
            if (nested.open > main->open && nested.close < main->close) {
                nested_.insert(nested.name);
            }
        }
        read_members(*main);
        return script_;
    }

private:
    struct Class {
        std::string name;
        std::vector<std::string> bases; // after ':', without namespaces
        std::size_t start = 0;          // where its attributes and modifiers start
        std::size_t keyword = 0;        // where "class" is
        std::size_t open = 0;           // its braces
        std::size_t close = 0;
        int depth = 0; // braces around it
    };

    // A member of a class: its attributes and declaration, and its braces when it has a body
    // in them.
    struct Member {
        std::size_t doc_from = 0; // where the comments before it start
        std::size_t start = 0;
        std::size_t end = 0; // past its ';' or '}'
        std::size_t open = kNone;
        std::size_t close = kNone;
    };

    [[nodiscard]] std::vector<Class> classes_of() const {
        std::vector<Class> out;
        int depth = 0;
        for (std::size_t i = 0; i < code_.size(); ++i) {
            const char c = code_[i];
            if (c == '{') {
                ++depth;
                continue;
            }
            if (c == '}') {
                --depth;
                continue;
            }
            const bool keyword = word_at(code_, i, "class") || word_at(code_, i, "struct") ||
                                 word_at(code_, i, "record") || word_at(code_, i, "interface");
            if (!keyword) {
                continue;
            }
            std::size_t after = i + 6; // struct or record
            if (code_[i] == 'i') {
                after = i + 9; // interface
            } else if (code_[i] == 'c') {
                after = i + 5; // class
            }
            std::size_t p = skip_blanks(code_, after);
            if (code_.substr(p).starts_with("class") || code_.substr(p).starts_with("struct")) {
                p = skip_blanks(code_, p + (code_[p] == 'c' ? 5 : 6)); // record class X
            }
            const std::string_view name = leading_identifier(code_.substr(p));
            const std::size_t open = code_.find_first_of("{;", p);
            if (name.empty() || open == kNone || code_[open] == ';') {
                continue;
            }
            Class found;
            found.name = std::string(name);
            found.keyword = i;
            found.open = open;
            found.close = closing(code_, open);
            if (found.close == kNone) {
                found.close = code_.size();
            }
            found.depth = depth;
            std::string_view header = code_.substr(p + name.size(), open - p - name.size());
            for (std::size_t w = 0; w < header.size(); ++w) {
                if (word_at(header, w, "where")) {
                    header = header.substr(0, w);
                    break;
                }
            }
            header = stripped(header);
            if (header.starts_with('<')) {
                const std::size_t close = closing(header, 0);
                header = close == kNone ? std::string_view() : stripped(header.substr(close + 1));
            }
            if (header.starts_with('(')) { // a primary constructor
                const std::size_t close = closing(header, 0);
                header = close == kNone ? std::string_view() : stripped(header.substr(close + 1));
            }
            if (header.starts_with(':')) {
                for (const std::string_view base : split_declarators(header.substr(1), ',')) {
                    std::string plain = plain_type(base);
                    if (const std::size_t angle = plain.find('<'); angle != std::string::npos) {
                        plain.resize(angle);
                    }
                    if (const std::size_t paren = plain.find('('); paren != std::string::npos) {
                        plain.resize(paren);
                    }
                    found.bases.push_back(std::move(plain));
                }
            }
            const std::size_t before = code_.find_last_of(";{}", i);
            found.start = before == kNone ? 0 : before + 1;
            out.push_back(std::move(found));
            i = p + name.size() - 1;
        }
        return out;
    }

    [[nodiscard]] std::vector<Member> members_of(std::size_t from, std::size_t to) const {
        std::vector<Member> out;
        std::size_t pos = from;
        while (true) {
            Member member;
            member.doc_from = pos;
            pos = skip_blanks(code_, pos, to);
            if (pos >= to) {
                break;
            }
            member.start = pos;
            int depth = 0;
            for (std::size_t i = pos; i < to; ++i) {
                const char c = code_[i];
                if (c == '(' || c == '[') {
                    ++depth;
                } else if (c == ')' || c == ']') {
                    --depth;
                } else if (depth == 0 && c == ';') {
                    member.end = i + 1;
                    break;
                } else if (depth == 0 && c == '{') {
                    const std::size_t close = closing(code_, i);
                    if (close == kNone || close >= to) {
                        break;
                    }
                    const std::string_view head = code_.substr(pos, i - pos);
                    if (assignment_at(head) != kNone || arrow_at(head) != kNone) {
                        i = close; // braces in a value
                        continue;
                    }
                    member.open = i;
                    member.close = close;
                    member.end = close + 1;
                    // A property's initializer: { get; set; } = 5;
                    if (const std::size_t next = skip_blanks(code_, close + 1, to);
                        next < to && code_[next] == '=' && code_[next + 1] != '>') {
                        const std::size_t semicolon = code_.find(';', next);
                        member.end = semicolon == kNone || semicolon >= to ? to : semicolon + 1;
                    }
                    break;
                }
            }
            if (member.end == 0) {
                member.end = to;
            }
            out.push_back(member);
            pos = member.end;
        }
        return out;
    }

    [[nodiscard]] int line_of(std::size_t offset) const {
        return 1 + static_cast<int>(std::count(
                       text_.begin(), text_.begin() + static_cast<std::ptrdiff_t>(offset), '\n'));
    }

    // The lines in a body's braces, one indent less than the least indented, tabs as four
    // spaces.
    [[nodiscard]] std::vector<std::string> body_of(std::size_t open, std::size_t close) const {
        std::vector<std::string> lines;
        const std::string_view inside = text_.substr(open + 1, close - open - 1);
        std::size_t start = 0;
        while (start <= inside.size()) {
            const std::size_t end = std::min(inside.find('\n', start), inside.size());
            std::string line = detab(inside.substr(start, end - start));
            while (!line.empty() && (line.back() == ' ' || line.back() == '\r')) {
                line.pop_back();
            }
            lines.push_back(std::move(line));
            start = end + 1;
        }
        while (!lines.empty() && lines.front().empty()) {
            lines.erase(lines.begin());
        }
        while (!lines.empty() && lines.back().empty()) {
            lines.pop_back();
        }
        std::size_t indent = kNone;
        for (const std::string& line : lines) {
            if (!line.empty()) {
                indent = std::min(indent, line.find_first_not_of(' '));
            }
        }
        for (std::string& line : lines) {
            line = line.size() > indent ? line.substr(indent) : std::string();
        }
        return lines;
    }

    // The parameters inside a method's parentheses.
    [[nodiscard]] std::vector<ScriptParam> parameters_of(std::string_view code,
                                                         std::string_view text) const {
        std::vector<ScriptParam> out;
        std::set<std::string> taken{"world", "entity", "self", "data", "tick_seconds"};
        for (const std::string_view piece : split_declarators(code, ',')) {
            // The same piece of the text as written, for its source.
            const auto at = static_cast<std::size_t>(piece.data() - code.data());
            std::string_view part = piece;
            std::string_view written = text.substr(at, piece.size());
            while (part.starts_with('[')) { // [CallerMemberName] string name = ""
                const std::size_t close = closing(part, 0);
                if (close == kNone) {
                    break;
                }
                const std::size_t next = skip_blanks(part, close + 1);
                part = part.substr(next);
                written = written.substr(next);
            }
            bool rest = false;
            for (bool modifier = true; modifier;) {
                modifier = false;
                for (const std::string_view word :
                     {"ref", "out", "in", "params", "this", "scoped", "readonly"}) {
                    if (word_at(part, 0, word)) {
                        rest = rest || word == "params";
                        part = stripped(part.substr(word.size()));
                        modifier = true;
                    }
                }
            }
            const std::size_t equals = assignment_at(part);
            const std::string_view declared = stripped(part.substr(0, equals));
            std::size_t name_start = declared.size();
            while (name_start > 0 && (is_identifier_char(declared[name_start - 1]) ||
                                      declared[name_start - 1] == '@')) {
                --name_start;
            }
            ScriptParam param;
            param.source = one_line(written);
            param.name = unique_name(csharp_member_name(declared.substr(name_start)), taken);
            const Mapped mapped = map_csharp(declared.substr(0, name_start));
            param.type = rest ? "const slash::json::Value&" : parameter_type(mapped.type);
            out.push_back(std::move(param));
        }
        return out;
    }

    // A C# type as C++.
    [[nodiscard]] Mapped map_csharp(std::string_view written) const {
        const std::string type = plain_type(written);
        if (type.empty() || type == "object" || type == "dynamic" || type == "var") {
            return {.type = "slash::json::Value", .comment = type};
        }
        if (type.ends_with("[]")) {
            const Mapped item = map_csharp(std::string_view(type).substr(0, type.size() - 2));
            return {.type = "std::vector<" + item.type + ">",
                    .comment = item.comment.empty() ? "" : type,
                    .kind =
                        item.kind == FieldKind::Entity ? FieldKind::Entities : FieldKind::Value};
        }
        if (type.find('[') != std::string::npos) {
            return {.type = "slash::json::Value", .comment = type}; // int[,] and the like
        }
        if (type == "int" || type == "uint" || type == "short" || type == "ushort" ||
            type == "byte" || type == "sbyte") {
            return {.type = "int"};
        }
        if (type == "char") {
            return {.type = "int", .comment = "char"};
        }
        if (type == "long" || type == "ulong") {
            return {.type = "std::int64_t"};
        }
        if (type == "float") {
            return {.type = "float"};
        }
        if (type == "double" || type == "decimal") {
            return {.type = "double"};
        }
        if (type == "bool") {
            return {.type = "bool"};
        }
        if (type == "string" || type == "String" || type == "StringName") {
            return {.type = "std::string"};
        }
        if (context_.enums.contains(type)) {
            return {.type = "int", .comment = type};
        }
        if (nested_.contains(type)) {
            return {.type = "slash::json::Value", .comment = type};
        }
        const std::string generic = type.substr(0, type.find('<'));
        const std::vector<std::string_view> args = generic_args(type);
        if (generic == "Nullable" && args.size() == 1) {
            return map_csharp(args[0]);
        }
        if ((generic == "List" || generic == "IList" || generic == "IReadOnlyList" ||
             generic == "IEnumerable" || generic == "Array" || generic == "ICollection") &&
            args.size() == 1) {
            const Mapped item = map_csharp(args[0]);
            return {.type = "std::vector<" + item.type + ">",
                    .comment = item.comment.empty() ? "" : type,
                    .kind =
                        item.kind == FieldKind::Entity ? FieldKind::Entities : FieldKind::Value};
        }
        if ((generic == "Dictionary" || generic == "IDictionary" ||
             generic == "IReadOnlyDictionary") &&
            args.size() == 2) {
            const Mapped key = map_csharp(args[0]);
            const Mapped item = map_csharp(args[1]);
            if (key.type == "std::string" && item.kind == FieldKind::Value) {
                return {.type = "std::map<std::string, " + item.type + ">",
                        .comment = item.comment.empty() ? "" : type};
            }
            return {.type = "slash::json::Value", .comment = type};
        }
        if (!args.empty() || generic == "Action" || generic == "Func" || type == "Delegate") {
            return {.type = "slash::json::Value", .comment = type};
        }
        if (source_ == ScriptSource::UnityCsharp) {
            return map_unity(type);
        }
        // Godot's types, as GDScript names them.
        std::string gdscript = type;
        for (const auto& [from, to] :
             {std::pair{"Vector2I", "Vector2i"}, std::pair{"Vector3I", "Vector3i"},
              std::pair{"Vector4I", "Vector4i"}, std::pair{"Rect2I", "Rect2i"},
              std::pair{"GodotObject", "Object"}}) {
            if (gdscript == from) {
                gdscript = to;
            }
        }
        return map_type(gdscript, context_);
    }

    static Mapped map_unity(const std::string& type) {
        static const std::map<std::string, Mapped, std::less<>> kTypes{
            {"Vector2", {.type = "slash::Vec2"}},
            {"Vector2Int", {.type = "slash::Vec2i"}},
            {"Vector3", {.type = "slash::Vec3"}},
            {"Vector4", {.type = "slash::Vec4"}},
            {"Color", {.type = "slash::Vec4", .comment = "Color"}},
            {"Color32", {.type = "slash::Vec4", .comment = "Color32"}},
            {"Quaternion", {.type = "slash::Quat"}},
            {"Rect", {.type = "slash::Vec4", .comment = "Rect: x, y, width, height"}},
            {"LayerMask", {.type = "int", .comment = "LayerMask"}}};
        if (const auto found = kTypes.find(type); found != kTypes.end()) {
            return found->second;
        }
        if (is_unity_enum(type)) {
            return {.type = "int", .comment = type};
        }
        if (is_unity_asset(type)) {
            return {.type = "std::string", .comment = type + ": its path"};
        }
        if (is_unity_object(type)) {
            return {.type = "entt::entity", .comment = type, .kind = FieldKind::Entity};
        }
        static constexpr auto kOther = std::to_array<std::string_view>(
            {"Vector3Int", "RectInt", "Bounds", "BoundsInt", "Matrix4x4", "AnimationCurve",
             "Gradient", "Hash128", "Object"});
        if (contains(kOther, type)) {
            return {.type = "slash::json::Value", .comment = type};
        }
        // One of the game's own MonoBehaviours, most likely: the entity it is on. The importer
        // makes it a path or a value when scenes set it to one.
        return {.type = "entt::entity", .comment = type, .kind = FieldKind::Entity};
    }

    // A value as written in C#, translated for a field of a C# type.
    [[nodiscard]] std::optional<Translated> translate_value(std::string_view text,
                                                            std::string_view type) const {
        const std::string value = gdscript_value(text, type, source_);
        if (value.empty()) {
            return std::nullopt;
        }
        return translate(value, context_);
    }

    void read_members(const Class& main) {
        const std::vector<Member> members = members_of(main.open + 1, main.close);
        functions_taken_.insert(script_.component);
        // Enums and constants first, which fields' values may name.
        for (const Member& member : members) {
            const Parsed parsed = parse(member);
            const std::string_view first = leading_identifier(parsed.code);
            if (first == "enum") {
                read_enum_member(member, parsed);
            } else if (parsed.modifiers.contains("const")) {
                read_constant_member(parsed);
            }
        }
        for (const Member& member : members) {
            const Parsed parsed = parse(member);
            if (parsed.code.empty()) {
                continue;
            }
            const std::string_view first = leading_identifier(parsed.code);
            if (first == "enum" || parsed.modifiers.contains("const")) {
                continue;
            }
            if (first == "class" || first == "struct" || first == "record" ||
                first == "interface") {
                const std::string_view name =
                    leading_identifier(stripped(parsed.code.substr(first.size())));
                script_.notes.push_back("The nested " + std::string(first) + " " +
                                        std::string(name) + ": port it as a struct of its own.");
                continue;
            }
            if (first == "delegate") {
                read_delegate(parsed);
                continue;
            }
            if (first == "event") {
                read_event(parsed, stripped(parsed.code.substr(5)));
                continue;
            }
            const std::size_t paren = parsed.code.find('(');
            const std::size_t equals = assignment_at(parsed.code);
            const std::size_t arrow = arrow_at(parsed.code);
            if (paren != kNone && (equals == kNone || paren < equals) &&
                (arrow == kNone || paren < arrow)) {
                read_method(member, parsed, paren);
            } else if (member.open != kNone || arrow != kNone) {
                read_property(member, parsed);
            } else {
                read_fields(parsed);
            }
        }
    }

    // A member's attributes, doc, modifiers and the declaration after them.
    struct Parsed {
        std::vector<Attribute> attributes;
        std::set<std::string> modifiers;
        std::string doc;
        std::string_view code;   // after the modifiers, comments as spaces, up to the body or ';'
        std::string_view text;   // the same, as written
        std::size_t offset = 0;  // where text starts in the file
        std::string declaration; // as written from the modifiers on, on one line
    };

    [[nodiscard]] Parsed parse(const Member& member) const {
        Parsed out;
        std::size_t p = member.start;
        std::size_t end = member.open;
        if (end == kNone) {
            end = code_[member.end - 1] == ';' ? member.end - 1 : member.end;
        }
        while (p < end && code_[p] == '[') {
            const std::size_t close = closing(code_, p);
            if (close == kNone || close >= end) {
                break;
            }
            for (Attribute& attribute : attributes_of(text_.substr(p + 1, close - p - 1))) {
                out.attributes.push_back(std::move(attribute));
            }
            p = skip_blanks(code_, close + 1, end);
        }
        out.doc = xml_doc(text_.substr(member.doc_from, member.start - member.doc_from));
        const std::size_t declaration = p;
        static constexpr auto kModifiers = std::to_array<std::string_view>(
            {"public", "private", "protected", "internal", "static", "readonly", "const",
             "override", "virtual", "abstract", "sealed", "new", "partial", "async", "extern",
             "unsafe", "volatile", "required", "file"});
        for (bool more = true; more;) {
            more = false;
            for (const std::string_view modifier : kModifiers) {
                if (p < end && word_at(code_, p, modifier)) {
                    out.modifiers.emplace(modifier);
                    p = skip_blanks(code_, p + modifier.size(), end);
                    more = true;
                }
            }
        }
        out.code = stripped(std::string_view(code_).substr(p, end - p));
        out.offset = p;
        out.text = std::string_view(text_).substr(p, out.code.size());
        out.declaration = one_line(text_.substr(declaration, end - declaration));
        return out;
    }

    [[nodiscard]] const Attribute* attribute(const Parsed& parsed, std::string_view name) const {
        const auto found = std::ranges::find_if(
            parsed.attributes, [&](const Attribute& attribute) { return attribute.name == name; });
        return found != parsed.attributes.end() ? &*found : nullptr;
    }

    void read_enum_member(const Member& member, const Parsed& parsed) {
        if (member.open == kNone) {
            return;
        }
        const std::string name(leading_identifier(stripped(parsed.code.substr(4))));
        std::string values;
        const std::string_view inside =
            std::string_view(code_).substr(member.open + 1, member.close - member.open - 1);
        for (std::size_t i = 0; i < inside.size(); ++i) {
            if (inside[i] == '[') { // [InspectorName("...")] on a value
                const std::size_t close = closing(inside, i);
                i = close == kNone ? inside.size() : close;
                continue;
            }
            values += inside[i];
        }
        if (auto read = read_enum(name + " {" + one_line(values) + "}", context_); read) {
            read->declaration = parsed.declaration + " { " + one_line(values) + " }";
            functions_taken_.insert(read->name);
            context_.enums.insert(name);
            script_.enums.push_back(std::move(*read));
        }
    }

    void read_constant_member(const Parsed& parsed) {
        for (const std::string_view piece : split_declarators(parsed.code, ',')) {
            const std::size_t equals = assignment_at(piece);
            if (equals == kNone) {
                continue;
            }
            const std::string_view left = stripped(piece.substr(0, equals));
            std::size_t name_start = left.size();
            while (name_start > 0 && is_identifier_char(left[name_start - 1])) {
                --name_start;
            }
            const std::string name(left.substr(name_start));
            const std::string_view type = stripped(left.substr(0, name_start));
            // The value as written: the same piece of the text.
            const auto at = static_cast<std::size_t>(piece.data() - parsed.code.data());
            const std::string_view value =
                stripped(parsed.text.substr(at + equals + 1, piece.size() - equals - 1));
            ScriptConstant constant;
            constant.name = constant_name(name);
            constant.declaration = parsed.declaration;
            const Mapped mapped = map_csharp(type);
            if (const auto translated = translate_value(value, type); translated) {
                if (auto init = initializer(*translated, mapped.type); init && !init->empty()) {
                    constant.type = mapped.type == "std::string" ? "std::string_view" : mapped.type;
                    constant.value =
                        init->starts_with(" = ") ? init->substr(3) : mapped.type + *init;
                }
            }
            context_.constants[name] = {
                constant.name, constant.type == "std::string_view" ? std::string() : constant.type};
            functions_taken_.insert(constant.name);
            script_.constants.push_back(std::move(constant));
        }
    }

    // Godot's [Signal] delegate void NameEventHandler(...), a signal named Name.
    void read_delegate(const Parsed& parsed) {
        const std::size_t paren = parsed.code.find('(');
        if (attribute(parsed, "Signal") == nullptr || paren == kNone) {
            const std::size_t name_end = paren == kNone ? parsed.code.size() : paren;
            std::size_t name_start = name_end;
            while (name_start > 0 && is_identifier_char(parsed.code[name_start - 1])) {
                --name_start;
            }
            script_.notes.push_back(
                "The delegate type " +
                std::string(parsed.code.substr(name_start, name_end - name_start)) +
                ": call the functions it names directly.");
            return;
        }
        std::size_t name_start = paren;
        while (name_start > 0 && is_identifier_char(parsed.code[name_start - 1])) {
            --name_start;
        }
        std::string name(parsed.code.substr(name_start, paren - name_start));
        if (name.ends_with("EventHandler") && name.size() > 12) {
            name.resize(name.size() - 12);
        }
        ScriptSignal signal;
        signal.name = name;
        signal.declaration = parsed.declaration;
        const std::size_t close = closing(parsed.code, paren);
        if (close != kNone) {
            signal.params = parameters_of(parsed.code.substr(paren + 1, close - paren - 1),
                                          parsed.text.substr(paren + 1, close - paren - 1));
        }
        functions_taken_.insert("emit_" + csharp_member_name(signal.name));
        script_.signals.push_back(std::move(signal));
    }

    // An event, or a UnityEvent field: a signal of its name, taking what its type's arguments
    // say.
    void read_event(const Parsed& parsed, std::string_view code) {
        for (const std::string_view piece : split_declarators(code, ',')) {
            std::size_t name_end = piece.size();
            if (const std::size_t equals = assignment_at(piece); equals != kNone) {
                name_end = equals;
            }
            const std::string_view left = stripped(piece.substr(0, name_end));
            std::size_t name_start = left.size();
            while (name_start > 0 && is_identifier_char(left[name_start - 1])) {
                --name_start;
            }
            const std::string name(left.substr(name_start));
            const std::string type = plain_type(left.substr(0, name_start));
            if (name.empty()) {
                continue;
            }
            ScriptSignal signal;
            signal.name = csharp_member_name(name);
            signal.declaration = parsed.declaration;
            std::set<std::string> taken{"world", "entity", "data"};
            for (const std::string_view arg : generic_args(type)) {
                const Mapped mapped = map_csharp(arg);
                ScriptParam param;
                param.name = unique_name("value", taken);
                param.type = parameter_type(mapped.type);
                param.source = std::string(arg);
                signal.params.push_back(std::move(param));
            }
            functions_taken_.insert("emit_" + csharp_member_name(signal.name));
            script_.signals.push_back(std::move(signal));
        }
    }

    void read_method(const Member& member, const Parsed& parsed, std::size_t paren) {
        // The name before the parentheses, after any generic parameters: T Find<T>(...).
        std::size_t name_end = paren;
        while (name_end > 0 && is_blank(parsed.code[name_end - 1])) {
            --name_end;
        }
        if (name_end > 0 && parsed.code[name_end - 1] == '>') {
            int depth = 0;
            while (name_end > 0) {
                const char c = parsed.code[--name_end];
                if (c == '>') {
                    ++depth;
                } else if (c == '<') {
                    --depth;
                }
                if (depth == 0) {
                    break;
                }
            }
        }
        std::size_t name_start = name_end;
        while (name_start > 0 && is_identifier_char(parsed.code[name_start - 1])) {
            --name_start;
        }
        const std::string source_name(parsed.code.substr(name_start, name_end - name_start));
        const std::string_view returns = stripped(parsed.code.substr(0, name_start));
        if (source_name.empty() || returns.ends_with("operator") || source_name == "operator") {
            script_.notes.push_back("The operator " + one_line(parsed.text.substr(0, paren)) +
                                    ": port it as a function.");
            return;
        }
        if (returns.empty() && source_name != script_.class_name) {
            return;
        }
        ScriptFunction function;
        function.source_name = source_name;
        function.line = line_of(parsed.offset + name_start);
        function.is_static = parsed.modifiers.contains("static");
        function.declaration = parsed.declaration;
        const std::size_t close = closing(parsed.code, paren);
        if (close != kNone) {
            function.params = parameters_of(parsed.code.substr(paren + 1, close - paren - 1),
                                            parsed.text.substr(paren + 1, close - paren - 1));
        }
        const std::string plain = plain_type(returns);
        if (plain.starts_with("IEnumerator")) {
            script_.notes.push_back(source_name +
                                    " is a coroutine: keep where it is in the component and go on "
                                    "from there each tick.");
        } else if (parsed.modifiers.contains("async") || plain.starts_with("Task") ||
                   plain.starts_with("ValueTask")) {
            script_.notes.push_back(source_name +
                                    " is async: keep where it is in the component and go on from "
                                    "there when what it awaits happens.");
        } else if (plain != "void" && !returns.empty()) {
            function.returns = map_csharp(returns).type;
        }
        if (member.open != kNone) {
            function.body = body_of(member.open, member.close);
        } else if (const std::size_t arrow = arrow_at(parsed.code); arrow != kNone) {
            const std::string expression = one_line(parsed.text.substr(arrow + 2));
            function.body.push_back((function.returns == "void" ? "" : "return ") + expression +
                                    ";");
        }
        // Unity's per-frame messages read the frame's time from Time.deltaTime.
        if (source_ == ScriptSource::UnityCsharp && function.params.empty() &&
            (source_name == "Update" || source_name == "LateUpdate" ||
             source_name == "FixedUpdate")) {
            function.params.push_back({.name = "delta",
                                       .type = "double",
                                       .source = source_name == "FixedUpdate"
                                                     ? "Time.fixedDeltaTime"
                                                     : "Time.deltaTime"});
        }
        function.name = unique_name(returns.empty() ? std::string("construct")
                                                    : csharp_member_name(source_name),
                                    functions_taken_);
        script_.functions.push_back(std::move(function));
    }

    // A property: a field when it is automatic, a function when it only gets.
    void read_property(const Member& member, const Parsed& parsed) {
        const std::size_t arrow = arrow_at(parsed.code);
        const std::string_view head = stripped(parsed.code.substr(0, arrow));
        std::size_t name_start = head.size();
        while (name_start > 0 && is_identifier_char(head[name_start - 1])) {
            --name_start;
        }
        const std::string name(head.substr(name_start));
        const std::string_view type = stripped(head.substr(0, name_start));
        if (name.empty() || name == "this" || type.ends_with("this")) {
            script_.notes.push_back("The indexer " + parsed.declaration +
                                    ": port it as a function.");
            return;
        }
        std::string accessors;
        if (member.open != kNone) {
            accessors = one_line(
                std::string_view(code_).substr(member.open + 1, member.close - member.open - 1));
        }
        const bool automatic = arrow == kNone && accessors.find('{') == std::string::npos &&
                               accessors.find("=>") == std::string::npos;
        if (automatic) {
            std::string value;
            if (member.end > member.close + 1) { // { get; set; } = value;
                const std::string_view after =
                    std::string_view(text_).substr(member.close + 1, member.end - member.close - 2);
                if (const std::size_t equals = after.find('='); equals != kNone) {
                    value = std::string(stripped(after.substr(equals + 1)));
                }
            }
            add_field(parsed, std::string(type), name, value, true);
            return;
        }
        const bool sets = accessors.find("set") != std::string::npos;
        ScriptFunction function;
        function.source_name = name;
        function.line = line_of(parsed.offset + name_start);
        function.declaration = parsed.declaration;
        function.returns = map_csharp(type).type;
        if (arrow != kNone) {
            function.body.push_back("return " + one_line(parsed.text.substr(arrow + 2)) + ";");
        } else {
            // The getter's body: get { ... } or get => ...;
            const std::string_view inside =
                std::string_view(code_).substr(member.open + 1, member.close - member.open - 1);
            for (std::size_t i = 0; i < inside.size(); ++i) {
                if (!word_at(inside, i, "get")) {
                    continue;
                }
                const std::size_t at = skip_blanks(inside, i + 3);
                const std::size_t offset = member.open + 1 + at;
                if (at < inside.size() && inside[at] == '{') {
                    const std::size_t close = closing(code_, offset);
                    if (close != kNone) {
                        function.body = body_of(offset, close);
                    }
                } else if (inside.substr(at).starts_with("=>")) {
                    const std::size_t semicolon = inside.find(';', at);
                    function.body.push_back(
                        "return " +
                        one_line(text_.substr(offset + 2,
                                              (semicolon == kNone ? inside.size() : semicolon) -
                                                  at - 2)) +
                        ";");
                }
                break;
            }
        }
        if (sets) {
            script_.notes.push_back("The property " + name +
                                    " has a setter: run its code where the value changes.");
        }
        if (function.body.empty()) {
            return;
        }
        function.name = unique_name("get_" + csharp_member_name(name), functions_taken_);
        script_.functions.push_back(std::move(function));
    }

    void read_fields(const Parsed& parsed) {
        std::string type;
        for (const std::string_view piece : split_declarators(parsed.code, ',')) {
            const auto offset = static_cast<std::size_t>(piece.data() - parsed.code.data());
            const std::size_t equals = assignment_at(piece);
            const std::string_view left = stripped(piece.substr(0, equals));
            std::size_t name_start = left.size();
            while (name_start > 0 && is_identifier_char(left[name_start - 1])) {
                --name_start;
            }
            const std::string name(left.substr(name_start));
            if (name.empty()) {
                continue;
            }
            if (name_start > 0) {
                type = std::string(stripped(left.substr(0, name_start)));
            }
            std::string value;
            if (equals != kNone) {
                value = std::string(
                    stripped(parsed.text.substr(offset + equals + 1, piece.size() - equals - 1)));
            }
            add_field(parsed, type, name, value, false);
        }
    }

    // A field of the component, unless it is static, a delegate or a UnityEvent.
    void add_field(const Parsed& parsed, const std::string& type, const std::string& name,
                   const std::string& value, bool property) {
        const std::string plain = plain_type(type);
        if (plain.starts_with("UnityEvent")) {
            read_event(parsed, plain + " " + name);
            return;
        }
        if (parsed.modifiers.contains("static")) {
            script_.notes.push_back("static " + name +
                                    ": keep what every entity shares in one entity's component.");
            return;
        }
        const Mapped mapped = map_csharp(type);
        ScriptField field;
        field.source_name = name;
        field.name = unique_name(csharp_member_name(name), fields_taken_);
        field.doc = parsed.doc;
        const bool readonly = parsed.modifiers.contains("readonly");
        if (source_ == ScriptSource::UnityCsharp) {
            const bool serialize_field =
                std::ranges::any_of(parsed.attributes, [&](const Attribute& attribute) {
                    return attribute.name == "SerializeField" &&
                           (property ? attribute.target == "field" : attribute.target.empty());
                });
            const bool serializable_type = mapped.comment.find("Dictionary") == std::string::npos &&
                                           mapped.comment.find("HashSet") == std::string::npos &&
                                           !mapped.comment.starts_with("Action") &&
                                           !mapped.comment.starts_with("Func");
            field.exported =
                serializable_type && !readonly && attribute(parsed, "NonSerialized") == nullptr &&
                (serialize_field || (!property && parsed.modifiers.contains("public")));
            if (property && serialize_field) {
                field.source_name = "<" + name + ">k__BackingField"; // how Unity saves it
            }
            if (const Attribute* tooltip = attribute(parsed, "Tooltip");
                tooltip != nullptr && !tooltip->args.empty() && field.doc.empty()) {
                field.doc = string_literal(tooltip->args.front()).value_or("");
            }
            if (const Attribute* range = attribute(parsed, "Range");
                range != nullptr && range->args.size() == 2) {
                field.min = number_of(range->args[0]);
                field.max = number_of(range->args[1]);
            }
            if (const Attribute* min = attribute(parsed, "Min");
                min != nullptr && min->args.size() == 1) {
                field.min = number_of(min->args[0]);
            }
        } else if (const Attribute* exported = attribute(parsed, "Export"); exported != nullptr) {
            field.exported = true;
            // [Export(PropertyHint.Range, "0,100,1")]
            if (exported->args.size() >= 2 && exported->args[0].ends_with("Range")) {
                const auto hint = string_literal(exported->args[1]);
                const auto parts = hint ? split_top(*hint, ',') : std::vector<std::string_view>();
                if (parts.size() >= 2) {
                    field.min = number_of(parts[0]);
                    field.max = number_of(parts[1]);
                }
            }
        }
        field.type = mapped.type;
        field.kind = mapped.kind;
        field.comment = mapped.comment;
        if (!value.empty()) {
            const std::string gdscript = gdscript_value(value, type, source_);
            const Typed typed_field =
                typed(mapped, gdscript,
                      gdscript.empty() ? std::nullopt : translate(gdscript, context_), context_);
            field.init = typed_field.init;
            field.comment = typed_field.mapped.comment;
            if (gdscript.empty() || !typed_field.untranslated.empty()) {
                field.comment += (field.comment.empty() ? "" : "; ") + std::string("= ") +
                                 one_line(value) + " in " + std::string(engine_name(source_));
            }
        } else if (field.type == "entt::entity") {
            field.init = " = entt::null";
        }
        script_.fields.push_back(std::move(field));
    }

    static std::optional<double> number_of(std::string_view text) {
        const auto value = number(stripped(without_suffix(stripped(text))));
        if (!value) {
            return std::nullopt;
        }
        std::string digits = value->value;
        if (digits.ends_with('F')) {
            digits.pop_back();
        }
        return std::stod(digits);
    }

    std::string_view text_;
    std::string code_text_; // text_ with comments and literals as spaces
    std::string_view code_{code_text_};
    ScriptSource source_;
    Script script_;
    Context context_;
    std::set<std::string> nested_; // the class's nested types
    std::set<std::string> fields_taken_;
    std::set<std::string> functions_taken_{"run"};
};

} // namespace

Script read_csharp(std::string_view path, std::string_view text, ScriptSource source) {
    return CsharpReader(path, text, source).read();
}

namespace {

// --- Writing C++ --------------------------------------------------------------------------------

enum class Call : std::uint8_t {
    None,
    Created,
    Tick,
    Input,
    Destroyed,
    Engine,
    TriggerEntered, // Unity's messages as bodies touch
    TriggerExited,
    CollisionEntered,
    CollisionExited,
};

struct Virtual {
    std::string_view name;
    Call call;
};

// Godot's calls to a script's functions, in the order Godot makes them.
constexpr std::array<Virtual, 15> kVirtuals{{{"_init", Call::Created},
                                             {"_enter_tree", Call::Created},
                                             {"_ready", Call::Created},
                                             {"_physics_process", Call::Tick},
                                             {"_process", Call::Tick},
                                             {"_input", Call::Input},
                                             {"_shortcut_input", Call::Input},
                                             {"_unhandled_key_input", Call::Input},
                                             {"_unhandled_input", Call::Input},
                                             {"_exit_tree", Call::Destroyed},
                                             {"_draw", Call::Engine},
                                             {"_notification", Call::Engine},
                                             {"_integrate_forces", Call::Engine},
                                             {"_gui_input", Call::Engine},
                                             {"_get_configuration_warnings", Call::Engine}}};

// Unity's messages to a MonoBehaviour, in the order Unity sends them.
constexpr std::array<Virtual, 37> kUnityMessages{{{"Awake", Call::Created},
                                                  {"OnEnable", Call::Created},
                                                  {"Start", Call::Created},
                                                  {"FixedUpdate", Call::Tick},
                                                  {"OnTriggerEnter2D", Call::TriggerEntered},
                                                  {"OnTriggerExit2D", Call::TriggerExited},
                                                  {"OnCollisionEnter2D", Call::CollisionEntered},
                                                  {"OnCollisionExit2D", Call::CollisionExited},
                                                  {"Update", Call::Tick},
                                                  {"LateUpdate", Call::Tick},
                                                  {"OnDisable", Call::Destroyed},
                                                  {"OnDestroy", Call::Destroyed},
                                                  {"OnTriggerStay2D", Call::Engine},
                                                  {"OnCollisionStay2D", Call::Engine},
                                                  {"OnGUI", Call::Engine},
                                                  {"OnApplicationQuit", Call::Engine},
                                                  {"OnApplicationPause", Call::Engine},
                                                  {"OnApplicationFocus", Call::Engine},
                                                  {"OnBecameVisible", Call::Engine},
                                                  {"OnBecameInvisible", Call::Engine},
                                                  {"OnMouseDown", Call::Engine},
                                                  {"OnMouseUp", Call::Engine},
                                                  {"OnMouseEnter", Call::Engine},
                                                  {"OnMouseExit", Call::Engine},
                                                  {"OnMouseOver", Call::Engine},
                                                  {"OnMouseDrag", Call::Engine},
                                                  {"OnValidate", Call::Engine},
                                                  {"Reset", Call::Engine},
                                                  {"OnDrawGizmos", Call::Engine},
                                                  {"OnDrawGizmosSelected", Call::Engine},
                                                  {"OnTriggerEnter", Call::Engine},
                                                  {"OnTriggerExit", Call::Engine},
                                                  {"OnCollisionEnter", Call::Engine},
                                                  {"OnCollisionExit", Call::Engine},
                                                  {"OnAnimatorMove", Call::Engine},
                                                  {"OnRenderObject", Call::Engine},
                                                  {"OnPostRender", Call::Engine}}};

// When the engine calls a function of a script, and where in the order it calls them.
std::pair<Call, int> call_and_order(const Script& script, const ScriptFunction& function) {
    if (function.is_static) {
        return {Call::None, 0};
    }
    // A C# class's constructor, first of all.
    if (script.source != ScriptSource::Gdscript && function.source_name == script.class_name) {
        return {Call::Created, -1};
    }
    std::string name = function.source_name;
    if (script.source == ScriptSource::GodotCsharp && name.starts_with('_')) {
        name = "_" + snake_case(name); // _PhysicsProcess -> _physics_process
    }
    const std::span<const Virtual> table = script.source == ScriptSource::UnityCsharp
                                               ? std::span<const Virtual>(kUnityMessages)
                                               : std::span<const Virtual>(kVirtuals);
    for (std::size_t i = 0; i < table.size(); ++i) {
        if (table[i].name == name) {
            return {table[i].call, static_cast<int>(i)};
        }
    }
    return {Call::None, static_cast<int>(table.size())};
}

// What the skeleton of a script from an engine calls the parts of the script.
struct Words {
    std::string_view variables; // the members a component keeps
    std::string_view signals;
    std::string_view function;
    std::string_view functions;
    std::string_view language;
    std::string_view exported; // what scenes set
    std::string_view node;     // what a script extends to go on objects
};

Words words_of(ScriptSource source) {
    switch (source) {
    case ScriptSource::GodotCsharp:
        return {.variables = "fields",
                .signals = "signals",
                .function = "method",
                .functions = "methods",
                .language = "C#",
                .exported = "exported fields",
                .node = "node"};
    case ScriptSource::UnityCsharp:
        return {.variables = "fields",
                .signals = "events",
                .function = "method",
                .functions = "methods",
                .language = "C#",
                .exported = "serialized fields",
                .node = "MonoBehaviour"};
    case ScriptSource::Gdscript:
        break;
    }
    return {.variables = "variables",
            .signals = "signals",
            .function = "function",
            .functions = "functions",
            .language = "GDScript",
            .exported = "exported variables",
            .node = "node"};
}

// The helper that emits a signal: emit_died.
std::string emitter_of(const Script& script, const ScriptSignal& signal) {
    return "emit_" + (script.source == ScriptSource::Gdscript ? member_name(signal.name)
                                                              : csharp_member_name(signal.name));
}

class Writer {
public:
    explicit Writer(const Script& script)
        : script_(script), words_(words_of(script.source)), engine_(engine_name(script.source)),
          has_self_(!script.fields.empty()) {
        for (const ScriptConnection& connection : script.connections) {
            connected_.insert(connection.method);
            const ScriptFunction* function = function_named(connection.method);
            if (function == nullptr || !direct(connection, *function)) {
                by_event_.insert(connection.method);
            }
        }
    }

    std::string write() {
        std::string body;
        body += "namespace imported::" + script_.ns + " {\n\n";
        body += "using slash::world::World;\n\n";
        enums(body);
        constants(body);
        component(body);
        signals(body);
        for (const ScriptFunction& function : script_.functions) {
            stub(body, function);
        }
        if (script_.node) {
            system(body);
        }
        body += "} // namespace imported::" + script_.ns + "\n";
        if (script_.node) {
            registration_function(body);
        }
        return header() + includes(body) + "\n" + body;
    }

private:
    [[nodiscard]] std::string header() const {
        std::string text =
            script_.component + ", from the " + std::string(engine_) + " script " + script_.path;
        if (!script_.extends.empty()) {
            text += " (extends " + script_.extends + ")";
        }
        text += ", for you to port to C++. ";
        if (script_.node) {
            text += "Its " + std::string(words_.variables) + " are the " + script_.component +
                    " component, its " + std::string(words_.signals) + " world events, and the " +
                    script_.system + " system calls its " + std::string(words_.functions) + " as " +
                    std::string(engine_) + " calls them. ";
        } else {
            text += "It extends no " + std::string(words_.node) + ", so its " +
                    std::string(words_.variables) +
                    " are a reflected struct for a component's field or a .json file. ";
        }
        text += "Each " + std::string(words_.function) + " keeps the " +
                std::string(words_.language) + " to port in its comments.";
        if (!script_.task.empty()) {
            text += " " + script_.task + " in the storyboard tracks the work.";
        }
        if (!script_.autoload.empty()) {
            text += " Godot loads it as the autoload " + script_.autoload +
                    ": give one entity of the main scene its component.";
        }
        text += " Importing again leaves this file as it is.";
        std::string out = wrap(text, "// ");
        if (!script_.notes.empty()) {
            out += "//\n// Left out:\n";
            for (const std::string& note : script_.notes) {
                out += wrap("- " + note, "// ", "//   ");
            }
        }
        return out + "\n";
    }

    static std::string includes(const std::string& body) {
        std::vector<std::string> engine{"slash/world/game_module.hpp"};
        std::vector<std::string> standard;
        const auto uses = [&](std::string_view text) {
            return body.find(text) != std::string::npos;
        };
        if (uses("slash::Vec") || uses("slash::Quat")) {
            engine.emplace_back("slash/core/math.hpp");
        }
        if (uses("slash::json::")) {
            engine.emplace_back("slash/core/json.hpp");
        }
        if (uses("slash::input::")) {
            engine.emplace_back("slash/input/input.hpp");
        }
        if (uses("slash::world::to_json")) {
            engine.emplace_back("slash/world/serialize.hpp");
        }
        if (uses("slash::world::Area2D")) {
            engine.emplace_back("slash/world/components.hpp");
        }
        engine.emplace_back("slash/world/world.hpp");
        std::ranges::sort(engine);
        if (uses("std::int64_t")) {
            standard.emplace_back("cstdint");
        }
        if (uses("std::map")) {
            standard.emplace_back("map");
        }
        if (uses("std::string ") || uses("std::string>") || uses("std::string&") ||
            uses("std::string,")) {
            standard.emplace_back("string");
        }
        if (uses("std::string_view")) {
            standard.emplace_back("string_view");
        }
        if (uses("std::move") || uses("std::pair")) {
            standard.emplace_back("utility");
        }
        if (uses("std::vector")) {
            standard.emplace_back("vector");
        }
        std::string out;
        for (const std::string& file : engine) {
            out += "#include \"" + file + "\"\n";
        }
        if (!standard.empty()) {
            out += "\n";
            for (const std::string& file : standard) {
                out += "#include <" + file + ">\n";
            }
        }
        return out;
    }

    // text in comment lines of at most 100 columns.
    static std::string wrap(const std::string& text, std::string_view first,
                            std::string_view rest = {}) {
        if (rest.empty()) {
            rest = first;
        }
        std::string out;
        std::string line(first);
        std::size_t start = 0;
        bool empty = true;
        while (start < text.size()) {
            std::size_t end = text.find(' ', start);
            if (end == std::string::npos) {
                end = text.size();
            }
            const std::string_view word = std::string_view(text).substr(start, end - start);
            if (!empty && line.size() + 1 + word.size() > 100) {
                out += line + "\n";
                line = std::string(rest);
                empty = true;
            }
            line += (empty ? "" : " ") + std::string(word);
            empty = false;
            start = end + 1;
        }
        return out + line + "\n";
    }

    void enums(std::string& out) const {
        for (const ScriptEnum& declared : script_.enums) {
            out += "// " + declared.declaration + "\n";
            out += "enum ";
            if (!declared.name.empty()) {
                out += declared.name + " ";
            }
            out += ": int { " + join(declared.values, ", ") + " };\n\n";
        }
    }

    void constants(std::string& out) const {
        bool any = false;
        for (const ScriptConstant& constant : script_.constants) {
            if (constant.type.empty()) {
                out += "// " + constant.declaration + "\n";
            } else {
                out += "inline constexpr " + constant.type + " " + constant.name + " = " +
                       constant.value + "; // " + constant.declaration + "\n";
            }
            any = true;
        }
        if (any) {
            out += "\n";
        }
    }

    // A type with no constructor, which C++ would leave unset.
    [[nodiscard]] bool scalar(const std::string& type) const {
        return type == "int" || type == "float" || type == "double" || type == "bool" ||
               type == "std::int64_t" ||
               std::ranges::any_of(script_.enums,
                                   [&](const ScriptEnum& item) { return item.name == type; });
    }

    void component(std::string& out) const {
        out +=
            wrap("What " + file_name() + " keeps" +
                     (script_.node ? " for each entity. Scenes set its " +
                                         std::string(words_.exported) + "; the rest is its state."
                                   : std::string(".")),
                 "// ");
        out += "struct " + script_.component + " {\n";
        std::vector<std::string> names;
        for (const ScriptField& field : script_.fields) {
            if (!field.doc.empty()) {
                out += wrap(field.doc, "    // ");
            }
            std::string init = field.init;
            if (init.empty() && scalar(field.type)) {
                init = "{}"; // as the script's language starts it: zero, false or its first value
            }
            std::string line = "    " + field.type + " " + field.name + init + ";";
            std::string comment = field.comment;
            if (field.name != field.source_name) {
                std::string named = field.source_name;
                if (!comment.empty()) {
                    named += ": ";
                    named += comment;
                }
                comment = std::move(named);
            }
            if (!comment.empty()) {
                line += " // " + comment;
            }
            out += line + "\n";
            names.push_back(field.name);
        }
        out += "};\n";
        std::string list = script_.component;
        for (const std::string& name : names) {
            list += ", " + name;
        }
        out += std::string(script_.node ? "SLASH_COMPONENT(" : "SLASH_REFLECT(") + list + ")\n\n";
    }

    void signals(std::string& out) const {
        for (const ScriptSignal& signal : script_.signals) {
            out += "// " + signal.declaration + "\n";
            const std::string name = emitter_of(script_, signal);
            std::vector<std::string> params{"World& world", "entt::entity entity"};
            for (const ScriptParam& param : signal.params) {
                params.push_back(param.type + " " + param.name);
            }
            out += signature("void " + name, params);
            if (signal.params.empty()) {
                out += "    world.emit(" + cpp_string(signal.name) + ", world.id_of(entity));\n";
            } else {
                out += "    slash::json::Value data = slash::json::Object{};\n";
                for (const ScriptParam& param : signal.params) {
                    out += "    data[" + cpp_string(param.name) +
                           "] = slash::world::to_json(world, " + param.name + ");\n";
                }
                out += "    world.emit(" + cpp_string(signal.name) +
                       ", world.id_of(entity), std::move(data));\n";
            }
            out += "}\n\n";
        }
    }

    [[nodiscard]] std::string file_name() const {
        const std::size_t slash = script_.path.rfind('/');
        return slash == std::string::npos ? script_.path : script_.path.substr(slash + 1);
    }

    // The function's C++ parameters, their names commented out until its body uses them.
    // "head(a, b) {" on one line when it fits in 100 columns, else with the parameters packed
    // onto lines under the first, as clang-format writes them.
    static std::string signature(const std::string& head, const std::vector<std::string>& params) {
        constexpr std::size_t kColumns = 100;
        std::string line = head + "(";
        const std::string indent(line.size(), ' ');
        std::string out;
        for (std::size_t i = 0; i < params.size(); ++i) {
            const std::string piece = params[i] + (i + 1 < params.size() ? "," : ") {");
            if (i == 0) {
                line += piece;
            } else if (line.size() + 1 + piece.size() > kColumns) {
                out += line + "\n";
                line = indent + piece;
            } else {
                line += " " + piece;
            }
        }
        if (params.empty()) {
            line += ") {";
        }
        return out + line + "\n";
    }

    [[nodiscard]] std::vector<std::string> parameters_of(const ScriptFunction& function) const {
        std::vector<std::string> params;
        const auto add = [&](const std::string& type, const std::string& name) {
            params.push_back(type + " /*" + name + "*/");
        };
        const Call call = call_of(function);
        if (!function.is_static) {
            if (script_.node) {
                add("World&", "world");
                add("entt::entity", "entity");
            }
            if (has_self_) {
                add(script_.component + "&", "self");
            }
        }
        if (by_event_.contains(function.source_name) && script_.node && !function.is_static) {
            add("const slash::world::Event&", "event");
            return params;
        }
        for (std::size_t i = 0; i < function.params.size(); ++i) {
            const ScriptParam& param = function.params[i];
            if (i == 0 && call == Call::Tick) {
                add("double", param.name);
            } else if (i == 0 && call == Call::Input) {
                add("const slash::input::Event&", param.name);
            } else {
                add(param.type, param.name);
            }
        }
        return params;
    }

    // Why and when the system calls a function, or "".
    [[nodiscard]] std::string when(const ScriptFunction& function) const {
        const std::string& name = function.source_name;
        const std::string unity_other =
            function.params.empty()
                ? "."
                : ", with the other entity as " + function.params.front().name + ".";
        switch (call_of(function)) {
        case Call::Created:
            return "run() calls it for each entity made since the last tick, as " + engine_ +
                   " calls " + name + ".";
        case Call::Tick: {
            std::string text =
                "run() calls it once a tick, as " + engine_ + " calls " + name + " each frame.";
            if (!function.params.empty() && function.params.front().source.starts_with("Time.")) {
                text += " delta is Unity's " + function.params.front().source + ".";
            }
            return text;
        }
        case Call::Input:
            return "run() calls it for each input event, as " + engine_ + " calls " + name + ".";
        case Call::Destroyed:
            return engine_ + " calls " + name + " as the " +
                   (script_.source == ScriptSource::UnityCsharp ? "object" : "node") +
                   " leaves; nothing calls this yet (see run()).";
        case Call::Engine:
            return engine_ + " calls " + name + " itself; nothing calls this yet.";
        case Call::TriggerEntered:
        case Call::TriggerExited:
            return "run() calls it on both entities when a body or area " +
                   std::string(call_of(function) == Call::TriggerEntered ? "enters" : "leaves") +
                   " an Area2D, as Unity calls " + name + unity_other;
        case Call::CollisionEntered:
        case Call::CollisionExited:
            return "run() calls it on both entities when a RigidBody2D with contact_monitor " +
                   std::string(call_of(function) == Call::CollisionEntered ? "touches"
                                                                           : "stops touching") +
                   " another body, as Unity calls " + name + unity_other;
        case Call::None:
            break;
        }
        if (connected_.contains(name) && !function.is_static) {
            std::string text = "run() calls it when ";
            bool first = true;
            for (const ScriptConnection& connection : script_.connections) {
                if (connection.method == name) {
                    text += std::string(first ? "" : " or ") + connection.from + " emits " +
                            connection.signal;
                    first = false;
                }
            }
            text += ".";
            std::string argument;
            for (const ScriptConnection& connection : script_.connections) {
                if (connection.method == name && !connection.argument.empty()) {
                    argument = connection.argument;
                }
            }
            if (!argument.empty()) {
                text += " " + engine_ + " passed " + argument + ".";
            } else if (!function.params.empty() && by_event_.contains(name)) {
                std::vector<std::string> sources;
                sources.reserve(function.params.size());
                for (const ScriptParam& param : function.params) {
                    sources.push_back(param.source);
                }
                text +=
                    " " + engine_ + " passed " + join(sources, ", ") + ": read it from event.data.";
            }
            return text;
        }
        return {};
    }

    void stub(std::string& out, const ScriptFunction& function) const {
        out += "// Line " + std::to_string(function.line) + ": " + function.declaration + "\n";
        if (const std::string text = when(function); !text.empty()) {
            out += wrap(text, "// ");
        }
        out += signature(function.returns + " " + function.name, parameters_of(function));
        constexpr std::size_t kMaxLines = 150;
        for (std::size_t i = 0; i < function.body.size() && i < kMaxLines; ++i) {
            const std::string& line = function.body[i];
            out += line.empty() ? "    //\n" : "    // " + line + "\n";
        }
        if (function.body.size() > kMaxLines) {
            out += "    // ... " + std::to_string(function.body.size() - kMaxLines) +
                   " more lines in " + script_.path + "\n";
        }
        if (function.returns != "void") {
            out += "    return {};\n";
        }
        out += "}\n\n";
    }

    // The calls to a function from run(): its arguments after the entity's.
    [[nodiscard]] std::string call(const ScriptFunction& function, const std::string& self,
                                   const std::string& argument) const {
        std::string text = function.name + "(world, entity";
        if (has_self_) {
            text += ", " + self;
        }
        if (!argument.empty()) {
            text += ", " + argument;
        }
        return text + ");";
    }

    // Lines that call body on entity when it has the component.
    [[nodiscard]] std::string with_component(const std::string& indent,
                                             const std::vector<std::string>& calls) const {
        std::string out;
        if (has_self_) {
            out += indent + "if (" + script_.component + "* self = ecs.try_get<" +
                   script_.component + ">(entity); self != nullptr) {\n";
        } else {
            out += indent + "if (ecs.all_of<" + script_.component + ">(entity)) {\n";
        }
        for (const std::string& line : calls) {
            out += indent;
            out += "    ";
            out += line;
            out += "\n";
        }
        return out + indent + "}\n";
    }

    void system(std::string& out) const {
        std::vector<const ScriptFunction*> created;
        std::vector<const ScriptFunction*> ticks;
        std::vector<const ScriptFunction*> inputs;
        std::vector<const ScriptFunction*> destroyed;
        // Unity's trigger and collision messages, as bodies start and stop touching.
        std::array<const ScriptFunction*, 4> contacts{};
        for (const ScriptFunction& function : script_.functions) {
            switch (call_of(function)) {
            case Call::Created:
                created.push_back(&function);
                break;
            case Call::Tick:
                ticks.push_back(&function);
                break;
            case Call::Input:
                inputs.push_back(&function);
                break;
            case Call::Destroyed:
                destroyed.push_back(&function);
                break;
            case Call::TriggerEntered:
                contacts[0] = &function;
                break;
            case Call::TriggerExited:
                contacts[1] = &function;
                break;
            case Call::CollisionEntered:
                contacts[2] = &function;
                break;
            case Call::CollisionExited:
                contacts[3] = &function;
                break;
            case Call::None:
            case Call::Engine:
                break;
            }
        }
        const auto by_order = [this](const ScriptFunction* a, const ScriptFunction* b) {
            return order_of(*a) < order_of(*b);
        };
        std::ranges::sort(created, by_order);
        std::ranges::sort(ticks, by_order);
        std::ranges::sort(inputs, by_order);

        std::string events;
        if (!created.empty()) {
            std::vector<std::string> names;
            std::vector<std::string> calls;
            for (const ScriptFunction* function : created) {
                names.push_back(function->source_name);
                const std::vector<std::string> arguments(function->params.size(), "{}");
                calls.push_back(call(*function, "*self", join(arguments, ", ")));
            }
            events += "        // " + join(names, ", ") +
                      ", on the entities made since the last "
                      "tick.\n";
            events += "        if (event.kind == slash::world::kEntityCreated) {\n";
            events += "            const entt::entity entity = world.find(event.id);\n";
            events += with_component("            ", calls);
            events += "        }\n";
        }
        for (const ScriptConnection& connection : script_.connections) {
            const ScriptFunction* found = function_named(connection.method);
            events += wrap(connection.signal + " from " + connection.from + " in " +
                               connection.scene + ", connected to " + connection.method + ".",
                           "        // ");
            if (found == nullptr) {
                events += "        // The script has no function " + connection.method + ".\n";
                continue;
            }
            std::string argument = "event";
            if (!by_event_.contains(found->source_name)) {
                argument = found->params.empty()
                               ? std::string()
                               : cpp_argument(connection.argument, found->params.front());
            }
            events += "        if (event.kind == " + cpp_string(connection.signal) +
                      " && event.id == " + cpp_string(connection.from) + ") {\n";
            events += "            const entt::entity entity = world.find(" +
                      cpp_string(connection.to) + ");\n";
            events += with_component("            ", {call(*found, "*self", argument)});
            events += "        }\n";
        }
        const bool touches = std::ranges::any_of(contacts, [](auto* f) { return f != nullptr; });
        for (std::size_t phase = 0; phase < 2; ++phase) {
            const ScriptFunction* trigger = contacts.at(phase);
            const ScriptFunction* collision = contacts.at(phase + 2);
            if (trigger == nullptr && collision == nullptr) {
                continue;
            }
            const std::string ending = phase == 0 ? "entered" : "exited";
            std::vector<std::string> names;
            for (const ScriptFunction* function : {trigger, collision}) {
                if (function != nullptr) {
                    names.push_back(function->source_name);
                }
            }
            events += "        // " + join(names, " and ") + ", on both entities, as Unity calls " +
                      (names.size() == 1 ? "it" : "them") + ".\n";
            events += "        if (event.kind == \"body_" + ending;
            events += "\" || event.kind == \"area_" + ending + "\") {\n";
            events += "            const auto [first, second] = touching(world, event);\n";
            events += "            const bool trigger =\n";
            events += "                event.kind == \"area_" + ending;
            events += "\" || ecs.all_of<slash::world::Area2D>(first);\n";
            events += "            for (const auto& [entity, other] :\n";
            events += "                 {std::pair{first, second}, std::pair{second, first}}) {\n";
            const auto contact_call = [&](const ScriptFunction& function) {
                return call(function, "*self", function.params.empty() ? "" : "other");
            };
            std::vector<std::string> lines;
            if (trigger != nullptr) {
                lines.emplace_back("if (trigger) {");
                lines.push_back("    " + contact_call(*trigger));
            } else {
                lines.emplace_back("if (!trigger) {");
                lines.push_back("    " + contact_call(*collision));
            }
            if (trigger != nullptr && collision != nullptr) {
                lines.emplace_back("} else {");
                lines.push_back("    " + contact_call(*collision));
            }
            lines.emplace_back("}");
            events += with_component("                ", lines);
            events += "            }\n";
            events += "        }\n";
        }

        std::string body;
        const bool uses_ecs = !events.empty() || !inputs.empty() || !ticks.empty();
        if (uses_ecs) {
            body += "    entt::registry& ecs = world.ecs();\n";
        }
        if (!events.empty()) {
            body += "    for (const slash::world::Event& event : world.events()) {\n" + events +
                    "    }\n";
        }
        const std::string each =
            has_self_
                ? "    ecs.view<" + script_.component + ">().each([&](entt::entity entity, " +
                      script_.component + "& self) {\n"
                : "    ecs.view<" + script_.component + ">().each([&](entt::entity entity) {\n";
        if (!inputs.empty()) {
            body += "    for (const slash::input::Event& event : slash::input::events(world)) {\n";
            body += "    " + each;
            for (const ScriptFunction* function : inputs) {
                body += "            " +
                        call(*function, "self", function->params.empty() ? "" : "event") + "\n";
            }
            body += "        });\n    }\n";
        }
        bool uses_seconds = false;
        if (!ticks.empty()) {
            body += each;
            for (const ScriptFunction* function : ticks) {
                const bool seconds = !function->params.empty();
                uses_seconds = uses_seconds || seconds;
                body += "        " + call(*function, "self", seconds ? "tick_seconds" : "") + "\n";
            }
            body += "    });\n";
        }
        if (!destroyed.empty()) {
            std::vector<std::string> sources;
            std::vector<std::string> names;
            for (const ScriptFunction* function : destroyed) {
                sources.push_back(function->source_name);
                names.push_back(function->name);
            }
            body += wrap(join(sources, ", ") +
                             ": entity.destroyed events come once the entity is gone, so call " +
                             join(names, " and ") + " where the game destroys it.",
                         "    // ");
        }
        if (touches) {
            out += "// The entities a physics event names: the area or body emitting it, and the "
                   "one entering\n// or leaving it.\n";
            out += "std::pair<entt::entity, entt::entity> touching(World& world,\n";
            out += "                                               const slash::world::Event& "
                   "event) {\n";
            out += "    const slash::json::Value* other =\n";
            out += "        event.data.find(event.kind.starts_with(\"area\") ? \"area\" : "
                   "\"body\");\n";
            out += "    return {world.find(event.id),\n";
            out += "            other != nullptr && other->is_string() ? "
                   "world.find(other->as_string()) : entt::null};\n";
            out += "}\n\n";
        }
        out += wrap("The " + script_.system + " system: calls the " +
                        std::string(words_.functions) + " as " + engine_ + " calls " + file_name() +
                        "'s, for each entity with a " + script_.component + ".",
                    "// ");
        out += std::string("void run(World& ") + (uses_ecs ? "world" : "/*world*/") + ", double " +
               (uses_seconds ? "tick_seconds" : "/*tick_seconds*/") + ") {\n";
        out += body + "}\n\n";
    }

    void registration_function(std::string& out) const {
        out += "\nnamespace imported {\n\n";
        out += "// Registers " + file_name() +
               "'s component and system; register_scripts calls "
               "it.\n";
        out += "void register_" + script_.ns + "(slash::world::GameModule& game) {\n";
        const std::string doc = script_.doc.empty()
                                    ? "What the " + engine_ + " script " + script_.path + " keeps."
                                    : script_.doc;
        out += "    game.add_component<" + script_.ns + "::" + script_.component + ">()\n";
        const std::string set_doc = "        .set_doc(";
        out += set_doc + cpp_strings(doc, set_doc.size(), 2) + ")";
        for (const ScriptField& field : script_.fields) {
            std::vector<std::string> range;
            if (field.min) {
                range.push_back(".min = " + double_text(*field.min));
            }
            if (field.max) {
                range.push_back(".max = " + double_text(*field.max));
            }
            if (field.doc.empty() && range.empty()) {
                continue;
            }
            const std::string head = "\n        .describe(" + cpp_string(field.name) + ", {";
            const std::string tail = (range.empty() ? "" : ", " + join(range, ", ")) + "})";
            if (field.doc.empty()) {
                out += head + join(range, ", ") + "})";
            } else {
                out += head;
                out += ".doc = ";
                out += cpp_strings(field.doc, head.size() - 1 + 7, tail.size() + 1);
                out += tail;
            }
        }
        out += ";\n";
        out += "    game.add_system(" + cpp_string(script_.system) + ", " + script_.ns +
               "::run,\n                    {.doc = " +
               cpp_strings("Calls the " + std::string(words_.functions) + " of " + script_.path +
                               ", ported to C++, as " + engine_ + " calls them.",
                           28, 3) +
               "});\n";
        out += "}\n\n} // namespace imported\n";
    }

    [[nodiscard]] Call call_of(const ScriptFunction& function) const {
        return call_and_order(script_, function).first;
    }

    [[nodiscard]] int order_of(const ScriptFunction& function) const {
        return call_and_order(script_, function).second;
    }

    [[nodiscard]] const ScriptFunction* function_named(const std::string& name) const {
        const auto found =
            std::ranges::find_if(script_.functions, [&](const ScriptFunction& function) {
                return function.source_name == name && !function.is_static;
            });
        return found != script_.functions.end() ? &*found : nullptr;
    }

    // Whether run() calls a connected function with its own parameters: a Unity UI call that
    // passes nothing to a method that takes nothing, or a value of its own to one that takes one.
    [[nodiscard]] bool direct(const ScriptConnection& connection,
                              const ScriptFunction& function) const {
        return script_.source == ScriptSource::UnityCsharp &&
               (connection.argument.empty() ? function.params.empty()
                                            : function.params.size() == 1);
    }

    // A Unity UI call's argument, as JSON, as a C++ argument for a parameter.
    static std::string cpp_argument(const std::string& argument, const ScriptParam& param) {
        if (argument.empty()) {
            return argument;
        }
        if (param.type == "entt::entity") {
            return argument.starts_with('"') ? "world.find(" + argument + ")" : "entt::null";
        }
        const bool number =
            argument.front() == '-' || (argument.front() >= '0' && argument.front() <= '9');
        if (number && param.type == "float") {
            return argument.find_first_of(".eE") == std::string::npos ? argument + ".0F"
                                                                      : argument + "F";
        }
        if (number && param.type == "double" &&
            argument.find_first_of(".eE") == std::string::npos) {
            return argument + ".0";
        }
        return argument;
    }

    const Script& script_;
    Words words_;
    std::string engine_;
    bool has_self_;
    std::set<std::string> connected_; // functions a scene's signal calls
    std::set<std::string> by_event_;  // those of them run() passes the signal's event
};

} // namespace

std::string skeleton(const Script& script) {
    return Writer(script).write();
}

std::string registration(const std::vector<std::string>& namespaces) {
    std::string out =
        "// Registers the components and systems of the scripts the import turned into C++. "
        "Importing\n// again rewrites this file; the files beside it are yours.\n\n"
        "#include \"slash/world/game_module.hpp\"\n\nnamespace imported {\n\n";
    for (const std::string& ns : namespaces) {
        out += "void register_" + ns + "(slash::world::GameModule& game);\n";
    }
    if (!namespaces.empty()) {
        out += "\n";
    }
    out += std::string("void register_scripts(slash::world::GameModule& ") +
           (namespaces.empty() ? "/*game*/" : "game") + ") {\n";
    for (const std::string& ns : namespaces) {
        out += "    register_" + ns + "(game);\n";
    }
    out += "}\n\n} // namespace imported\n";
    return out;
}

std::string module_source() {
    return "// The game's own code: the components and systems it adds to the engine's. The "
           "scripts the\n// import turned into C++ register theirs here; add yours after them.\n\n"
           "#include \"slash/world/game_module.hpp\"\n\nnamespace imported {\n"
           "void register_scripts(slash::world::GameModule& game);\n} // namespace imported\n\n"
           "SLASH_GAME_MODULE(game) {\n    imported::register_scripts(game);\n}\n";
}

} // namespace slash::importer
