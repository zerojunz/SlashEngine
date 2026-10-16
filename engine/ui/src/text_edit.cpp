#include "text_edit.hpp"

#include "slash/core/text.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <string>
#include <utility>
#include <vector>

namespace slash::ui {
namespace {

// Godot's TextEdit tab size: a tab reaches the next multiple of four spaces.
constexpr int kTabSize = 4;
// The tab and space icons (text_edit_tab.svg and text_edit_space.svg): 8 by 8 pixels, #b2b2b2
// at a quarter alpha, drawn modulated by the text's color.
constexpr float kIconSize = 8.0F;
constexpr Vec4 kIconColor{0.698F, 0.698F, 0.698F, 0.25F};

int to_int(float value) {
    return static_cast<int>(value);
}

int to_int(double value) {
    return static_cast<int>(value);
}

Vec4 times(Vec4 a, Vec4 b) {
    return {a.x * b.x, a.y * b.y, a.z * b.z, a.w * b.w};
}

// A plain rect in one color, as canvas_item_add_rect draws.
Box rect_box(Vec2 position, Vec2 size, Vec4 color) {
    Box box;
    box.style.bg_color = color;
    box.style.anti_aliasing = false;
    box.position = position;
    box.size = size;
    return box;
}

// Draws a rect cut to the TextEdit's rect, as its text canvas item clips what it draws.
void push_clipped(std::vector<Box>& out, Vec2 size, Vec2 position, Vec2 extent, Vec4 color) {
    if (extent.x < 0.0F) {
        position.x += extent.x;
        extent.x = -extent.x;
    }
    if (extent.y < 0.0F) {
        position.y += extent.y;
        extent.y = -extent.y;
    }
    const float x0 = std::max(position.x, 0.0F);
    const float y0 = std::max(position.y, 0.0F);
    const float x1 = std::min(position.x + extent.x, size.x);
    const float y1 = std::min(position.y + extent.y, size.y);
    if (x0 >= x1 || y0 >= y1) {
        return;
    }
    out.push_back(rect_box({x0, y0}, {x1 - x0, y1 - y0}, color));
}

// _draw_rect_unfilled: a rect's outline one pixel wide, centered on its edges.
void push_unfilled(std::vector<Box>& out, Vec2 size, Vec2 position, Vec2 extent, Vec4 color) {
    if (extent.x < 0.0F) {
        position.x += extent.x;
        extent.x = -extent.x;
    }
    if (extent.y < 0.0F) {
        position.y += extent.y;
        extent.y = -extent.y;
    }
    if (extent.x <= 1.0F || extent.y <= 1.0F) {
        push_clipped(out, size, position - Vec2{0.5F, 0.5F}, extent + Vec2{1.0F, 1.0F}, color);
        return;
    }
    const Vec2 at = position - Vec2{0.5F, 0.5F};
    push_clipped(out, size, at, {extent.x + 1.0F, 1.0F}, color);
    push_clipped(out, size, {at.x, at.y + extent.y}, {extent.x + 1.0F, 1.0F}, color);
    push_clipped(out, size, {at.x, at.y + 1.0F}, {1.0F, extent.y - 1.0F}, color);
    push_clipped(out, size, {at.x + extent.x, at.y + 1.0F}, {1.0F, extent.y - 1.0F}, color);
}

// What a TextEdit shows for a character of its text: control characters hidden (Godot's
// preserve_control off), and line and paragraph separators as spaces so a line stays one line
// until it wraps; the placeholder breaks at line breaks as a paragraph does.
char32_t shown_character(char32_t c, bool placeholder) {
    if (c == U'\t' || (placeholder && (c == U'\n' || c == U'\r'))) {
        return c;
    }
    if (c < 0x20U || (c >= 0x7FU && c <= 0x9FU)) {
        return 0x200BU;
    }
    if (!placeholder && (c == 0x2028U || c == 0x2029U)) {
        return 0x205FU;
    }
    return c;
}

text::Direction direction_of(const world::TextEdit& edit, bool rtl) {
    if (edit.text_direction == "inherited") {
        return rtl ? text::Direction::Rtl : text::Direction::Ltr;
    }
    return text::parse_direction(edit.text_direction).value_or(text::Direction::Auto);
}

text::Autowrap autowrap_of(const world::TextEdit& edit) {
    if (edit.autowrap_mode == "arbitrary") {
        return text::Autowrap::Arbitrary;
    }
    if (edit.autowrap_mode == "word") {
        return text::Autowrap::Word;
    }
    return text::Autowrap::WordSmart;
}

// A caret as Godot's CaretInfo gives one: a rect from the baseline, and its direction.
struct CaretRect {
    bool found = false;
    Vec2 position;
    Vec2 size;
    text::Direction direction = text::Direction::Auto;
};

struct CaretPair {
    CaretRect leading;
    CaretRect trailing;
};

// shaped_text_get_carets, with rects measured from the baseline: a trailing caret is as wide
// as a character of the cluster after it (negative for right to left), a leading one as wide
// as one of the cluster before it.
CaretPair carets_of(const text::ShapedLine& row, int position, float ascent) {
    CaretPair out;
    const text::ShapedLine::Carets carets = row.carets(static_cast<std::uint32_t>(position));
    const auto convert = [&](const text::ShapedLine::Caret& caret, CaretRect& to) {
        to.found = caret.found;
        to.position = {caret.x, caret.top - ascent};
        to.size = {0.0F, caret.height};
        to.direction = caret.direction;
    };
    convert(carets.leading, out.leading);
    convert(carets.trailing, out.trailing);
    for (const text::ShapedLine::Cluster& cluster : row.clusters()) {
        if (cluster.end <= cluster.start) {
            continue;
        }
        const float char_advance =
            cluster.advance / static_cast<float>(cluster.end - cluster.start);
        if (std::cmp_equal(cluster.start, position) && out.trailing.found &&
            out.trailing.direction != text::Direction::Auto) {
            out.trailing.size.x = cluster.rtl ? -char_advance : char_advance;
        }
        if (std::cmp_equal(cluster.end, position) && out.leading.found &&
            out.leading.direction != text::Direction::Auto) {
            out.leading.size.x = cluster.rtl ? char_advance : -char_advance;
        }
        if (std::cmp_less(cluster.start, position) && std::cmp_greater(cluster.end, position)) {
            out.leading.size.x = cluster.rtl ? -char_advance : char_advance;
            out.trailing.size.x = out.leading.size.x;
        }
    }
    return out;
}

} // namespace

// ---- Lines ----

std::vector<std::u32string> split_lines(std::string_view text) {
    std::vector<std::u32string> lines(1);
    for (const char32_t c : to_utf32(text)) {
        if (c == U'\n') {
            lines.emplace_back();
        } else if (c != U'\r') {
            lines.back().push_back(c);
        }
    }
    return lines;
}

std::string join_lines(const std::vector<std::u32string>& lines) {
    std::u32string all;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) {
            all.push_back(U'\n');
        }
        all += lines[i];
    }
    return to_utf8(all);
}

SelectionRange selection_range(const TextEditState::Caret& caret) {
    SelectionRange range{caret.line, caret.column, caret.line, caret.column};
    const TextEditState::Selection& selection = caret.selection;
    if (!selection.active) {
        return range;
    }
    if (selection.origin_line < caret.line) {
        range = {selection.origin_line, selection.origin_column, caret.line, caret.column};
    } else if (selection.origin_line > caret.line) {
        range = {caret.line, caret.column, selection.origin_line, selection.origin_column};
    } else {
        range.from_column = std::min(selection.origin_column, caret.column);
        range.to_column = std::max(selection.origin_column, caret.column);
    }
    return range;
}

