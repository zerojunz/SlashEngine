#include "line_edit.hpp"

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

constexpr float kEpsilon = 0.00001F; // CMP_EPSILON, for Math::is_zero_approx

bool is_zero(float value) {
    return std::abs(value) < kEpsilon;
}

int to_int(float value) {
    return static_cast<int>(value);
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

// What a LineEdit shows for one character: line breaks and other controls as spaces, so each
// character keeps its place and the text stays one line.
char32_t displayable(char32_t c) {
    if (c < 0x20U) {
        return U' ';
    }
    if (c == 0x2028U || c == 0x2029U) {
        return 0x205FU; // as wide in UTF-8, so bytes keep their places too
    }
    return c;
}

text::Direction direction_of(const world::LineEdit& edit, bool rtl) {
    if (edit.text_direction == "inherited") {
        return rtl ? text::Direction::Rtl : text::Direction::Ltr;
    }
    return text::parse_direction(edit.text_direction).value_or(text::Direction::Auto);
}

} // namespace

std::u32string characters(const world::LineEdit& edit) {
    return to_utf32(edit.text);
}

Field::Field(text::Fonts& fonts, const LineEditView& view, Vec2 size)
    : fonts_(&fonts), view_(&view), size_(size) {
    std::string font = view.font;
    auto metrics = fonts.metrics(font, view.font_size);
    if (!metrics && !font.empty()) {
        error_ = metrics.error().message;
        font.clear();
        metrics = fonts.metrics(font, view.font_size);
    }
    if (metrics) {
        font_height_ = metrics.value().height();
    }
    // Font::get_char_size('W').
    if (auto w = fonts.layout("W", {.font = font, .font_size = view.font_size}); w) {
        em_ = w.value().width;
    }
}

void Field::shape(const world::LineEdit& edit, const EditState& state) {
    const std::u32string text = characters(edit);
    std::u32string shown;
    placeholder_ = text.empty() && state.ime_text.empty();
    if (placeholder_) {
        shown = to_utf32(edit.placeholder_text);
    } else if (edit.secret) {
        const std::u32string secret = to_utf32(edit.secret_character);
        const char32_t c = secret.empty() ? char32_t{0x2022} : secret.front();
        shown.assign(text.size() + state.ime_text.size(), c);
    } else if (!state.ime_text.empty()) {
        const std::size_t caret =
            std::min(static_cast<std::size_t>(std::max(edit.caret_column, 0)), text.size());
        shown = text.substr(0, caret) + state.ime_text + text.substr(caret);
    } else {
        shown = text;
    }
    std::ranges::transform(shown, shown.begin(), displayable);
    shown_ = to_utf8(shown);
    text::LayoutOptions options{.font = error_.empty() ? view_->font : std::string(),
                                .font_size = view_->font_size,
                                .autowrap = text::Autowrap::Off,
                                .width = 0.0F,
                                .direction = direction_of(edit, view_->rtl),
                                .language = edit.language};
    auto laid = fonts_->layout(shown_, options);
    if (!laid && !options.font.empty()) {
        error_ = laid.error().message;
        options.font.clear();
        laid = fonts_->layout(shown_, options);
    }
    if (laid) {
        layout_ = std::move(laid.value());
    } else {
        layout_ = {};
        layout_.lines.emplace_back();
    }
    line_ = text::ShapedLine(shown_, layout_, 0);
}

bool Field::clear_icon_shown(const world::LineEdit& edit) const {
    return !placeholder_ && edit.editable && edit.clear_button_enabled;
}

bool Field::icon_shown(const world::LineEdit& edit) const {
    return (view_->right_icon.has_value() && !edit.right_icon.empty()) || clear_icon_shown(edit);
}

Vec2 Field::icon_size(const world::LineEdit& edit, bool clear) const {
    Vec2 texture =
        clear ? Vec2{kClearIconSize, kClearIconSize} : view_->right_icon.value_or(Vec2{});
    if (!clear && (!view_->right_icon || edit.right_icon.empty())) {
        return {};
    }
    if (edit.icon_expand_mode == "fit_to_text") {
        return {font_height_, font_height_};
    }
    if (edit.icon_expand_mode == "fit_to_line_edit") {
        if (texture.x <= 0.0F || texture.y <= 0.0F) {
            return {};
        }
        float width = texture.x * size_.y / texture.y;
        float height = size_.y;
        if (width > size_.x) {
            width = size_.x;
            height = texture.y * width / texture.x;
        }
        return Vec2{width, height} * edit.right_icon_scale;
    }
    return texture;
}

