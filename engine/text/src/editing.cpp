#include "slash/core/text.hpp"
#include "slash/text/text.hpp"

#include <SheenBidi/SheenBidi.h>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Carets, selections and clicks over a shaped line, as Godot's TextServer does them
// (servers/text/text_server.cpp: shaped_text_get_carets, shaped_text_get_selection,
// shaped_text_hit_test_position, shaped_text_get_word_breaks and the character positions), and
// character breaks as Unicode's UAX #29 extended grapheme clusters, without the Prepend rule.
namespace slash::text {
namespace {

// The Grapheme_Cluster_Break classes the rules need.
enum class Break : std::uint8_t {
    Other,
    Cr,
    Lf,
    Control,
    Extend,
    Zwj,
    RegionalIndicator,
    SpacingMark,
    L,
    V,
    T,
    Lv,
    Lvt,
};

SBGeneralCategory category(char32_t c) {
    return SBCodepointGetGeneralCategory(static_cast<SBCodepoint>(c));
}

// Extended_Pictographic, closely enough for emoji sequences: the emoji blocks and the older
// symbols that have emoji forms.
bool is_pictographic(char32_t c) {
    if (c >= 0x1F1E6U && c <= 0x1F1FFU) {
        return false; // regional indicators
    }
    if (c >= 0x1F3FBU && c <= 0x1F3FFU) {
        return false; // skin tone modifiers, which extend
    }
    return c == 0xA9U || c == 0xAEU || c == 0x203CU || c == 0x2049U || c == 0x2122U ||
           c == 0x2139U || (c >= 0x2194U && c <= 0x21AAU) || (c >= 0x231AU && c <= 0x23FFU) ||
           c == 0x24C2U || (c >= 0x25AAU && c <= 0x25FEU) || (c >= 0x2600U && c <= 0x27BFU) ||
           (c >= 0x2934U && c <= 0x2935U) || (c >= 0x2B05U && c <= 0x2B55U) || c == 0x3030U ||
           c == 0x303DU || c == 0x3297U || c == 0x3299U || (c >= 0x1F000U && c <= 0x1FAFFU) ||
           (c >= 0x1FC00U && c <= 0x1FFFDU);
}

Break break_of(char32_t c) {
    if (c == U'\r') {
        return Break::Cr;
    }
    if (c == U'\n') {
        return Break::Lf;
    }
    if (c == 0x200DU) {
        return Break::Zwj;
    }
    if (c == 0x200CU || (c >= 0x1F3FBU && c <= 0x1F3FFU) || (c >= 0xE0020U && c <= 0xE007FU) ||
        c == 0xFF9EU || c == 0xFF9FU) {
        return Break::Extend;
    }
    if (c >= 0x1F1E6U && c <= 0x1F1FFU) {
        return Break::RegionalIndicator;
    }
    if ((c >= 0x1100U && c <= 0x115FU) || (c >= 0xA960U && c <= 0xA97CU)) {
        return Break::L;
    }
    if ((c >= 0x1160U && c <= 0x11A7U) || (c >= 0xD7B0U && c <= 0xD7C6U)) {
        return Break::V;
    }
    if ((c >= 0x11A8U && c <= 0x11FFU) || (c >= 0xD7CBU && c <= 0xD7FBU)) {
        return Break::T;
    }
    if (c >= 0xAC00U && c <= 0xD7A3U) {
        constexpr char32_t kSyllables = 28;
        return (c - 0xAC00U) % kSyllables == 0 ? Break::Lv : Break::Lvt;
    }
    switch (category(c)) {
    case SBGeneralCategoryMN:
    case SBGeneralCategoryME:
        return Break::Extend;
    case SBGeneralCategoryMC:
        return Break::SpacingMark;
    case SBGeneralCategoryCC:
    case SBGeneralCategoryZL:
    case SBGeneralCategoryZP:
    case SBGeneralCategoryCF:
        return Break::Control;
    default:
        return Break::Other;
    }
}

// Godot's is_whitespace (core/string/char_utils.h).
bool is_whitespace(char32_t c) {
    return c == U' ' || c == 0xA0U || c == 0x1680U || (c >= 0x2000U && c <= 0x200AU) ||
           c == 0x202FU || c == 0x205FU || c == 0x3000U || c == 0x2028U || c == 0x2029U ||
           (c >= 0x09U && c <= 0x0DU) || c == 0x85U;
}

// u_ispunct, without '_', as TextServerAdvanced flags GRAPHEME_IS_PUNCTUATION.
bool is_punctuation(char32_t c) {
    const SBGeneralCategory found = category(c);
    return c != U'_' && found >= SBGeneralCategoryPC && found <= SBGeneralCategoryPO;
}

// Whether UAX #29's rules keep two characters in one extended grapheme cluster: previous and
// next are their break properties, pictographic_run says an ExtPict Extend* ZWJ run ends at
// previous, and regional counts the regional indicators up to it.
bool joins(Break previous, Break next, char32_t c, bool pictographic_run, int regional) {
    if (previous == Break::Cr && next == Break::Lf) {
        return true; // GB3
    }
    if (previous == Break::Cr || previous == Break::Lf || previous == Break::Control ||
        next == Break::Cr || next == Break::Lf || next == Break::Control) {
        return false; // GB4, GB5
    }
    if (previous == Break::L &&
        (next == Break::L || next == Break::V || next == Break::Lv || next == Break::Lvt)) {
        return true; // GB6
    }
    if ((previous == Break::Lv || previous == Break::V) && (next == Break::V || next == Break::T)) {
        return true; // GB7
    }
    if ((previous == Break::Lvt || previous == Break::T) && next == Break::T) {
        return true; // GB8
    }
    if (next == Break::Extend || next == Break::Zwj || next == Break::SpacingMark) {
        return true; // GB9, GB9a
    }
    if (previous == Break::Zwj && pictographic_run && is_pictographic(c)) {
        return true; // GB11
    }
    if (previous == Break::RegionalIndicator && next == Break::RegionalIndicator) {
        return regional % 2 == 1; // GB12, GB13
    }
    return false; // GB999
}

} // namespace

std::vector<std::uint32_t> character_breaks(std::u32string_view text) {
    std::vector<std::uint32_t> breaks;
    if (text.empty()) {
        return breaks;
    }
    Break previous = break_of(text.front());
    bool pictographic_run = is_pictographic(text.front()); // ExtPict Extend* (ZWJ)?
    int regional = previous == Break::RegionalIndicator ? 1 : 0;
    for (std::size_t i = 1; i < text.size(); ++i) {
        const char32_t c = text[i];
        const Break next = break_of(c);
        if (!joins(previous, next, c, pictographic_run, regional)) {
            breaks.push_back(static_cast<std::uint32_t>(i));
        }
        // ExtPict Extend* ZWJ carries on to the next character.
        if (is_pictographic(c)) {
            pictographic_run = true;
        } else if (next != Break::Extend && next != Break::Zwj) {
            pictographic_run = false;
        }
        regional = next == Break::RegionalIndicator ? regional + 1 : 0;
        previous = next;
    }
    breaks.push_back(static_cast<std::uint32_t>(text.size()));
    return breaks;
}

ShapedLine::ShapedLine(std::string_view text, const TextLayout& layout, std::size_t line) {
    if (line >= layout.lines.size()) {
        return;
    }
    const Line& at = layout.lines[line];
    height_ = layout.metrics.height();
    // Character indices of the text's bytes.
    std::vector<std::uint32_t> char_of(text.size() + 1, 0);
    std::uint32_t count = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const bool lead = (static_cast<unsigned char>(text[i]) & 0xC0U) != 0x80U;
        char_of[i] = lead ? count++ : std::max(count, 1U) - 1;
    }
    char_of[text.size()] = count;
    const auto to_char = [&](std::uint32_t byte) {
        return char_of[std::min<std::size_t>(byte, text.size())];
    };
    start_ = to_char(at.start);
    end_ = to_char(at.end);

