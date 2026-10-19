#include "syntax_highlighter.hpp"

#include "slash/core/text.hpp"

#include <algorithm>
#include <cstddef>
#include <map>

namespace slash::ui {
namespace {

// Godot's Color(): opaque black, which a keyword's color can't be and colors start from.
constexpr Vec4 kNoColor{0.0F, 0.0F, 0.0F, 1.0F};

bool is_digit(char32_t c) {
    return c >= U'0' && c <= U'9';
}

bool is_hex_digit(char32_t c) {
    return is_digit(c) || (c >= U'a' && c <= U'f') || (c >= U'A' && c <= U'F');
}

bool is_ascii_alphabet_char(char32_t c) {
    return (c >= U'a' && c <= U'z') || (c >= U'A' && c <= U'Z');
}

// set_color_regions: "start end" split at spaces, as get_slicec(' ', 0) and (' ', 1).
std::pair<std::u32string, std::u32string> split_key(const std::string& key) {
    const std::u32string all = to_utf32(key);
    const std::size_t space = all.find(U' ');
    if (space == std::u32string::npos) {
        return {all, {}};
    }
    const std::size_t end = all.find(U' ', space + 1);
    return {all.substr(0, space),
            all.substr(space + 1,
                       end == std::u32string::npos ? std::u32string::npos : end - space - 1)};
}

// Why add_color_region refuses keys, or "".
std::string key_error(const std::u32string& start, const std::u32string& end) {
    // Godot takes an empty start key, which then starts a region at every symbol.
    if (start.empty()) {
        return "a color region needs a start key";
    }
    for (const char32_t c : start) {
        if (!is_symbol(c)) {
            return "color regions must start with a symbol";
        }
    }
    for (const char32_t c : end) {
        if (!is_symbol(c)) {
            return "color regions must end with a symbol";
        }
    }
    return {};
}

} // namespace

CodeHighlight::CodeHighlight(const world::CodeHighlighter& highlighter, Vec4 font_color)
    : highlighter_(&highlighter), font_color_(font_color) {
    for (const auto& [key, color] : highlighter.color_regions) {
        auto [start, end] = split_key(key);
        if (!key_error(start, end).empty()) {
            continue;
        }
        std::size_t at = 0;
        bool taken = false;
        for (const Region& region : regions_) {
            taken = taken || region.start_key == start;
            if (start.size() < region.start_key.size()) {
                ++at;
            }
        }
        if (taken) {
            continue;
        }
        const bool line_only = end.empty();
        regions_.insert(regions_.begin() + static_cast<std::ptrdiff_t>(at),
                        Region{.start_key = std::move(start),
                               .end_key = std::move(end),
                               .color = color,
                               .line_only = line_only});
    }
}

Vec4 CodeHighlight::keyword(std::u32string_view str, int from, int to) const {
    const std::string word =
        to_utf8(str.substr(static_cast<std::size_t>(from), static_cast<std::size_t>(to - from)));
    if (const auto found = highlighter_->keyword_colors.find(word);
        found != highlighter_->keyword_colors.end()) {
        return found->second;
    }
    const auto found = highlighter_->member_keyword_colors.find(word);
    if (found == highlighter_->member_keyword_colors.end()) {
        return kNoColor;
    }
    // Member indexing isn't allowed: not after a '.'.
    for (int k = from - 1; k >= 0; --k) {
        if (str[static_cast<std::size_t>(k)] == U'.') {
            return kNoColor;
        }
        if (str[static_cast<std::size_t>(k)] > 32) {
            break;
        }
    }
    return found->second;
}

ColorMap CodeHighlight::line(std::u32string_view str, int& region) const {
    // Godot's Dictionary, whose keys go in as the columns grow.
    std::map<int, Vec4> color_map;
    bool prev_is_char = false;
    bool prev_is_number = false;
    bool in_keyword = false;
    bool in_word = false;
    bool in_function_name = false;
    bool in_member_variable = false;
    bool is_hex_notation = false;
    Vec4 keyword_color = kNoColor;
    int in_region = region;
    region = -1;
    const auto line_length = static_cast<int>(str.size());
    // A String reads its terminating zero at its length.
    const auto at = [&](int i) {
        return i >= 0 && i < line_length ? str[static_cast<std::size_t>(i)] : U'\0';
    };
    const auto has_escape_from = [&](int from) {
        return str.find(U'\\', static_cast<std::size_t>(std::max(from, 0))) !=
               std::u32string_view::npos;
    };
    const world::CodeHighlighter& h = *highlighter_;
    Vec4 prev_color = kNoColor;

    if (in_region != -1 && line_length == 0) {
        region = in_region;
    }
    for (int j = 0; j < line_length; ++j) {
        Vec4 color = font_color_;
        bool is_char = !is_symbol(at(j));
        bool is_a_symbol = is_symbol(at(j));
        bool is_number = is_digit(at(j));

        // Color regions.
        if (is_a_symbol || in_region != -1) {
            int from = j;
            if (in_region == -1) {
                for (; from < line_length; ++from) {
                    if (at(from) == U'\\') {
                        ++from;
                        continue;
                    }
                    break;
                }
            }
            if (from != line_length) {
                // Entering a region?
                if (in_region == -1) {
                    for (std::size_t c = 0; c < regions_.size(); ++c) {
                        const Region& r = regions_[c];
                        const auto start_length = static_cast<int>(r.start_key.size());
                        const auto end_length = static_cast<int>(r.end_key.size());
                        if (line_length - from < start_length ||
                            str.substr(static_cast<std::size_t>(from),
                                       static_cast<std::size_t>(start_length)) != r.start_key) {
                            continue;
                        }
                        in_region = static_cast<int>(c);
                        from += start_length;
                        // The whole line?
                        if (end_length == 0 || r.line_only || from + end_length > line_length) {
                            // A string with a '\' left goes on, to color its escapes.
                            if (from + end_length > line_length &&
                                (r.start_key == U"\"" || r.start_key == U"'") &&
                                has_escape_from(from)) {
                                break;
                            }
                            prev_color = r.color;
                            color_map[j] = r.color;
                            j = line_length;
                            if (!r.line_only) {
                                region = static_cast<int>(c);
                            }
                        }
                        break;
                    }
                    if (j == line_length) {
                        continue;
                    }
                }
                // In one: find its end key.
                if (in_region != -1) {
                    const Region& r = regions_[static_cast<std::size_t>(in_region)];
                    const bool is_string = r.start_key == U"\"" || r.start_key == U"'";
                    prev_color = r.color;
                    color_map[j] = r.color;
                    int region_end_index = -1;
                    const auto end_length = static_cast<int>(r.end_key.size());
                    for (; from < line_length; ++from) {
                        if (line_length - from < end_length) {
                            // Not while a '\' is left, to color the escapes.
                            if (!is_string || !has_escape_from(from)) {
                                break;
                            }
                        }
                        if (!is_symbol(at(from))) {
                            continue;
                        }
                        if (at(from) == U'\\') {
                            if (is_string) {
                                color_map[from] = h.symbol_color;
                            }
                            ++from;
                            if (is_string) {
                                prev_color = r.color;
                                color_map[from + 1] = r.color;
                            }
                            continue;
                        }
                        region_end_index = from;
                        for (int k = 0; k < end_length; ++k) {
                            if (r.end_key[static_cast<std::size_t>(k)] != at(from + k)) {
                                region_end_index = -1;
                                break;
                            }
                        }
                        if (region_end_index != -1) {
                            break;
                        }
                    }
                    j = from + (end_length - 1);
                    if (region_end_index == -1) {
                        region = in_region;
                    }
                    in_region = -1;
                    prev_is_char = false;
                    prev_is_number = false;
                    continue;
                }
            }
        }

        const char32_t ch = at(j);
        // Allow ABCDEF in hex notation.
        if (is_hex_notation && (is_hex_digit(ch) || is_number)) {
            is_number = true;
        } else {
            is_hex_notation = false;
        }
        // A dot, an underscore, an 'x' for hex notation, an 'f' or an exponent's 'e' in a number.
        if ((ch == U'.' || ch == U'x' || ch == U'X' || ch == U'_' || ch == U'f' || ch == U'e' ||
             ch == U'E') &&
            !in_word && prev_is_number && !is_number) {
            is_number = true;
            is_a_symbol = false;
            is_char = false;
            if ((ch == U'x' || ch == U'X') && at(j - 1) == U'0') {
                is_hex_notation = true;
            }
        }
        if (!in_word && (is_ascii_alphabet_char(ch) || ch == U'_') && !is_number) {
            in_word = true;
        }
        if ((in_keyword || in_word) && !is_hex_notation) {
            is_number = false;
        }
        if (is_a_symbol && ch != U'.' && in_word) {
            in_word = false;
        }
        if (!is_char) {
            in_keyword = false;
        }
        if (!in_keyword && is_char && !prev_is_char) {
            int to = j;
            while (to < line_length && !is_symbol(at(to))) {
                ++to;
            }
            if (const Vec4 col = keyword(str, j, to); col != kNoColor) {
                in_keyword = true;
                keyword_color = col;
            }
        }
        if (!in_function_name && in_word && !in_keyword) {
            int k = j;
            while (k < line_length && !is_symbol(at(k)) && at(k) != U'\t' && at(k) != U' ') {
                ++k;
            }
            // A space between the name and the bracket.
            while (k < line_length && (at(k) == U'\t' || at(k) == U' ')) {
                ++k;
            }
            if (at(k) == U'(') {
                in_function_name = true;
            }
        }
        if (!in_function_name && !in_member_variable && !in_keyword && !is_number && in_word) {
            int k = j;
            while (k > 0 && !is_symbol(at(k)) && at(k) != U'\t' && at(k) != U' ') {
                --k;
            }
            if (at(k) == U'.') {
                in_member_variable = true;
            }
        }
        if (is_a_symbol) {
            in_function_name = false;
            in_member_variable = false;
        }
        if (in_keyword) {
            color = keyword_color;
        } else if (in_member_variable) {
            color = h.member_variable_color;
        } else if (in_function_name) {
            color = h.function_color;
        } else if (is_a_symbol) {
            color = h.symbol_color;
        } else if (is_number) {
            color = h.number_color;
        }
        prev_is_char = is_char;
        prev_is_number = is_number;
        if (color != prev_color) {
            prev_color = color;
            color_map[j] = color;
        }
    }
    return {color_map.begin(), color_map.end()};
}

std::string color_region_error(const world::CodeHighlighter& highlighter, const std::string& key) {
    const auto [start, end] = split_key(key);
    if (std::string error = key_error(start, end); !error.empty()) {
        return error;
    }
    for (const auto& [other, color] : highlighter.color_regions) {
        if (other == key) {
            break;
        }
        const auto [other_start, other_end] = split_key(other);
        if (other_start == start && key_error(other_start, other_end).empty()) {
            return "the color region " + other + " has the same start key";
        }
    }
    return {};
}

} // namespace slash::ui