// ---- TextArea ----

// One line shaped as it shows, Godot's Text::Line with its TextParagraph.
struct TextArea::LineData {
    std::string utf8; // what it shows
    text::TextLayout layout;
    std::vector<text::ShapedLine> rows;
    std::vector<std::pair<int, int>> ranges; // each row's [start, end) columns
    std::vector<std::uint32_t> breaks;       // the ends of its user-perceived characters
    int width = 0;                           // its widest row, Line::width
    int height = 0;                          // its tallest row, at least the font's, Line::height
};

void TextArea::Bar::set_value(double to) {
    value = std::max(std::min(to, max - page), 0.0);
}

void TextArea::Bar::set_max(double to) {
    max = std::max(to, 0.0);
    page = std::clamp(page, 0.0, max);
    set_value(value);
}

void TextArea::Bar::set_page(double to) {
    page = std::clamp(to, 0.0, max);
    set_value(value);
}

TextArea::TextArea(text::Fonts& fonts, const TextEditView& view, Vec2 size,
                   const world::TextEdit& edit, std::vector<std::u32string> lines)
    : fonts_(&fonts), view_(&view), size_(size), editable_(edit.editable),
      wrapping_(edit.wrap_mode == "boundary"), autowrap_(autowrap_of(edit)),
      direction_(direction_of(edit, view.rtl)), language_(edit.language),
      placeholder_(to_utf32(edit.placeholder_text)), scroll_past_end_(edit.scroll_past_end_of_file),
      fit_content_height_(edit.scroll_fit_content_height),
      fit_content_width_(edit.scroll_fit_content_width), font_(view.font),
      lines_(std::move(lines)) {
    if (lines_.empty()) {
        lines_.emplace_back();
    }
    ime_.resize(lines_.size());
    data_.resize(lines_.size());
    auto metrics = fonts.metrics(font_, view.font_size);
    if (!metrics && !font_.empty()) {
        error_ = metrics.error().message;
        font_.clear();
        metrics = fonts.metrics(font_, view.font_size);
    }
    if (metrics) {
        font_height_ = metrics.value().height();
    }
    const auto advance = [&](std::string_view c) {
        auto laid = fonts.layout(c, {.font = font_, .font_size = view.font_size});
        if (!laid && !font_.empty()) {
            error_ = laid.error().message;
            font_.clear();
            laid = fonts.layout(c, {.font = font_, .font_size = view.font_size});
        }
        return laid && !laid.value().glyphs.empty() ? laid.value().glyphs.front().advance : 0.0F;
    };
    space_width_ = advance(" ");
    m_width_ = advance("m");
    update_wrap_at_column();
    refresh();
    set_v_scroll(edit.scroll_vertical);
    set_h_scroll(edit.scroll_horizontal);
}

TextArea::~TextArea() = default;
TextArea::TextArea(TextArea&&) noexcept = default;

const std::u32string& TextArea::line(int line) const {
    static const std::u32string kNone;
    return line >= 0 && line < line_count() ? lines_[static_cast<std::size_t>(line)] : kNone;
}

const std::u32string& TextArea::shown(int line) const {
    if (line < 0 || line >= line_count()) {
        return this->line(line);
    }
    const std::u32string& ime = ime_[static_cast<std::size_t>(line)];
    return ime.empty() ? lines_[static_cast<std::size_t>(line)] : ime;
}

void TextArea::shape(LineData& data, std::u32string_view source, bool placeholder) const {
    std::u32string shown(source);
    std::ranges::transform(shown, shown.begin(),
                           [placeholder](char32_t c) { return shown_character(c, placeholder); });
    data.utf8 = to_utf8(shown);
    // Text::invalidate_cache: wrapped to the column when wrapping, tabs aligned to four spaces.
    const text::LayoutOptions options{
        .font = font_,
        .font_size = view_->font_size,
        .autowrap = wrapping_ ? autowrap_ : text::Autowrap::Off,
        .width = wrapping_ ? wrap_at_ : 0.0F,
        .direction = direction_,
        .language = language_,
        .tab_stops = {std::max(1.0F, space_width_ * static_cast<float>(kTabSize))}};
    auto laid = fonts_->layout(data.utf8, options);
    if (laid) {
        data.layout = std::move(laid.value());
    } else {
        error_ = laid.error().message;
        data.layout = {};
        data.layout.lines.emplace_back();
    }
    data.rows.clear();
    data.ranges.clear();
    float width = 0.0F;
    float height = font_height_;
    for (std::size_t i = 0; i < data.layout.lines.size(); ++i) {
        const text::ShapedLine& row = data.rows.emplace_back(data.utf8, data.layout, i);
        data.ranges.emplace_back(static_cast<int>(row.start()), static_cast<int>(row.end()));
        width = std::max(width, row.width());
        height = std::max(height, row.height());
    }
    if (!data.ranges.empty()) {
        // The last row ends at the line's end.
        data.ranges.back().second = static_cast<int>(shown.size());
    }
    data.width = to_int(width);
    data.height = to_int(height);
    data.breaks = text::character_breaks(shown);
}

const TextArea::LineData& TextArea::data(int line) const {
    const auto at = static_cast<std::size_t>(std::clamp(line, 0, line_count() - 1));
    if (!data_[at]) {
        data_[at] = std::make_unique<LineData>();
        shape(*data_[at], shown(static_cast<int>(at)), false);
    }
    return *data_[at];
}

const TextArea::LineData& TextArea::placeholder_data() const {
    if (!placeholder_data_) {
        placeholder_data_ = std::make_unique<LineData>();
        shape(*placeholder_data_, placeholder_, true);
    }
    return *placeholder_data_;
}

void TextArea::invalidate(int line) {
    if (line >= 0 && line < line_count()) {
        data_[static_cast<std::size_t>(line)].reset();
    }
}

std::pair<int, int> TextArea::insert(int line, int column, std::u32string_view inserted) {
    line = std::clamp(line, 0, line_count() - 1);
    column = std::max(column, 0);
    std::vector<std::u32string> parts(1);
    for (const char32_t c : inserted) {
        if (c == U'\n') {
            parts.emplace_back();
        } else if (c != U'\r') {
            parts.back().push_back(c);
        }
    }
    std::u32string& at = lines_[static_cast<std::size_t>(line)];
    while (std::cmp_greater(column, at.size())) {
        at.push_back(U' ');
    }
    const std::u32string post = at.substr(static_cast<std::size_t>(column));
    parts.front() = at.substr(0, static_cast<std::size_t>(column)) + parts.front();
    parts.back() += post;
    at = std::move(parts.front());
    invalidate(line);
    const auto after = static_cast<std::ptrdiff_t>(line) + 1;
    lines_.insert(lines_.begin() + after, std::make_move_iterator(parts.begin() + 1),
                  std::make_move_iterator(parts.end()));
    ime_.insert(ime_.begin() + after, parts.size() - 1, std::u32string());
    std::vector<std::unique_ptr<LineData>> fresh(parts.size() - 1);
    data_.insert(data_.begin() + after, std::make_move_iterator(fresh.begin()),
                 std::make_move_iterator(fresh.end()));
    const int end_line = line + static_cast<int>(parts.size()) - 1;
    return {end_line, length(end_line) - static_cast<int>(post.size())};
}