    // Each cluster ends where the next one in the text starts.
    std::vector<std::uint32_t> starts;
    for (std::uint32_t g = at.first; g < at.first + at.count; ++g) {
        starts.push_back(layout.glyphs[g].cluster);
    }
    starts.push_back(at.end);
    std::ranges::sort(starts);
    const auto unique = std::ranges::unique(starts);
    starts.erase(unique.begin(), unique.end());

    float pen = 0.0F;
    for (std::uint32_t g = at.first; g < at.first + at.count;) {
        const Glyph& head = layout.glyphs[g];
        Cluster cluster;
        cluster.first = g;
        cluster.x = pen;
        cluster.rtl = head.rtl;
        while (g < at.first + at.count && layout.glyphs[g].cluster == head.cluster) {
            cluster.advance += layout.glyphs[g].advance;
            ++g;
            ++cluster.count;
        }
        pen += cluster.advance;
        const auto next = std::ranges::upper_bound(starts, head.cluster);
        cluster.start = to_char(head.cluster);
        cluster.end = to_char(next != starts.end() ? *next : at.end);
        if (head.cluster < text.size()) {
            std::size_t byte = head.cluster;
            const char32_t c = next_utf8(text, byte);
            cluster.space = is_whitespace(c);
            cluster.punctuation = is_punctuation(c);
        }
        clusters_.push_back(cluster);
    }
    width_ = pen;
    origin_ = at.rtl ? width_ - at.width : 0.0F;

