#include "../engine/ui/src/syntax_highlighter.hpp"

#include "slash/asset/assets.hpp"
#include "slash/text/text.hpp"
#include "slash/ui/ui.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/world.hpp"

#include <doctest/doctest.h>

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// CodeHighlighter, as Godot's (scene/resources/syntax_highlighter.cpp): the colors expected here
// follow _get_line_syntax_highlighting_impl step by step.

namespace asset = slash::asset;
namespace ui = slash::ui;
namespace world = slash::world;
using slash::Vec4;

namespace {

constexpr Vec4 kFont{0.9F, 0.9F, 0.9F, 1.0F};
constexpr Vec4 kNumber{0.0F, 1.0F, 0.0F, 1.0F};
constexpr Vec4 kSymbol{0.0F, 0.0F, 1.0F, 1.0F};
constexpr Vec4 kFunction{1.0F, 1.0F, 0.0F, 1.0F};
constexpr Vec4 kMember{0.0F, 1.0F, 1.0F, 1.0F};
constexpr Vec4 kKeyword{1.0F, 0.0F, 0.0F, 1.0F};
constexpr Vec4 kSelf{1.0F, 0.0F, 1.0F, 1.0F};
constexpr Vec4 kString{0.5F, 0.5F, 0.0F, 1.0F};
constexpr Vec4 kComment{0.5F, 0.5F, 0.5F, 1.0F};
constexpr Vec4 kBlock{0.2F, 0.2F, 0.2F, 1.0F};

world::CodeHighlighter highlighter() {
    return {.number_color = kNumber,
            .symbol_color = kSymbol,
            .function_color = kFunction,
            .member_variable_color = kMember,
            .keyword_colors = {{"if", kKeyword}, {"return", kKeyword}},
            .member_keyword_colors = {{"self", kSelf}},
            .color_regions = {{"\" \"", kString}, {"#", kComment}, {"/* */", kBlock}}};
}

// One line's colors, from the region open before it; region becomes the one open after it.
ui::ColorMap colors(std::u32string_view line, int& region) {
    const world::CodeHighlighter h = highlighter();
    return ui::CodeHighlight(h, kFont).line(line, region);
}
ui::ColorMap colors(std::u32string_view line) {
    int region = -1;
    return colors(line, region);
}

} // namespace

TEST_CASE("keywords, function names, symbols and numbers take their colors") {
    CHECK(
        colors(U"if x(1)") ==
        ui::ColorMap{
            {0, kKeyword}, {2, kSymbol}, {3, kFunction}, {4, kSymbol}, {5, kNumber}, {6, kSymbol}});
    // A space between a function's name and its bracket.
    CHECK(colors(U"foo (1)") ==
          ui::ColorMap{{0, kFunction}, {3, kSymbol}, {5, kNumber}, {6, kSymbol}});
    // A keyword inside a longer word is no keyword, and digits in a word are no number.
    CHECK(colors(U"iffy x1") == ui::ColorMap{{0, kFont}, {4, kSymbol}, {5, kFont}});
}

TEST_CASE("numbers take hex digits, decimal points and exponents") {
    // After a '.', a word is a member variable; 0x1F is one number and so is 2.5e3.
    CHECK(colors(U"a.b 0x1F 2.5e3") == ui::ColorMap{{0, kFont},
                                                    {1, kSymbol},
                                                    {2, kMember},
                                                    {3, kSymbol},
                                                    {4, kNumber},
                                                    {8, kSymbol},
                                                    {9, kNumber}});
}

TEST_CASE("member keywords take their color, but not after a '.'") {
    CHECK(colors(U"self.x") == ui::ColorMap{{0, kSelf}, {4, kSymbol}, {5, kMember}});
    CHECK(colors(U"a.self") == ui::ColorMap{{0, kFont}, {1, kSymbol}, {2, kMember}});
}