void TextArea::remove(int from_line, int from_column, int to_line, int to_column) {
    from_line = std::clamp(from_line, 0, line_count() - 1);
    to_line = std::clamp(to_line, from_line, line_count() - 1);
    const std::u32string& first = lines_[static_cast<std::size_t>(from_line)];
    const std::u32string& last = lines_[static_cast<std::size_t>(to_line)];
    std::u32string joined =
        first.substr(0, std::min(static_cast<std::size_t>(std::max(from_column, 0)), first.size()));
    joined += last.substr(std::min(static_cast<std::size_t>(std::max(to_column, 0)), last.size()));
    const auto begin = static_cast<std::ptrdiff_t>(from_line) + 1;
    const auto end = static_cast<std::ptrdiff_t>(to_line) + 1;
    lines_.erase(lines_.begin() + begin, lines_.begin() + end);
    ime_.erase(ime_.begin() + begin, ime_.begin() + end);
    data_.erase(data_.begin() + begin, data_.begin() + end);
    lines_[static_cast<std::size_t>(from_line)] = std::move(joined);
    invalidate(from_line);
}

void TextArea::set_lines(std::vector<std::u32string> lines) {
    lines_ = std::move(lines);
    if (lines_.empty()) {
        lines_.emplace_back();
    }
    ime_.assign(lines_.size(), std::u32string());
    data_.clear();
    data_.resize(lines_.size());
}

std::u32string TextArea::get(int from_line, int from_column, int to_line, int to_column) const {
    std::u32string out;
    from_line = std::clamp(from_line, 0, line_count() - 1);
    to_line = std::clamp(to_line, from_line, line_count() - 1);
    for (int i = from_line; i <= to_line; ++i) {
        const std::u32string& text = line(i);
        const int begin = std::clamp(i == from_line ? from_column : 0, 0, length(i));
        const int end = std::clamp(i == to_line ? to_column : length(i), begin, length(i));
        if (i > from_line) {
            out.push_back(U'\n');
        }
        out += text.substr(static_cast<std::size_t>(begin), static_cast<std::size_t>(end - begin));
    }
    return out;
}

void TextArea::set_ime(int line, std::u32string shown) {
    if (line < 0 || line >= line_count()) {
        return;
    }
    ime_[static_cast<std::size_t>(line)] = std::move(shown);
    invalidate(line);
}

void TextArea::clear_ime() {
    for (int i = 0; i < line_count(); ++i) {
        if (!ime_[static_cast<std::size_t>(i)].empty()) {
            ime_[static_cast<std::size_t>(i)].clear();
            invalidate(i);
        }
    }
}

bool TextArea::using_placeholder() const {
    return lines_.size() == 1 && lines_.front().empty() && ime_.front().empty();
}

// ---- Characters and words ----

std::vector<int> TextArea::word_breaks(int line) const {
    std::vector<int> words;
    const LineData& d = data(line);
    for (std::size_t r = 0; r < d.rows.size(); ++r) {
        const std::vector<std::uint32_t> row = d.rows[r].word_breaks();
        for (std::size_t i = 0; i + 1 < row.size(); i += 2) {
            const auto start = static_cast<int>(row[i]);
            const auto end = static_cast<int>(row[i + 1]);
            // A word a wrap cut in two is one word.
            if (r > 0 && !words.empty() && words.back() == start && start == d.ranges[r].first) {
                words.back() = end;
                continue;
            }
            words.push_back(start);
            words.push_back(end);
        }
    }
    return words;
}

int TextArea::next_character(int line, int column) const {
    for (const std::uint32_t each : data(line).breaks) {
        if (std::cmp_greater(each, column)) {
            return static_cast<int>(each);
        }
    }
    return column;
}

int TextArea::previous_character(int line, int column) const {
    int previous = 0;
    for (const std::uint32_t each : data(line).breaks) {
        if (std::cmp_greater_equal(each, column)) {
            break;
        }
        previous = static_cast<int>(each);
    }
    return previous;
}

bool TextArea::at_character_break(int line, int column) const {
    if (column <= 0) {
        return true;
    }
    const std::vector<std::uint32_t>& breaks = data(line).breaks;
    return std::ranges::binary_search(breaks, static_cast<std::uint32_t>(column));
}

text::Direction TextArea::dominant_direction(int line, int from, int to) const {
    if (from == to) {
        return text::Direction::Auto;
    }
    const int start = std::min(from, to);
    const int end = std::max(from, to);
    int rtl = 0;
    int ltr = 0;
    for (const text::ShapedLine& row : data(line).rows) {
        for (const text::ShapedLine::Cluster& cluster : row.clusters()) {
            if (std::cmp_greater(cluster.end, start) && std::cmp_less(cluster.start, end)) {
                (cluster.rtl ? rtl : ltr) += 1;
            }
        }
    }
    if (rtl == ltr) {
        return text::Direction::Auto;
    }
    return ltr > rtl ? text::Direction::Ltr : text::Direction::Rtl;
}

// ---- Wrapping ----

int TextArea::wrap_count(int line) const {
    if (line < 0 || line >= line_count()) {
        return 0;
    }
    return static_cast<int>(data(line).rows.size()) - 1;
}

int TextArea::wrap_index_at_column(int line, int column) const {
    if (line < 0 || line >= line_count() || column < 0 ||
        std::cmp_greater(column, shown(line).size()) || wrap_count(line) == 0) {
        return 0;
    }
    const std::vector<std::pair<int, int>>& ranges = data(line).ranges;
    for (std::size_t i = 0; i < ranges.size(); ++i) {
        if (ranges[i].second > column) {
            return static_cast<int>(i);
        }
    }
    return ranges.empty() ? 0 : static_cast<int>(ranges.size()) - 1;
}

const std::vector<std::pair<int, int>>& TextArea::wrap_ranges(int line) const {
    return data(line).ranges;
}

const text::ShapedLine& TextArea::row(int line, int wrap_index) const {
    const LineData& d = data(line);
    return d.rows[static_cast<std::size_t>(
        std::clamp(wrap_index, 0, static_cast<int>(d.rows.size()) - 1))];
}

int TextArea::total_rows() const {
    int total = 0;
    for (int i = 0; i < line_count(); ++i) {
        total += 1 + wrap_count(i);
    }
    return total;
}

int TextArea::row_count(int line) const {
    return using_placeholder() ? static_cast<int>(placeholder_data().rows.size())
                               : 1 + wrap_count(line);
}

int TextArea::max_width() const {
    int width = 0;
    for (int i = 0; i < line_count(); ++i) {
        width = std::max(width, data(i).width);
    }
    return width;
}

bool TextArea::update_wrap_at_column() {
    int wrap_at = to_int(size_.x - style().minimum_size().x);
    if (v_.visible) {
        wrap_at = to_int(static_cast<float>(wrap_at) - v_scroll_min().x);
    }
    wrap_at -= view_->wrap_offset;
    if (static_cast<float>(wrap_at) == wrap_at_) {
        return false;
    }
    wrap_at_ = static_cast<float>(wrap_at);
    if (!wrapping_) {
        return false;
    }
    for (int i = 0; i < line_count(); ++i) {
        invalidate(i);
    }
    placeholder_data_.reset();
    // Keep the first visible line in view.
    const int first = first_visible_line();
    first_visible_wrap_ = std::min(first_visible_wrap_, wrap_count(first));
    set_line_as_first_visible(first, first_visible_wrap_);
    return true;
}

// ---- Viewport ----

int TextArea::line_height() const {
    int height = 0;
    for (int i = 0; i < line_count(); ++i) {
        height = std::max(height, data(i).height);
    }
    return std::max(height + view_->line_spacing, 1);
}

int TextArea::control_height() const {
    int height = to_int(size_.y - style().minimum_size().y);
    if (h_.visible) {
        height = to_int(static_cast<float>(height) - h_scroll_min().y);
    }
    return height;
}