float Field::x_ofs(const world::LineEdit& edit, const EditState& state, bool draw) const {
    const StyleBox& style = draw && !edit.editable ? view_->read_only : view_->normal;
    const bool rtl = view_->rtl;
    const float left = style.margin(0);
    const float right = style.margin(2);
    const float width = draw ? static_cast<float>(to_int(size_.x)) : size_.x;
    const float text_width = full_width();
    // The right-aligned start: drawing rounds the text's end up.
    const int end_aligned =
        draw ? to_int(width - std::ceil(right + text_width)) : to_int(width - right - text_width);
    int x = 0;
    if (edit.alignment == "center") {
        if (!is_zero(state.scroll_offset)) {
            x = to_int(left);
        } else {
            const int total_margin = to_int(left + right);
            const int centered = to_int(width - static_cast<float>(total_margin) - text_width) / 2;
            x = to_int(left + static_cast<float>(std::max(0, centered)));
        }
    } else if (edit.alignment == "right") {
        x = rtl ? to_int(left) : to_int(std::max(left, static_cast<float>(end_aligned)));
    } else {
        x = rtl ? to_int(std::max(left, static_cast<float>(end_aligned))) : to_int(left);
    }
    if (icon_shown(edit)) {
        const Vec2 icon = icon_size(edit, clear_icon_shown(edit));
        if (edit.alignment == "center") {
            if (is_zero(state.scroll_offset)) {
                const int total_margin = to_int(left + right);
                const int center =
                    to_int(width - static_cast<float>(total_margin) - text_width - icon.x) / 2;
                x = to_int(left + static_cast<float>(std::max(0, center)));
            }
            if (rtl) {
                x = to_int(static_cast<float>(x) + icon.x);
            }
        } else if (rtl) {
            x = to_int(std::max(left + icon.x, static_cast<float>(x)));
        } else {
            x = to_int(std::max(left, static_cast<float>(x) - icon.x - right));
        }
    }
    return static_cast<float>(x);
}

float Field::ofs_max(const world::LineEdit& edit) const {
    int max = to_int(size_.x - view_->normal.margin(2));
    if (icon_shown(edit) && !view_->rtl) {
        max = to_int(static_cast<float>(max) - icon_size(edit, clear_icon_shown(edit)).x);
    }
    return static_cast<float>(max);
}

float Field::y_ofs() const {
    const StyleBox& style = view_->normal;
    const int y_area = to_int(static_cast<float>(to_int(size_.y)) - style.minimum_size().y);
    return static_cast<float>(
        to_int(style.margin(1) + ((static_cast<float>(y_area) - text_height()) / 2.0F)));
}

Vec2 Field::caret_pixel_pos(const world::LineEdit& edit, const EditState& state) const {
    const float x = x_ofs(edit, state);
    const int caret = edit.caret_column;
    const auto pick = [&](int position) {
        const text::ShapedLine::Carets carets = line_.carets(static_cast<std::uint32_t>(position));
        const text::ShapedLine::Caret& leading = carets.leading;
        if ((leading.found && (leading.direction == text::Direction::Auto ||
                               leading.direction == state.input_direction)) ||
            !carets.trailing.found) {
            return leading.x;
        }
        return carets.trailing.x;
    };
    const bool composing = !state.ime_text.empty();
    Vec2 out;
    out.x = x + state.scroll_offset +
            pick(composing && state.ime_selection_start != 0 ? caret + state.ime_selection_start
                                                             : caret);
    if (composing) {
        const int end = state.ime_selection_length != 0
                            ? caret + state.ime_selection_start + state.ime_selection_length
                            : caret + static_cast<int>(state.ime_text.size());
        out.y = x + state.scroll_offset + pick(end);
    } else {
        out.y = out.x;
    }
    return out;
}

