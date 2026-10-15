#pragma once

#include "slash/asset/assets.hpp"
#include "slash/core/json.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/core/result.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

// Fonts and text layout, as Godot's text server: fonts are .ttf and .otf files, or .font.json
// files that name one with settings; HarfBuzz shapes text with them, lines break at line breaks
// and, when asked, wrap to a width; and glyph images go into atlas pages that FreeType fills
// (grayscale, the default) or msdfgen does (multichannel signed distance fields, which stay sharp
// at any size).
//
//   text::Fonts fonts(assets);
//   auto layout = fonts.layout("Hello, world", {.font_size = 24,
//                                               .autowrap = text::Autowrap::Word,
//                                               .width = 200.0F});
//   for (const text::Line& line : layout.value().lines) { ... }
//
// A font path of "" is the default font, Roboto Regular, built into the engine.
namespace slash::text {

inline constexpr std::string_view kFontSuffix = ".font.json";
// Godot's default font size.
inline constexpr int kDefaultFontSize = 16;
inline constexpr int kMaxFontSize = 1024;

enum class Antialiasing : std::uint8_t {
    None,      // one bit per pixel, for pixel art fonts
    Grayscale, // smooth edges, the default
};

enum class Hinting : std::uint8_t {
    None,   // the outlines as drawn
    Light,  // lines snapped to whole pixels vertically, the default
    Normal, // snapped both ways
};

// A .font.json file: a font file and how it draws, as Godot's FontFile import options.
//
//   {"format": 1, "font": "fonts/Title.ttf", "multichannel_signed_distance_field": true}
struct FontSettings {
    std::string font; // the .ttf or .otf file
    Antialiasing antialiasing = Antialiasing::Grayscale;
    Hinting hinting = Hinting::Light;
    // Draw from a distance field made once at msdf_size, which scales to any size and outline
    // without blurring, instead of from glyph images made for each size.
    bool multichannel_signed_distance_field = false;
    // How far from the outline, in pixels at msdf_size, the field reaches. Outlines are at most
    // half of it.
    int msdf_pixel_range = 16;
    // The font size the field is made at.
    int msdf_size = 48;

    bool operator==(const FontSettings&) const = default;
};

// A .ttf or .otf file's bytes.
struct FontData {
    std::string bytes;
};

// Asset types "font_data" (.ttf and .otf files as FontData) and "font" (.font.json files as
// FontSettings).
void add_asset_types(asset::Assets& assets);

// JSON Schema for .font.json files.
[[nodiscard]] json::Value font_schema();
// Every error in a .font.json document, each with the JSON Pointer of the bad value.
[[nodiscard]] std::vector<json::ValidationError> validate_font(const json::Value& doc);
// The settings in a .font.json document, or its first error.
[[nodiscard]] Result<FontSettings> parse_font(const json::Value& doc);

// Whether a path names a font: a .ttf, .otf or .font.json file.
[[nodiscard]] bool is_font_path(std::string_view path);

// Nothing if the bytes are a font file FreeType can draw (scalable TrueType or OpenType), else
// why not.
[[nodiscard]] Result<void> check_font_data(std::string_view bytes);

// The default font's bytes: Roboto Regular.
[[nodiscard]] std::span<const std::uint8_t> default_font();

// How a font measures at a size, in pixels.
struct FontMetrics {
    float ascent = 0.0F;  // from the baseline up to the top of a line
    float descent = 0.0F; // from the baseline down to its bottom
    [[nodiscard]] float height() const { return ascent + descent; }
};

// Where lines wrap, as Godot's autowrap modes.
enum class Autowrap : std::uint8_t {
    Off,       // only at line breaks
    Arbitrary, // anywhere, between characters
    Word,      // between words; a word longer than the line sticks out
    WordSmart, // between words, and inside a word longer than the line
};

// How a line too long for its width is cut, as Godot's text_overrun_behavior.
enum class Overrun : std::uint8_t {
    NoTrimming,
    TrimChar,         // at the last character that fits
    TrimWord,         // at the last word that fits
    TrimEllipsis,     // at the last character that fits with an ellipsis after it
    TrimWordEllipsis, // at the last word that fits with an ellipsis after it
};

// The mode or behavior with Godot's name ("word_smart", "trim_word_ellipsis"), or nullopt.
[[nodiscard]] std::optional<Autowrap> parse_autowrap(std::string_view name);
[[nodiscard]] std::optional<Overrun> parse_overrun(std::string_view name);

struct LayoutOptions {
    std::string font{}; // NOLINT(readability-redundant-member-init): "" is the default font
    int font_size = kDefaultFontSize;
    Autowrap autowrap = Autowrap::Off;
    // The width lines wrap to when autowrap is on; 0 or less wraps nothing.
    float width = 0.0F;
};

struct Glyph {
    std::uint32_t face = 0;    // the font face, for Fonts::glyph
    std::uint32_t index = 0;   // the glyph in its font; 0 is the font's missing-glyph box
    std::uint32_t cluster = 0; // the byte in the text where the characters it shows start
    float x = 0.0F;            // from the line's start to where it draws, along the baseline
    float y = 0.0F;            // from the baseline, y down
    float advance = 0.0F;
    bool space = false; // whitespace, which justified lines stretch
    bool rtl = false;   // from a right-to-left run, such as Arabic or Hebrew
};

struct Line {
    std::uint32_t first = 0; // its first glyph in TextLayout::glyphs
    std::uint32_t count = 0; // its glyphs, trailing spaces included
    std::uint32_t start = 0; // the bytes of text it shows, without the line break
    std::uint32_t end = 0;
    float width = 0.0F; // without trailing spaces
    // It ends at a line break or the end of the text, not where it wrapped.
    bool paragraph_end = false;
};

struct TextLayout {
    std::vector<Glyph> glyphs;
    std::vector<Line> lines; // at least one, empty for empty text
    FontMetrics metrics;
    std::string font; // the font it was shaped with, for Fonts::trim
    int font_size = kDefaultFontSize;
    float width = 0.0F;        // its widest line
    std::uint32_t missing = 0; // characters the font has no glyph for, drawn as boxes
};

// A glyph's image in an atlas page.
struct GlyphImage {
    std::uint32_t page = 0;
    // Its texels in the page, with a clear border texel on each side.
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    // Where its top-left corner draws from the pen position on the baseline, in texels: x right,
    // y down.
    float left = 0.0F;
    float top = 0.0F;
    // Pixels of drawn text per texel: 1 for glyph images, the font size over msdf_size for
    // distance fields.
    float scale = 1.0F;
};

// An atlas page: straight RGBA texels in rows from the top. A glyph image page holds coverage in
// alpha under white; a distance field page holds the field (msdfgen's MTSDF).
struct AtlasPage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba;
    std::uint64_t version = 0; // one more after each change
    bool msdf = false;
};