TEST_CASE("strings color their escapes, and a region to the line's end colors the rest") {
    int region = -1;
    // The backslash and the character it escapes are symbols; the comment runs to the end.
    CHECK(colors(U"x = \"a\\\"b\" # note", region) == ui::ColorMap{{0, kFont},
                                                                   {1, kSymbol},
                                                                   {4, kString},
                                                                   {6, kSymbol},
                                                                   {8, kString},
                                                                   {10, kSymbol},
                                                                   {11, kComment}});
    CHECK(region == -1);
}

TEST_CASE("a color region left open goes on into the lines after it") {
    int region = -1;
    CHECK(colors(U"x /* open", region) == ui::ColorMap{{0, kFont}, {1, kSymbol}, {2, kBlock}});
    const int block = region;
    CHECK(block != -1);
    // An empty line keeps it open; the next line is in it up to its end key.
    CHECK(colors(U"", region).empty());
    CHECK(region == block);
    CHECK(colors(U"still */ y", region) == ui::ColorMap{{0, kBlock}, {8, kSymbol}, {9, kFont}});
    CHECK(region == -1);
    // A whole line inside one.
    region = block;
    CHECK(colors(U"inside", region) == ui::ColorMap{{0, kBlock}});
    CHECK(region == block);
}

TEST_CASE("color region keys must be symbols, one region per start key") {
    world::CodeHighlighter h;
    h.color_regions = {{"\" \"", kString}, {"\" '", kString}, {"a b", kString},
                       {"< x", kString},   {"", kString},     {"//", kComment}};
    CHECK(ui::color_region_error(h, "\" \"").empty());
    CHECK(ui::color_region_error(h, "\" '") == "the color region \" \" has the same start key");
    CHECK(ui::color_region_error(h, "a b") == "color regions must start with a symbol");
    CHECK(ui::color_region_error(h, "< x") == "color regions must end with a symbol");
    CHECK(ui::color_region_error(h, "") == "a color region needs a start key");
    CHECK(ui::color_region_error(h, "//").empty());
    // Bad keys are left out and '"' keeps its first end key, so "'" doesn't end the string and
    // the "//" inside it starts no comment.
    int region = -1;
    CHECK(ui::CodeHighlight(h, kFont).line(U"\"a' // b", region) == ui::ColorMap{{0, kString}});
    CHECK(region != -1);
}

TEST_CASE("check finds a CodeHighlighter with no TextEdit and color regions Godot refuses") {
    asset::Assets assets{asset::FileSource{
        .read = [](std::string_view) -> slash::Result<std::string> {
            return slash::make_error(slash::ErrorCode::NotFound, "no files");
        },
        .stamp = [](std::string_view) -> std::optional<asset::FileStamp> { return std::nullopt; },
    }};
    world::ComponentRegistry components;
    world::register_builtin_components(components);
    world::World w{components};
    ui::install(w, assets,
                {.viewport = {400.0F, 300.0F}, .theme = "", .allow_system_fallback = false});
    const entt::entity code = w.create("code").value();
    w.ecs().emplace<world::Control>(code);
    w.ecs().emplace<world::TextEdit>(code);
    world::CodeHighlighter regions;
    regions.color_regions = {{"/* */", kBlock}, {"/* x", kBlock}, {"a", kComment}};
    w.ecs().emplace<world::CodeHighlighter>(code, regions);
    const entt::entity loose = w.create("loose").value();
    w.ecs().emplace<world::CodeHighlighter>(loose);
    std::vector<std::string> lines;
    for (const ui::Problem& problem : ui::check(w, *ui::fonts(w))) {
        lines.push_back(problem.id + problem.path + ": " + problem.message);
    }
    CHECK(lines ==
          std::vector<std::string>{
              "code/components/CodeHighlighter/color_regions/~1* x: color regions must end with "
              "a symbol",
              "code/components/CodeHighlighter/color_regions/a: color regions must start with a "
              "symbol",
              "loose/components/CodeHighlighter: a CodeHighlighter colors the TextEdit on its "
              "entity, and this one has none",
          });
}
