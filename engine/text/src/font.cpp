#include "slash/core/text.hpp"
#include "slash/text/text.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace slash::text {

namespace detail {
// The default font (cmake/embed.cmake writes them from Roboto-Regular.ttf).
extern const unsigned char kDefaultFont[];
extern const std::size_t kDefaultFont_size; // NOLINT(readability-identifier-naming)
} // namespace detail

namespace {

constexpr std::array<std::string_view, 2> kFontFileSuffixes{".ttf", ".otf"};
constexpr std::array<std::pair<std::string_view, Antialiasing>, 2> kAntialiasing{{
    {"none", Antialiasing::None},
    {"grayscale", Antialiasing::Grayscale},
}};
constexpr std::array<std::pair<std::string_view, Hinting>, 3> kHinting{{
    {"none", Hinting::None},
    {"light", Hinting::Light},
    {"normal", Hinting::Normal},
}};

// Fallbacks a .font.json file may list.
constexpr std::int64_t kMaxFallbacks = 64;

bool ends_with(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}

bool is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool is_alnum(char c) {
    return is_alpha(c) || (c >= '0' && c <= '9');
}

} // namespace

bool is_language_code(std::string_view code) {
    std::size_t start = 0;
    bool first = true;
    while (start <= code.size()) {
        std::size_t end = code.find('-', start);
        if (end == std::string_view::npos) {
            end = code.size();
        }
        const std::string_view part = code.substr(start, end - start);
        const std::size_t min = first ? 2 : 1;
        const std::size_t max = first ? 3 : 8;
        if (part.size() < min || part.size() > max ||
            !std::ranges::all_of(part, first ? is_alpha : is_alnum)) {
            return false;
        }
        first = false;
        start = end + 1;
    }
    return true;
}

bool is_script_code(std::string_view code) {
    return code.size() == 4 && code[0] >= 'A' && code[0] <= 'Z' &&
           std::ranges::all_of(code.substr(1), [](char c) { return c >= 'a' && c <= 'z'; });
}