    const std::u32string chars = to_utf32(text.substr(at.start, at.end - at.start));
    breaks_ = character_breaks(chars);
    for (std::uint32_t& each : breaks_) {
        each += start_;
    }
}

ShapedLine::Carets ShapedLine::carets(std::uint32_t position) const {
    Carets carets;
    const float half = height_ / 2.0F;
    for (const Cluster& cluster : clusters_) {
        if (cluster.end <= cluster.start) {
            continue;
        }
        const float char_advance =
            cluster.advance / static_cast<float>(cluster.end - cluster.start);
        // Before the cluster.
        if (position == cluster.start) {
            Caret& caret = carets.trailing;
            caret.found = true;
            caret.top = 0.0F;
            caret.height = cluster.start == start_ ? height_ : half;
            caret.x = cluster.rtl ? cluster.x + cluster.advance : cluster.x;
            caret.direction = cluster.rtl ? Direction::Rtl : Direction::Ltr;
        }
        // After it.
        if (position == cluster.end) {
            Caret& caret = carets.leading;
            caret.found = true;
            caret.top = cluster.end == end_ ? 0.0F : half;
            caret.height = cluster.end == end_ ? height_ : half;
            caret.x = cluster.rtl ? cluster.x : cluster.x + cluster.advance;
            caret.direction = cluster.rtl ? Direction::Rtl : Direction::Ltr;
        }
        // Inside it: a ligature, split evenly.
        if (position > cluster.start && position < cluster.end) {
            Caret caret;
            caret.found = true;
            caret.top = 0.0F;
            caret.height = height_;
            caret.x =
                cluster.rtl
                    ? cluster.x + (char_advance * static_cast<float>(cluster.end - position))
                    : cluster.x + (char_advance * static_cast<float>(position - cluster.start));
            carets.leading = caret;
            carets.trailing = caret;
        }
    }
    return carets;
}

std::vector<std::pair<float, float>> ShapedLine::selection(std::uint32_t start,
                                                           std::uint32_t end) const {
    std::vector<std::pair<float, float>> ranges;
    if (start == end) {
        return ranges;
    }
    if (start > end) {
        std::swap(start, end);
    }
    const auto push = [&ranges](float from, float to) {
        constexpr float kEpsilon = 0.00001F; // UNIT_EPSILON
        if (!ranges.empty() && std::abs(ranges.back().second - from) <= kEpsilon) {
            ranges.back().second = to;
        } else {
            ranges.emplace_back(from, to);
        }
    };
    for (const Cluster& cluster : clusters_) {
        if (cluster.start >= end || cluster.end <= start || cluster.end <= cluster.start) {
            continue;
        }
        const float off = cluster.x;
        const float advance = cluster.advance;
        const float char_advance = advance / static_cast<float>(cluster.end - cluster.start);
        const auto chars = [](std::uint32_t a, std::uint32_t b) {
            return static_cast<float>(a - b);
        };
        if (cluster.start >= start && cluster.end <= end) {
            push(off, off + advance);
        } else if (cluster.start >= start) {
            // Only its start is selected.
            if (cluster.rtl) {
                push(off + (char_advance * chars(cluster.end, end)), off + advance);
            } else {
                push(off, off + (char_advance * chars(end, cluster.start)));
            }
        } else if (cluster.end <= end) {
            // Only its end.
            if (cluster.rtl) {
                push(off, off + (char_advance * chars(cluster.end, start)));
            } else {
                push(off + (char_advance * chars(start, cluster.start)), off + advance);
            }
        } else {
            // The selection is inside it.
            if (cluster.rtl) {
                push(off + (char_advance * chars(cluster.end, end)),
                     off + (char_advance * chars(cluster.end, start)));
            } else {
                push(off + (char_advance * chars(start, cluster.start)),
                     off + (char_advance * chars(end, cluster.start)));
            }
        }
    }
    return ranges;
}

