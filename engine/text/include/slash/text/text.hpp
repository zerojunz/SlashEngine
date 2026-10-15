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
// files that name one with settings and fallbacks; HarfBuzz shapes text with them, characters a
// font lacks come from its fallbacks and then from fonts installed on the machine, right-to-left
// and left-to-right text mix in a line by the Unicode Bidirectional Algorithm (SheenBidi), lines
// break at line breaks and, when asked, wrap to a width; and glyph images go into atlas pages that
// FreeType fills (grayscale, the default) or msdfgen does (multichannel signed distance fields,
// which stay sharp at any size).
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

// A .font.json file: a font file and how it draws, as Godot's FontFile import options, with
// Godot's Font fallbacks and support overrides.
//
//   {"format": 1, "font": "fonts/Title.ttf", "multichannel_signed_distance_field": true,
//    "fallbacks": ["fonts/NotoSansJP.ttf", "fonts/NotoSansArabic.ttf"]}
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
    // Fonts (.ttf, .otf or .font.json files) tried in order for characters this one has no
    // glyph for; theirs come after them.
    std::vector<std::string> fallbacks;
    // When no font of the chain has a character, look for one installed on the machine, as
    // Godot's allow_system_fallback. The first font of a chain decides.
    bool allow_system_fallback = true;
    // Whether the font is for a language (BCP 47, such as "ja") or a script (ISO 15924, such as
    // "Hani"), as Godot's support overrides; "*" stands for any other. Fallbacks for the text's
    // language and script are tried first; a font says yes to anything it doesn't list.
    std::map<std::string, bool, std::less<>> language_support;
    std::map<std::string, bool, std::less<>> script_support;

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

// The direction of a paragraph, as Godot's TextServer::Direction: from its first letter that
// has one (left to right if none does), or set.
enum class Direction : std::uint8_t {
    Auto,
    Ltr,
    Rtl,
};

// The mode, behavior or direction with Godot's name ("word_smart", "trim_word_ellipsis",
// "rtl"), or nullopt.
[[nodiscard]] std::optional<Autowrap> parse_autowrap(std::string_view name);
[[nodiscard]] std::optional<Overrun> parse_overrun(std::string_view name);
[[nodiscard]] std::optional<Direction> parse_direction(std::string_view name);

// A TrueType font whose glyph for each of the characters is a box 600 units wide (a blank for
// spaces) in 1000 units per em, with a hollow box for any other: placeholder text for tests,
// examples and prototypes, which write it instead of committing a font file.
[[nodiscard]] std::vector<std::uint8_t> box_font(std::u32string_view characters,
                                                 std::string_view family = "Boxes");

// Whether a code has the shape of a BCP 47 language tag (a language of two or three letters,
// then subtags of one to eight letters and digits: "ja", "zh-Hant", "sr-Latn-RS") or an ISO 15924
// script code (four letters, the first upper case: "Arab", "Hani").
[[nodiscard]] bool is_language_code(std::string_view code);
[[nodiscard]] bool is_script_code(std::string_view code);