namespace {

json::Value integer(std::string_view description, std::int64_t minimum, std::int64_t maximum,
                    std::int64_t fallback) {
    json::Value schema;
    schema["type"] = "integer";
    schema["minimum"] = minimum;
    schema["maximum"] = maximum;
    schema["default"] = fallback;
    schema["description"] = description;
    return schema;
}

template <typename T, std::size_t N>
json::Value choice(std::string_view description,
                   const std::array<std::pair<std::string_view, T>, N>& names, T fallback) {
    json::Value schema;
    schema["type"] = "string";
    json::Array values;
    for (const auto& [name, value] : names) {
        values.emplace_back(name);
        if (value == fallback) {
            schema["default"] = name;
        }
    }
    schema["enum"] = std::move(values);
    schema["description"] = description;
    return schema;
}

template <typename T, std::size_t N>
T parse_choice(const json::Value* value, const std::array<std::pair<std::string_view, T>, N>& names,
               T fallback) {
    if (value == nullptr) {
        return fallback;
    }
    for (const auto& [name, choice] : names) {
        if (value->as_string() == name) {
            return choice;
        }
    }
    return fallback;
}

// One UTF-8 character's length from its first byte; a stray continuation byte counts as one.
std::size_t utf8_length(unsigned char first) {
    if (first >= 0xF0U) {
        return 4;
    }
    if (first >= 0xE0U) {
        return 3;
    }
    if (first >= 0xC0U) {
        return 2;
    }
    return 1;
}

// The next character of UTF-8 text from i, which moves past it. Malformed bytes read as U+FFFD.
char32_t next_char(std::string_view text, std::size_t& i) {
    const auto first = static_cast<unsigned char>(text[i]);
    const std::size_t length = utf8_length(first);
    if (length == 1 || i + length > text.size()) {
        ++i;
        return first < 0x80U ? char32_t{first} : char32_t{0xFFFD};
    }
    char32_t code = first & (0xFFU >> (length + 1));
    for (std::size_t k = 1; k < length; ++k) {
        const auto byte = static_cast<unsigned char>(text[i + k]);
        if ((byte & 0xC0U) != 0x80U) {
            ++i;
            return 0xFFFD;
        }
        code = (code << 6U) | (byte & 0x3FU);
    }
    i += length;
    return code;
}

void append_utf8(std::string& out, char32_t code) {
    if (code < 0x80U) {
        out += static_cast<char>(code);
    } else if (code < 0x800U) {
        out += static_cast<char>(0xC0U | (code >> 6U));
        out += static_cast<char>(0x80U | (code & 0x3FU));
    } else if (code < 0x10000U) {
        out += static_cast<char>(0xE0U | (code >> 12U));
        out += static_cast<char>(0x80U | ((code >> 6U) & 0x3FU));
        out += static_cast<char>(0x80U | (code & 0x3FU));
    } else {
        out += static_cast<char>(0xF0U | (code >> 18U));
        out += static_cast<char>(0x80U | ((code >> 12U) & 0x3FU));
        out += static_cast<char>(0x80U | ((code >> 6U) & 0x3FU));
        out += static_cast<char>(0x80U | (code & 0x3FU));
    }
}

// Unicode's simple upper-case mapping for Latin, Greek and Cyrillic letters.
char32_t upper_of(char32_t c) {
    // Pairs where the upper case letter is even and the lower case one odd, or the other way.
    const auto pair = [c](char32_t first, char32_t last, bool upper_even) {
        return c >= first && c <= last && ((c % 2U == 1U) == upper_even);
    };
    if (c >= U'a' && c <= U'z') {
        return c - 0x20U;
    }
    if (c < 0xB5U) {
        return c;
    }
    if (c == 0xB5U) {
        return 0x39CU; // micro sign to capital mu
    }
    if ((c >= 0xE0U && c <= 0xFEU && c != 0xF7U) || (c >= 0x3B1U && c <= 0x3CBU && c != 0x3C2U) ||
        (c >= 0x430U && c <= 0x44FU)) {
        return c - 0x20U;
    }
    if (c == 0xFFU) {
        return 0x178U;
    }
    if (c == 0x131U) {
        return U'I'; // dotless i
    }
    if (c == 0x17FU) {
        return U'S'; // long s
    }
    if (pair(0x100U, 0x137U, true) || pair(0x14AU, 0x177U, true) || pair(0x139U, 0x148U, false) ||
        pair(0x179U, 0x17EU, false) || pair(0x460U, 0x481U, true) || pair(0x48AU, 0x4BFU, true) ||
        pair(0x4C1U, 0x4CEU, false) || pair(0x4D0U, 0x52FU, true)) {
        return c - 1U;
    }
    if (c == 0x4CFU) {
        return 0x4C0U;
    }
    if (c == 0x3C2U) {
        return 0x3A3U; // final sigma
    }
    if (c == 0x3ACU) {
        return 0x386U;
    }
    if (c >= 0x3ADU && c <= 0x3AFU) {
        return c - 0x25U;
    }
    if (c == 0x3CCU) {
        return 0x38CU;
    }
    if (c == 0x3CDU || c == 0x3CEU) {
        return c - 0x3FU;
    }
    if (c >= 0x450U && c <= 0x45FU) {
        return c - 0x50U;
    }
    return c;
}

} // namespace

json::Value font_schema() {
    const FontSettings defaults;
    json::Value schema;
    schema["$schema"] = "https://json-schema.org/draft/2020-12/schema";
    schema["title"] = "SlashEngine font";
    schema["description"] = "A font file and how it draws, as Godot's FontFile import options.";
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["required"] = json::Array{"format", "font"};
    json::Value& properties = schema["properties"];
    properties["format"]["const"] = 1;
    properties["format"]["description"] = "Format version. Must be 1.";
    properties["font"]["type"] = "string";
    properties["font"]["minLength"] = 1;
    properties["font"]["description"] = "The .ttf or .otf file.";
    properties["antialiasing"] =
        choice("Smooth edges, or none for pixel art fonts.", kAntialiasing, defaults.antialiasing);
    properties["hinting"] = choice("Snaps outlines to pixels: light does it vertically only.",
                                   kHinting, defaults.hinting);
    properties["multichannel_signed_distance_field"]["type"] = "boolean";
    properties["multichannel_signed_distance_field"]["default"] = false;
    properties["multichannel_signed_distance_field"]["description"] =
        "Draw from one distance field per glyph, sharp at any size, scale and outline.";
    properties["msdf_pixel_range"] =
        integer("How far from the outline the field reaches, in pixels at msdf_size; outlines "
                "are at most half of it.",
                1, 256, defaults.msdf_pixel_range);
    properties["msdf_size"] =
        integer("The font size the field is made at.", 1, 256, defaults.msdf_size);
    json::Value& fallbacks = properties["fallbacks"];
    fallbacks["type"] = "array";
    fallbacks["items"]["type"] = "string";
    fallbacks["items"]["minLength"] = 1;
    fallbacks["maxItems"] = kMaxFallbacks;
    fallbacks["description"] =
        "Fonts (.ttf, .otf or .font.json) tried in order for characters this one lacks.";
    properties["allow_system_fallback"]["type"] = "boolean";
    properties["allow_system_fallback"]["default"] = true;
    properties["allow_system_fallback"]["description"] =
        "When no font of the chain has a character, use one installed on the machine.";
    const auto support = [](std::string_view what) {
        json::Value value;
        value["type"] = "object";
        value["propertyNames"]["minLength"] = 1;
        value["additionalProperties"]["type"] = "boolean";
        value["description"] = what;
        return value;
    };
    properties["language_support"] =
        support(R"(Whether the font is for each language (BCP 47 such as "ja"; "*" for others).)");
    properties["script_support"] = support(
        R"(Whether the font is for each script (ISO 15924 such as "Hani"; "*" for others).)");
    return schema;
}