std::uint32_t ShapedLine::hit_test(float x) const {
    if (clusters_.empty()) {
        return start_;
    }
    if (std::floor(x) <= 0.0F) {
        const Cluster& first = clusters_.front();
        return first.rtl ? first.end : first.start;
    }
    if (std::ceil(x) >= width_) {
        const Cluster& last = clusters_.back();
        return last.rtl ? last.start : last.end;
    }
    for (const Cluster& cluster : clusters_) {
        const float off = cluster.x;
        const float advance = cluster.advance;
        if (x < off || x >= off + advance) {
            continue;
        }
        if (cluster.end > cluster.start + 1) {
            // A ligature: the nearer side of the character clicked.
            const std::uint32_t count = cluster.end - cluster.start;
            const float char_advance = advance / static_cast<float>(count);
            float sub = off;
            for (std::uint32_t j = 0; j < count; ++j) {
                if (x >= sub && x < sub + (char_advance / 2.0F)) {
                    return cluster.rtl ? cluster.end - j : cluster.start + j;
                }
                if (x >= sub + (char_advance / 2.0F) && x < sub + char_advance) {
                    return cluster.rtl ? cluster.start + (count - 1) - j
                                       : cluster.end - (count - 1) + j;
                }
                sub += char_advance;
            }
        }
        if (x < off + (advance / 2.0F)) {
            return cluster.rtl ? cluster.end : cluster.start;
        }
        return cluster.rtl ? cluster.start : cluster.end;
    }
    return end_;
}

std::vector<std::uint32_t> ShapedLine::word_breaks() const {
    std::vector<std::uint32_t> words;
    std::vector<const Cluster*> logical;
    logical.reserve(clusters_.size());
    for (const Cluster& cluster : clusters_) {
        logical.push_back(&cluster);
    }
    std::ranges::stable_sort(logical, {}, &Cluster::start);
    std::uint32_t word_start = start_;
    for (const Cluster* cluster : logical) {
        if (!cluster->space && !cluster->punctuation) {
            continue;
        }
        if (word_start < cluster->start) {
            words.push_back(word_start);
            words.push_back(cluster->start);
        }
        word_start = cluster->end;
    }
    if (!logical.empty() && word_start < end_) {
        words.push_back(word_start);
        words.push_back(end_);
    }
    return words;
}

std::uint32_t ShapedLine::next_character(std::uint32_t position) const {
    std::uint32_t previous = start_;
    for (const std::uint32_t each : breaks_) {
        if (each > position) {
            return each;
        }
        previous = each;
    }
    return previous;
}

std::uint32_t ShapedLine::previous_character(std::uint32_t position) const {
    std::uint32_t previous = start_;
    for (const std::uint32_t each : breaks_) {
        if (each >= position) {
            return previous;
        }
        previous = each;
    }
    return previous;
}

std::uint32_t ShapedLine::closest_character(std::uint32_t position) const {
    std::uint32_t previous = start_;
    for (const std::uint32_t each : breaks_) {
        if (each == position) {
            return each;
        }
        if (each > position) {
            return each - position < position - previous ? each : previous;
        }
        previous = each;
    }
    return previous;
}

Direction ShapedLine::dominant_direction(std::uint32_t start, std::uint32_t end) const {
    if (start == end) {
        return Direction::Auto;
    }
    if (start > end) {
        std::swap(start, end);
    }
    int rtl = 0;
    int ltr = 0;
    for (const Cluster& cluster : clusters_) {
        if (cluster.end > start && cluster.start < end) {
            (cluster.rtl ? rtl : ltr) += 1;
        }
    }
    if (rtl == ltr) {
        return Direction::Auto;
    }
    return ltr > rtl ? Direction::Ltr : Direction::Rtl;
}

} // namespace slash::text