int TextArea::visible_line_count() const {
    return control_height() / line_height();
}

int TextArea::first_visible_line() const {
    return std::clamp(first_visible_line_, 0, line_count() - 1);
}

double TextArea::v_scroll_offset() const {
    return std::clamp(v_.value - std::floor(v_.value), 0.0, 1.0);
}

double TextArea::visible_lines_offset() const {
    double total = static_cast<double>(control_height()) / static_cast<double>(line_height());
    total -= std::floor(total);
    // Whole rows filling the height exactly give 1, where Godot's clamp gives 0.999 and
    // set_line_as_last_visible leaves the last row a thousandth of a row short of fitting.
    if (total == 0.0) {
        return 1.0;
    }
    return 1.0 - std::clamp(total, 0.001, 1.0);
}

Vec2 TextArea::v_scroll_min() const {
    const Vec2 scroll = view_->v_scroll.minimum_size();
    const auto padding = static_cast<float>(std::max(view_->v_padding_left, 0) +
                                            std::max(view_->v_padding_right, 0));
    return {std::max(0.0F, scroll.x) + padding, scroll.y + view_->v_grabber.minimum_size().y};
}

Vec2 TextArea::h_scroll_min() const {
    const Vec2 scroll = view_->h_scroll.minimum_size();
    const auto padding = static_cast<float>(std::max(view_->h_padding_top, 0) +
                                            std::max(view_->h_padding_bottom, 0));
    return {scroll.x + view_->h_grabber.minimum_size().x, std::max(0.0F, scroll.y) + padding};
}

void TextArea::set_v_scroll(double value) {
    v_.set_value(value);
    scroll_moved();
}

void TextArea::set_h_scroll(double value) {
    h_.set_value(value);
    scroll_moved();
}

void TextArea::update_scrollbars() {
    const bool placeholder = using_placeholder();
    const int visible_rows = visible_line_count();
    int total = placeholder ? static_cast<int>(placeholder_data().rows.size()) : total_rows();
    if ((scroll_past_end_ && !fit_content_height_) || visible_rows == 0) {
        total += visible_rows - 1;
    }
    const int visible_width = to_int(size_.x - style().minimum_size().x);
    const int total_width = placeholder ? placeholder_data().width : max_width();
    content_size_ = {static_cast<float>(total_width + 10),
                     static_cast<float>(std::max(total, 1) * line_height())};
    if (!fit_content_height_ && total > visible_rows) {
        const double exact =
            static_cast<double>(control_height()) / static_cast<double>(line_height());
        const double fraction = std::clamp(exact - static_cast<double>(visible_rows), 0.0, 1.0);
        v_.visible = true;
        v_.set_max(total);
        v_.set_page(static_cast<double>(visible_rows) + fraction);
        v_.set_value(v_.value);
    } else {
        v_.set_value(0.0);
        v_.set_max(0.0);
        v_.visible = false;
    }
    if (!fit_content_width_ && total_width > visible_width) {
        h_.visible = true;
        h_.set_max(total_width);
        h_.set_page(visible_width);
    } else {
        h_.set_value(0.0);
        h_.set_max(0.0);
        h_.visible = false;
    }
    scroll_moved();
}

void TextArea::refresh() {
    for (int pass = 0; pass < 4; ++pass) {
        const bool v = v_.visible;
        const bool h = h_.visible;
        update_scrollbars();
        bool changed = v != v_.visible || h != h_.visible;
        if (update_wrap_at_column()) {
            changed = true;
        }
        if (!changed) {
            break;
        }
    }
}

// _scroll_moved: the first visible line, its first visible row and the first visible column
// from the scroll bars' values.
void TextArea::scroll_moved() {
    first_visible_col_ = h_.visible ? to_int(h_.value) : 0;
    if (!v_.visible) {
        first_visible_line_ = 0;
        first_visible_wrap_ = 0;
        return;
    }
    const int v_i = to_int(std::floor(v_.value));
    int sc = 0;
    int n = 0;
    for (n = 0; n < line_count(); ++n) {
        sc += row_count(n);
        if (sc > v_i) {
            break;
        }
    }
    n = std::min(n, line_count() - 1);
    const int amount = row_count(n) - 1;
    first_visible_line_ = n;
    first_visible_wrap_ = std::clamp(amount - (sc - v_i - 1), 0, amount);
}

std::pair<int, int> TextArea::next_visible_line_index_offset_from(int line_from,
                                                                  int wrap_index_from,
                                                                  int visible_amount) const {
    if (line_from < 0 || line_from >= line_count() || !wrapping_) {
        return {std::abs(visible_amount), 0};
    }
    int num_visible = 0;
    int num_total = 0;
    int wrap_index = 0;
    if (visible_amount > 0) {
        num_visible -= wrap_index_from;
        int i = line_from;
        for (; i < line_count(); ++i) {
            ++num_total;
            num_visible += 1 + wrap_count(i);
            if (num_visible >= visible_amount) {
                break;
            }
        }
        wrap_index =
            wrap_count(std::min(i, line_count() - 1)) - std::max(0, num_visible - visible_amount);
    } else if (visible_amount < 0) {
        const int amount = -visible_amount;
        num_visible -= wrap_count(line_from) - wrap_index_from;
        for (int i = line_from; i >= 0; --i) {
            ++num_total;
            num_visible += 1 + wrap_count(i);
            if (num_visible >= amount) {
                break;
            }
        }
        wrap_index = std::max(0, num_visible - amount);
    }
    return {num_total, std::max(wrap_index, 0)};
}

double TextArea::scroll_pos_for_line(int line, int wrap_index) const {
    if (line < 0 || line >= line_count() || wrap_index < 0 || wrap_index > wrap_count(line)) {
        return 0.0;
    }
    if (!wrapping_) {
        return line;
    }
    double pos = 0.0;
    if (line > 0) {
        pos = visible_line_count_in_range(0, std::min(line - 1, line_count() - 1));
    }
    return pos + wrap_index;
}

int TextArea::visible_line_count_in_range(int from_line, int to_line) const {
    if (from_line < 0 || from_line >= line_count() || to_line < 0 || to_line >= line_count()) {
        return 0;
    }
    if (from_line > to_line) {
        std::swap(from_line, to_line);
    }
    if (!wrapping_) {
        return to_line - from_line + 1;
    }
    int total = 0;
    for (int i = from_line; i <= to_line; ++i) {
        total += 1 + wrap_count(i);
    }
    return total;
}

int TextArea::last_full_visible_line() const {
    const int first = first_visible_line();
    const int last =
        first +
        next_visible_line_index_offset_from(first, first_visible_wrap_, visible_line_count())
            .first -
        1;
    return std::clamp(last, 0, line_count() - 1);
}

int TextArea::last_full_visible_line_wrap_index() const {
    const int first = first_visible_line();
    return next_visible_line_index_offset_from(first, first_visible_wrap_, visible_line_count())
        .second;
}

void TextArea::set_line_as_first_visible(int line, int wrap_index) {
    if (line < 0 || line >= line_count() || wrap_index < 0 || wrap_index > wrap_count(line)) {
        return;
    }
    set_v_scroll(scroll_pos_for_line(line, wrap_index));
}

void TextArea::set_line_as_last_visible(int line, int wrap_index) {
    if (line < 0 || line >= line_count() || wrap_index < 0 || wrap_index > wrap_count(line)) {
        return;
    }
    const auto next =
        next_visible_line_index_offset_from(line, wrap_index, -visible_line_count() - 1);
    const int first = line - next.first + 1;
    // Showing a bit of the line after it reads better; near the top, show line 0.
    if (visible_line_count_in_range(0, line) < visible_line_count() + 1) {
        set_v_scroll(0.0);
        return;
    }
    set_v_scroll(scroll_pos_for_line(first, next.second) + visible_lines_offset());
}