std::vector<json::ValidationError> validate_font(const json::Value& doc) {
    std::vector<json::ValidationError> errors = json::validate(doc, font_schema());
    if (!errors.empty()) {
        return errors;
    }
    const std::string& font = doc.find("font")->as_string();
    if (!is_project_path(font)) {
        errors.push_back({"/font", "expected a path inside the project folder, separated by "
                                   "'/', with no \".\" or \"..\" parts"});
    } else if (!ends_with(font, kFontFileSuffixes[0]) && !ends_with(font, kFontFileSuffixes[1])) {
        errors.push_back({"/font", "expected a .ttf or .otf file"});
    }
    if (const json::Value* fallbacks = doc.find("fallbacks"); fallbacks != nullptr) {
        const json::Array& list = fallbacks->as_array();
        for (std::size_t i = 0; i < list.size(); ++i) {
            const std::string& path = list[i].as_string();
            const std::string at = "/fallbacks/" + std::to_string(i);
            if (!is_project_path(path)) {
                errors.push_back({at, "expected a path inside the project folder, separated by "
                                      "'/', with no \".\" or \"..\" parts"});
            } else if (!is_font_path(path)) {
                errors.push_back({at, "expected a .ttf, .otf or .font.json file"});
            }
        }
    }
    if (const json::Value* languages = doc.find("language_support"); languages != nullptr) {
        for (const auto& [code, supported] : languages->as_object()) {
            if (code != "*" && !is_language_code(code)) {
                errors.push_back({"/language_support/" + json::pointer_escape(code),
                                  "expected a BCP 47 language code such as \"ja\" or "
                                  "\"zh-Hant\", or \"*\""});
            }
        }
    }
    if (const json::Value* scripts = doc.find("script_support"); scripts != nullptr) {
        for (const auto& [code, supported] : scripts->as_object()) {
            if (code != "*" && !is_script_code(code)) {
                errors.push_back({"/script_support/" + json::pointer_escape(code),
                                  "expected an ISO 15924 script code such as \"Latn\" or "
                                  "\"Hani\", or \"*\""});
            }
        }
    }
    return errors;
}

Result<FontSettings> parse_font(const json::Value& doc) {
    if (const auto errors = validate_font(doc); !errors.empty()) {
        const json::ValidationError& first = errors.front();
        return make_error(ErrorCode::InvalidArgument,
                          (first.path.empty() ? std::string("root") : first.path) + ": " +
                              first.message);
    }
    FontSettings out;
    out.font = doc.find("font")->as_string();
    out.antialiasing = parse_choice(doc.find("antialiasing"), kAntialiasing, out.antialiasing);
    out.hinting = parse_choice(doc.find("hinting"), kHinting, out.hinting);
    if (const json::Value* msdf = doc.find("multichannel_signed_distance_field"); msdf != nullptr) {
        out.multichannel_signed_distance_field = msdf->as_bool();
    }
    if (const json::Value* range = doc.find("msdf_pixel_range"); range != nullptr) {
        out.msdf_pixel_range = static_cast<int>(range->as_int());
    }
    if (const json::Value* size = doc.find("msdf_size"); size != nullptr) {
        out.msdf_size = static_cast<int>(size->as_int());
    }
    if (const json::Value* fallbacks = doc.find("fallbacks"); fallbacks != nullptr) {
        for (const json::Value& path : fallbacks->as_array()) {
            out.fallbacks.push_back(path.as_string());
        }
    }
    if (const json::Value* system = doc.find("allow_system_fallback"); system != nullptr) {
        out.allow_system_fallback = system->as_bool();
    }
    if (const json::Value* languages = doc.find("language_support"); languages != nullptr) {
        for (const auto& [code, supported] : languages->as_object()) {
            out.language_support.insert_or_assign(code, supported.as_bool());
        }
    }
    if (const json::Value* scripts = doc.find("script_support"); scripts != nullptr) {
        for (const auto& [code, supported] : scripts->as_object()) {
            out.script_support.insert_or_assign(code, supported.as_bool());
        }
    }
    return out;
}