// How a face draws: from glyph images, or from a distance field with this pixel range.
struct FaceInfo {
    bool msdf = false;
    int msdf_pixel_range = 0;
    Antialiasing antialiasing = Antialiasing::Grayscale;
};

// Fonts, shaping and glyph atlases for a game. Fonts load through assets, by path, and are
// loaded again when their files change. Not thread-safe.
class Fonts {
public:
    explicit Fonts(asset::Assets& assets);
    ~Fonts();
    Fonts(const Fonts&) = delete;
    Fonts& operator=(const Fonts&) = delete;
    Fonts(Fonts&&) = delete;
    Fonts& operator=(Fonts&&) = delete;

    // Lays out text with a font: shaped with HarfBuzz, broken into lines at line breaks ("\n",
    // "\r\n", U+2028, U+2029) and, with autowrap, wrapped to the width. Fails when the font
    // doesn't load or the size is out of range (1 to kMaxFontSize).
    [[nodiscard]] Result<TextLayout> layout(std::string_view text, const LayoutOptions& options);

    // How a font measures at a size.
    [[nodiscard]] Result<FontMetrics> metrics(std::string_view font, int size);

    // Cuts a line of a layout to fit a width, as overrun says, putting the ellipsis after it for
    // the ellipsis modes. Returns whether it cut anything; a line that fits is left alone, unless
    // force_ellipsis asks for the ellipsis anyway (for the last line shown when more are hidden).
    bool trim(TextLayout& layout, std::size_t line, float width, Overrun overrun,
              std::string_view ellipsis = "…", bool force_ellipsis = false);

    // A glyph's image at a font size, with an outline outline_size wide (0 for none; a quarter of
    // it in pixels on each side, as Godot's), made the first time it is asked for. nullopt for
    // glyphs that draw nothing, such as spaces.
    [[nodiscard]] std::optional<GlyphImage> glyph(std::uint32_t face, std::uint32_t index, int size,
                                                  int outline_size = 0);
    [[nodiscard]] FaceInfo face_info(std::uint32_t face) const;

    [[nodiscard]] const std::vector<AtlasPage>& pages() const;
    // Glyph images made so far.
    [[nodiscard]] std::size_t glyph_count() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Text in upper case, as Godot's Label uppercase: Latin, Greek and Cyrillic letters.
[[nodiscard]] std::string to_upper(std::string_view text);

// The first count Unicode characters of UTF-8 text.
[[nodiscard]] std::string_view first_characters(std::string_view text, std::size_t count);

// How many Unicode characters come before a byte offset in UTF-8 text.
[[nodiscard]] std::size_t character_index(std::string_view text, std::size_t byte);

} // namespace slash::text