void TextArea::adjust_viewport_to_caret(const TextEditState& state, bool maximize_selection) {
    const TextEditState::Caret& caret = state.caret;
    const int cur_line = caret.line;
    const int cur_wrap = wrap_index_at_column(caret.line, caret.column);
    const int first_line = first_visible_line();
    const int first_wrap = first_visible_wrap_;
    const int last_line = last_full_visible_line();
    const int last_wrap = last_full_visible_line_wrap_index();
    if (cur_line < first_line || (cur_line == first_line && cur_wrap < first_wrap)) {
        set_line_as_first_visible(cur_line, cur_wrap);
    } else if (cur_line > last_line || (cur_line == last_line && cur_wrap > last_wrap)) {
        set_line_as_last_visible(cur_line, cur_wrap);
    }

    // _adjust_viewport_to_caret_horizontally.
    if (wrapping_) {
        set_h_scroll(0.0);
        return;
    }
    int visible_width = to_int(size_.x - style().minimum_size().x);
    if (v_.visible) {
        visible_width = to_int(static_cast<float>(visible_width) - v_scroll_min().x);
    }
    visible_width -= 20; // a little more room
    if (visible_width <= 0) {
        return;
    }
    int start = 0;
    int end = 0;
    bool prioritize_end = true;
    const SelectionRange selection = selection_range(caret);
    if (!state.ime_text.empty()) {
        const int ime_start = caret.column + state.ime_selection_start;
        start = column_x_offset(caret.line, ime_start, state.input_direction);
        const int ime_end =
            caret.column + (state.ime_selection_length > 0
                                ? state.ime_selection_start + state.ime_selection_length
                                : static_cast<int>(state.ime_text.size()));
        end = column_x_offset(caret.line, ime_end, state.input_direction);
        prioritize_end = false;
    } else if (maximize_selection && caret.selection.active &&
               selection.from_line == selection.to_line) {
        start = column_x_offset(caret.line, selection.from_column, state.input_direction);
        end = column_x_offset(caret.line, selection.to_column, state.input_direction);
        prioritize_end = caret.line > caret.selection.origin_line ||
                         (caret.line == caret.selection.origin_line &&
                          caret.column >= caret.selection.origin_column);
    } else {
        start = column_x_offset(caret.line, caret.column, state.input_direction);
        end = start;
    }
    if (start > end) {
        std::swap(start, end);
        prioritize_end = !prioritize_end;
    }
    int col = first_visible_col_;
    if (!prioritize_end && end > col + visible_width) {
        col = end - visible_width + 1;
    }
    if (start < col) {
        col = start;
    }
    if (prioritize_end && end > col + visible_width) {
        col = end - visible_width + 1;
    }
    set_h_scroll(col);
}

// ---- Positions ----

std::pair<int, int> TextArea::line_column_at(Vec2 point, bool clamp_line, bool clamp_column,
                                             bool mid_grapheme) const {
    const int px = to_int(point.x);
    const int py = to_int(point.y);
    const StyleBox& st = style();
    float rows = static_cast<float>(py) - st.margin(1);
    rows /= static_cast<float>(line_height());
    rows += static_cast<float>(v_scroll_offset());
    const int first = first_visible_line();
    int row = first + to_int(std::floor(rows));
    int wrap_index = 0;
    if (wrapping_) {
        const auto sign =
            static_cast<float>(static_cast<int>(rows > 0.0F) - static_cast<int>(rows < 0.0F));
        const auto offset =
            next_visible_line_index_offset_from(first, first_visible_wrap_, to_int(rows + sign));
        wrap_index = offset.second;
        row = rows < 0.0F ? first - (offset.first - 1) : first + (offset.first - 1);
    }
    row = std::clamp(row, 0, line_count() - 1);
    const int visible = visible_line_count_in_range(first, row);
    if (rows > static_cast<float>(visible)) {
        if (clamp_line) {
            return {length(row), row};
        }
        return {-1, -1};
    }
    int colx = to_int(static_cast<float>(px) - std::ceil(st.margin(0)));
    colx += first_visible_col_;
    const text::ShapedLine& shaped = this->row(row, wrap_index);
    if (view_->rtl) {
        colx = to_int(shaped.width() - static_cast<float>(colx));
    }
    if (!clamp_column && (colx < 0 || static_cast<float>(colx) > shaped.width())) {
        return {-1, -1};
    }
    std::uint32_t col = shaped.hit_test(static_cast<float>(colx));
    if (!mid_grapheme) {
        col = shaped.closest_character(col);
    }
    return {static_cast<int>(col), row};
}

int TextArea::column_x_offset(int line, int column, text::Direction input_direction) const {
    if (line < 0 || line >= line_count()) {
        return 0;
    }
    const LineData& d = data(line);
    std::size_t wrap_index = 0;
    for (std::size_t i = 0; i < d.ranges.size(); ++i) {
        const auto [start, end] = d.ranges[i];
        if (column >= start && (column < end || (i + 1 == d.ranges.size() && column == end))) {
            wrap_index = i;
            break;
        }
    }
    const text::ShapedLine::Carets carets =
        d.rows[wrap_index].carets(static_cast<std::uint32_t>(std::max(column, 0)));
    const text::ShapedLine::Caret& leading = carets.leading;
    if ((leading.found &&
         (leading.direction == text::Direction::Auto || leading.direction == input_direction)) ||
        !carets.trailing.found) {
        return to_int(leading.x);
    }
    return to_int(carets.trailing.x);
}

int TextArea::char_pos_for_line(int x, int line, int wrap_index, bool mid_grapheme) const {
    if (line < 0 || line >= line_count()) {
        return 0;
    }
    const text::ShapedLine& shaped = row(line, wrap_index);
    if (view_->rtl) {
        x = to_int(shaped.width() - static_cast<float>(x));
    }
    std::uint32_t column = shaped.hit_test(static_cast<float>(x));
    if (!mid_grapheme) {
        column = shaped.closest_character(column);
    }
    return static_cast<int>(column);
}

const StyleBox& TextArea::style() const {
    return editable_ ? view_->normal : view_->read_only;
}

Vec2 TextArea::minimum_size() const {
    Vec2 size = style().minimum_size();
    if (fit_content_height_) {
        size.y += content_size_.y;
    }
    if (fit_content_width_) {
        size.x += content_size_.x;
    }
    return size;
}

// ---- Drawing ----

