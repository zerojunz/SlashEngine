#pragma once

#include "slash/ui/ui.hpp"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Godot's CodeHighlighter (scene/resources/syntax_highlighter.cpp): the colors a TextEdit's
// lines take, and what is wrong with a CodeHighlighter's color regions.
namespace slash::ui {

// Godot's is_symbol: ASCII punctuation but '_', and tabs and spaces.
[[nodiscard]] constexpr bool is_symbol(char32_t c) {
    return c != U'_' &&
           ((c >= U'!' && c <= U'/') || (c >= U':' && c <= U'@') || (c >= U'[' && c <= U'`') ||
            (c >= U'{' && c <= U'~') || c == U'\t' || c == U' ');
}

// Where each color of a line starts, in columns, in order: a color runs to the next one, and
// the line starts in the TextEdit's own color.
using ColorMap = std::vector<std::pair<int, Vec4>>;

// A CodeHighlighter ready to color lines: its color regions as add_color_region keeps them,
// longest start key first, and the TextEdit's font color.
class CodeHighlight {
public:
    CodeHighlight(const world::CodeHighlighter& highlighter, Vec4 font_color);

    // _get_line_syntax_highlighting_impl for one line. region is the color region still open at
    // the end of the line before (-1 for none), as color_region_cache keeps it, and becomes this
    // line's.
    [[nodiscard]] ColorMap line(std::u32string_view str, int& region) const;

private:
    struct Region {
        std::u32string start_key;
        std::u32string end_key;
        Vec4 color;
        bool line_only = false;
    };
    [[nodiscard]] Vec4 keyword(std::u32string_view str, int from, int to) const;

    const world::CodeHighlighter* highlighter_;
    Vec4 font_color_;
    std::vector<Region> regions_;
};

// Why a color region's key is wrong, or "": add_color_region takes keys of symbols only, a start
// and an optional end separated by a space, and one region per start key.
[[nodiscard]] std::string color_region_error(const world::CodeHighlighter& highlighter,
                                             const std::string& key);

} // namespace slash::ui