void Field::fit(const world::LineEdit& edit, EditState& state) const {
    const float x = x_ofs(edit, state);
    const float max = ofs_max(edit);
    const Vec2 caret = caret_pixel_pos(edit, state);
    const float low = std::min(caret.x, caret.y);
    const float high = std::max(caret.x, caret.y);
    if (low <= x) {
        state.scroll_offset += x - low;
    } else if (high >= max) {
        state.scroll_offset += max - high;
    }
    // Show as much text as fits.
    if (full_width() + state.scroll_offset + x < max) {
        state.scroll_offset = max - x - full_width();
    }
    state.scroll_offset = std::min(0.0F, state.scroll_offset);
}

int Field::column_at(const world::LineEdit& edit, const EditState& state, float x) const {
    auto column = line_.hit_test(x - x_ofs(edit, state) - state.scroll_offset);
    if (!edit.caret_mid_grapheme) {
        column = line_.closest_character(column);
    }
    return static_cast<int>(column);
}

bool Field::over_clear_button(const world::LineEdit& edit, Vec2 point) const {
    if (!edit.clear_button_enabled || point.x < 0.0F || point.y < 0.0F || point.x >= size_.x ||
        point.y >= size_.y) {
        return false;
    }
    const Vec2 icon = icon_size(edit, true);
    const StyleBox& style = view_->normal;
    return view_->rtl ? point.x < style.margin(0) + icon.x
                      : point.x > size_.x - icon.y - style.margin(2);
}

Vec2 Field::minimum_size(const world::LineEdit& edit) const {
    Vec2 size{static_cast<float>(view_->minimum_character_width) * em_, 0.0F};
    if (edit.expand_to_text_length) {
        // Room for the caret at the end.
        size.x = std::max(size.x, full_width() + static_cast<float>(view_->caret_width));
    }
    size.y = std::max(text_height(), font_height_);
    int icon_max_width = 0;
    const bool fits_field = edit.icon_expand_mode == "fit_to_line_edit";
    if (view_->right_icon && !edit.right_icon.empty()) {
        const Vec2 icon = icon_size(edit, false);
        if (!fits_field) {
            size.y = std::max(size.y, icon.y);
        }
        icon_max_width = to_int(icon.x);
    }
    if (edit.clear_button_enabled) {
        const Vec2 icon = icon_size(edit, true);
        if (!fits_field) {
            size.y = std::max(size.y, icon.y);
        }
        icon_max_width = std::max(icon_max_width, to_int(icon.x));
    }
    size.x += static_cast<float>(icon_max_width);
    const Vec2 normal = view_->normal.minimum_size();
    const Vec2 read_only = view_->read_only.minimum_size();
    return {std::max(normal.x, read_only.x) + size.x, std::max(normal.y, read_only.y) + size.y};
}