struct LayoutOptions {
    std::string font{}; // NOLINT(readability-redundant-member-init): "" is the default font
    int font_size = kDefaultFontSize;
    Autowrap autowrap = Autowrap::Off;
    // The width lines wrap to when autowrap is on; 0 or less wraps nothing.
    float width = 0.0F;
    Direction direction = Direction::Auto;
    // The text's language (BCP 47, such as "ja" or "sr-Latn"), which picks fallback fonts and
    // the glyphs a font has for it; "" for none.
    std::string language{}; // NOLINT(readability-redundant-member-init)
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

// Glyphs of a line are in the order they draw, left to right; a right-to-left run's come last
// character first.

struct Line {
    std::uint32_t first = 0; // its first glyph in TextLayout::glyphs
    std::uint32_t count = 0; // its glyphs, trailing spaces included
    std::uint32_t start = 0; // the bytes of text it shows, without the line break
    std::uint32_t end = 0;
    float width = 0.0F; // without trailing spaces
    // It ends at a line break or the end of the text, not where it wrapped.
    bool paragraph_end = false;
    // Its paragraph goes right to left, so its text starts at the right: the side it is aligned
    // to by default, cut from by Fonts::trim, and where trailing spaces hang out of its width.
    bool rtl = false;
};

// Whether a Label's fill alignment stretches a glyph of a line: a space between its words, not
// one of the spaces hanging out of its width at its end.
[[nodiscard]] bool stretches(const Line& line, const Glyph& glyph);

struct TextLayout {
    std::vector<Glyph> glyphs;
    std::vector<Line> lines; // at least one, empty for empty text
    FontMetrics metrics;
    std::string font; // the font it was shaped with, for Fonts::trim
    int font_size = kDefaultFontSize;
    float width = 0.0F;        // its widest line
    std::uint32_t missing = 0; // characters no font has a glyph for, drawn as boxes
    // Characters drawn with fonts installed on the machine rather than the project's, which
    // other machines (and web exports, which have none) may draw differently or not at all.
    std::uint32_t system = 0;
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
    std::string path;    // the font file: a project path, "" for the default font, or the
                         // installed file for a system font
    bool system = false; // installed on the machine, not the project's
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

    // Lays out text with a font: shaped with HarfBuzz, each character with the first font of
    // the chain (the font, its fallbacks, then installed fonts) that has it, broken into lines
    // at line breaks ("\n", "\r\n", U+2028, U+2029) and, with autowrap, wrapped to the width,
    // then each line put in display order by the Unicode Bidirectional Algorithm. Fails when the
    // font or a fallback doesn't load, or the size is out of range (1 to kMaxFontSize).
    [[nodiscard]] Result<TextLayout> layout(std::string_view text, const LayoutOptions& options);

    // Whether characters the project's fonts lack are looked for in fonts installed on the
    // machine (for chains whose first font allows it). On by default; turn it off for layouts
    // that must be the same on every machine, as tests do.
    void set_system_fallback(bool on);
    [[nodiscard]] bool system_fallback() const;

    // How a font measures at a size.
    [[nodiscard]] Result<FontMetrics> metrics(std::string_view font, int size);

    // Cuts a line of a layout to fit a width, as overrun says, putting the ellipsis after it for
    // the ellipsis modes: at its end, which is its left for a right-to-left line. Returns whether
    // it cut anything; a line that fits is left alone, unless force_ellipsis asks for the
    // ellipsis anyway (for the last line shown when more are hidden).
    bool trim(TextLayout& layout, std::size_t line, float width, Overrun overrun,
              std::string_view ellipsis = "…", bool force_ellipsis = false);

    // A glyph's image at a font size, with an outline outline_size wide (0 for none; a quarter of
    // it in pixels on each side, as Godot's), made the first time it is asked for. nullopt for
    // glyphs that draw nothing, such as spaces.
    [[nodiscard]] std::optional<GlyphImage> glyph(std::uint32_t face, std::uint32_t index, int size,
                                                  int outline_size = 0);
    [[nodiscard]] FaceInfo face_info(std::uint32_t face) const;

