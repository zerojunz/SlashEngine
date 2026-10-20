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

Typed type_of(const Declared& declaration, const Context& context) {
    Typed out;
    std::optional<Translated> value;
    if (!declaration.value.empty()) {
        value = translate(declaration.value, context);
    }
    if (!declaration.type.empty()) {
        out.mapped = map_type(declaration.type, context);
    } else if (value && !value->type.empty() && value->type != "null" && value->type != "[]" &&
               value->type != "{}") {
        out.mapped.type = value->type;
        out.mapped.comment = value->comment;
        if (value->type == "entt::entity") {
            out.mapped.kind = FieldKind::Entity;
        }
    } else {
        out.mapped.type = "slash::json::Value";
    }
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
    const std::string_view list = declaration.value;
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
    if (!declaration.value.empty() && !(value && value->type == "entt::entity")) {
        out.untranslated = declaration.value;
    }
    return out;
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
    static constexpr std::array<std::string_view, 11> kSystems{
        "animated_sprite_2d", "animation_player", "audio",   "camera_2d",
        "cpu_particles_2d",   "gpu_particles_2d", "gui",     "input",
        "parallax_2d",        "physics2d",        "services"};
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

namespace {

// --- Writing C++ --------------------------------------------------------------------------------

enum class Call : std::uint8_t { None, Created, Tick, Input, Destroyed, Engine };

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

Call call_of(const ScriptFunction& function) {
    if (function.is_static) {
        return Call::None;
    }
    for (const Virtual& entry : kVirtuals) {
        if (entry.name == function.source_name) {
            return entry.call;
        }
    }
    return Call::None;
}

int order_of(const ScriptFunction& function) {
    for (std::size_t i = 0; i < kVirtuals.size(); ++i) {
        if (kVirtuals.at(i).name == function.source_name) {
            return static_cast<int>(i);
        }
    }
    return static_cast<int>(kVirtuals.size());
}

class Writer {
public:
    explicit Writer(const Script& script) : script_(script), has_self_(!script.fields.empty()) {
        for (const ScriptConnection& connection : script.connections) {
            connected_.insert(connection.method);
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
        std::string text = script_.component + ", from the Godot script " + script_.path;
        if (!script_.extends.empty()) {
            text += " (extends " + script_.extends + ")";
        }
        text += ", for you to port to C++. ";
        if (script_.node) {
            text += "Its variables are the " + script_.component +
                    " component, its signals world events, and the " + script_.system +
                    " system calls its functions as Godot calls them. ";
        } else {
            text += "It extends no node, so its variables are a reflected struct for a "
                    "component's field or a .json file. ";
        }
        text += "Each function keeps the GDScript to port in its comments.";
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
        if (uses("std::move")) {
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

    void component(std::string& out) const {
        out += wrap("What " + file_name() + " keeps" +
                        (script_.node
                             ? " for each entity. Scenes set its exported variables; the rest is "
                               "its state."
                             : "."),
                    "// ");
        out += "struct " + script_.component + " {\n";
        std::vector<std::string> names;
        for (const ScriptField& field : script_.fields) {
            if (!field.doc.empty()) {
                out += wrap(field.doc, "    // ");
            }
            std::string line = "    " + field.type + " " + field.name + field.init + ";";
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
            const std::string name = "emit_" + member_name(signal.name);
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
        if (connected_.contains(function.source_name) && script_.node && !function.is_static) {
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
        switch (call_of(function)) {
        case Call::Created:
            return "run() calls it for each entity made since the last tick, as Godot calls " +
                   name + ".";
        case Call::Tick:
            return "run() calls it once a tick, as Godot calls " + name + " each frame.";
        case Call::Input:
            return "run() calls it for each input event, as Godot calls " + name + ".";
        case Call::Destroyed:
            return "Godot calls " + name +
                   " as the node leaves; nothing calls this yet (see run()).";
        case Call::Engine:
            return "Godot calls " + name + " itself; nothing calls this yet.";
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
            if (!function.params.empty()) {
                std::vector<std::string> sources;
                sources.reserve(function.params.size());
                for (const ScriptParam& param : function.params) {
                    sources.push_back(param.source);
                }
                text += " Godot passed " + join(sources, ", ") + ": read it from event.data.";
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
        const ScriptFunction* destroyed = nullptr;
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
                destroyed = &function;
                break;
            case Call::None:
            case Call::Engine:
                break;
            }
        }
        const auto by_order = [](const ScriptFunction* a, const ScriptFunction* b) {
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
            const auto found =
                std::ranges::find_if(script_.functions, [&](const ScriptFunction& function) {
                    return function.source_name == connection.method && !function.is_static;
                });
            events += wrap(connection.signal + " from " + connection.from + " in " +
                               connection.scene + ", connected to " + connection.method + ".",
                           "        // ");
            if (found == script_.functions.end()) {
                events += "        // The script has no function " + connection.method + ".\n";
                continue;
            }
            events += "        if (event.kind == " + cpp_string(connection.signal) +
                      " && event.id == " + cpp_string(connection.from) + ") {\n";
            events += "            const entt::entity entity = world.find(" +
                      cpp_string(connection.to) + ");\n";
            events += with_component("            ", {call(*found, "*self", "event")});
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
        if (destroyed != nullptr) {
            body += wrap(destroyed->source_name +
                             ": entity.destroyed events come once the entity is gone, so call " +
                             destroyed->name + " where the game destroys it.",
                         "    // ");
        }
        out += wrap("The " + script_.system + " system: calls the functions as Godot calls " +
                        file_name() + "'s, for each entity with a " + script_.component + ".",
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
        const std::string doc =
            script_.doc.empty() ? "What the Godot script " + script_.path + " keeps." : script_.doc;
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
               cpp_strings("Calls the functions of " + script_.path +
                               ", ported to C++, as Godot calls them.",
                           28, 3) +
               "});\n";
        out += "}\n\n} // namespace imported\n";
    }

    const Script& script_;
    bool has_self_;
    std::set<std::string> connected_;
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