// ScrollBar's NOTIFICATION_DRAW for one of the TextEdit's scroll bars, placed where
// _update_scrollbars puts it (no buttons: the default theme's icons are empty).
void TextArea::draw_scroll_bar(bool vertical, std::vector<Box>& out) const {
    const Vec2 vmin = v_scroll_min();
    const Vec2 hmin = h_scroll_min();
    const StyleBox& st = style();
    Vec2 position;
    Vec2 size;
    if (vertical) {
        position = {size_.x - vmin.x, st.margin(1)};
        const Vec2 end{size_.x, size_.y - st.margin(1) - st.margin(3)};
        size = {std::max(end.x - position.x, vmin.x), std::max(end.y - position.y, vmin.y)};
    } else {
        position = {0.0F, size_.y - hmin.y};
        const Vec2 end{size_.x - vmin.x, size_.y};
        size = {std::max(end.x - position.x, hmin.x), std::max(end.y - position.y, hmin.y)};
    }
    const Bar& bar = vertical ? v_ : h_;
    const StyleBox& scroll = vertical ? view_->v_scroll : view_->h_scroll;
    const StyleBox& grabber = vertical ? view_->v_grabber : view_->h_grabber;
    out.push_back({.style = scroll, .position = position, .size = size, .texture_size = {}});
    const double grabber_min = vertical ? grabber.minimum_size().y : grabber.minimum_size().x;
    const double area =
        (vertical ? size.y - scroll.minimum_size().y : size.x - scroll.minimum_size().x) -
        grabber_min;
    double grabber_size = 0.0;
    if (const auto range = static_cast<float>(bar.max); range > 0.0F) {
        const float page = bar.page > 0.0 ? static_cast<float>(bar.page) : 0.0F;
        grabber_size = (static_cast<double>(page / range) * area) + grabber_min;
    }
    constexpr double kApprox = 0.00001; // CMP_EPSILON, for is_equal_approx
    const double ratio =
        std::abs(bar.max) < kApprox ? 1.0 : std::clamp(bar.value / bar.max, 0.0, 1.0);
    const auto offset = static_cast<float>(area * ratio);
    Box box{.style = grabber, .position = {}, .size = {}, .texture_size = {}};
    if (vertical) {
        const auto left = static_cast<float>(std::max(view_->v_padding_left, 0));
        const auto right = static_cast<float>(std::max(view_->v_padding_right, 0));
        box.position = position + Vec2{left, offset + scroll.margin(1)};
        box.size = {size.x - left - right, static_cast<float>(grabber_size)};
    } else {
        const auto top = static_cast<float>(std::max(view_->h_padding_top, 0));
        const auto bottom = static_cast<float>(std::max(view_->h_padding_bottom, 0));
        box.position = position + Vec2{offset + scroll.margin(0), top};
        box.size = {static_cast<float>(grabber_size), size.y - top - bottom};
    }
    out.push_back(std::move(box));
}