    [[nodiscard]] const std::vector<AtlasPage>& pages() const;
    // The assets fonts load from.
    [[nodiscard]] asset::Assets& assets() const;
    // Glyph images made so far.
    [[nodiscard]] std::size_t glyph_count() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ---- Editing ----

// Where text may be cut into user-perceived characters (extended grapheme clusters, as Unicode's
// UAX #29 and ICU's character break iterator): the end of each, in characters from the start. A
// letter and its accents, a flag or an emoji sequence joined by U+200D is one, as is a Hangul
// syllable made of jamo. Godot's TextServer::string_get_character_breaks.
[[nodiscard]] std::vector<std::uint32_t> character_breaks(std::u32string_view text);

// One line of a layout seen as Godot's TextServer sees a shaped text for editing it: its
// clusters (the glyphs that show some characters together) in display order, so carets,
// selections and clicks map between character positions and x. Positions count characters (code
// points) from the start of the text; x counts pixels from the line's left edge, its spaces
// included, as Godot's shaped text does.
class ShapedLine {
public:
    struct Cluster {
        std::uint32_t start = 0; // the characters it shows: [start, end)
        std::uint32_t end = 0;
        std::uint32_t first = 0; // its glyphs in the layout: [first, first + count)
        std::uint32_t count = 0;
        float x = 0.0F;
        float advance = 0.0F;
        bool rtl = false;
        bool space = false;       // it starts with whitespace
        bool punctuation = false; // it starts with punctuation other than '_'
    };
    // Where a caret draws, from the top of the line: TextServer's CaretInfo half.
    struct Caret {
        bool found = false;
        float x = 0.0F;
        float top = 0.0F;
        float height = 0.0F;
        Direction direction = Direction::Auto; // Auto for a caret inside a cluster
    };
    struct Carets {
        Caret leading;  // after the cluster that ends at the position
        Caret trailing; // before the one that starts there
    };

    ShapedLine() = default;
    // text is what was laid out; the layout's line must be one of it.
    ShapedLine(std::string_view text, const TextLayout& layout, std::size_t line = 0);

    [[nodiscard]] const std::vector<Cluster>& clusters() const { return clusters_; }
    // The characters the line shows, [start, end), and its width with every space.
    [[nodiscard]] std::uint32_t start() const { return start_; }
    [[nodiscard]] std::uint32_t end() const { return end_; }
    [[nodiscard]] float width() const { return width_; }
    // What to add to a glyph's x in the layout to put it in this line's x.
    [[nodiscard]] float origin() const { return origin_; }
    [[nodiscard]] float height() const { return height_; }

    // shaped_text_get_carets: where a caret at a position draws. Between clusters of one
    // direction the two halves line up; where directions meet they split.
    [[nodiscard]] Carets carets(std::uint32_t position) const;
    // shaped_text_get_selection: the [left, right] spans characters [start, end) cover, spans
    // that touch merged.
    [[nodiscard]] std::vector<std::pair<float, float>> selection(std::uint32_t start,
                                                                 std::uint32_t end) const;
    // shaped_text_hit_test_position: the position a click at x puts the caret at.
    [[nodiscard]] std::uint32_t hit_test(float x) const;
    // shaped_text_get_word_breaks: the words, as [start, end) pairs in a flat list, between
    // spaces and punctuation.
    [[nodiscard]] std::vector<std::uint32_t> word_breaks() const;
    // shaped_text_next_character_pos, _prev_character_pos and _closest_character_pos: the
    // nearest character break after, before or around a position.
    [[nodiscard]] std::uint32_t next_character(std::uint32_t position) const;
    [[nodiscard]] std::uint32_t previous_character(std::uint32_t position) const;
    [[nodiscard]] std::uint32_t closest_character(std::uint32_t position) const;
    // shaped_text_get_dominant_direction_in_range: whether more clusters of [start, end) go
    // right to left or left to right; Auto if neither.
    [[nodiscard]] Direction dominant_direction(std::uint32_t start, std::uint32_t end) const;

private:
    std::vector<Cluster> clusters_;
    std::vector<std::uint32_t> breaks_;
    std::uint32_t start_ = 0;
    std::uint32_t end_ = 0;
    float width_ = 0.0F;
    float origin_ = 0.0F;
    float height_ = 0.0F;
};

// Text in upper case, as Godot's Label uppercase: Latin, Greek and Cyrillic letters.
[[nodiscard]] std::string to_upper(std::string_view text);

// The first count Unicode characters of UTF-8 text.
[[nodiscard]] std::string_view first_characters(std::string_view text, std::size_t count);

// How many Unicode characters come before a byte offset in UTF-8 text.
[[nodiscard]] std::size_t character_index(std::string_view text, std::size_t byte);

} // namespace slash::text