void Field::draw(const world::LineEdit& edit, const EditState& state, bool focused,
                 std::vector<Box>& under, LabelText& text, std::vector<Box>& over,
                 LineEditView& out) const {
    const LineEditView& view = *view_;
    const bool rtl = view.rtl;
    const int width = to_int(size_.x);
    const int height = to_int(size_.y);
    const StyleBox& style = edit.editable ? view.normal : view.read_only;
    if (!edit.flat) {
        under.push_back({.style = style, .position = {}, .size = size_, .texture_size = {}});
    }
    if (focused) {
        under.push_back({.style = view.focus, .position = {}, .size = size_, .texture_size = {}});
    }
    const float x_start = x_ofs(edit, state, true);
    auto max = static_cast<float>(to_int(static_cast<float>(width) - style.margin(2)));
    const int y_area = to_int(static_cast<float>(height) - style.minimum_size().y);
    const auto y = static_cast<float>(
        to_int(style.margin(1) + ((static_cast<float>(y_area) - text_height()) / 2.0F)));
    Vec4 font_color = edit.editable ? view.font_color : view.font_uneditable_color;
    if (placeholder_) {
        font_color = view.font_placeholder_color;
    }

    // The right icon, or the clear button.
    if (icon_shown(edit)) {
        const bool clear = clear_icon_shown(edit);
        const Vec2 icon = icon_size(edit, clear);
        Vec4 color{1.0F, 1.0F, 1.0F, edit.editable ? 0.9F : 0.45F};
        if (clear) {
            color = state.clear_press_attempt && state.clear_pressing_inside
                        ? view.clear_button_color_pressed
                        : view.clear_button_color;
        }
        Vec2 at{static_cast<float>(width) - icon.x - style.margin(2),
                std::trunc(static_cast<float>(height) / 2.0F) - (icon.y / 2.0F)};
        if (rtl) {
            at.x = style.margin(0);
        }
        if (clear) {
            // Godot's line_edit_clear icon: a cross of two strokes 2 pixels wide in 16.
            const float sx = icon.x / kClearIconSize;
            const float sy = icon.y / kClearIconSize;
            for (const float skew : {-1.0F, 1.0F}) {
                constexpr float kHalfStroke = std::numbers::sqrt2_v<float>;
                Box stroke = rect_box({at.x + ((8.0F - kHalfStroke) * sx), at.y + (3.0F * sy)},
                                      {2.0F * kHalfStroke * sx, 10.0F * sy}, color);
                stroke.style.skew = {sy > 0.0F ? skew * sx / sy : 0.0F, 0.0F};
                stroke.style.anti_aliasing = true;
                under.push_back(std::move(stroke));
            }
        } else {
            Box box{.style = {},
                    .position = at,
                    .size = icon,
                    .texture_size = view.right_icon.value_or(icon)};
            box.style.type = StyleBox::Type::Texture;
            box.style.texture = edit.right_icon;
            box.style.modulate_color = color;
            under.push_back(std::move(box));
        }
        if (!rtl) {
            max = static_cast<float>(to_int(max - icon.x));
        }
    }

    // The selection, cut to the room the text has.
    const Vec2 ofs{x_start + state.scroll_offset, y};
    const auto spans = [&](int from, int to, float top, float span_height, bool clip_both,
                           Vec4 color, std::vector<Box>& into) {
        if (from < 0 || to < 0) {
            return;
        }
        for (const auto& [left, right] :
             line_.selection(static_cast<std::uint32_t>(from), static_cast<std::uint32_t>(to))) {
            float rx = left + ofs.x;
            float rw = right - left;
            if (rx + rw <= x_start || rx > max) {
                continue;
            }
            if (rx < x_start) {
                rw -= x_start - rx;
                rx = x_start;
                if (clip_both && rx + rw > max) {
                    rw = max - rx;
                }
            } else if (rx + rw > max) {
                rw = max - rx;
            }
            into.push_back(rect_box({rx, top}, {rw, span_height}, color));
        }
    };
    if (state.selection.enabled) {
        spans(state.selection.begin, state.selection.end, ofs.y, text_height(), true,
              view.selection_color, under);
    }

    // The text: whole glyphs that fit between the start and the end of the room.
    text.source = shown_;
    text.font = error_.empty() ? view.font : std::string();
    text.font_size = view.font_size;
    text.font_color = font_color;
    text.outline_color = view.font_outline_color;
    text.outline_size = std::max(view.outline_size, 0);
    text.shadow_color = {};
    text.error = error_;
    text.text = layout_;
    text.text.glyphs.clear();
    text::Line& line = text.text.lines.front();
    const text::Line& shaped = layout_.lines.front();
    line.first = 0;
    line.count = 0;
    float pen = ofs.x;
    for (std::uint32_t g = shaped.first; g < shaped.first + shaped.count; ++g) {
        const text::Glyph& glyph = layout_.glyphs[g];
        if (std::ceil(pen) >= x_start && pen + glyph.advance <= max) {
            text.text.glyphs.push_back(glyph);
            ++line.count;
        }
        pen += glyph.advance;
        if (pen >= max) {
            break;
        }
    }
    text.first = 0;
    text.last = 1;
    text.pens = {{ofs.x + line_.origin(), ofs.y + layout_.metrics.ascent}};
    text.stretch = {0.0F};
    if (state.selection.enabled && !placeholder_) {
        const auto byte_of = [&](int column) {
            const std::u32string all = to_utf32(shown_);
            const auto count = std::min(static_cast<std::size_t>(std::max(column, 0)), all.size());
            return to_utf8(all.substr(0, count)).size();
        };
        text.selected_start = byte_of(state.selection.begin);
        text.selected_end = byte_of(state.selection.end);
        text.selected_color = view.font_selected_color;
    }

    // What shows of the text, for ui.layout.
    out.scroll_offset = state.scroll_offset;
    out.visible_start = 0;
    out.visible_end = 0;
    if (!placeholder_) {
        bool any = false;
        for (const text::ShapedLine::Cluster& cluster : line_.clusters()) {
            const float x = ofs.x + cluster.x;
            if (std::ceil(x) >= x_start && x + cluster.advance <= max) {
                out.visible_start =
                    any ? std::min(out.visible_start, cluster.start) : cluster.start;
                out.visible_end = any ? std::max(out.visible_end, cluster.end) : cluster.end;
                any = true;
            }
        }
    }
    out.caret_x = caret_pixel_pos(edit, state).x;

    // Carets, while editing (or forced), in the blink's shown phase.
    const bool can_draw = edit.caret_force_displayed || (state.editing && focused);
    if (!can_draw || !state.draw_caret) {
        return;
    }
    const auto caret_width = static_cast<float>(std::max(view.caret_width, 0));
    const int caret = edit.caret_column;
    if (state.ime_text.empty() || state.ime_selection_length == 0) {
        const text::ShapedLine::Carets carets = line_.carets(static_cast<std::uint32_t>(
            state.ime_text.empty() ? caret : caret + state.ime_selection_start));
        if (placeholder_ || (!carets.leading.found && !carets.trailing.found)) {
            // No carets: one at the start.
            const int h = to_int(font_height_);
            const auto top = static_cast<float>(
                to_int(style.margin(1) + std::trunc(static_cast<float>(y_area - h) / 2.0F)));
            float x = 0.0F;
            if (edit.alignment == "center") {
                const int icon_width =
                    view.right_icon && !edit.right_icon.empty() ? to_int(view.right_icon->x) : 0;
                const int total_margin = to_int(style.margin(0) + style.margin(2));
                const int center = to_int(size_.x - static_cast<float>(total_margin) -
                                          static_cast<float>(icon_width)) /
                                   2;
                x = style.margin(0) + static_cast<float>(std::max(0, center));
            } else if (edit.alignment == "right") {
                x = rtl ? x_start : max;
            } else {
                x = rtl ? max : style.margin(0);
            }
            over.push_back(
                rect_box({x, top}, {caret_width, static_cast<float>(h)}, view.caret_color));
        } else {
            const text::ShapedLine::Caret& leading = carets.leading;
            const text::ShapedLine::Caret& trailing = carets.trailing;
            if (leading.found && leading.direction == text::Direction::Auto) {
                // A marker over a caret inside a ligature.
                over.push_back(
                    rect_box({ofs.x + leading.x - (2.5F * caret_width), ofs.y + leading.top},
                             {6.0F * caret_width, caret_width}, view.caret_color));
            } else if (leading.found && trailing.found && leading.direction != trailing.direction) {
                // Where directions meet, a flag on each half says which way it goes.
                float d = leading.direction == text::Direction::Ltr ? 0.5F : -3.0F;
                over.push_back(rect_box({ofs.x + leading.x + (d * caret_width),
                                         ofs.y + leading.top + leading.height - caret_width},
                                        {3.0F * caret_width, caret_width}, view.caret_color));
                d = trailing.direction == text::Direction::Ltr ? 0.5F : -3.0F;
                over.push_back(
                    rect_box({ofs.x + trailing.x + (d * caret_width), ofs.y + trailing.top},
                             {3.0F * caret_width, caret_width}, view.caret_color));
            }
            for (const text::ShapedLine::Caret* half : {&leading, &trailing}) {
                if (half->found) {
                    over.push_back(rect_box({ofs.x + half->x, ofs.y + half->top},
                                            {caret_width, half->height}, view.caret_color));
                }
            }
        }
    }
    if (!state.ime_text.empty()) {
        // The composition underlined on the baseline, its selection thicker.
        const float baseline = ofs.y + layout_.metrics.ascent;
        spans(caret, caret + static_cast<int>(state.ime_text.size()), baseline, caret_width, false,
              view.caret_color, over);
        if (state.ime_selection_length > 0) {
            const int from = caret + state.ime_selection_start;
            spans(from, from + state.ime_selection_length, baseline, caret_width * 3.0F, false,
                  view.caret_color, over);
        }
    }
}

} // namespace slash::ui