void TextArea::draw(const world::TextEdit& edit, const TextEditState& state, bool focused,
                    Draw& out, TextEditView& view) const {
    const TextEditView& theme = *view_;
    const StyleBox& st = style();
    const bool rtl = theme.rtl;
    const Vec2 size = size_;
    out.under.push_back({.style = st, .position = {}, .size = size, .texture_size = {}});
    if (focused) {
        out.under.push_back(
            {.style = theme.focus, .position = {}, .size = size, .texture_size = {}});
    }
    bool draw_caret = state.draw_caret && focused;
    if (!edit.editable) {
        draw_caret = edit.caret_draw_when_editable_disabled;
    }
    const int xmargin_beg = to_int(std::ceil(st.margin(0)));
    const int xmargin_end = to_int(size.x - std::floor(st.margin(2)));
    const int visible_rows = visible_line_count() + 1;
    if (theme.background_color.w > 0.01F) {
        push_clipped(out.under, size, {}, size, theme.background_color);
    }

    const bool placeholder = using_placeholder();
    const TextEditState::Caret& caret = state.caret;
    const int caret_wrap = wrap_index_at_column(caret.line, caret.column);
    const SelectionRange selection = selection_range(caret);
    const bool selecting = caret.selection.active;
    const int first_line = first_visible_line() - 1;
    int draw_amount = visible_rows + 1;
    draw_amount += placeholder ? static_cast<int>(placeholder_data().rows.size()) - 1
                               : wrap_count(first_line + 1);
    const int top_limit = to_int(st.margin(1));
    const int bottom_limit = to_int(size.y - st.margin(3));
    const int row_height =
        placeholder ? placeholder_data().height + theme.line_spacing : line_height();
    const int caret_width = std::max(theme.caret_width, 0);
    const auto cw = static_cast<float>(caret_width);
    const int font_h = to_int(font_height_);
    const int half_font_h = font_h / 2;
    Vec4 font_color = theme.font_color;
    if (placeholder) {
        font_color = theme.font_placeholder_color;
    } else if (!edit.editable) {
        font_color = theme.font_readonly_color;
    }
    const bool use_selected_color = theme.font_selected_color != Vec4{};

    LabelText& text = out.text;
    text.font = font_;
    text.font_size = theme.font_size;
    text.font_color = font_color;
    text.outline_color = theme.font_outline_color;
    text.outline_size = std::max(theme.outline_size, 0);
    text.shadow_color = {};
    text.error = error_;
    text.clip = true;
    text.text.font = font_;
    text.text.font_size = theme.font_size;
    // The source holds each drawn line's text, so the selection is one range of its bytes.
    std::size_t selected_start = std::string::npos;
    std::size_t selected_end = std::string::npos;
    const auto byte_of = [](const std::string& utf8, int column) {
        std::size_t i = 0;
        for (int c = 0; c < column && i < utf8.size(); ++c) {
            static_cast<void>(next_utf8(utf8, i));
        }
        return i;
    };

    view.caret_visible = false;
    int first_drawn = -1; // the first and last lines with a row in view
    int last_drawn = -1;
    int line = first_line;
    for (int i = 0; i < draw_amount; ++i) {
        ++line;
        if (line < 0 || line >= line_count()) {
            continue;
        }
        const LineData& d = placeholder ? placeholder_data() : data(line);
        const int wrap_amount = static_cast<int>(d.rows.size()) - 1;
        const std::size_t source_offset = text.source.size();
        text.source += d.utf8;
        if (!placeholder && selecting && use_selected_color) {
            if (line == selection.from_line) {
                selected_start = source_offset + byte_of(d.utf8, selection.from_column);
            } else if (line > selection.from_line && selected_start == std::string::npos) {
                selected_start = source_offset;
            }
            if (line == selection.to_line) {
                selected_end = source_offset + byte_of(d.utf8, selection.to_column);
            } else if (line < selection.to_line) {
                selected_end = text.source.size();
            }
        }
        for (int wrap = 0; wrap <= wrap_amount; ++wrap) {
            if (wrap != 0) {
                ++i;
                if (i >= draw_amount) {
                    break;
                }
            }
            const text::ShapedLine& shaped = d.rows[static_cast<std::size_t>(wrap)];
            int char_margin = xmargin_beg - first_visible_col_;
            int ofs_y = to_int(st.margin(1));
            ofs_y += i * row_height;
            ofs_y -= first_visible_wrap_ * row_height;
            ofs_y -= static_cast<int>(std::round(v_scroll_offset() * row_height));
            // A row above the top is passed over, and drawing stops below the bottom.
            // Rows wholly in the margins aren't drawn (Godot draws one that starts on the bottom
            // margin), so the rows drawn are the rows in view.
            const bool clipped = ofs_y + row_height <= top_limit;
            if (ofs_y >= bottom_limit) {
                i = draw_amount;
                break;
            }
            if (edit.highlight_current_line && line == caret.line && wrap == caret_wrap) {
                if (rtl) {
                    push_clipped(
                        out.under, size,
                        {size.x - static_cast<float>(xmargin_end), static_cast<float>(ofs_y)},
                        {static_cast<float>(xmargin_end), static_cast<float>(row_height)},
                        theme.current_line_color);
                } else {
                    push_clipped(out.under, size, {0.0F, static_cast<float>(ofs_y)},
                                 {static_cast<float>(xmargin_end), static_cast<float>(row_height)},
                                 theme.current_line_color);
                }
            }
            const float row_width = shaped.width();
            if (rtl) {
                char_margin = to_int(size.x - static_cast<float>(char_margin) - row_width);
            }
            const auto margin = static_cast<float>(char_margin);
            const auto clip_x = [&](float& x, float& w) {
                if (x + w <= static_cast<float>(xmargin_beg) ||
                    x > static_cast<float>(xmargin_end)) {
                    return false;
                }
                if (x < static_cast<float>(xmargin_beg)) {
                    w -= static_cast<float>(xmargin_beg) - x;
                    x = static_cast<float>(xmargin_beg);
                }
                if (x + w > static_cast<float>(xmargin_end)) {
                    w = static_cast<float>(xmargin_end) - x;
                }
                return true;
            };

            // The selection, and past the end of a line it goes on from.
            if (!clipped && !placeholder && selecting && line >= selection.from_line &&
                line <= selection.to_line) {
                const int sel_from = line > selection.from_line ? static_cast<int>(shaped.start())
                                                                : selection.from_column;
                const int sel_to =
                    line < selection.to_line ? static_cast<int>(shaped.end()) : selection.to_column;
                auto spans = shaped.selection(static_cast<std::uint32_t>(std::max(sel_from, 0)),
                                              static_cast<std::uint32_t>(std::max(sel_to, 0)));
                if (wrap == wrap_amount && line < selection.to_line) {
                    if (rtl) {
                        spans.emplace_back(-space_width_, 0.0F);
                    } else {
                        spans.emplace_back(row_width, row_width + space_width_);
                    }
                }
                for (const auto& [left, right] : spans) {
                    float x = std::ceil(left) + margin;
                    float w = std::ceil(right) - std::ceil(left);
                    if (clip_x(x, w)) {
                        push_clipped(out.under, size, {x, static_cast<float>(ofs_y)},
                                     {w, static_cast<float>(row_height)}, theme.selection_color);
                    }
                }
            }

            const float text_height = shaped.height();
            const float ascent = d.layout.metrics.ascent;
            const float descent = d.layout.metrics.descent;
            const int baseline =
                to_int(static_cast<float>(
                           to_int(static_cast<float>(ofs_y) +
                                  ((static_cast<float>(row_height) - text_height) / 2.0F))) +
                       ascent);
            const auto base = static_cast<float>(baseline);

            // The glyphs that fit between the margins, and the tab and space marks.
            const text::Line& laid = d.layout.lines[static_cast<std::size_t>(wrap)];
            text::Line& drawn = text.text.lines.emplace_back();
            drawn.first = static_cast<std::uint32_t>(text.text.glyphs.size());
            drawn.start = static_cast<std::uint32_t>(source_offset) + laid.start;
            drawn.end = static_cast<std::uint32_t>(source_offset) + laid.end;
            drawn.rtl = laid.rtl;
            float char_ofs = 0.0F;
            for (std::uint32_t g = laid.first; g < laid.first + laid.count; ++g) {
                const text::Glyph& glyph = d.layout.glyphs[g];
                const float char_pos = char_ofs + margin;
                if (char_pos >= static_cast<float>(xmargin_beg) &&
                    (edit.draw_tabs || edit.draw_spaces)) {
                    std::size_t at = glyph.cluster;
                    const char32_t c = at < d.utf8.size() ? next_utf8(d.utf8, at) : U'\0';
                    Vec4 color = font_color;
                    if (!placeholder && selecting && use_selected_color) {
                        const int column =
                            static_cast<int>(text::character_index(d.utf8, glyph.cluster));
                        const bool after =
                            line > selection.from_line || column >= selection.from_column;
                        const bool before =
                            line < selection.to_line || column < selection.to_column;
                        if (after && before) {
                            color = theme.font_selected_color;
                        }
                    }
                    Vec2 icon_at;
                    bool icon = false;
                    if (edit.draw_tabs && c == U'\t') {
                        icon_at = {char_pos,
                                   base + static_cast<float>(
                                              to_int(((text_height - kIconSize) / 2.0F) - ascent))};
                        icon = true;
                    } else if (edit.draw_spaces && glyph.space && c != U'\t') {
                        const int xofs = to_int((glyph.advance - kIconSize) / 2.0F);
                        icon_at = {char_pos + static_cast<float>(xofs),
                                   base + static_cast<float>(
                                              to_int(((text_height - kIconSize) / 2.0F) - ascent))};
                        icon = true;
                    }
                    const bool inside = icon_at.x >= 0.0F && icon_at.y >= 0.0F &&
                                        icon_at.x + kIconSize <= size.x &&
                                        icon_at.y + kIconSize <= size.y;
                    if (icon && inside) {
                        const Vec4 mark = times(kIconColor, color);
                        if (c == U'\t') {
                            // A bar on the right and a chevron pointing at it.
                            out.under.push_back(
                                rect_box(icon_at + Vec2{6.0F, 0.0F}, {2.0F, kIconSize}, mark));
                            constexpr float kHalf = std::numbers::sqrt2_v<float>;
                            for (const float skew : {-1.0F, 1.0F}) {
                                Box stroke = rect_box(
                                    icon_at + Vec2{2.25F - kHalf, skew < 0.0F ? 0.5F : 4.0F},
                                    {2.0F * kHalf, 3.5F}, mark);
                                stroke.style.skew = {skew, 0.0F};
                                stroke.style.anti_aliasing = true;
                                out.under.push_back(std::move(stroke));
                            }
                        } else {
                            Box dot = rect_box(icon_at + Vec2{2.5F, 2.5F}, {3.0F, 3.0F}, mark);
                            dot.style.corner_radius = {2, 2, 2, 2};
                            dot.style.anti_aliasing = true;
                            out.under.push_back(std::move(dot));
                        }
                    }
                }
                if (!clipped && char_ofs + margin >= static_cast<float>(xmargin_beg) &&
                    char_ofs + glyph.advance + margin <= static_cast<float>(xmargin_end)) {
                    text::Glyph copy = glyph;
                    copy.cluster += static_cast<std::uint32_t>(source_offset);
                    text.text.glyphs.push_back(copy);
                    ++drawn.count;
                }
                char_ofs += glyph.advance;
                if (char_ofs + margin >= static_cast<float>(xmargin_end)) {
                    break;
                }
            }
            if (clipped) {
                text.text.lines.pop_back();
                continue;
            }
            if (first_drawn < 0) {
                first_drawn = line;
            }
            last_drawn = line;
            text.pens.push_back({margin + shaped.origin(), base});
            text.stretch.push_back(0.0F);

            // The caret.
            if (line != caret.line || wrap != caret_wrap) {
                continue;
            }
            view.caret_position.y = base + descent;
            const bool composing = !state.ime_text.empty();
            if (!composing || state.ime_selection_length == 0) {
                CaretPair carets;
                const bool row_empty = shaped.clusters().empty();
                if (!row_empty || composing) {
                    carets = carets_of(
                        shaped, composing ? caret.column + state.ime_selection_start : caret.column,
                        ascent);
                } else {
                    // No carets: one at the start.
                    carets.leading.found = true;
                    carets.leading.direction = rtl ? text::Direction::Rtl : text::Direction::Ltr;
                    carets.leading.position = {rtl ? row_width : char_ofs,
                                               static_cast<float>(-half_font_h)};
                    carets.leading.size = {cw * 4.0F, static_cast<float>(font_h)};
                }
                CaretRect& l = carets.leading;
                CaretRect& t = carets.trailing;
                if ((l.found && (l.direction == text::Direction::Auto ||
                                 l.direction == state.input_direction)) ||
                    !t.found) {
                    view.caret_position.x = margin + l.position.x;
                } else {
                    view.caret_position.x = margin + t.position.x;
                }
                const float x = view.caret_position.x;
                if (x >= static_cast<float>(xmargin_beg) && x <= static_cast<float>(xmargin_end)) {
                    view.caret_visible = true;
                    if (draw_caret) {
                        if (edit.caret_type == "block" || state.overtype_mode) {
                            // A box round the character after it, or a line under it.
                            if (t.found) {
                                if (state.overtype_mode) {
                                    t.position.y = descent;
                                    t.size.y = cw;
                                } else {
                                    t.position.y = -ascent;
                                    t.size.y = static_cast<float>(font_h);
                                }
                                t.position = t.position + Vec2{margin, base};
                                if (state.overtype_mode) {
                                    push_clipped(out.over, size, t.position, t.size,
                                                 theme.caret_color);
                                } else {
                                    push_unfilled(out.over, size, t.position, t.size,
                                                  theme.caret_color);
                                }
                                if (l.found && l.direction != t.direction) {
                                    // The leading half of a split caret, and its flag.
                                    l.position = l.position + Vec2{margin, base};
                                    l.size.x = cw;
                                    push_clipped(out.over, size, l.position, l.size,
                                                 theme.caret_color);
                                    const float flag =
                                        l.direction == text::Direction::Ltr ? 0.5F : -3.0F;
                                    push_clipped(
                                        out.over, size,
                                        {l.position.x + (flag * cw), l.position.y + l.size.y - cw},
                                        {3.0F * cw, cw}, theme.caret_color);
                                }
                            } else {
                                // At the end of the line.
                                if (!row_empty) {
                                    if (state.overtype_mode) {
                                        l.position.y = descent;
                                        l.size.y = cw;
                                    } else {
                                        l.position.y = -ascent;
                                        l.size.y = static_cast<float>(font_h);
                                    }
                                } else if (state.overtype_mode) {
                                    l.position.y += l.size.y;
                                    l.size.y = cw;
                                }
                                l.size.x =
                                    std::ceil(l.position.x) >= row_width ? m_width_ : 3.0F * cw;
                                l.position = l.position + Vec2{margin, base};
                                if (l.direction == text::Direction::Rtl) {
                                    l.position.x -= l.size.x;
                                }
                                if (state.overtype_mode) {
                                    push_clipped(out.over, size, l.position, l.size,
                                                 theme.caret_color);
                                } else {
                                    push_unfilled(out.over, size, l.position, l.size,
                                                  theme.caret_color);
                                }
                            }
                        } else {
                            if (l.found && l.direction == text::Direction::Auto) {
                                // A marker over a caret inside a ligature.
                                push_clipped(
                                    out.over, size,
                                    {margin + l.position.x - (2.5F * cw), base + l.position.y},
                                    {6.0F * cw, cw}, theme.caret_color);
                            } else if (l.found && t.found && l.direction != t.direction) {
                                // Where directions meet, a flag on each half says which way.
                                float flag = l.direction == text::Direction::Ltr ? 0.5F : -3.0F;
                                push_clipped(out.over, size,
                                             {margin + l.position.x + (flag * cw),
                                              base + l.position.y + l.size.y - cw},
                                             {3.0F * cw, cw}, theme.caret_color);
                                flag = t.direction == text::Direction::Ltr ? 0.5F : -3.0F;
                                push_clipped(
                                    out.over, size,
                                    {margin + t.position.x + (flag * cw), base + t.position.y},
                                    {3.0F * cw, cw}, theme.caret_color);
                            }
                            // The two halves of a caret between characters going the same way
                            // make one line: drawn as one box.
                            if (l.found && t.found && l.direction == t.direction &&
                                l.position.x == t.position.x) {
                                const float top = std::min(l.position.y, t.position.y);
                                l.size.y =
                                    std::max(l.position.y + l.size.y, t.position.y + t.size.y) -
                                    top;
                                l.position.y = top;
                                t.found = false;
                            }
                            for (const CaretRect* half : {&l, &t}) {
                                if (half->found) {
                                    push_clipped(
                                        out.over, size,
                                        {margin + half->position.x, base + half->position.y},
                                        {cw, half->size.y}, theme.caret_color);
                                }
                            }
                        }
                    }
                }
            }
            if (composing) {
                // The composition underlined on the baseline, its selection thicker.
                const auto underline = [&](int from, int to, float thickness) {
                    for (const auto& [left, right] :
                         shaped.selection(static_cast<std::uint32_t>(std::max(from, 0)),
                                          static_cast<std::uint32_t>(std::max(to, 0)))) {
                        float x = left + margin;
                        float w = right - left;
                        if (!clip_x(x, w)) {
                            continue;
                        }
                        push_clipped(out.over, size, {x, base}, {w, thickness}, theme.caret_color);
                        view.caret_position.x = x;
                    }
                };
                underline(caret.column, caret.column + static_cast<int>(state.ime_text.size()), cw);
                if (state.ime_selection_length > 0) {
                    const int from = caret.column + state.ime_selection_start;
                    underline(from, from + state.ime_selection_length, cw * 3.0F);
                }
            }
        }
    }
    text.first = 0;
    text.last = text.text.lines.size();
    if (selected_start != std::string::npos && selected_end != std::string::npos &&
        selected_start < selected_end) {
        text.selected_start = selected_start;
        text.selected_end = selected_end;
        text.selected_color = theme.font_selected_color;
    }
    if (v_.visible) {
        draw_scroll_bar(true, out.over);
    }
    if (h_.visible) {
        draw_scroll_bar(false, out.over);
    }

    view.error = error_;
    view.first_visible_line = first_drawn >= 0 ? first_drawn : first_visible_line();
    view.last_visible_line = last_drawn >= 0 ? last_drawn : view.first_visible_line;
    view.total_rows = total_rows();
    view.scroll_vertical = v_.value;
    view.scroll_horizontal = h_scroll();
    view.v_scroll_visible = v_.visible;
    view.h_scroll_visible = h_.visible;
    view.font_height = font_height_;
    view.caret_line = caret.line;
    view.caret_column = caret.column;
    if (selecting) {
        view.selection = {selection.from_line, selection.from_column, selection.to_line,
                          selection.to_column};
    }
    view.ime = to_utf8(state.ime_text);
    view.placeholder = placeholder && !placeholder_.empty();
}

