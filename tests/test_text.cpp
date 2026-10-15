#include "slash/asset/assets.hpp"
#include "slash/core/json.hpp"
#include "slash/text/text.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace asset = slash::asset;
namespace json = slash::json;
namespace text = slash::text;

namespace {

struct MemoryFiles {
    std::map<std::string, std::string, std::less<>> files;
    std::map<std::string, std::int64_t, std::less<>> edits;

    void write(const std::string& path, std::string bytes) {
        files[path] = std::move(bytes);
        ++edits[path];
    }

    asset::FileSource source() {
        return {
            .read = [this](std::string_view path) -> slash::Result<std::string> {
                const auto it = files.find(path);
                if (it == files.end()) {
                    return slash::make_error(slash::ErrorCode::NotFound, "no such file");
                }
                return it->second;
            },
            .stamp = [this](std::string_view path) -> std::optional<asset::FileStamp> {
                const auto it = files.find(path);
                if (it == files.end()) {
                    return std::nullopt;
                }
                return asset::FileStamp{.modified = edits.find(path)->second,
                                        .size = it->second.size()};
            },
        };
    }
};

std::string default_font_bytes() {
    const auto bytes = text::default_font();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

// Each line's text.
std::vector<std::string> lines_of(const text::TextLayout& layout, std::string_view source) {
    std::vector<std::string> out;
    for (const text::Line& line : layout.lines) {
        out.emplace_back(source.substr(line.start, line.end - line.start));
    }
    return out;
}

text::TextLayout lay_out(text::Fonts& fonts, std::string_view source,
                         const text::LayoutOptions& options = {}) {
    auto layout = fonts.layout(source, options);
    REQUIRE(layout.ok());
    return std::move(layout).value();
}

} // namespace

TEST_CASE("the default font measures and shapes text") {
    MemoryFiles files;
    asset::Assets assets(files.source());
    text::Fonts fonts(assets);

    auto metrics = fonts.metrics("", 16);
    REQUIRE(metrics.ok());
    MESSAGE("ascent " << metrics.value().ascent << " descent " << metrics.value().descent);
    CHECK(metrics.value().ascent > 10.0F);
    CHECK(metrics.value().descent > 2.0F);

    const text::TextLayout layout = lay_out(fonts, "Hello");
    REQUIRE(layout.lines.size() == 1);
    CHECK(layout.glyphs.size() == 5);
    CHECK(layout.missing == 0);
    float pen = 0.0F;
    for (const text::Glyph& glyph : layout.glyphs) {
        CHECK(glyph.x == doctest::Approx(pen));
        CHECK(glyph.advance == std::round(glyph.advance)); // whole pixels
        pen += glyph.advance;
    }
    CHECK(layout.lines[0].width == doctest::Approx(pen));
    CHECK(layout.width == doctest::Approx(pen));
    MESSAGE("Hello is " << layout.width << " px wide");
}

TEST_CASE("text breaks at line breaks and wraps to a width") {
    MemoryFiles files;
    asset::Assets assets(files.source());
    text::Fonts fonts(assets);

    const std::string two = "one\ntwo\r\n\nthree";
    const text::TextLayout paragraphs = lay_out(fonts, two);
    CHECK(lines_of(paragraphs, two) == std::vector<std::string>{"one", "two", "", "three"});
    CHECK(
        std::ranges::all_of(paragraphs.lines, [](const text::Line& l) { return l.paragraph_end; }));

    const std::string sentence = "The quick brown fox jumps over the lazy dog";
    // The widest line, so each other line can't take the next word.
    const float width = lay_out(fonts, "the lazy dog").width;
    const text::TextLayout wrapped =
        lay_out(fonts, sentence, {.autowrap = text::Autowrap::Word, .width = width});
    CHECK(lines_of(wrapped, sentence) ==
          std::vector<std::string>{"The quick ", "brown fox ", "jumps over ", "the lazy dog"});
    for (const text::Line& line : wrapped.lines) {
        CHECK(line.width <= width);
    }
    CHECK_FALSE(wrapped.lines[0].paragraph_end);
    CHECK(wrapped.lines.back().paragraph_end);
    // Trailing spaces don't count toward a line's width.
    CHECK(wrapped.lines[0].width == doctest::Approx(lay_out(fonts, "The quick").width));

    // A word longer than the line sticks out, unless word_smart breaks it.
    const std::string long_word = "a extraordinarily b";
    const float narrow = lay_out(fonts, "extra").width;
    CHECK(lines_of(lay_out(fonts, long_word, {.autowrap = text::Autowrap::Word, .width = narrow}),
                   long_word) == std::vector<std::string>{"a ", "extraordinarily ", "b"});
    const auto smart = lines_of(
        lay_out(fonts, long_word, {.autowrap = text::Autowrap::WordSmart, .width = narrow}),
        long_word);
    CHECK(smart.size() > 3);
    CHECK(smart.front() == "a ");
    CHECK(smart.back().ends_with(" b"));
    const auto arbitrary = lines_of(
        lay_out(fonts, long_word, {.autowrap = text::Autowrap::Arbitrary, .width = narrow}),
        long_word);
    CHECK(arbitrary.front().starts_with("a ex"));

    // Lines break after a hyphen inside a word.
    const std::string hyphen = "well-known";
    CHECK(lines_of(
              lay_out(fonts, hyphen,
                      {.autowrap = text::Autowrap::Word, .width = lay_out(fonts, "well-").width}),
              hyphen) == std::vector<std::string>{"well-", "known"});

    // No break at a no-break space.
    const std::string glued = "10 km";
    CHECK(lay_out(fonts, glued, {.autowrap = text::Autowrap::Word, .width = 1.0F}).lines.size() ==
          1);
}

TEST_CASE("lines break between ideographs but not before closing marks") {
    MemoryFiles files;
    asset::Assets assets(files.source());
    text::Fonts fonts(assets);
    // The default font has no Japanese glyphs, so these shape as missing-glyph boxes of one
    // width, which still break as Japanese text does.
    const std::string japanese = "日本語。。";
    const text::TextLayout one = lay_out(fonts, japanese);
    CHECK(one.missing == 5);
    const float box = one.glyphs[0].advance;
    const text::TextLayout wrapped =
        lay_out(fonts, japanese, {.autowrap = text::Autowrap::Word, .width = (box * 3.0F) + 1.0F});
    const auto lines = lines_of(wrapped, japanese);
    REQUIRE(lines.size() == 2);
    // The line could hold three boxes, but a full stop can't start one, so two move down.
    CHECK(lines[0] == "日本");
    CHECK(lines[1] == "語。。");
}

TEST_CASE("lines too long are trimmed with an ellipsis") {
    MemoryFiles files;
    asset::Assets assets(files.source());
    text::Fonts fonts(assets);
    const std::string source = "Continue the game";
    const float width = lay_out(fonts, "Continue th").width;

    text::TextLayout chars = lay_out(fonts, source);
    CHECK(fonts.trim(chars, 0, width, text::Overrun::TrimChar));
    CHECK(source.substr(chars.lines[0].start, chars.lines[0].end) == "Continue th");
    CHECK(chars.lines[0].width <= width);

    text::TextLayout words = lay_out(fonts, source);
    CHECK(fonts.trim(words, 0, width, text::Overrun::TrimWordEllipsis));
    CHECK(source.substr(words.lines[0].start, words.lines[0].end) == "Continue");
    CHECK(words.lines[0].width <= width);
    CHECK(words.lines[0].count == 9); // eight letters and the ellipsis

    text::TextLayout fits = lay_out(fonts, source);
    CHECK_FALSE(fonts.trim(fits, 0, 1000.0F, text::Overrun::TrimEllipsis));
}

TEST_CASE("glyph images are made once into atlas pages") {
    MemoryFiles files;
    asset::Assets assets(files.source());
    text::Fonts fonts(assets);
    const text::TextLayout layout = lay_out(fonts, "H ");
    const text::Glyph& h = layout.glyphs[0];

    const auto image = fonts.glyph(h.face, h.index, 16);
    REQUIRE(image.has_value());
    CHECK(fonts.glyph_count() == 1);
    CHECK(image->scale == 1.0F);
    CHECK(image->top < 0.0F); // above the baseline
    REQUIRE(fonts.pages().size() == 1);
    const text::AtlasPage& page = fonts.pages()[0];
    CHECK_FALSE(page.msdf);
    // The border texels are clear and the inside is covered.
    std::size_t covered = 0;
    for (std::uint32_t y = 0; y < image->height; ++y) {
        for (std::uint32_t x = 0; x < image->width; ++x) {
            const std::uint8_t alpha =
                page.rgba[((static_cast<std::size_t>(image->y + y) * page.width) + image->x + x) *
                              4 +
                          3];
            if (x == 0 || y == 0 || x + 1 == image->width || y + 1 == image->height) {
                CHECK(alpha == 0);
            } else if (alpha > 0) {
                ++covered;
            }
        }
    }
    CHECK(covered > 20);
    const std::uint64_t version = page.version;
    CHECK(fonts.glyph(h.face, h.index, 16)->x == image->x);
    CHECK(fonts.pages()[0].version == version);
    // Spaces draw nothing.
    CHECK_FALSE(fonts.glyph(h.face, layout.glyphs[1].index, 16).has_value());
    // An outline is a bigger image.
    const auto outline = fonts.glyph(h.face, h.index, 16, 8);
    REQUIRE(outline.has_value());
    CHECK(outline->width > image->width);
    CHECK(fonts.glyph_count() == 2);
}

TEST_CASE("font files load through assets and distance field fonts draw from one field") {
    MemoryFiles files;
    files.write("fonts/body.ttf", default_font_bytes());
    files.write(
        "fonts/title.font.json",
        R"({"format": 1, "font": "fonts/body.ttf", "multichannel_signed_distance_field": true})");
    files.write("fonts/broken.ttf", "not a font");
    asset::Assets assets(files.source());
    text::add_asset_types(assets);
    text::Fonts fonts(assets);

    const text::TextLayout body = lay_out(fonts, "T", {.font = "fonts/body.ttf"});
    CHECK(body.width == lay_out(fonts, "T").width);
    CHECK_FALSE(fonts.face_info(body.glyphs[0].face).msdf);

    const text::TextLayout title =
        lay_out(fonts, "T", {.font = "fonts/title.font.json", .font_size = 96});
    const text::FaceInfo info = fonts.face_info(title.glyphs[0].face);
    CHECK(info.msdf);
    CHECK(info.msdf_pixel_range == 16);
    const auto big = fonts.glyph(title.glyphs[0].face, title.glyphs[0].index, 96);
    const auto small = fonts.glyph(title.glyphs[0].face, title.glyphs[0].index, 24);
    REQUIRE(big.has_value());
    REQUIRE(small.has_value());
    CHECK(big->x == small->x); // one field for every size
    CHECK(big->scale == doctest::Approx(2.0));
    CHECK(small->scale == doctest::Approx(0.5));
    CHECK(fonts.glyph_count() == 1);
    // A T's bar is at its top: inside the outline there, outside below it at the same column.
    const text::AtlasPage& page = fonts.pages()[0];
    CHECK(page.msdf);
    const auto median = [&](std::uint32_t x, std::uint32_t y) {
        const std::size_t at =
            ((static_cast<std::size_t>(big->y + y) * page.width) + big->x + x) * 4;
        const int r = page.rgba[at];
        const int g = page.rgba[at + 1];
        const int b = page.rgba[at + 2];
        return std::max(std::min(r, g), std::min(std::max(r, g), b));
    };
    const std::uint32_t column = 8 + 4; // pixel range / 2, then a few texels into the bar
    const std::uint32_t bar = 8 + 2;
    CHECK(median(column, bar) > 128);
    CHECK(median(column, big->height - 8 - 2) < 128);

    auto broken = fonts.layout("x", {.font = "fonts/broken.ttf"});
    REQUIRE_FALSE(broken.ok());
    CHECK(broken.error().message == "fonts/broken.ttf is not a TrueType or OpenType font");
    auto missing = fonts.layout("x", {.font = "fonts/none.otf"});
    CHECK_FALSE(missing.ok());
    auto wrong = fonts.layout("x", {.font = "fonts/body.png"});
    REQUIRE_FALSE(wrong.ok());
    CHECK(wrong.error().message == "\"fonts/body.png\" is not a .ttf, .otf or .font.json file");
}

TEST_CASE(".font.json files are checked") {
    auto parsed = json::parse(R"({"format": 1, "font": "a.ttf", "hinting": "normal",
                                  "antialiasing": "none", "msdf_size": 64})");
    REQUIRE(parsed.ok());
    auto settings = text::parse_font(parsed.value());
    REQUIRE(settings.ok());
    CHECK(settings.value().hinting == text::Hinting::Normal);
    CHECK(settings.value().antialiasing == text::Antialiasing::None);
    CHECK(settings.value().msdf_size == 64);

    auto bad = json::parse(R"({"format": 1, "font": "../a.woff", "hinting": "full"})");
    REQUIRE(bad.ok());
    const auto errors = text::validate_font(bad.value());
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].path == "/hinting");
    auto wrong_file = json::parse(R"({"format": 1, "font": "a.woff"})");
    REQUIRE(wrong_file.ok());
    REQUIRE(text::validate_font(wrong_file.value()).size() == 1);
    CHECK(text::validate_font(wrong_file.value())[0].message == "expected a .ttf or .otf file");
}

TEST_CASE("text helpers upper-case and count characters") {
    CHECK(text::to_upper("café ßtraße σοφία "
                         "привет x1") == "CAFÉ ßTRAßE ΣΟΦΊΑ "
                                         "ПРИВЕТ X1");
    CHECK(text::first_characters("héllo", 2) == "hé");
    CHECK(text::first_characters("hi", 5) == "hi");
    CHECK(text::character_index("héllo", 3) == 2);
}