bool is_font_path(std::string_view path) {
    return ends_with(path, kFontSuffix) || ends_with(path, kFontFileSuffixes[0]) ||
           ends_with(path, kFontFileSuffixes[1]);
}

void add_asset_types(asset::Assets& assets) {
    assets.add_type<FontData>("font_data", {".ttf", ".otf"},
                              [](std::string_view bytes, std::string_view /*path*/) {
                                  return Result<FontData>(FontData{std::string(bytes)});
                              });
    assets.add_type<FontSettings>(
        "font", {std::string(kFontSuffix)},
        [](std::string_view text, std::string_view /*path*/) -> Result<FontSettings> {
            auto doc = json::parse(text);
            if (!doc) {
                return doc.error();
            }
            return parse_font(doc.value());
        });
}

std::optional<Autowrap> parse_autowrap(std::string_view name) {
    static constexpr std::array<std::pair<std::string_view, Autowrap>, 4> kNames{{
        {"off", Autowrap::Off},
        {"arbitrary", Autowrap::Arbitrary},
        {"word", Autowrap::Word},
        {"word_smart", Autowrap::WordSmart},
    }};
    for (const auto& [each, mode] : kNames) {
        if (each == name) {
            return mode;
        }
    }
    return std::nullopt;
}

std::optional<Overrun> parse_overrun(std::string_view name) {
    static constexpr std::array<std::pair<std::string_view, Overrun>, 5> kNames{{
        {"no_trimming", Overrun::NoTrimming},
        {"trim_char", Overrun::TrimChar},
        {"trim_word", Overrun::TrimWord},
        {"trim_ellipsis", Overrun::TrimEllipsis},
        {"trim_word_ellipsis", Overrun::TrimWordEllipsis},
    }};
    for (const auto& [each, behavior] : kNames) {
        if (each == name) {
            return behavior;
        }
    }
    return std::nullopt;
}

std::optional<Direction> parse_direction(std::string_view name) {
    static constexpr std::array<std::pair<std::string_view, Direction>, 3> kNames{{
        {"auto", Direction::Auto},
        {"ltr", Direction::Ltr},
        {"rtl", Direction::Rtl},
    }};
    for (const auto& [each, direction] : kNames) {
        if (each == name) {
            return direction;
        }
    }
    return std::nullopt;
}

bool stretches(const Line& line, const Glyph& glyph) {
    // Spaces hanging out of the line sit past its width, or left of 0 on a right-to-left line.
    const float middle = glyph.x + (glyph.advance * 0.5F);
    return glyph.space && middle > 0.0F && middle < line.width;
}

std::span<const std::uint8_t> default_font() {
    return {detail::kDefaultFont, detail::kDefaultFont_size};
}

char32_t to_upper(char32_t c) {
    return upper_of(c);
}

std::string to_upper(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    std::size_t i = 0;
    while (i < text.size()) {
        append_utf8(out, upper_of(next_char(text, i)));
    }
    return out;
}

std::string_view first_characters(std::string_view text, std::size_t count) {
    std::size_t i = 0;
    for (std::size_t n = 0; n < count && i < text.size(); ++n) {
        i += utf8_length(static_cast<unsigned char>(text[i]));
    }
    return text.substr(0, i < text.size() ? i : text.size());
}

std::size_t character_index(std::string_view text, std::size_t byte) {
    std::size_t count = 0;
    for (std::size_t i = 0; i < byte && i < text.size();
         i += utf8_length(static_cast<unsigned char>(text[i]))) {
        ++count;
    }
    return count;
}

} // namespace slash::text