TextArea text_area_for(text::Fonts& fonts, const TextEditView& view, Vec2 size,
                       const world::TextEdit& edit, const TextEditState& state) {
    TextArea area(fonts, view, size, edit, split_lines(edit.text));
    if (state.known && state.known_text == edit.text && !state.ime_text.empty()) {
        const int line = std::clamp(state.caret.line, 0, area.line_count() - 1);
        const std::u32string& text = area.line(line);
        const auto column = static_cast<std::size_t>(
            std::clamp(state.caret.column, 0, static_cast<int>(text.size())));
        area.set_ime(line, text.substr(0, column) + state.ime_text + text.substr(column));
        area.refresh();
        area.set_v_scroll(edit.scroll_vertical);
        area.set_h_scroll(edit.scroll_horizontal);
    }
    return area;
}

const TextEditState& shown_state(const world::TextEdit& edit, const TextEditState* state,
                                 const TextArea& area, TextEditState& fallback) {
    if (state != nullptr && state->known && state->known_text == edit.text &&
        state->caret.line == edit.caret_line && state->caret.column == edit.caret_column) {
        return *state;
    }
    fallback = {};
    if (state != nullptr) {
        fallback.draw_caret = state->draw_caret;
        fallback.overtype_mode = state->overtype_mode;
        fallback.input_direction = state->input_direction;
    }
    fallback.caret.line = std::clamp(edit.caret_line, 0, area.line_count() - 1);
    fallback.caret.column = std::clamp(edit.caret_column, 0, area.length(fallback.caret.line));
    return fallback;
}

} // namespace slash::ui
