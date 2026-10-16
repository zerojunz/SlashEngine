#include "internal.hpp"
#include "line_edit.hpp"
#include "text_edit.hpp"

#include "slash/core/assert.hpp"
#include "slash/render/image.hpp"
#include "slash/ui/ui.hpp"

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstddef>
#include <deque>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// Controls are placed as Godot's Control::_size_changed places them, containers place their
// children as Godot's containers sort them (scene/gui/*_container.cpp), and labels are laid out
// as Godot's Label shapes and draws them (scene/gui/label.cpp): the same minimum sizes, lines
// shown, trimming and alignment, down to where Godot rounds to whole pixels. Godot's desired and
// maximum sizes aren't here.
//
// A layout is two walks: minimum sizes from the leaves up, then rects from the roots down. Some
// minimums depend on a control's own size, as a wrapping label's height on its width; a control
// that a container sizes is measured at the size it got, and if that changes its minimum, the
// walks run again with that size, as Godot's next sort would, at most kMaxPasses times.
namespace slash::ui {
namespace {

constexpr int kMaxZ = 4096;
constexpr int kMaxPasses = 4;
constexpr std::size_t kNone = static_cast<std::size_t>(-1);
constexpr std::size_t kLeft = 0;
constexpr std::size_t kTop = 1;
constexpr std::size_t kRight = 2;
constexpr std::size_t kBottom = 3;
constexpr float kRoomForAll = 1.0e6F; // a TextEdit's height with room for every row

const world::Control kDefaultControl{};

std::string name_of(const world::World& world, entt::entity entity) {
    const std::string_view id = world.id_of(entity);
    return id.empty() ? "entity " + std::to_string(entt::to_entity(entity))
                      : "\"" + std::string(id) + "\"";
}

Vec4 times(Vec4 a, Vec4 b) {
    return {a.x * b.x, a.y * b.y, a.z * b.z, a.w * b.w};
}

Vec2 max(Vec2 a, Vec2 b) {
    return {std::max(a.x, b.x), std::max(a.y, b.y)};
}

json::Value decimal(float value) {
    return std::round(static_cast<double>(value) * 1e6) / 1e6;
}

// ---- Themes ----

// A theme a control uses, and the next one: its own, its parents', then the project's.
struct ThemeLink {
    const Theme* theme = nullptr;
    std::string path;
    const ThemeLink* parent = nullptr;
};

// Looks up theme items for one component of a control, as Godot's ThemeOwner: the control's
// overrides, then each theme it uses, over its type variation's types and its type's, then
// Godot's default theme.
class Resolver {
public:
    Resolver(const ThemeLink* themes, const world::Control& control, std::string_view type)
        : themes_(themes), control_(&control) {
        const std::string& variation = control.theme_type_variation;
        for (const ThemeLink* link = themes; link != nullptr && !variation.empty();
             link = link->parent) {
            const auto found = link->theme->types.find(variation);
            if (found == link->theme->types.end() || found->second.base_type.empty()) {
                continue;
            }
            // Down the variation's base types in that theme, until the control's own type.
            std::string_view name = variation;
            for (int depth = 0; !name.empty() && depth < 64; ++depth) {
                types_.push_back(name);
                const auto each = link->theme->types.find(name);
                name = each != link->theme->types.end() ? std::string_view(each->second.base_type)
                                                        : std::string_view();
                if (name == type) {
                    break;
                }
            }
            variation_found_ = true;
            break;
        }
        for (const std::string_view base : base_types(type)) {
            types_.push_back(base);
        }
    }

    [[nodiscard]] bool variation_found() const { return variation_found_; }

    Vec4 color(const std::string& name, std::string* from = nullptr) const {
        return find(
            &ThemeItems::colors, name, control_->theme_override_colors,
            [](const Theme& /*theme*/) { return std::optional<Vec4>(); },
            Vec4{0.0F, 0.0F, 0.0F, 1.0F}, from);
    }
    int constant(const std::string& name, std::string* from = nullptr) const {
        return find(
            &ThemeItems::constants, name, control_->theme_override_constants,
            [](const Theme& /*theme*/) { return std::optional<int>(); }, 0, from);
    }
    std::string font(const std::string& name, std::string* from = nullptr) const {
        return find(
            &ThemeItems::fonts, name, control_->theme_override_fonts,
            [](const Theme& theme) { return theme.default_font; }, std::string(), from);
    }
    int font_size(const std::string& name, std::string* from = nullptr) const {
        return find(
            &ThemeItems::font_sizes, name, control_->theme_override_font_sizes,
            [](const Theme& theme) {
                return theme.default_font_size > 0 ? std::optional<int>(theme.default_font_size)
                                                   : std::nullopt;
            },
            text::kDefaultFontSize, from);
    }
    std::string icon(const std::string& name, std::string* from = nullptr) const {
        return find(
            &ThemeItems::icons, name, control_->theme_override_icons,
            [](const Theme& /*theme*/) { return std::optional<std::string>(); }, std::string(),
            from);
    }
    // Whether an override, a theme or the default theme has a style box, as Godot's
    // has_theme_stylebox: Button's hover_pressed only when a theme gives one, CheckBox's always.
    [[nodiscard]] bool has_style(const std::string& name) const {
        if (control_->theme_override_styles.contains(name)) {
            return true;
        }
        for (const ThemeLink* link = themes_; link != nullptr; link = link->parent) {
            for (const std::string_view type : types_) {
                const auto found = link->theme->types.find(type);
                if (found != link->theme->types.end() &&
                    found->second.items.styles.contains(name)) {
                    return true;
                }
            }
        }
        return std::ranges::any_of(types_, [&](std::string_view type) {
            const auto found = own_defaults().find(type);
            return found != own_defaults().end() && found->second.styles.contains(name);
        });
    }
    StyleBox style(const std::string& name, std::string* from = nullptr) const {
        // An override that isn't a style box is left out; check() reports it.
        std::map<std::string, StyleBox, std::less<>> overrides;
        if (const auto found = control_->theme_override_styles.find(name);
            found != control_->theme_override_styles.end()) {
            if (auto parsed = parse_style_box(found->second); parsed) {
                overrides.emplace(name, std::move(parsed).value());
            }
        }
        StyleBox empty;
        empty.type = StyleBox::Type::Empty;
        return find(
            &ThemeItems::styles, name, overrides,
            [](const Theme& /*theme*/) { return std::optional<StyleBox>(); }, empty, from);
    }

private:
    template <typename T, typename Overrides, typename Default>
    T find(std::map<std::string, T, std::less<>> ThemeItems::*member, const std::string& name,
           const Overrides& overrides, Default theme_default, T fallback, std::string* from) const {
        if (const auto found = overrides.find(name); found != overrides.end()) {
            if (from != nullptr) {
                *from = "override";
            }
            return found->second;
        }
        for (const ThemeLink* link = themes_; link != nullptr; link = link->parent) {
            for (const std::string_view type : types_) {
                const auto found = link->theme->types.find(type);
                if (found != link->theme->types.end()) {
                    const auto& items = found->second.items.*member;
                    if (const auto item = items.find(name); item != items.end()) {
                        if (from != nullptr) {
                            *from = link->path;
                        }
                        return item->second;
                    }
                }
                // A theme's default font and size answer for every type, from the first on.
                if (auto value = theme_default(*link->theme); value) {
                    if (from != nullptr) {
                        *from = link->path;
                    }
                    return *value;
                }
            }
        }
        if (from != nullptr) {
            *from = "default";
        }
        for (const std::string_view type : types_) {
            const auto found = own_defaults().find(type);
            if (found != own_defaults().end()) {
                const auto& items = found->second.*member;
                if (const auto item = items.find(name); item != items.end()) {
                    return item->second;
                }
            }
        }
        return fallback;
    }

    const ThemeLink* themes_;
    const world::Control* control_;
    std::vector<std::string_view> types_;
    bool variation_found_ = false;
};

// ---- Labels ----

// A Label being laid out.
struct LabelWork {
    const world::Label* label = nullptr;
    text::Autowrap autowrap = text::Autowrap::Off;
    text::Overrun overrun = text::Overrun::NoTrimming;
    int line_spacing = 0;
    int paragraph_spacing = 0;
    float line_height = 0.0F;
    std::optional<float> shaped_width; // the width it was last wrapped to; nullopt for none
    bool shaped = false;
    text::Direction direction = text::Direction::Auto;
    std::string language;
    LabelText out;
};

// A Label's or Button's text_direction as a direction for text::Fonts: inherited takes the
// control's layout direction.
text::Direction text_direction(std::string_view name, bool rtl) {
    if (name == "inherited") {
        return rtl ? text::Direction::Rtl : text::Direction::Ltr;
    }
    return text::parse_direction(name).value_or(text::Direction::Auto);
}

LabelWork start_label(const world::Label& label, const Resolver& theme, bool rtl) {
    LabelWork work;
    work.label = &label;
    work.direction = text_direction(label.text_direction, rtl);
    work.language = label.language;
    work.autowrap = text::parse_autowrap(label.autowrap_mode).value_or(text::Autowrap::Off);
    work.overrun =
        text::parse_overrun(label.text_overrun_behavior).value_or(text::Overrun::NoTrimming);
    work.line_spacing = theme.constant("line_spacing");
    work.paragraph_spacing = theme.constant("paragraph_spacing");
    LabelText& out = work.out;
    out.font = theme.font("font");
    out.font_size = std::clamp(theme.font_size("font_size"), 1, text::kMaxFontSize);
    out.font_color = theme.color("font_color");
    out.outline_color = theme.color("font_outline_color");
    out.shadow_color = theme.color("font_shadow_color");
    out.outline_size = std::max(theme.constant("outline_size"), 0);
    out.shadow_outline_size = std::max(theme.constant("shadow_outline_size"), 0);
    out.shadow_offset = {static_cast<float>(theme.constant("shadow_offset_x")),
                         static_cast<float>(theme.constant("shadow_offset_y"))};
    // Godot takes the ellipsis's first character, or U+2026.
    if (work.overrun == text::Overrun::TrimEllipsis ||
        work.overrun == text::Overrun::TrimWordEllipsis) {
        out.ellipsis = std::string(text::first_characters(label.ellipsis_char, 1));
        if (out.ellipsis.empty()) {
            out.ellipsis = "…";
        }
    }
    out.source = label.uppercase ? text::to_upper(label.text) : label.text;
    if (label.visible_characters >= 0) {
        const auto count = static_cast<std::size_t>(label.visible_characters);
        if (label.visible_characters_behavior == "chars_after_shaping") {
            out.visible_bytes = text::first_characters(out.source, count).size();
        } else {
            out.source = std::string(text::first_characters(out.source, count));
        }
    }
    return work;
}

// Shapes a label's text, wrapped to a width when it wraps and one is given; a font that doesn't
// load is reported and the default font used instead.
void shape(text::Fonts& fonts, LabelWork& work, std::optional<float> width) {
    const bool wraps = work.autowrap != text::Autowrap::Off && width.has_value();
    // Godot wraps to the width in whole pixels.
    const std::optional<float> wrap =
        wraps ? std::optional<float>(std::max(std::trunc(*width), 1.0F)) : std::nullopt;
    if (work.shaped && work.shaped_width == wrap) {
        return;
    }
    text::LayoutOptions options{.font = work.out.font,
                                .font_size = work.out.font_size,
                                .autowrap = wraps ? work.autowrap : text::Autowrap::Off,
                                .width = wrap.value_or(0.0F),
                                .direction = work.direction,
                                .language = work.language};
    auto shaped = fonts.layout(work.out.source, options);
    if (!shaped && !options.font.empty()) {
        work.out.error = shaped.error().message;
        work.out.font.clear();
        options.font.clear();
        shaped = fonts.layout(work.out.source, options);
    }
    if (shaped) {
        work.out.text = std::move(shaped.value());
    } else {
        work.out.text = {};
        work.out.text.lines.emplace_back();
    }
    work.line_height = work.out.text.metrics.height();
    work.shaped = true;
    work.shaped_width = wrap;
}

// The first line shown and the one after the last that max_lines_visible lets show.
std::pair<std::size_t, std::size_t> lines_allowed(const LabelWork& work) {
    const std::size_t total = work.out.text.lines.size();
    const std::size_t skipped =
        std::min(static_cast<std::size_t>(std::max(work.label->lines_skipped, 0)), total);
    std::size_t shown = total - skipped;
    if (work.label->max_lines_visible >= 0) {
        shown = std::min(shown, static_cast<std::size_t>(work.label->max_lines_visible));
    }
    return {skipped, skipped + shown};
}

// The height of lines [first, last): line spacing between lines, paragraph spacing between
// paragraphs, as Label::_update_visible adds them up.
float lines_height(const LabelWork& work, std::size_t first, std::size_t last) {
    const auto& lines = work.out.text.lines;
    const auto ls = static_cast<float>(work.line_spacing);
    const auto ps = static_cast<float>(work.paragraph_spacing);
    float height = 0.0F;
    for (std::size_t l = first; l < last; ++l) {
        height += work.line_height + ls;
        if (lines[l].paragraph_end || l + 1 == last) {
            height += ps;
        }
    }
    return height > 0.0F ? height - (ls + ps) : 0.0F;
}

// A label's minimum size, as Label::get_minimum_size: its lines' width (1 when it wraps, clips
// or trims) and height (at least a line; 1 when it clips or trims with wrapping).
Vec2 label_minimum(const LabelWork& work) {
    const world::Label& label = *work.label;
    const float lh = work.line_height;
    Vec2 size{1.0F, lh};
    if (!label.text.empty()) {
        const auto [first, last] = lines_allowed(work);
        size = {work.out.text.width, std::max(lines_height(work, first, last), lh)};
    }
    const bool trims = work.overrun != text::Overrun::NoTrimming;
    if (work.autowrap != text::Autowrap::Off) {
        if (!label.clip_text && trims && label.max_lines_visible > 0) {
            size.y = std::min(size.y, (lh + static_cast<float>(work.line_spacing)) *
                                          static_cast<float>(label.max_lines_visible));
        } else if (label.clip_text || trims) {
            size.y = 1.0F;
        }
        return {1.0F, size.y};
    }
    if (label.clip_text || trims) {
        size.x = 1.0F;
    }
    return size;
}

// Cuts, stretches and places a label's lines in its rect, as Label's _shape_internal,
// get_layout_data and NOTIFICATION_DRAW do; rtl is the control's layout direction, which swaps
// left and right alignment.
void finish_label(text::Fonts& fonts, LabelWork& work, Vec2 size, bool rtl) {
    const world::Label& label = *work.label;
    LabelText& out = work.out;
    text::TextLayout& layout = out.text;
    const auto ls = static_cast<float>(work.line_spacing);
    const auto ps = static_cast<float>(work.paragraph_spacing);
    const float lh = work.line_height;
    const std::size_t total = layout.lines.size();
    const auto [skipped, allowed] = lines_allowed(work);

    // The lines that fit the rect's height.
    std::size_t fit = 0;
    float height = 0.0F;
    const float room = std::ceil(size.y + ls);
    for (std::size_t l = skipped; l < total; ++l) {
        height += lh + ls;
        if (height > room) {
            break;
        }
        ++fit;
        if (layout.lines[l].paragraph_end) {
            height += ps;
        }
    }
    const std::size_t visible = std::min(fit, allowed - skipped);
    out.first = skipped;
    out.last = skipped + visible;
    out.overflow = visible < allowed - skipped;

    // Trimming: every line too wide when lines don't wrap; when they do, only the last line
    // shown when more are hidden, with the ellipsis whether it fits or not.
    const float width = std::trunc(size.x);
    if (work.autowrap == text::Autowrap::Off) {
        for (std::size_t l = 0; l < total; ++l) {
            if (fonts.trim(layout, l, width, work.overrun, out.ellipsis)) {
                out.trimmed.push_back(l);
            }
        }
    } else if (visible > 0 && visible < total - skipped) {
        if (fonts.trim(layout, out.last - 1, width, work.overrun, out.ellipsis, true)) {
            out.trimmed.push_back(out.last - 1);
        }
    }

    // Down the rect.
    const float lines_h = lines_height(work, out.first, out.last);
    float vbegin = 0.0F;
    float vsep = 0.0F;
    if (visible > 0) {
        if (label.vertical_alignment == "center") {
            vbegin = static_cast<float>(static_cast<int>((size.y - lines_h) / 2.0F));
        } else if (label.vertical_alignment == "bottom") {
            vbegin = static_cast<float>(static_cast<int>(size.y - lines_h));
        } else if (label.vertical_alignment == "fill" && visible > 1) {
            vsep = static_cast<float>(
                static_cast<int>((size.y - lines_h) / static_cast<float>(visible - 1)));
        }
    }

    // Across it, each line stretched to the width for fill except a wrapped paragraph's last.
    const bool fill = label.horizontal_alignment == "fill";
    float y = vbegin;
    for (std::size_t l = out.first; l < out.last; ++l) {
        const text::Line& line = layout.lines[l];
        float line_width = line.width;
        float stretch = 0.0F;
        const bool wrapped_last = line.paragraph_end && l > 0 && !layout.lines[l - 1].paragraph_end;
        if (fill && !wrapped_last && line_width < size.x) {
            const auto spaces = std::count_if(
                layout.glyphs.begin() + line.first, layout.glyphs.begin() + line.first + line.count,
                [&](const text::Glyph& glyph) { return text::stretches(line, glyph); });
            if (spaces > 0) {
                stretch = (size.x - line_width) / static_cast<float>(spaces);
                line_width = size.x;
            }
        }
        // A wrapped right-to-left paragraph's lines that fill doesn't stretch start at the right.
        const std::string& align = label.horizontal_alignment;
        const bool right = align == "fill" ? line.rtl && work.autowrap != text::Autowrap::Off
                                           : align == (rtl ? "left" : "right");
        float x = 0.0F;
        if (align == "center") {
            x = std::trunc(std::trunc(size.x - line_width) / 2.0F);
        } else if (right) {
            x = std::trunc(size.x - line_width);
        }
        if (line_width > size.x + 0.01F) {
            out.overflow = true;
        }
        y += layout.metrics.ascent;
        out.pens.push_back({x, y});
        out.stretch.push_back(stretch);
        y += layout.metrics.descent + ls + vsep;
        if (line.paragraph_end) {
            y += ps;
        }
    }
}

// ---- Buttons ----

// What a CheckBox or CheckButton draws besides a Button's: the icon showing whether it is on.
struct CheckWork {
    std::string icon;         // the icon for its state and whether it is on
    std::optional<Vec2> size; // that icon's size, if it loads
    Vec2 largest;             // CheckBox::get_icon_size: the largest of its state's icons, fitted
    Vec4 color{1.0F, 1.0F, 1.0F, 1.0F};
    float normal_left = 0.0F; // the normal style's margins, which the icon keeps from the edge
    float normal_right = 0.0F;
    int v_offset = 0;
    bool end = false; // at the end of the button (a CheckButton), not its start
};

// A Button being laid out: the theme items of the state it is drawn in, and its text, shaped as
// a label's.
struct ButtonWork {
    const world::Button* button = nullptr;
    StyleBox style; // the state's style box
    StyleBox focus;
    bool focused = false; // it has focus and shows it
    bool rtl = false;     // laid out right to left, which swaps left and right alignment
    // The largest of its style boxes' minimum sizes and margins, for align_to_largest_stylebox.
    Vec2 largest;
    std::array<float, 4> largest_margins{};
    bool align = false;
    int h_separation = 0;
    int icon_max_width = 0;
    Vec4 icon_color{1.0F, 1.0F, 1.0F, 1.0F};
    std::optional<Vec2> icon; // the icon's size, if it loads
    // Room kept for a CheckBox's or CheckButton's icon on the left and the right, as Button's
    // _internal_margin.
    std::array<float, 2> internal{};
    std::optional<CheckWork> check;
    LabelWork text;
};

// A Button's theme items for its state, as Button::_update_theme_item_cache,
// _get_current_stylebox and the colors its NOTIFICATION_DRAW picks.
ButtonWork start_button(const world::Button& button, const Resolver& theme, DrawMode mode,
                        bool focused, bool rtl) {
    ButtonWork work;
    work.button = &button;
    work.focused = focused;
    work.rtl = rtl;
    // Laid out right to left, a state's "_mirrored" style box when there is one.
    const auto state_style = [&](const std::string& name) {
        if (rtl && theme.has_style(name + "_mirrored")) {
            return theme.style(name + "_mirrored");
        }
        return theme.style(name);
    };
    const StyleBox normal = state_style("normal");
    const StyleBox hover = state_style("hover");
    const StyleBox pressed = state_style("pressed");
    const StyleBox disabled = state_style("disabled");
    // hover_pressed only when there is one; pressed stands in for it.
    const std::optional<StyleBox> hover_pressed = theme.has_style("hover_pressed")
                                                      ? std::optional(state_style("hover_pressed"))
                                                      : std::nullopt;
    std::string_view color = "font_color";
    std::string_view icon = "icon_normal_color";
    switch (mode) {
    case DrawMode::Normal:
        work.style = normal;
        if (focused) {
            color = "font_focus_color";
            icon = "icon_focus_color";
        }
        break;
    case DrawMode::HoverPressed:
        work.style = hover_pressed.value_or(pressed);
        color = "font_hover_pressed_color";
        icon = "icon_hover_pressed_color";
        break;
    case DrawMode::Pressed:
        work.style = pressed;
        color = "font_pressed_color";
        icon = "icon_pressed_color";
        break;
    case DrawMode::Hover:
        work.style = hover;
        color = "font_hover_color";
        icon = "icon_hover_color";
        break;
    case DrawMode::Disabled:
        work.style = disabled;
        color = "font_disabled_color";
        icon = "icon_disabled_color";
        break;
    }
    work.focus = theme.style("focus");
    std::vector<const StyleBox*> styles{&normal, &pressed, &hover, &disabled};
    if (hover_pressed) {
        styles.push_back(&*hover_pressed);
    }
    for (const StyleBox* style : styles) {
        work.largest = max(work.largest, style->minimum_size());
        for (std::size_t side = 0; side < 4; ++side) {
            work.largest_margins.at(side) =
                std::max(work.largest_margins.at(side), style->margin(side));
        }
    }
    const auto& m = work.largest_margins;
    work.largest = max(work.largest, {m[kLeft] + m[kRight], m[kTop] + m[kBottom]});
    work.align = theme.constant("align_to_largest_stylebox") != 0;
    work.h_separation = theme.constant("h_separation");
    work.icon_max_width = theme.constant("icon_max_width");
    work.icon_color = theme.color(std::string(icon));

    LabelWork& text = work.text;
    text.direction = text_direction(button.text_direction, rtl);
    text.language = button.language;
    text.autowrap = text::parse_autowrap(button.autowrap_mode).value_or(text::Autowrap::Off);
    text.overrun =
        text::parse_overrun(button.text_overrun_behavior).value_or(text::Overrun::NoTrimming);
    text.line_spacing = theme.constant("line_spacing");
    LabelText& out = text.out;
    out.font = theme.font("font");
    out.font_size = std::clamp(theme.font_size("font_size"), 1, text::kMaxFontSize);
    out.font_color = theme.color(std::string(color));
    out.outline_color = theme.color("font_outline_color");
    out.shadow_color = {0.0F, 0.0F, 0.0F, 0.0F};
    out.outline_size = std::max(theme.constant("outline_size"), 0);
    if (text.overrun == text::Overrun::TrimEllipsis ||
        text.overrun == text::Overrun::TrimWordEllipsis) {
        out.ellipsis = "…";
    }
    out.source = button.text;
    return work;
}

// Its style margins: the state's, or the largest with align_to_largest_stylebox.
std::array<float, 4> button_margins(const ButtonWork& work) {
    if (work.align) {
        return work.largest_margins;
    }
    return {work.style.margin(kLeft), work.style.margin(kTop), work.style.margin(kRight),
            work.style.margin(kBottom)};
}

// The text's size as shaped: its widest line, and its lines' heights with line_spacing between,
// as TextParagraph::get_size; none for no text.
Vec2 button_text_size(const ButtonWork& work) {
    if (work.button->text.empty()) {
        return {};
    }
    const text::TextLayout& layout = work.text.out.text;
    const auto lines = static_cast<float>(layout.lines.size());
    return {layout.width,
            (lines * work.text.line_height) +
                (std::max(lines - 1.0F, 0.0F) * static_cast<float>(work.text.line_spacing))};
}

bool button_clips(const ButtonWork& work) {
    return work.button->clip_text || work.text.overrun != text::Overrun::NoTrimming ||
           work.text.autowrap != text::Autowrap::Off;
}

// Button::_fit_icon_size: no wider than icon_max_width, keeping its aspect.
Vec2 fit_icon(const ButtonWork& work, Vec2 size) {
    if (work.icon_max_width > 0 && size.x > static_cast<float>(work.icon_max_width)) {
        const auto max_width = static_cast<float>(work.icon_max_width);
        return {max_width, size.y * max_width / size.x};
    }
    return size;
}

// The size the icon is drawn at in room left by the style's margins: its own, or with
// expand_icon as big as fits beside the text, then fitted and rounded.
Vec2 button_icon_size(const ButtonWork& work, Vec2 room) {
    const world::Button& button = *work.button;
    Vec2 size = work.icon.value_or(Vec2{});
    if (button.expand_icon && size.x > 0.0F && size.y > 0.0F) {
        const Vec2 text = button_text_size(work);
        if (!button_clips(work) && button.icon_alignment != "center" && text.x > 0.0F) {
            room.x -= text.x + static_cast<float>(std::max(work.h_separation, 0));
        }
        if (button.vertical_icon_alignment != "center") {
            room.y -= text.y;
        }
        float width = size.x * room.y / size.y;
        float height = room.y;
        if (width > room.x) {
            width = room.x;
            height = size.y * width / size.x;
        }
        size = {width, height};
    }
    size = fit_icon(work, size);
    return {std::round(size.x), std::round(size.y)};
}

// The internal margins with h_separation after each that isn't 0, as Button's NOTIFICATION_DRAW
// keeps them from the text and icon.
std::array<float, 2> internal_margins(const ButtonWork& work) {
    const auto h_separation = static_cast<float>(std::max(work.h_separation, 0));
    std::array<float, 2> out = work.internal;
    for (float& side : out) {
        if (side > 0.0F) {
            side += h_separation;
        }
    }
    return out;
}

// The width its text gets: its rect less the style's margins, the internal margins and the icon
// beside the text, in whole pixels and at least 1, as Button's NOTIFICATION_DRAW sets its text
// buffer's width.
float button_text_width(const ButtonWork& work, Vec2 size) {
    const std::array<float, 4> m = button_margins(work);
    const std::array<float, 2> internal = internal_margins(work);
    const Vec2 room{size.x - m[kLeft] - m[kRight] - internal[0] - internal[1],
                    size.y - m[kTop] - m[kBottom]};
    float width = room.x;
    if (work.icon && !work.button->text.empty() && work.button->icon_alignment != "center") {
        width -=
            button_icon_size(work, room).x + static_cast<float>(std::max(work.h_separation, 0));
    }
    return std::ceil(std::max(1.0F, width));
}

// Button::get_minimum_size_for_text_and_icon.
Vec2 button_minimum(const ButtonWork& work) {
    const world::Button& button = *work.button;
    Vec2 size = button_text_size(work);
    if (button_clips(work)) {
        size.x = 0.0F;
    }
    const bool center = button.vertical_icon_alignment == "center";
    if (!button.expand_icon && work.icon) {
        const Vec2 icon = fit_icon(work, *work.icon);
        size.y = center ? std::max(size.y, icon.y) : size.y + icon.y;
        if (button.icon_alignment != "center") {
            size.x += icon.x;
            if (!button.text.empty()) {
                size.x += static_cast<float>(std::max(work.h_separation, 0));
            }
        } else {
            size.x = std::max(size.x, icon.x);
        }
    }
    if (!button.text.empty()) {
        const float font_height = work.text.line_height;
        size.y = center ? std::max(size.y, font_height) : size.y + font_height;
    }
    const Vec2 style = work.align ? work.largest : work.style.minimum_size();
    Vec2 minimum{style.x + size.x, style.y + size.y};
    // CheckBox::get_minimum_size and CheckButton's: room for the icon beside the content.
    if (work.check) {
        const Vec2 icon = work.check->largest;
        if (icon.x > 0.0F || icon.y > 0.0F) {
            const Vec2 padding = work.largest;
            Vec2 content{minimum.x - padding.x, minimum.y - padding.y};
            if (content.x > 0.0F && icon.x > 0.0F) {
                content.x += static_cast<float>(std::max(0, work.h_separation));
            }
            content.x += icon.x;
            content.y = std::max(content.y, icon.y);
            minimum = {content.x + padding.x, content.y + padding.y};
        }
    }
    return minimum;
}

// ---- Placement ----

Transform local_transform(const world::Control& control, Vec2 position) {
    const Vec2 cs = control.rotation == 0.0F ? Vec2{1.0F, 0.0F} : cos_sin(control.rotation);
    Transform t{.a = cs.x * control.scale.x,
                .b = cs.y * control.scale.x,
                .c = -cs.y * control.scale.y,
                .d = cs.x * control.scale.y};
    const Vec2 pivot = control.pivot_offset;
    t.tx = position.x + pivot.x - ((t.a * pivot.x) + (t.c * pivot.y));
    t.ty = position.y + pivot.y - ((t.b * pivot.x) + (t.d * pivot.y));
    return t;
}

// Moves an edge pair out to a minimum length, the way grow says.
void grow(float& position, float& length, float minimum, std::string_view direction) {
    if (minimum <= length) {
        return;
    }
    if (direction == "begin") {
        position += length - minimum;
    } else if (direction == "both") {
        position += 0.5F * (length - minimum);
    }
    length = minimum;
}

// Godot's SizeFlags.
struct Flags {
    bool fill = true;
    bool expand = false;
    int shrink = 0; // where it goes when it doesn't fill: 0 begin, 1 center, 2 end
};

Flags flags_of(std::string_view name) {
    Flags flags;
    flags.expand = name.starts_with("expand");
    flags.fill = name == "fill" || name == "expand_fill";
    if (name.ends_with("center")) {
        flags.shrink = 1;
    } else if (name.ends_with("end")) {
        flags.shrink = 2;
    }
    return flags;
}

int alignment_of(std::string_view name) {
    if (name == "center") {
        return 1;
    }
    return name == "end" ? 2 : 0;
}

StyleBox::Stretch stretch_of(std::string_view name) {
    if (name == "tile") {
        return StyleBox::Stretch::Tile;
    }
    return name == "tile_fit" ? StyleBox::Stretch::TileFit : StyleBox::Stretch::Stretch;
}

struct Rect {
    Vec2 position;
    Vec2 size;
};

// A Slider being laid out: its theme items, the icons of its state, and its range.
struct SliderWork {
    const world::Slider* slider = nullptr;
    StyleBox style;
    StyleBox grabber_area; // or grabber_area_highlight
    std::string grabber_icon;
    Vec2 grabber_size;    // that icon's
    Vec2 minimum_grabber; // the grabber icon's, which the minimum size fits
    Vec2 drag_grabber;    // the grabber_highlight icon's, which dragging measures with
    std::string tick_icon;
    Vec2 tick_size;
    int grabber_offset = 0;
    int grabber_max_size = 0;
    int tick_offset = 0;
    int tick_max_size = 0;
    bool center_grabber = false;
    bool highlighted = false;
    bool rtl = false;
    Range range;
};

enum class Kind : std::uint8_t { None, Box, Grid, Margin, Center, Flow, Panel };

// Which way a control's minimum size depends on its own size.
enum class Depends : std::uint8_t { None, Width, Height };

struct Node {
    entt::entity entity = entt::null;
    const world::Control* control = &kDefaultControl;
    std::size_t parent = kNone;
    std::vector<std::size_t> kids;
    const ThemeLink* themes = nullptr;
    std::vector<std::string_view> classes;
    Kind kind = Kind::None;
    Depends depends = Depends::None;
    bool rtl = false; // it lays out right to left

    // Theme items and what doesn't change between passes.
    const world::BoxContainer* box = nullptr;
    const world::GridContainer* grid = nullptr;
    const world::CenterContainer* center = nullptr;
    const world::FlowContainer* flow = nullptr;
    int separation = 0;
    int h_separation = 0;
    int v_separation = 0;
    std::array<int, 4> margins{};
    std::optional<StyleBox> panel;           // Panel's or PanelContainer's
    std::optional<StyleBox> container_panel; // PanelContainer's, for its children's rect
    std::optional<LabelWork> label;
    const world::ProgressBar* bar = nullptr;
    StyleBox bar_background;
    StyleBox bar_fill;
    LabelText percent;
    float percent_height = 0.0F;
    const world::TextureRect* texture_rect = nullptr;
    std::optional<Vec2> texture_size; // TextureRect's, if it loads
    const world::NinePatchRect* nine_patch = nullptr;
    std::optional<Vec2> patch_texture_size;
    const world::ColorRect* color_rect = nullptr;
    std::optional<ButtonWork> button;
    const world::TextureButton* texture_button = nullptr;
    // TextureButton's textures' sizes, those that load: normal, pressed, hover, disabled and
    // focused.
    std::array<std::optional<Vec2>, 5> button_textures;
    std::optional<ButtonState> button_state;
    std::optional<SliderWork> slider;
    // A LineEdit: a copy with its caret column in its text, its theme items, its shaped text and
    // the gui system's state for it.
    std::optional<world::LineEdit> line_edit;
    std::unique_ptr<LineEditView> edit_view;
    std::optional<Field> field;
    EditState edit;
    // A TextEdit, its theme items and the gui system's state for it.
    const world::TextEdit* text_edit = nullptr;
    std::unique_ptr<TextEditView> text_view;
    const TextEditState* text_state = nullptr;

    // Each pass.
    Vec2 minimum;                 // combined minimum size
    std::optional<Vec2> hint;     // its size in the last pass, for minimums that depend on it
    std::optional<Rect> assigned; // the rect its container gave it
    Placed placed;
};

// One line of a FlowContainer.
struct FlowLine {
    int child_count = 0;
    int min_line_height = 0;
    int min_line_length = 0;
    int stretch_avail = 0;
    float stretch_ratio_total = 0.0F;
    bool is_filled = false;
};

struct Builder {
    Builder(const world::World& in_world, text::Fonts& in_fonts, Layout& in_out)
        : world(in_world), fonts(in_fonts), out(in_out) {}

    const world::World& world;
    text::Fonts& fonts;
    Layout& out;
    std::unordered_map<entt::entity, std::vector<entt::entity>> children;
    std::unordered_map<entt::entity, entt::entity> parents;
    std::vector<Node> nodes;
    std::deque<ThemeLink> links;
    std::map<std::string, Result<asset::Handle<Theme>>, std::less<>> themes;
    std::map<std::string, Result<asset::Handle<render::Image>>, std::less<>> images;

    [[nodiscard]] const world::Control& control_of(entt::entity entity) const {
        const auto* control = world.ecs().try_get<world::Control>(entity);
        return control != nullptr ? *control : kDefaultControl;
    }

    void problem(entt::entity entity, std::string path, std::string message) {
        out.problems.push_back({.id = std::string(world.id_of(entity)),
                                .path = std::move(path),
                                .message = std::move(message)});
    }

    void child_problem(entt::entity entity, std::size_t child, std::string message) {
        problem(entity, "/components/Control/children/" + std::to_string(child),
                std::move(message));
    }

    void link(const std::vector<entt::entity>& controls) {
        for (const entt::entity entity : controls) {
            const auto* control = world.ecs().try_get<world::Control>(entity);
            if (control == nullptr) {
                continue;
            }
            for (std::size_t i = 0; i < control->children.size(); ++i) {
                const entt::entity child = control->children[i];
                if (child == entt::null || !world.valid(child)) {
                    child_problem(entity, i, "names no entity");
                } else if (child == entity) {
                    child_problem(entity, i, "a control can't be its own child");
                } else if (!is_control(world, child)) {
                    child_problem(entity, i,
                                  name_of(world, child) +
                                      " isn't a control; give it a Control or a UI component");
                } else if (const auto found = parents.find(child); found != parents.end()) {
                    child_problem(entity, i,
                                  name_of(world, child) + " is already a child of " +
                                      name_of(world, found->second));
                } else {
                    parents.emplace(child, entity);
                    children[entity].push_back(child);
                }
            }
        }
    }

    // Breaks each loop of children at the entry that closes it, so every control is reached
    // from a root.
    void break_loops(const std::vector<entt::entity>& controls) {
        std::unordered_set<entt::entity> reached;
        const auto reach = [&](entt::entity root) {
            std::vector<entt::entity> stack{root};
            while (!stack.empty()) {
                const entt::entity entity = stack.back();
                stack.pop_back();
                if (!reached.insert(entity).second) {
                    continue;
                }
                const auto found = children.find(entity);
                if (found != children.end()) {
                    stack.insert(stack.end(), found->second.begin(), found->second.end());
                }
            }
        };
        for (const entt::entity entity : controls) {
            if (!parents.contains(entity)) {
                reach(entity);
            }
        }
        for (const entt::entity entity : controls) {
            if (reached.contains(entity)) {
                continue;
            }
            // Up the parents until one repeats: that one's link to its parent closes the loop.
            std::unordered_set<entt::entity> seen;
            entt::entity at = entity;
            while (seen.insert(at).second) {
                at = parents.at(at);
            }
            const entt::entity parent = parents.at(at);
            auto& list = children[parent];
            list.erase(std::ranges::find(list, at));
            parents.erase(at);
            const auto& listed = world.ecs().get<world::Control>(parent).children;
            const auto index =
                static_cast<std::size_t>(std::ranges::find(listed, at) - listed.begin());
            child_problem(parent, index,
                          name_of(world, at) + " contains " + name_of(world, parent) +
                              ", so it can't be its child too");
            reach(at);
        }
    }

    // A theme file, loaded once per layout; nullptr and why if it doesn't load.
    const Theme* theme(const std::string& path, std::string* error) {
        auto found = themes.find(path);
        if (found == themes.end()) {
            found = themes.emplace(path, fonts.assets().load<Theme>(path)).first;
        }
        if (!found->second) {
            if (error != nullptr) {
                *error = found->second.error().message;
            }
            return nullptr;
        }
        return found->second.value().get();
    }

    // A texture's size in pixels: a file's or a built-in icon's; nullopt and why if it doesn't
    // load, and nullopt for "".
    std::optional<Vec2> texture_size(const std::string& path, std::string* error = nullptr) {
        if (path.empty()) {
            return std::nullopt;
        }
        if (path.starts_with(kBuiltinPrefix)) {
            const render::Image* icon = builtin_icon(path);
            if (icon == nullptr) {
                if (error != nullptr) {
                    *error = icon_path_error(path);
                }
                return std::nullopt;
            }
            return Vec2{static_cast<float>(icon->width), static_cast<float>(icon->height)};
        }
        auto found = images.find(path);
        if (found == images.end()) {
            found = images.emplace(path, fonts.assets().load<render::Image>(path)).first;
        }
        if (!found->second) {
            if (error != nullptr) {
                *error = found->second.error().message;
            }
            return std::nullopt;
        }
        const render::Image& image = *found->second.value();
        return Vec2{static_cast<float>(image.width), static_cast<float>(image.height)};
    }

    // The project's theme, the last of every control's themes.
    const ThemeLink* project_theme() {
        const std::string path = settings(world).theme;
        if (path.empty()) {
            return nullptr;
        }
        const Theme* loaded = theme(path, nullptr);
        if (loaded == nullptr) {
            return nullptr;
        }
        return &links.emplace_back(ThemeLink{.theme = loaded, .path = path, .parent = nullptr});
    }

    // ---- Nodes ----

    void start(Node& node) {
        const auto& ecs = world.ecs();
        const entt::entity entity = node.entity;
        node.classes = control_classes(world, entity);
        const auto resolver = [&](std::string_view type) {
            return Resolver(node.themes, *node.control, type);
        };

        // The first container decides how children are placed; check() reports the others.
        if (const auto* box = ecs.try_get<world::BoxContainer>(entity); box != nullptr) {
            node.kind = Kind::Box;
            node.box = box;
            node.separation =
                resolver(box->vertical ? "VBoxContainer" : "HBoxContainer").constant("separation");
        } else if (const auto* grid = ecs.try_get<world::GridContainer>(entity); grid != nullptr) {
            node.kind = Kind::Grid;
            node.grid = grid;
            const Resolver theme = resolver("GridContainer");
            node.h_separation = theme.constant("h_separation");
            node.v_separation = theme.constant("v_separation");
        } else if (ecs.all_of<world::MarginContainer>(entity)) {
            node.kind = Kind::Margin;
            const Resolver theme = resolver("MarginContainer");
            node.margins = {theme.constant("margin_left"), theme.constant("margin_top"),
                            theme.constant("margin_right"), theme.constant("margin_bottom")};
        } else if (const auto* center = ecs.try_get<world::CenterContainer>(entity);
                   center != nullptr) {
            node.kind = Kind::Center;
            node.center = center;
        } else if (const auto* flow = ecs.try_get<world::FlowContainer>(entity); flow != nullptr) {
            node.kind = Kind::Flow;
            node.flow = flow;
            const Resolver theme = resolver(flow->vertical ? "VFlowContainer" : "HFlowContainer");
            node.h_separation = theme.constant("h_separation");
            node.v_separation = theme.constant("v_separation");
            node.depends = flow->vertical ? Depends::Height : Depends::Width;
        } else if (ecs.all_of<world::PanelContainer>(entity)) {
            node.kind = Kind::Panel;
            node.container_panel = resolver("PanelContainer").style("panel");
            node.panel = node.container_panel;
        }
        if (ecs.all_of<world::Panel>(entity) && !node.panel) {
            node.panel = resolver("Panel").style("panel");
        }
        node.color_rect = ecs.try_get<world::ColorRect>(entity);
        if (const auto* rect = ecs.try_get<world::TextureRect>(entity); rect != nullptr) {
            node.texture_rect = rect;
            if (!rect->texture.empty()) {
                std::string error;
                node.texture_size = texture_size(rect->texture, &error);
                if (!node.texture_size) {
                    problem(entity, "/components/TextureRect/texture", error);
                }
            }
            if (rect->expand_mode.starts_with("fit_height")) {
                node.depends = Depends::Width;
            } else if (rect->expand_mode.starts_with("fit_width")) {
                node.depends = Depends::Height;
            }
        }
        if (const auto* patch = ecs.try_get<world::NinePatchRect>(entity); patch != nullptr) {
            node.nine_patch = patch;
            if (!patch->texture.empty()) {
                std::string error;
                node.patch_texture_size = texture_size(patch->texture, &error);
                if (!node.patch_texture_size) {
                    problem(entity, "/components/NinePatchRect/texture", error);
                }
            }
        }
        if (const auto* bar = ecs.try_get<world::ProgressBar>(entity); bar != nullptr) {
            start_bar(node, *bar, resolver("ProgressBar"));
        }
        if (const auto* label = ecs.try_get<world::Label>(entity); label != nullptr) {
            node.label = start_label(*label, resolver("Label"), node.rtl);
            if (node.label->autowrap == text::Autowrap::Off) {
                shape(fonts, *node.label, std::nullopt);
            } else {
                node.depends = Depends::Width;
            }
        }
        if (const world::Button* button = button_of(world, entity); button != nullptr) {
            const std::string_view type = button_type(world, entity);
            start_button_node(node, *button, resolver(type), type);
        }
        if (const auto* slider = ecs.try_get<world::Slider>(entity); slider != nullptr) {
            start_slider(node, *slider, resolver(slider->vertical ? "VSlider" : "HSlider"));
        }
        if (const auto* edit = ecs.try_get<world::LineEdit>(entity); edit != nullptr) {
            start_line_edit(node, *edit, resolver("LineEdit"));
        }
        if (const auto* edit = ecs.try_get<world::TextEdit>(entity); edit != nullptr) {
            start_text_edit(node, *edit, resolver("TextEdit"));
        }
        if (const auto* texture = ecs.try_get<world::TextureButton>(entity); texture != nullptr) {
            node.texture_button = texture;
            const std::array<std::pair<const std::string*, std::string_view>, 5> paths{{
                {&texture->texture_normal, "texture_normal"},
                {&texture->texture_pressed, "texture_pressed"},
                {&texture->texture_hover, "texture_hover"},
                {&texture->texture_disabled, "texture_disabled"},
                {&texture->texture_focused, "texture_focused"},
            }};
            for (std::size_t i = 0; i < paths.size(); ++i) {
                const std::string& path = *paths.at(i).first;
                if (path.empty()) {
                    continue;
                }
                std::string error;
                node.button_textures.at(i) = texture_size(path, &error);
                if (!node.button_textures.at(i)) {
                    problem(entity, "/components/TextureButton/" + std::string(paths.at(i).second),
                            error);
                }
            }
            if (!node.button_state) {
                node.button_state =
                    ButtonState{.mode = draw_mode(world, entity),
                                .click_margin = static_cast<float>(
                                    resolver("TextureButton").constant("click_margin"))};
            }
        }
        if (!node.control->theme_type_variation.empty() &&
            !resolver(node.classes.front()).variation_found()) {
            problem(entity, "/components/Control/theme_type_variation",
                    "\"" + node.control->theme_type_variation +
                        "\" is no type variation in the themes this control uses; give it a "
                        "base_type in one");
        }
    }

    void start_button_node(Node& node, const world::Button& button, const Resolver& theme,
                           std::string_view type) {
        const DrawMode mode = draw_mode(world, node.entity);
        node.button = start_button(button, theme, mode, shows_focus(world, node.entity), node.rtl);
        node.button_state = ButtonState{
            .mode = mode, .click_margin = static_cast<float>(theme.constant("click_margin"))};
        ButtonWork& work = *node.button;
        if (!button.icon.empty()) {
            std::string error;
            work.icon = texture_size(button.icon, &error);
            if (!work.icon) {
                problem(node.entity, "/components/" + std::string(type) + "/icon", error);
            }
        }
        if (type == "CheckBox" || type == "CheckButton") {
            start_check(node, work, button, theme, type == "CheckButton");
        }
        if (work.text.autowrap == text::Autowrap::Off) {
            shape(fonts, work.text, std::nullopt);
        } else {
            node.depends = Depends::Width;
        }
    }

    // The icon a CheckBox or CheckButton shows, as its NOTIFICATION_DRAW picks it, and the room
    // kept for it, as its NOTIFICATION_THEME_CHANGED sets the internal margins.
    void start_check(const Node& node, ButtonWork& work, const world::Button& button,
                     const Resolver& theme, bool check_button) {
        CheckWork& check = work.check.emplace();
        check.end = check_button;
        const bool rtl = node.rtl;
        const bool disabled = button.disabled;
        std::string on = disabled ? "checked_disabled" : "checked";
        std::string off = disabled ? "unchecked_disabled" : "unchecked";
        std::vector<std::string> sized;
        if (check_button) {
            if (rtl) {
                on += "_mirrored";
                off += "_mirrored";
            }
            sized = {on, off};
        } else {
            // In a group it is a radio button.
            if (!button.button_group.empty()) {
                on = "radio_" + on;
                off = "radio_" + off;
            }
            sized = {"checked",
                     "unchecked",
                     "radio_checked",
                     "radio_unchecked",
                     "checked_disabled",
                     "unchecked_disabled",
                     "radio_checked_disabled",
                     "radio_unchecked_disabled"};
        }
        Vec2 largest;
        for (const std::string& name : sized) {
            if (const auto size = texture_size(theme.icon(name)); size) {
                largest = max(largest, *size);
            }
        }
        check.largest = fit_icon(work, largest);
        const bool pressed = is_pressed(world, node.entity);
        check.icon = theme.icon(pressed ? on : off);
        check.size = texture_size(check.icon);
        const std::string prefix = check_button ? "button_" : "checkbox_";
        check.color = theme.color(prefix + (pressed ? "checked_color" : "unchecked_color"));
        const StyleBox normal = theme.style("normal");
        check.normal_left = normal.margin(kLeft);
        check.normal_right = normal.margin(kRight);
        check.v_offset = theme.constant("check_v_offset");
        // On the icon's side.
        work.internal.at(check_button != rtl ? 1 : 0) = check.largest.x;
    }

    // A Slider's theme items and its value kept in range, as Slider's theme cache and
    // NOTIFICATION_DRAW read them.
    void start_slider(Node& node, const world::Slider& slider, const Resolver& theme) {
        SliderWork& work = node.slider.emplace();
        work.slider = &slider;
        work.style = theme.style("slider");
        const bool editable = slider.editable;
        work.highlighted =
            editable && (is_hovered(world, node.entity) || has_focus(world, node.entity));
        work.grabber_area =
            theme.style(work.highlighted ? "grabber_area_highlight" : "grabber_area");
        std::string grabber = work.highlighted ? "grabber_highlight" : "grabber";
        if (!editable) {
            grabber = "grabber_disabled";
        }
        work.grabber_icon = theme.icon(grabber);
        work.grabber_size = texture_size(work.grabber_icon).value_or(Vec2{});
        work.minimum_grabber = texture_size(theme.icon("grabber")).value_or(Vec2{});
        work.drag_grabber = texture_size(theme.icon("grabber_highlight")).value_or(Vec2{});
        work.tick_icon = theme.icon("tick");
        work.tick_size = texture_size(work.tick_icon).value_or(Vec2{});
        work.center_grabber = theme.constant("center_grabber") != 0;
        work.grabber_offset = theme.constant("grabber_offset");
        work.grabber_max_size = theme.constant("grabber_max_size");
        work.tick_offset = theme.constant("tick_offset");
        work.tick_max_size = theme.constant("tick_max_size");
        work.rtl = node.rtl;
        work.range = range_of(slider);
    }

    // A LineEdit's theme items and its text shaped as it shows.
    void start_line_edit(Node& node, const world::LineEdit& edit, const Resolver& theme) {
        auto view = std::make_unique<LineEditView>();
        view->normal = theme.style("normal");
        view->read_only = theme.style("read_only");
        view->focus = theme.style("focus");
        view->font = theme.font("font");
        view->font_size = std::clamp(theme.font_size("font_size"), 1, text::kMaxFontSize);
        view->font_color = theme.color("font_color");
        view->font_selected_color = theme.color("font_selected_color");
        view->font_uneditable_color = theme.color("font_uneditable_color");
        view->font_placeholder_color = theme.color("font_placeholder_color");
        view->font_outline_color = theme.color("font_outline_color");
        view->caret_color = theme.color("caret_color");
        view->selection_color = theme.color("selection_color");
        view->clear_button_color = theme.color("clear_button_color");
        view->clear_button_color_pressed = theme.color("clear_button_color_pressed");
        view->minimum_character_width = theme.constant("minimum_character_width");
        view->outline_size = theme.constant("outline_size");
        view->caret_width = theme.constant("caret_width");
        view->rtl = node.rtl;
        if (!edit.right_icon.empty()) {
            std::string error;
            view->right_icon = texture_size(edit.right_icon, &error);
            if (!view->right_icon) {
                problem(node.entity, "/components/LineEdit/right_icon", error);
            }
        }
        node.line_edit = edit;
        const auto length = static_cast<int>(characters(edit).size());
        node.line_edit->caret_column = std::clamp(edit.caret_column, 0, length);
        if (const EditState* state = edit_state(world, node.entity); state != nullptr) {
            node.edit = *state;
        }
        node.edit_view = std::move(view);
        node.field.emplace(fonts, *node.edit_view, node.hint.value_or(Vec2{}));
        node.field->shape(*node.line_edit, node.edit);
        if (edit.icon_expand_mode == "fit_to_line_edit") {
            node.depends = Depends::Width;
        }
    }

    // A TextEdit's theme items, its scroll bars' from the VScrollBar and HScrollBar types (they
    // are its children in Godot, so its own overrides don't reach them).
    void start_text_edit(Node& node, const world::TextEdit& edit, const Resolver& theme) {
        auto view = std::make_unique<TextEditView>();
        view->normal = theme.style("normal");
        view->read_only = theme.style("read_only");
        view->focus = theme.style("focus");
        view->font = theme.font("font");
        view->font_size = std::clamp(theme.font_size("font_size"), 1, text::kMaxFontSize);
        view->font_color = theme.color("font_color");
        view->font_selected_color = theme.color("font_selected_color");
        view->font_readonly_color = theme.color("font_readonly_color");
        view->font_placeholder_color = theme.color("font_placeholder_color");
        view->font_outline_color = theme.color("font_outline_color");
        view->caret_color = theme.color("caret_color");
        view->caret_background_color = theme.color("caret_background_color");
        view->selection_color = theme.color("selection_color");
        view->current_line_color = theme.color("current_line_color");
        view->background_color = theme.color("background_color");
        view->line_spacing = theme.constant("line_spacing");
        view->outline_size = theme.constant("outline_size");
        view->caret_width = theme.constant("caret_width");
        view->wrap_offset = theme.constant("wrap_offset");
        static const world::Control kNoOverrides;
        const Resolver v_bar(node.themes, kNoOverrides, "VScrollBar");
        view->v_scroll = v_bar.style("scroll");
        view->v_grabber = v_bar.style("grabber");
        view->v_padding_left = v_bar.constant("padding_left");
        view->v_padding_right = v_bar.constant("padding_right");
        const Resolver h_bar(node.themes, kNoOverrides, "HScrollBar");
        view->h_scroll = h_bar.style("scroll");
        view->h_grabber = h_bar.style("grabber");
        view->h_padding_top = h_bar.constant("padding_top");
        view->h_padding_bottom = h_bar.constant("padding_bottom");
        view->rtl = node.rtl;
        node.text_edit = &edit;
        node.text_view = std::move(view);
        node.text_state = text_edit_state(world, node.entity);
        if (edit.scroll_fit_content_height && edit.wrap_mode == "boundary") {
            node.depends = Depends::Width;
        }
    }

    // The state a TextEdit shows: the gui system's, or a new one.
    [[nodiscard]] static const TextEditState& text_state_of(const Node& node) {
        static const TextEditState kFresh;
        return node.text_state != nullptr ? *node.text_state : kFresh;
    }

    void start_bar(Node& node, const world::ProgressBar& bar, const Resolver& theme) {
        node.bar = &bar;
        node.bar_background = theme.style("background");
        node.bar_fill = theme.style("fill");
        LabelText& percent = node.percent;
        percent.font = theme.font("font");
        percent.font_size = std::clamp(theme.font_size("font_size"), 1, text::kMaxFontSize);
        percent.font_color = theme.color("font_color");
        percent.outline_color = theme.color("font_outline_color");
        percent.shadow_color = {0.0F, 0.0F, 0.0F, 0.0F};
        percent.outline_size = std::max(theme.constant("outline_size"), 0);
        if (!bar.show_percentage) {
            return;
        }
        // Its minimum height fits "100%" in the font, as Godot's.
        text::LayoutOptions options{.font = percent.font,
                                    .font_size = percent.font_size,
                                    .autowrap = text::Autowrap::Off,
                                    .width = 0.0F};
        auto shaped = fonts.layout("100%", options);
        if (!shaped && !options.font.empty()) {
            percent.error = shaped.error().message;
            percent.font.clear();
            options.font.clear();
            shaped = fonts.layout("100%", options);
        }
        node.percent_height = shaped ? shaped.value().metrics.height() : 0.0F;
    }

    // The visible children a container places.
    [[nodiscard]] std::vector<std::size_t> sorted(const Node& node) const {
        std::vector<std::size_t> visible;
        for (const std::size_t kid : node.kids) {
            if (nodes[kid].control->visible) {
                visible.push_back(kid);
            }
        }
        return visible;
    }

    // ---- Minimum sizes ----

    // A FlowContainer's lines at a length along it, as the first pass of FlowContainer::_resort;
    // returns the length across it, its cached_size.
    int flow_lines(const Node& node, int length, std::vector<FlowLine>* lines) const {
        const bool vertical = node.flow->vertical;
        int ofs_main = 0;
        int ofs_cross = 0;
        int line_height = 0;
        float stretch_total = 0.0F;
        int in_line = 0;
        const int main_sep = vertical ? node.v_separation : node.h_separation;
        const int cross_sep = vertical ? node.h_separation : node.v_separation;
        std::size_t last = kNone;
        for (const std::size_t kid : sorted(node)) {
            const Node& child = nodes[kid];
            const auto main = static_cast<int>(vertical ? child.minimum.y : child.minimum.x);
            const auto cross = static_cast<int>(vertical ? child.minimum.x : child.minimum.y);
            if (in_line > 0) {
                ofs_main += main_sep;
            }
            if (ofs_main + main > length) {
                const int line_length = ofs_main - main_sep;
                if (lines != nullptr) {
                    lines->push_back({in_line, line_height, line_length, length - line_length,
                                      stretch_total, true});
                }
                ofs_cross += line_height + cross_sep;
                ofs_main = 0;
                line_height = 0;
                stretch_total = 0.0F;
                in_line = 0;
            }
            line_height = std::max(line_height, cross);
            const Flags flags = flags_of(vertical ? child.control->size_flags_vertical
                                                  : child.control->size_flags_horizontal);
            if (flags.expand) {
                stretch_total += child.control->size_flags_stretch_ratio;
            }
            ofs_main += main;
            ++in_line;
            last = kid;
        }
        bool filled = false;
        if (last != kNone) {
            const Node& child = nodes[last];
            filled =
                ofs_main + static_cast<int>(vertical ? child.minimum.y : child.minimum.x) > length;
        }
        if (lines != nullptr) {
            lines->push_back(
                {in_line, line_height, ofs_main, length - ofs_main, stretch_total, filled});
        }
        return ofs_cross + line_height;
    }

    // A container's minimum size from its children's, as each container's get_minimum_size.
    Vec2 container_minimum(const Node& node, std::optional<Vec2> size) const {
        const std::vector<std::size_t> kids = sorted(node);
        // The children's biggest minimum, as Container::_get_minimum_size.
        Vec2 biggest;
        for (const std::size_t kid : kids) {
            biggest = max(biggest, nodes[kid].minimum);
        }
        switch (node.kind) {
        case Kind::None:
            break;
        case Kind::Box: {
            const bool vertical = node.box->vertical;
            Vec2 sum;
            for (const std::size_t kid : kids) {
                const Vec2 m{std::ceil(nodes[kid].minimum.x), std::ceil(nodes[kid].minimum.y)};
                sum = vertical ? Vec2{std::max(sum.x, m.x), sum.y + m.y}
                               : Vec2{sum.x + m.x, std::max(sum.y, m.y)};
            }
            if (!kids.empty()) {
                const auto gaps =
                    static_cast<float>(node.separation * static_cast<int>(kids.size() - 1));
                (vertical ? sum.y : sum.x) += gaps;
            }
            return sum;
        }
        case Kind::Grid: {
            const int columns = std::max(node.grid->columns, 1);
            std::map<int, int> col_minw;
            std::map<int, int> row_minh;
            int index = 0;
            for (const std::size_t kid : kids) {
                const int row = index / columns;
                const int col = index % columns;
                ++index;
                const Vec2 m = nodes[kid].minimum;
                col_minw[col] = std::max(col_minw[col], static_cast<int>(std::ceil(m.x)));
                row_minh[row] = std::max(row_minh[row], static_cast<int>(std::ceil(m.y)));
            }
            Vec2 sum;
            for (const auto& [col, width] : col_minw) {
                sum.x += static_cast<float>(width);
            }
            for (const auto& [row, height] : row_minh) {
                sum.y += static_cast<float>(height);
            }
            if (!kids.empty()) {
                sum.x +=
                    static_cast<float>(node.h_separation * static_cast<int>(col_minw.size() - 1));
                sum.y +=
                    static_cast<float>(node.v_separation * static_cast<int>(row_minh.size() - 1));
            }
            return sum;
        }
        case Kind::Margin:
            return {biggest.x + static_cast<float>(node.margins[kLeft] + node.margins[kRight]),
                    biggest.y + static_cast<float>(node.margins[kTop] + node.margins[kBottom])};
        case Kind::Center:
            return node.center->use_top_left ? Vec2{} : biggest;
        case Kind::Flow: {
            if (kids.empty()) {
                return {};
            }
            const bool vertical = node.flow->vertical;
            const float along = std::trunc(vertical ? biggest.y : biggest.x);
            // Across, what its lines take at its length; with no length yet, one line.
            int length = INT_MAX;
            if (size) {
                length = static_cast<int>(vertical ? size->y : size->x);
            }
            const auto across = static_cast<float>(flow_lines(node, length, nullptr));
            return vertical ? Vec2{across, along} : Vec2{along, across};
        }
        case Kind::Panel: {
            const Vec2 style = node.container_panel->minimum_size();
            return {biggest.x + style.x, biggest.y + style.y};
        }
        }
        return {};
    }

    // A control's minimum size, as get_combined_minimum_size: each component's minimum and
    // custom_minimum_size, whichever is biggest. size is the control's own size, for minimums
    // that depend on it; without one they take the size it had in the last pass, or none.
    Vec2 minimum_of(Node& node, std::optional<Vec2> size) {
        if (!size) {
            size = node.hint;
        }
        Vec2 minimum = max(node.control->custom_minimum_size, container_minimum(node, size));
        if (node.label) {
            LabelWork& work = *node.label;
            if (work.autowrap != text::Autowrap::Off) {
                shape(fonts, work, size ? std::optional<float>(size->x) : std::nullopt);
            }
            minimum = max(minimum, label_minimum(work));
        }
        if (node.bar != nullptr) {
            Vec2 bar = max(node.bar_background.minimum_size(), node.bar_fill.minimum_size());
            if (node.bar->show_percentage) {
                bar.y = std::max(bar.y, node.bar_background.minimum_size().y + node.percent_height);
            } else {
                bar = max(bar, {1.0F, 1.0F});
            }
            minimum = max(minimum, bar);
        }
        if (node.texture_rect != nullptr && node.texture_size) {
            const Vec2 texture = *node.texture_size;
            const std::string& mode = node.texture_rect->expand_mode;
            const Vec2 own = size.value_or(Vec2{});
            if (mode == "keep_size") {
                minimum = max(minimum, texture);
            } else if (mode == "fit_width") {
                minimum = max(minimum, {own.y, 0.0F});
            } else if (mode == "fit_width_proportional") {
                minimum = max(minimum, {own.y * texture.x / texture.y, 0.0F});
            } else if (mode == "fit_height") {
                minimum = max(minimum, {0.0F, own.x});
            } else if (mode == "fit_height_proportional") {
                minimum = max(minimum, {0.0F, own.x * texture.y / texture.x});
            }
        }
        if (node.button) {
            ButtonWork& work = *node.button;
            if (work.text.autowrap != text::Autowrap::Off) {
                shape(fonts, work.text,
                      size ? std::optional<float>(button_text_width(work, *size)) : std::nullopt);
            }
            minimum = max(minimum, button_minimum(work));
        }
        if (node.field && node.line_edit) {
            node.field->resize(size.value_or(Vec2{}));
            minimum = max(minimum, node.field->minimum_size(*node.line_edit));
        }
        if (node.text_edit != nullptr) {
            const world::TextEdit& edit = *node.text_edit;
            if (edit.scroll_fit_content_height || edit.scroll_fit_content_width) {
                // Fitting its height, it is measured with room for every row, the size Godot's
                // TextEdit settles on a frame or two after its minimum size changes.
                Vec2 at = size.value_or(Vec2{});
                if (edit.scroll_fit_content_height) {
                    at.y = kRoomForAll;
                }
                const TextArea area =
                    text_area_for(fonts, *node.text_view, at, edit, text_state_of(node));
                minimum = max(minimum, area.minimum_size());
            } else {
                minimum = max(minimum,
                              (edit.editable ? node.text_view->normal : node.text_view->read_only)
                                  .minimum_size());
            }
        }
        if (node.slider) {
            // Slider::get_minimum_size, in whole pixels.
            const SliderWork& work = *node.slider;
            const Vec2 style = work.style.minimum_size();
            const Vec2 track{std::trunc(style.x), std::trunc(style.y)};
            const Vec2 grabber{std::trunc(work.minimum_grabber.x),
                               std::trunc(work.minimum_grabber.y)};
            minimum =
                max(minimum, work.slider->vertical ? Vec2{std::max(track.x, grabber.x), track.y}
                                                   : Vec2{track.x, std::max(track.y, grabber.y)});
        }
        if (node.texture_button != nullptr && !node.texture_button->ignore_texture_size) {
            // TextureButton::get_minimum_size: the normal texture's size, else the pressed
            // one's, else the hover one's.
            for (std::size_t i = 0; i < 3; ++i) {
                if (const auto& texture = node.button_textures.at(i); texture) {
                    minimum = max(minimum, {std::abs(texture->x), std::abs(texture->y)});
                    break;
                }
            }
        }
        if (node.nine_patch != nullptr) {
            const world::NinePatchRect& patch = *node.nine_patch;
            minimum = max(minimum,
                          {static_cast<float>(patch.patch_margin_left + patch.patch_margin_right),
                           static_cast<float>(patch.patch_margin_top + patch.patch_margin_bottom)});
        }
        return minimum;
    }

    // ---- Containers ----

    // Fits a child in a rect, as Container::fit_child_in_rect: the whole rect on an axis it
    // fills, else its minimum size at the start, center or end; across, the start is the right
    // in a container laid out right to left.
    void fit(std::size_t kid, Rect rect, bool rtl) {
        Node& child = nodes[kid];
        const Flags h = flags_of(child.control->size_flags_horizontal);
        const Flags v = flags_of(child.control->size_flags_vertical);
        Rect r = rect;
        if (!h.fill) {
            r.size.x = child.minimum.x;
            if (h.shrink == 1) {
                r.position.x += std::floor((rect.size.x - r.size.x) / 2.0F);
            } else if ((h.shrink == 2) != rtl) {
                r.position.x += rect.size.x - r.size.x;
            }
        }
        if (!v.fill) {
            r.size.y = child.minimum.y;
            if (v.shrink == 2) {
                r.position.y += rect.size.y - r.size.y;
            } else if (v.shrink == 1) {
                r.position.y += std::floor((rect.size.y - r.size.y) / 2.0F);
            }
        }
        child.assigned = r;
    }

    // BoxContainer::_resort.
    void sort_box(const Node& node, Vec2 size) {
        const bool vertical = node.box->vertical;
        const int width = static_cast<int>(size.x);
        const int height = static_cast<int>(size.y);
        struct Item {
            std::size_t kid;
            int min_size;
            int final_size;
            float stretch_ratio;
            bool will_stretch;
        };
        std::vector<Item> items;
        int combined_min = 0;
        int stretch_space = 0;
        float stretch_ratio_total = 0.0F;
        for (const std::size_t kid : sorted(node)) {
            const Node& child = nodes[kid];
            const int min_size =
                static_cast<int>(std::ceil(vertical ? child.minimum.y : child.minimum.x));
            const Flags flags = flags_of(vertical ? child.control->size_flags_vertical
                                                  : child.control->size_flags_horizontal);
            const Item item{.kid = kid,
                            .min_size = min_size,
                            .final_size = min_size,
                            .stretch_ratio = child.control->size_flags_stretch_ratio,
                            .will_stretch = flags.expand};
            if (item.will_stretch) {
                stretch_space += min_size;
                stretch_ratio_total += item.stretch_ratio;
            }
            combined_min += min_size;
            items.push_back(item);
        }
        if (items.empty()) {
            return;
        }
        const auto count = static_cast<int>(items.size());
        const int max_space = (vertical ? height : width) - (node.separation * (count - 1));
        stretch_space += std::max(max_space - combined_min, 0);

        // Space for those that expand, by stretch ratio; one that would get less than its
        // minimum keeps its minimum and the rest share again.
        while (stretch_ratio_total > 0.0F) {
            bool refit_successful = true;
            float error = 0.0F;
            for (Item& item : items) {
                if (!item.will_stretch) {
                    continue;
                }
                float final_pixel_size =
                    static_cast<float>(stretch_space) * item.stretch_ratio / stretch_ratio_total;
                error += final_pixel_size - static_cast<float>(static_cast<int>(final_pixel_size));
                if (error >= 1.0F) {
                    final_pixel_size += 1.0F;
                    error -= 1.0F;
                }
                if (final_pixel_size < static_cast<float>(item.min_size)) {
                    item.will_stretch = false;
                    stretch_ratio_total -= item.stretch_ratio;
                    refit_successful = false;
                    stretch_space -= item.min_size;
                    item.final_size = item.min_size;
                    break;
                }
                item.final_size = static_cast<int>(final_pixel_size);
            }
            if (refit_successful) {
                break;
            }
        }

        int final_stretch_diff = max_space - combined_min;
        for (const Item& item : items) {
            final_stretch_diff -= item.final_size - item.min_size;
        }
        final_stretch_diff = std::max(final_stretch_diff, 0);
        // Laid out right to left, a row starts at the right: its children go last first and
        // begin and end alignment swap.
        const bool reversed = node.rtl && !vertical;
        int ofs = 0;
        const int alignment = alignment_of(node.box->alignment);
        if (alignment == 1) {
            ofs = final_stretch_diff / 2;
        } else if ((alignment == 2) != reversed) {
            ofs = final_stretch_diff;
        }
        if (reversed) {
            std::ranges::reverse(items);
        }
        for (int i = 0; i < count; ++i) {
            const Item& item = items[static_cast<std::size_t>(i)];
            if (i > 0) {
                ofs += node.separation;
            }
            const int from = ofs;
            int to = ofs + item.final_size;
            if (item.will_stretch && i == count - 1) {
                // The last one ends at the end, whatever was lost to rounding.
                to = vertical ? height : width;
            }
            const auto length = static_cast<float>(to - from);
            fit(item.kid,
                vertical
                    ? Rect{{0.0F, static_cast<float>(from)}, {static_cast<float>(width), length}}
                    : Rect{{static_cast<float>(from), 0.0F}, {length, static_cast<float>(height)}},
                node.rtl);
            ofs = to;
        }
    }

    // GridContainer::_resort.
    void sort_grid(const Node& node, Vec2 size) {
        const int columns = std::max(node.grid->columns, 1);
        std::map<int, int> col_minw;
        std::map<int, int> row_minh;
        std::set<int> col_expanded;
        std::set<int> row_expanded;
        std::map<int, int> col_fixed;
        std::map<int, int> row_fixed;
        const std::vector<std::size_t> kids = sorted(node);
        int index = 0;
        for (const std::size_t kid : kids) {
            const int row = index / columns;
            const int col = index % columns;
            ++index;
            const Node& child = nodes[kid];
            const auto w = static_cast<int>(std::ceil(child.minimum.x));
            const auto h = static_cast<int>(std::ceil(child.minimum.y));
            col_minw[col] = col_minw.contains(col) ? std::max(col_minw[col], w) : w;
            row_minh[row] = row_minh.contains(row) ? std::max(row_minh[row], h) : h;
            if (flags_of(child.control->size_flags_horizontal).expand) {
                col_expanded.insert(col);
            }
            if (flags_of(child.control->size_flags_vertical).expand) {
                row_expanded.insert(row);
            }
        }
        const int valid = index;
        const int max_col = std::min(valid, columns);
        const auto max_row =
            static_cast<int>(std::ceil(static_cast<float>(valid) / static_cast<float>(columns)));
        // Empty columns count as expanded.
        for (int i = valid; i < columns; ++i) {
            col_expanded.insert(i);
        }
        Vec2 remaining = size;
        for (const auto& [col, width] : col_minw) {
            if (!col_expanded.contains(col)) {
                remaining.x -= static_cast<float>(width);
            }
        }
        for (const auto& [row, height] : row_minh) {
            if (!row_expanded.contains(row)) {
                remaining.y -= static_cast<float>(height);
            }
        }
        remaining.y -= static_cast<float>(node.v_separation * std::max(max_row - 1, 0));
        remaining.x -= static_cast<float>(node.h_separation * std::max(max_col - 1, 0));

        // Expanded columns and rows that can't fit their minimum in an even share stop
        // expanding, biggest first.
        const auto settle = [](std::set<int>& expanded, std::map<int, int>& minimums,
                               std::map<int, int>& fixed, float& space) {
            bool can_fit = false;
            while (!can_fit && !expanded.empty()) {
                can_fit = true;
                int max_index = *expanded.begin();
                for (const int each : expanded) {
                    if (minimums[each] > minimums[max_index]) {
                        max_index = each;
                    }
                    if (can_fit && (space / static_cast<float>(expanded.size())) <
                                       static_cast<float>(minimums[each])) {
                        can_fit = false;
                    }
                }
                if (!can_fit) {
                    expanded.erase(max_index);
                    space -= static_cast<float>(minimums[max_index]);
                    fixed[max_index] = minimums[max_index];
                }
            }
        };
        settle(col_expanded, col_minw, col_fixed, remaining.x);
        settle(row_expanded, row_minh, row_fixed, remaining.y);

        // An even share for each expanded column and row, the pixels left over to the first.
        const auto share = [](const std::set<int>& expanded, float space, int count,
                              int& each) -> int {
            if (expanded.empty()) {
                return 0;
            }
            each = static_cast<int>(space / static_cast<float>(expanded.size()));
            int left = static_cast<int>(
                space - static_cast<float>(static_cast<int>(expanded.size()) * each));
            int last = 0;
            for (int i = 0; i < count && left > 0; ++i) {
                if (expanded.contains(i)) {
                    last = i + 1;
                    --left;
                }
            }
            return last;
        };
        int col_expand = 0;
        int row_expand = 0;
        const int col_remaining_pixel_index = share(col_expanded, remaining.x, max_col, col_expand);
        const int row_remaining_pixel_index = share(row_expanded, remaining.y, max_row, row_expand);

        int col_ofs = 0;
        int row_ofs = 0;
        index = 0;
        for (const std::size_t kid : kids) {
            const int row = index / columns;
            const int col = index % columns;
            ++index;
            if (col == 0) {
                col_ofs = 0;
                if (row > 0) {
                    row_ofs += (row_expanded.contains(row - 1) ? row_expand : row_minh[row - 1]) +
                               node.v_separation;
                    if (row_expanded.contains(row - 1) && row - 1 < row_remaining_pixel_index) {
                        ++row_ofs;
                    }
                }
            }
            int w = col_expand;
            if (!col_expanded.contains(col)) {
                w = col_fixed.contains(col) ? col_fixed[col] : col_minw[col];
            }
            int h = row_expand;
            if (!row_expanded.contains(row)) {
                h = row_fixed.contains(row) ? row_fixed[row] : row_minh[row];
            }
            if (col_expanded.contains(col) && col < col_remaining_pixel_index) {
                ++w;
            }
            if (row_expanded.contains(row) && row < row_remaining_pixel_index) {
                ++h;
            }
            // Laid out right to left, columns go from the right.
            const int x = node.rtl ? static_cast<int>(size.x) - col_ofs - w : col_ofs;
            fit(kid,
                {{static_cast<float>(x), static_cast<float>(row_ofs)},
                 {static_cast<float>(w), static_cast<float>(h)}},
                node.rtl);
            col_ofs += w + node.h_separation;
        }
    }

    // Where a FlowContainer line's first child starts, as FlowContainer's alignment and
    // last_wrap_alignment put what its children leave of the line.
    static float flow_offset(int alignment, std::string_view last_wrap, bool last_line, float avail,
                             float prior) {
        const bool own = last_wrap != "inherit" && last_line;
        if (alignment == 0) {
            if (own && last_wrap == "end") {
                return avail - prior;
            }
            return own && last_wrap == "center" ? (avail - prior) * 0.5F : 0.0F;
        }
        if (alignment == 1) {
            if (own && last_wrap != "center") {
                return last_wrap == "end" ? avail - (prior * 0.5F) : prior * 0.5F;
            }
            return avail * 0.5F;
        }
        if (own && last_wrap != "end") {
            return last_wrap == "begin" ? prior : prior + ((avail - prior) * 0.5F);
        }
        return avail;
    }

    // FlowContainer::_resort.
    void sort_flow(const Node& node, Vec2 size) {
        const bool vertical = node.flow->vertical;
        const int length = static_cast<int>(vertical ? size.y : size.x);
        std::vector<FlowLine> lines;
        flow_lines(node, length, &lines);
        const int main_sep = vertical ? node.v_separation : node.h_separation;
        const int cross_sep = vertical ? node.h_separation : node.v_separation;
        const int alignment = alignment_of(node.flow->alignment);
        const auto main_flags = [&](const Node& child) {
            return flags_of(vertical ? child.control->size_flags_vertical
                                     : child.control->size_flags_horizontal);
        };
        const std::vector<std::size_t> kids = sorted(node);
        std::vector<int> stretch(kids.size(), 0);
        std::size_t line_index = 0;
        int in_line = 0;
        int ofs_main = 0;
        int ofs_cross = 0;
        for (std::size_t i = 0; i < kids.size(); ++i) {
            const Node& child = nodes[kids[i]];
            if (in_line >= lines[line_index].child_count) {
                ofs_cross += lines[line_index].min_line_height + cross_sep;
                ofs_main = 0;
                ++line_index;
                in_line = 0;
            }
            FlowLine& line = lines[line_index];
            if (in_line == 0) {
                // Share the line's space among its children that expand.
                const std::size_t end =
                    std::min(i + static_cast<std::size_t>(line.child_count), kids.size());
                float total = 0.0F;
                for (std::size_t j = i; j < end; ++j) {
                    if (main_flags(nodes[kids[j]]).expand) {
                        total += nodes[kids[j]].control->size_flags_stretch_ratio;
                    }
                }
                int used = 0;
                for (std::size_t j = i; j < end && total > 0.0F; ++j) {
                    const Node& each = nodes[kids[j]];
                    if (main_flags(each).expand) {
                        stretch[j] =
                            static_cast<int>(static_cast<float>(line.stretch_avail) *
                                             each.control->size_flags_stretch_ratio / total);
                        used += stretch[j];
                    }
                }
                line.stretch_avail = std::max(line.stretch_avail - used, 0);
                if (line.stretch_avail > 0) {
                    const bool last_line = line_index != 0 && !line.is_filled;
                    const float prior =
                        last_line ? static_cast<float>(lines[line_index - 1].stretch_avail) : 0.0F;
                    ofs_main += static_cast<int>(
                        flow_offset(alignment, node.flow->last_wrap_alignment, last_line,
                                    static_cast<float>(line.stretch_avail), prior));
                }
            }
            auto main = static_cast<int>(vertical ? child.minimum.y : child.minimum.x);
            auto cross = static_cast<int>(vertical ? child.minimum.x : child.minimum.y);
            const Flags cross_flags = flags_of(vertical ? child.control->size_flags_horizontal
                                                        : child.control->size_flags_vertical);
            if (cross_flags.fill || cross_flags.shrink != 0) {
                cross = line.min_line_height;
            }
            if (main_flags(child).expand) {
                main += stretch[i];
            }
            const Vec2 at{static_cast<float>(vertical ? ofs_cross : ofs_main),
                          static_cast<float>(vertical ? ofs_main : ofs_cross)};
            const Vec2 extent{static_cast<float>(vertical ? cross : main),
                              static_cast<float>(vertical ? main : cross)};
            Rect rect{at, extent};
            if (node.flow->reverse_fill && !vertical) {
                rect.position.y = size.y - rect.position.y - rect.size.y;
            }
            // Laid out right to left, rows start at the right and columns fill from it.
            if (vertical ? node.rtl != node.flow->reverse_fill : node.rtl) {
                rect.position.x = size.x - rect.position.x - rect.size.x;
            }
            fit(kids[i], rect, node.rtl);
            ofs_main += main + main_sep;
            ++in_line;
        }
    }

    // Gives each visible child of a container its rect.
    void sort(const Node& node, Vec2 size) {
        switch (node.kind) {
        case Kind::None:
            return;
        case Kind::Box:
            sort_box(node, size);
            return;
        case Kind::Grid:
            sort_grid(node, size);
            return;
        case Kind::Margin: {
            const auto w = static_cast<float>(static_cast<int>(size.x) - node.margins[kLeft] -
                                              node.margins[kRight]);
            const auto h = static_cast<float>(static_cast<int>(size.y) - node.margins[kTop] -
                                              node.margins[kBottom]);
            for (const std::size_t kid : sorted(node)) {
                fit(kid,
                    {{static_cast<float>(node.margins[kLeft]),
                      static_cast<float>(node.margins[kTop])},
                     {w, h}},
                    node.rtl);
            }
            return;
        }
        case Kind::Center:
            for (const std::size_t kid : sorted(node)) {
                const Vec2 m = nodes[kid].minimum;
                const Vec2 at = node.center->use_top_left
                                    ? Vec2{std::floor(-m.x * 0.5F), std::floor(-m.y * 0.5F)}
                                    : Vec2{std::floor((size.x - m.x) / 2.0F),
                                           std::floor((size.y - m.y) / 2.0F)};
                fit(kid, {at, m}, node.rtl);
            }
            return;
        case Kind::Flow:
            sort_flow(node, size);
            return;
        case Kind::Panel: {
            SLASH_ASSERT(node.container_panel.has_value(), "a panel container has its style");
            const StyleBox& style = *node.container_panel;
            const Vec2 inner{size.x - style.minimum_size().x, size.y - style.minimum_size().y};
            for (const std::size_t kid : sorted(node)) {
                fit(kid, {{style.margin(kLeft), style.margin(kTop)}, inner}, node.rtl);
            }
            return;
        }
        }
    }

    // ---- Placing ----

    void place(Node& node) {
        const world::Control& control = *node.control;
        const Node* parent = node.parent != kNone ? &nodes[node.parent] : nullptr;
        const Vec2 area = parent != nullptr ? parent->placed.size : out.viewport;
        float left = 0.0F;
        float top = 0.0F;
        float width = 0.0F;
        float height = 0.0F;
        Vec2 minimum = node.minimum;
        // Laid out right to left, a control is placed as if the parent were mirrored, then
        // mirrored back: anchors and offsets count from the right, and it grows the other way.
        const auto mirror = [&] {
            if (node.rtl) {
                left = area.x - left - width;
            }
        };
        if (node.assigned) {
            // Its container gave it a rect where it draws.
            left = node.assigned->position.x;
            top = node.assigned->position.y;
            width = node.assigned->size.x;
            height = node.assigned->size.y;
            mirror();
            grow(left, width, minimum.x, control.grow_horizontal);
            grow(top, height, minimum.y, control.grow_vertical);
        } else {
            left = control.offset_left + (control.anchor_left * area.x);
            top = control.offset_top + (control.anchor_top * area.y);
            width = control.offset_right + (control.anchor_right * area.x) - left;
            height = control.offset_bottom + (control.anchor_bottom * area.y) - top;
            // A minimum that depends on the control's size is taken at that size: a wrapping
            // label's height at its width, once its width is known.
            if (node.depends == Depends::Height) {
                grow(top, height, minimum.y, control.grow_vertical);
                minimum = minimum_of(node, Vec2{width, height});
                grow(left, width, minimum.x, control.grow_horizontal);
            } else {
                grow(left, width, minimum.x, control.grow_horizontal);
                if (node.depends == Depends::Width) {
                    minimum = minimum_of(node, Vec2{width, height});
                }
                grow(top, height, minimum.y, control.grow_vertical);
            }
        }
        mirror();
        Placed& placed = node.placed;
        placed.entity = node.entity;
        placed.parent = parent != nullptr ? parent->entity : entt::null;
        placed.position = {left, top};
        placed.size = {width, height};
        placed.minimum_size = minimum;
        const Transform local = local_transform(control, placed.position);
        placed.transform = parent != nullptr ? parent->placed.transform * local : local;
        placed.visible = control.visible && (parent == nullptr || parent->placed.visible);
        placed.modulate =
            parent != nullptr ? times(parent->placed.modulate, control.modulate) : control.modulate;
        placed.z =
            std::clamp(control.z_index + (parent != nullptr ? parent->placed.z : 0), -kMaxZ, kMaxZ);
        placed.texture_filter = control.texture_filter != "inherit" || parent == nullptr
                                    ? control.texture_filter
                                    : parent->placed.texture_filter;
        for (const std::size_t kid : node.kids) {
            nodes[kid].assigned.reset();
        }
        sort(node, placed.size);
    }

    // ---- What controls draw ----

    // A TextureRect's texture in its rect, as TextureRect's NOTIFICATION_DRAW.
    static Box texture_box(const Node& node, Vec2 texture, Vec2 size) {
        const world::TextureRect& rect = *node.texture_rect;
        Box box;
        box.style.type = StyleBox::Type::Texture;
        box.style.texture = rect.texture;
        box.style.flip_h = rect.flip_h;
        box.style.flip_v = rect.flip_v;
        box.texture_size = texture;
        box.size = size;
        const std::string& mode = rect.stretch_mode;
        if (mode == "tile") {
            box.style.axis_stretch_horizontal = StyleBox::Stretch::Tile;
            box.style.axis_stretch_vertical = StyleBox::Stretch::Tile;
        } else if (mode == "keep") {
            box.size = texture;
        } else if (mode == "keep_centered") {
            box.position = {(size.x - texture.x) / 2.0F, (size.y - texture.y) / 2.0F};
            box.size = texture;
        } else if (mode == "keep_aspect" || mode == "keep_aspect_centered") {
            auto tex_width = static_cast<int>(texture.x * size.y / texture.y);
            auto tex_height = static_cast<int>(size.y);
            if (static_cast<float>(tex_width) > size.x) {
                tex_width = static_cast<int>(size.x);
                tex_height =
                    static_cast<int>(texture.y * static_cast<float>(tex_width) / texture.x);
            }
            if (mode == "keep_aspect_centered") {
                box.position = {(size.x - static_cast<float>(tex_width)) / 2.0F,
                                (size.y - static_cast<float>(tex_height)) / 2.0F};
            }
            box.size = {static_cast<float>(tex_width), static_cast<float>(tex_height)};
        } else if (mode == "keep_aspect_covered") {
            const float scale = std::max(size.x / texture.x, size.y / texture.y);
            const Vec2 scaled{texture.x * scale, texture.y * scale};
            box.style.region_rect = {std::abs(scaled.x - size.x) / scale / 2.0F,
                                     std::abs(scaled.y - size.y) / scale / 2.0F, size.x / scale,
                                     size.y / scale};
        }
        return box;
    }

    // A ProgressBar's boxes and percentage, as ProgressBar's NOTIFICATION_DRAW.
    void bar_boxes(const Node& node, Vec2 size, std::vector<Box>& boxes) {
        const world::ProgressBar& bar = *node.bar;
        boxes.push_back(
            {.style = node.bar_background, .position = {}, .size = size, .texture_size = {}});
        // Range::set_value: stepped, then kept in range.
        double value = bar.value;
        if (bar.step > 0.0) {
            value = (std::round((value - bar.min_value) / bar.step) * bar.step) + bar.min_value;
        }
        if (!bar.allow_greater && value > bar.max_value) {
            value = bar.max_value;
        }
        if (!bar.allow_lesser && value < bar.min_value) {
            value = bar.min_value;
        }
        // Range::get_as_ratio.
        const double span = bar.max_value - bar.min_value;
        const bool empty_range = std::abs(span) <= 1e-5;
        double ratio = 1.0;
        if (!empty_range) {
            const double clamped = std::clamp(value, std::min(bar.min_value, bar.max_value),
                                              std::max(bar.min_value, bar.max_value));
            ratio = std::clamp((clamped - bar.min_value) / span, 0.0, 1.0);
        }
        const auto r = static_cast<float>(ratio);
        const Vec2 fill_min = node.bar_fill.minimum_size();
        const bool across = bar.fill_mode == "begin_to_end" || bar.fill_mode == "end_to_begin";
        const float length = across ? size.x : size.y;
        const float margins = std::trunc(across ? fill_min.x : fill_min.y);
        const auto p = static_cast<int>(std::round(r * (length - margins)));
        if (p > 0) {
            // Laid out right to left, begin_to_end fills from the right.
            const bool from_end = bar.fill_mode == (node.rtl ? "begin_to_end" : "end_to_begin") ||
                                  bar.fill_mode == "bottom_to_top";
            const float start = from_end ? std::round((1.0F - r) * (length - margins)) : 0.0F;
            const float filled = static_cast<float>(p) + (across ? fill_min.x : fill_min.y);
            boxes.push_back({.style = node.bar_fill,
                             .position = across ? Vec2{start, 0.0F} : Vec2{0.0F, start},
                             .size = across ? Vec2{filled, size.y} : Vec2{size.x, filled},
                             .texture_size = {}});
        }
        if (!bar.show_percentage) {
            return;
        }
        double shown = 1.0;
        if (!empty_range) {
            const double percentage = (value - bar.min_value) / span;
            shown = std::clamp(percentage, bar.allow_lesser ? percentage : 0.0,
                               bar.allow_greater ? percentage : 1.0);
        }
        LabelText percent = node.percent;
        percent.source = std::to_string(static_cast<int>(std::round(shown * 100.0))) + "%";
        auto shaped = fonts.layout(percent.source, {.font = percent.font,
                                                    .font_size = percent.font_size,
                                                    .autowrap = text::Autowrap::Off,
                                                    .width = 0.0F});
        if (!shaped) {
            return;
        }
        percent.text = std::move(shaped).value();
        percent.first = 0;
        percent.last = percent.text.lines.size();
        // Centered, on whole pixels.
        const Vec2 at{std::round((size.x - percent.text.width) / 2.0F),
                      std::round((size.y - percent.text.metrics.height()) / 2.0F)};
        for (std::size_t l = 0; l < percent.last; ++l) {
            percent.pens.push_back({at.x, at.y + percent.text.metrics.ascent});
            percent.stretch.push_back(0.0F);
        }
        out.percentages.emplace(node.entity, std::move(percent));
    }

    // Keeps a box if its style draws something: not an empty one, nor a texture one whose
    // texture doesn't load.
    bool keep(Box& box) {
        if (box.style.type == StyleBox::Type::Empty) {
            return false;
        }
        if (box.style.type == StyleBox::Type::Texture && box.texture_size == Vec2{}) {
            const auto texture = texture_size(box.style.texture);
            if (!texture) {
                return false;
            }
            box.texture_size = *texture;
        }
        return true;
    }

    void finish(Node& node) {
        const Vec2 size = node.placed.size;
        std::vector<Box> boxes;
        if (node.panel) {
            boxes.push_back(
                {.style = *node.panel, .position = {}, .size = size, .texture_size = {}});
        }
        if (node.color_rect != nullptr) {
            StyleBox fill;
            fill.bg_color = node.color_rect->color;
            fill.anti_aliasing = false;
            boxes.push_back({.style = fill, .position = {}, .size = size, .texture_size = {}});
        }
        if (node.texture_rect != nullptr && node.texture_size) {
            boxes.push_back(texture_box(node, *node.texture_size, size));
        }
        if (node.nine_patch != nullptr && node.patch_texture_size) {
            const world::NinePatchRect& patch = *node.nine_patch;
            Box box{.style = {},
                    .position = {},
                    .size = size,
                    .texture_size = *node.patch_texture_size};
            box.style.type = StyleBox::Type::Texture;
            box.style.texture = patch.texture;
            box.style.region_rect = patch.region_rect;
            box.style.texture_margin = {static_cast<float>(patch.patch_margin_left),
                                        static_cast<float>(patch.patch_margin_top),
                                        static_cast<float>(patch.patch_margin_right),
                                        static_cast<float>(patch.patch_margin_bottom)};
            box.style.draw_center = patch.draw_center;
            box.style.axis_stretch_horizontal = stretch_of(patch.axis_stretch_horizontal);
            box.style.axis_stretch_vertical = stretch_of(patch.axis_stretch_vertical);
            boxes.push_back(std::move(box));
        }
        if (node.bar != nullptr) {
            bar_boxes(node, size, boxes);
        }
        if (node.button) {
            finish_button(*node.button, size, boxes);
        }
        if (node.texture_button != nullptr && node.button_state) {
            texture_button_boxes(node, size, boxes);
        }
        if (node.slider) {
            slider_boxes(*node.slider, size, boxes);
            const SliderWork& work = *node.slider;
            out.sliders.emplace(node.entity, SliderState{.value = work.range.value,
                                                         .ratio = range_ratio(work.range),
                                                         .grabber = work.drag_grabber,
                                                         .center_grabber = work.center_grabber,
                                                         .highlighted = work.highlighted,
                                                         .rtl = work.rtl});
        }
        if (node.field) {
            finish_line_edit(node, size, boxes);
        }
        if (node.text_edit != nullptr) {
            finish_text_edit(node, size, boxes);
        }
        std::erase_if(boxes, [&](Box& box) { return !keep(box); });
        if (!boxes.empty()) {
            out.boxes.emplace(node.entity, std::move(boxes));
        }
        if (node.button_state) {
            out.buttons.emplace(node.entity, *node.button_state);
        }
        // A Button's text is drawn over a Label's on the same entity; check() reports both.
        if (node.button && !node.button->button->text.empty()) {
            out.labels.emplace(node.entity, std::move(node.button->text.out));
        }
        if (node.label) {
            LabelWork& work = *node.label;
            if (work.autowrap != text::Autowrap::Off) {
                shape(fonts, work, size.x);
            }
            finish_label(fonts, work, size, node.rtl);
            work.out.clip = work.label->clip_text;
            out.labels.emplace(node.entity, std::move(work.out));
        }
    }

    // What a LineEdit draws, scrolled to show its caret at the size it got.
    void finish_line_edit(Node& node, Vec2 size, std::vector<Box>& boxes) {
        if (!node.field || !node.line_edit) {
            return;
        }
        Field& field = *node.field;
        field.resize(size);
        field.fit(*node.line_edit, node.edit);
        LabelText text;
        std::vector<Box> over;
        LineEditView view = *node.edit_view;
        field.draw(*node.line_edit, node.edit, has_focus(world, node.entity), boxes, text, over,
                   view);
        view.error = field.error();
        out.labels.emplace(node.entity, std::move(text));
        if (!over.empty()) {
            out.overlays.emplace(node.entity, std::move(over));
        }
        out.line_edits.emplace(node.entity, std::move(view));
    }

    // What a TextEdit draws at the size it got.
    void finish_text_edit(Node& node, Vec2 size, std::vector<Box>& boxes) {
        const world::TextEdit& edit = *node.text_edit;
        const TextArea area =
            text_area_for(fonts, *node.text_view, size, edit, text_state_of(node));
        TextEditState fallback;
        const TextEditState& shown = shown_state(edit, node.text_state, area, fallback);
        TextArea::Draw draw;
        TextEditView view = *node.text_view;
        area.draw(edit, shown, has_focus(world, node.entity), draw, view);
        view.error = area.error();
        boxes.insert(boxes.end(), std::make_move_iterator(draw.under.begin()),
                     std::make_move_iterator(draw.under.end()));
        out.labels.emplace(node.entity, std::move(draw.text));
        if (!draw.over.empty()) {
            out.overlays.emplace(node.entity, std::move(draw.over));
        }
        out.text_edits.emplace(node.entity, std::move(view));
    }

    // A Button's style box, focus box, icon and text, as its NOTIFICATION_DRAW draws them.
    void finish_button(ButtonWork& work, Vec2 size, std::vector<Box>& boxes) {
        const world::Button& button = *work.button;
        if (!button.flat) {
            boxes.push_back(
                {.style = work.style, .position = {}, .size = size, .texture_size = {}});
        }
        if (work.focused) {
            boxes.push_back(
                {.style = work.focus, .position = {}, .size = size, .texture_size = {}});
        }
        finish_button_content(work, size, boxes);
        if (work.check) {
            check_box(work, *work.check, size, boxes);
        }
    }

    // A CheckBox's or CheckButton's icon, as its NOTIFICATION_DRAW draws it after Button's.
    static void check_box(const ButtonWork& work, const CheckWork& check, Vec2 size,
                          std::vector<Box>& boxes) {
        if (!check.size) {
            return;
        }
        const Vec2 largest = check.largest;
        Vec2 at;
        // A CheckBox's icon starts the button, a CheckButton's ends it; laid out right to left,
        // the other way round.
        const bool right = check.end != work.rtl;
        at.x = right ? size.x - check.normal_right - largest.x : check.normal_left;
        // CheckBox rounds down to a whole pixel; CheckButton doesn't.
        const float y = (size.y - largest.y) / 2.0F;
        at.y = (check.end ? y : std::trunc(y)) + static_cast<float>(check.v_offset);
        Box box{.style = {},
                .position = at,
                .size = fit_icon(work, *check.size),
                .texture_size = *check.size};
        box.style.type = StyleBox::Type::Texture;
        box.style.texture = check.icon;
        box.style.modulate_color = check.color;
        boxes.push_back(std::move(box));
    }

    // A Button's icon and text, as its NOTIFICATION_DRAW places them.
    void finish_button_content(ButtonWork& work, Vec2 size, std::vector<Box>& boxes) {
        const world::Button& button = *work.button;
        if (button.text.empty() && !work.icon) {
            return;
        }
        const std::array<float, 4> m = button_margins(work);
        const std::array<float, 2> internal = internal_margins(work);
        const auto h_separation = static_cast<float>(std::max(work.h_separation, 0));
        Vec2 room{size.x - m[kLeft] - m[kRight] - internal[0] - internal[1],
                  size.y - m[kTop] - m[kBottom]};
        const Vec2 element = room;
        const bool center_v = button.vertical_icon_alignment == "center";
        // Laid out right to left, left and right swap for the icon and the text.
        const auto mirrored = [&](const std::string& alignment) -> std::string_view {
            if (work.rtl && alignment != "center") {
                return alignment == "left" ? "right" : "left";
            }
            return alignment;
        };
        const std::string_view icon_alignment = mirrored(button.icon_alignment);
        const std::string_view alignment = mirrored(button.alignment);
        if (work.icon) {
            const Vec2 icon = button_icon_size(work, element);
            if (icon.x > 0.0F) {
                Vec2 at;
                if (icon_alignment == "center") {
                    at.x = ((element.x - icon.x) / 2.0F) + m[kLeft] + internal[0];
                } else if (icon_alignment == "right") {
                    at.x = size.x - m[kRight] - internal[1] - icon.x;
                } else {
                    at.x = m[kLeft] + internal[0];
                }
                if (center_v) {
                    at.y = ((element.y - icon.y) / 2.0F) + m[kTop];
                } else if (button.vertical_icon_alignment == "bottom") {
                    at.y = size.y - m[kBottom] - icon.y;
                } else {
                    at.y = m[kTop];
                }
                Box box{.style = {},
                        .position = {std::floor(at.x), std::floor(at.y)},
                        .size = icon,
                        .texture_size = *work.icon};
                box.style.type = StyleBox::Type::Texture;
                box.style.texture = button.icon;
                box.style.modulate_color = work.icon_color;
                boxes.push_back(std::move(box));
            }
            if (!button.text.empty()) {
                if (icon_alignment != "center") {
                    room.x -= icon.x + h_separation;
                }
                if (!center_v) {
                    room.y -= icon.y;
                }
            }
        }
        if (button.text.empty()) {
            return;
        }
        LabelWork& text = work.text;
        const float width = std::ceil(std::max(1.0F, room.x));
        if (text.autowrap != text::Autowrap::Off) {
            shape(fonts, text, width);
        }
        const Vec2 text_size = button_text_size(work);
        LabelText& shown = text.out;
        text::TextLayout& layout = shown.text;
        shown.first = 0;
        shown.last = layout.lines.size();
        if (text.autowrap == text::Autowrap::Off && text.overrun != text::Overrun::NoTrimming) {
            for (std::size_t l = 0; l < layout.lines.size(); ++l) {
                if (fonts.trim(layout, l, width, text.overrun, shown.ellipsis)) {
                    shown.trimmed.push_back(l);
                }
            }
        }
        // Lines are aligned by their whole width, then glyphs that don't fit aren't drawn.
        std::vector<float> widths;
        for (std::size_t l = 0; l < layout.lines.size(); ++l) {
            widths.push_back(layout.lines[l].width);
            if (layout.lines[l].width > width + 0.01F) {
                shown.overflow = true;
                if (fonts.trim(layout, l, width, text::Overrun::TrimChar, "") &&
                    std::ranges::find(shown.trimmed, l) == shown.trimmed.end()) {
                    shown.trimmed.push_back(l);
                }
            }
        }
        Vec2 at;
        if (alignment == "center") {
            at.x = (room.x - width) / 2.0F;
        }
        at.x += m[kLeft] + internal[0];
        if (work.icon && icon_alignment == "left") {
            at.x += element.x - room.x;
        }
        at.y = ((room.y - text_size.y) / 2.0F) + m[kTop];
        if (button.vertical_icon_alignment == "top") {
            at.y += element.y - room.y;
        }
        float y = at.y;
        for (std::size_t l = 0; l < layout.lines.size(); ++l) {
            float x = 0.0F;
            if (alignment == "center" && widths[l] <= width) {
                x = std::floor((width - widths[l]) / 2.0F);
            } else if (alignment == "right") {
                x = width - widths[l];
            }
            y += layout.metrics.ascent;
            shown.pens.push_back({at.x + x, y});
            shown.stretch.push_back(0.0F);
            y += layout.metrics.descent + static_cast<float>(text.line_spacing);
        }
    }

    // Slider::_fit_icon_size: scaled down to fit max_size each way, in whole pixels.
    static Vec2 fit_slider_icon(Vec2 size, int max_size) {
        const int largest = static_cast<int>(std::max(size.x, size.y));
        if (max_size <= 0 || largest <= max_size) {
            return size;
        }
        const double scale = static_cast<double>(max_size) / largest;
        return {static_cast<float>(static_cast<int>(size.x * scale)),
                static_cast<float>(static_cast<int>(size.y * scale))};
    }

    // A Slider's track, the filled part of it, its ticks and its grabber, as its
    // NOTIFICATION_DRAW draws them, with Godot's whole-pixel rects.
    static void slider_boxes(const SliderWork& work, Vec2 rect_size, std::vector<Box>& boxes) {
        const world::Slider& slider = *work.slider;
        const int width = static_cast<int>(rect_size.x);
        const int height = static_cast<int>(rect_size.y);
        double ratio = range_ratio(work.range);
        if (std::isnan(ratio)) {
            ratio = 0.0;
        }
        const auto rounded = [](Vec2 v) { return Vec2{std::round(v.x), std::round(v.y)}; };
        const Vec2 grabber = rounded(fit_slider_icon(work.grabber_size, work.grabber_max_size));
        const Vec2 tick = rounded(fit_slider_icon(work.tick_size, work.tick_max_size));
        const auto box = [&](const StyleBox& style, int x, int y, int w, int h) {
            boxes.push_back({.style = style,
                             .position = {static_cast<float>(x), static_cast<float>(y)},
                             .size = {static_cast<float>(w), static_cast<float>(h)},
                             .texture_size = {}});
        };
        const auto icon = [&](const std::string& path, Vec2 texture, Vec2 at, Vec2 size) {
            if (path.empty() || texture == Vec2{}) {
                return;
            }
            // A negative size flips the texture, as RenderingServer's texture rects do.
            Box out{.style = {},
                    .position = at,
                    .size = {std::abs(size.x), std::abs(size.y)},
                    .texture_size = texture};
            out.style.type = StyleBox::Type::Texture;
            out.style.texture = path;
            out.style.flip_h = size.x < 0.0F;
            out.style.flip_v = size.y < 0.0F;
            boxes.push_back(std::move(out));
        };
        const std::string& position = slider.ticks_position;
        const bool after = position == "bottom_right" || position == "both";
        const bool before = position == "top_left" || position == "both";
        const int ticks = slider.tick_count;
        if (slider.vertical) {
            const int widget = static_cast<int>(work.style.minimum_size().x);
            const int side = (width - widget) / 2; // the track's left
            const double area =
                static_cast<float>(height) - (work.center_grabber ? 0.0F : grabber.y);
            const int shift = work.center_grabber ? static_cast<int>(grabber.y / 2.0F) : 0;
            box(work.style, (width / 2) - (widget / 2), 0, widget, height);
            box(work.grabber_area, side,
                static_cast<int>(std::round(height - (area * ratio) - (grabber.y / 2.0F) + shift)),
                widget, static_cast<int>(std::round((area * ratio) + (grabber.y / 2.0F) - shift)));
            if (ticks > 1) {
                const int offset = static_cast<int>((grabber.y / 2.0F) - (tick.y / 2.0F));
                for (int i = 0; i < ticks; ++i) {
                    if (!slider.ticks_on_borders && (i == 0 || i + 1 == ticks)) {
                        continue;
                    }
                    const auto at = static_cast<float>(
                        static_cast<int>((i * area / (ticks - 1)) + offset - shift));
                    const int right = widget + side + work.tick_offset;
                    const int left = side - static_cast<int>(tick.x) - work.tick_offset;
                    if (after) {
                        icon(work.tick_icon, work.tick_size, {static_cast<float>(right), at}, tick);
                    }
                    if (before) {
                        icon(work.tick_icon, work.tick_size, {static_cast<float>(left), at},
                             {-tick.x, tick.y});
                    }
                    if (position == "center") {
                        icon(work.tick_icon, work.tick_size,
                             {static_cast<float>(
                                  static_cast<int>(((static_cast<float>(width) - tick.x) / 2.0F) +
                                                   static_cast<float>(work.tick_offset))),
                              at},
                             tick);
                    }
                }
            }
            const int middle = width / 2;
            const Vec2 at{
                static_cast<float>(static_cast<int>(static_cast<float>(middle) -
                                                    (grabber.x / 2.0F) +
                                                    static_cast<float>(work.grabber_offset))),
                static_cast<float>(static_cast<int>(height - (ratio * area) - grabber.y + shift))};
            icon(work.grabber_icon, work.grabber_size, at, grabber);
            return;
        }
        const int widget = static_cast<int>(work.style.minimum_size().y);
        const int top = (height - widget) / 2; // the track's top
        const double area = static_cast<float>(width) - (work.center_grabber ? 0.0F : grabber.x);
        const int shift = work.center_grabber ? static_cast<int>(-grabber.x / 2.0F) : 0;
        const double along = work.rtl ? 1.0 - ratio : ratio;
        box(work.style, 0, top, width, widget);
        const int p = static_cast<int>((area * along) + (grabber.x / 2.0F) + shift);
        if (work.rtl) {
            box(work.grabber_area, p, top, width - p, widget);
        } else {
            box(work.grabber_area, 0, top, p, widget);
        }
        if (ticks > 1) {
            const int offset = static_cast<int>((grabber.x / 2.0F) - (tick.x / 2.0F));
            for (int i = 0; i < ticks; ++i) {
                if (!slider.ticks_on_borders && (i == 0 || i + 1 == ticks)) {
                    continue;
                }
                const auto at =
                    static_cast<float>(static_cast<int>((i * area / (ticks - 1)) + offset + shift));
                const int below = widget + top + work.tick_offset;
                const int above = top - static_cast<int>(tick.y) - work.tick_offset;
                if (after) {
                    icon(work.tick_icon, work.tick_size, {at, static_cast<float>(below)}, tick);
                }
                if (before) {
                    icon(work.tick_icon, work.tick_size, {at, static_cast<float>(above)},
                         {tick.x, -tick.y});
                }
                if (position == "center") {
                    icon(work.tick_icon, work.tick_size,
                         {at, static_cast<float>(
                                  static_cast<int>(((static_cast<float>(height) - tick.y) / 2.0F) +
                                                   static_cast<float>(work.tick_offset)))},
                         tick);
                }
            }
        }
        const int middle = height / 2;
        const Vec2 at{
            static_cast<float>(static_cast<int>((along * area) + shift)),
            static_cast<float>(static_cast<int>(static_cast<float>(middle) - (grabber.y / 2.0F) +
                                                static_cast<float>(work.grabber_offset)))};
        icon(work.grabber_icon, work.grabber_size, at, grabber);
    }

    // A TextureButton's texture for its state and its focused texture over it, as its
    // NOTIFICATION_DRAW.
    void texture_button_boxes(const Node& node, Vec2 size, std::vector<Box>& boxes) const {
        if (!node.button_state) {
            return;
        }
        const world::TextureButton& button = *node.texture_button;
        const auto& textures = node.button_textures;
        const std::array<const std::string*, 5> paths{
            &button.texture_normal, &button.texture_pressed, &button.texture_hover,
            &button.texture_disabled, &button.texture_focused};
        constexpr std::size_t kNormal = 0;
        constexpr std::size_t kPressed = 1;
        constexpr std::size_t kHover = 2;
        constexpr std::size_t kDisabled = 3;
        constexpr std::size_t kFocused = 4;
        std::size_t drawn = kNone;
        const auto first = [&](std::initializer_list<std::size_t> order) {
            for (const std::size_t i : order) {
                if (textures.at(i)) {
                    return i;
                }
            }
            return kNone;
        };
        switch (node.button_state->mode) {
        case DrawMode::Normal:
            drawn = first({kNormal});
            break;
        case DrawMode::HoverPressed:
        case DrawMode::Pressed:
            drawn = first({kPressed, kHover, kNormal});
            break;
        case DrawMode::Hover:
            drawn = first({kHover, kNormal});
            break;
        case DrawMode::Disabled:
            drawn = first({kDisabled, kNormal});
            break;
        }
        const bool focus = shows_focus(world, node.entity) && textures.at(kFocused);
        // With no other texture, the focused one gives the rect.
        const bool focus_only = focus && drawn == kNone;
        if (focus_only) {
            drawn = kFocused;
        }
        if (drawn == kNone || !textures.at(drawn)) {
            return;
        }
        const Vec2 texture = textures.at(drawn).value_or(Vec2{});
        Box box{.style = {}, .position = {}, .size = texture, .texture_size = texture};
        box.style.type = StyleBox::Type::Texture;
        box.style.texture = *paths.at(drawn);
        box.style.flip_h = button.flip_h;
        box.style.flip_v = button.flip_v;
        const std::string& mode = button.stretch_mode;
        if (mode == "scale") {
            box.size = size;
        } else if (mode == "tile") {
            box.size = size;
            box.style.axis_stretch_horizontal = StyleBox::Stretch::Tile;
            box.style.axis_stretch_vertical = StyleBox::Stretch::Tile;
        } else if (mode == "keep_centered") {
            box.position = {(size.x - texture.x) / 2.0F, (size.y - texture.y) / 2.0F};
        } else if (mode == "keep_aspect" || mode == "keep_aspect_centered") {
            float width = texture.x * size.y / texture.y;
            float height = size.y;
            if (width > size.x) {
                width = size.x;
                height = texture.y * width / texture.x;
            }
            if (mode == "keep_aspect_centered") {
                box.position = {(size.x - width) / 2.0F, (size.y - height) / 2.0F};
            }
            box.size = {width, height};
        } else if (mode == "keep_aspect_covered") {
            box.size = size;
            const float scale = std::max(size.x / texture.x, size.y / texture.y);
            box.style.region_rect = {std::abs((texture.x * scale) - size.x) / scale / 2.0F,
                                     std::abs((texture.y * scale) - size.y) / scale / 2.0F,
                                     size.x / scale, size.y / scale};
        }
        if (!focus_only) {
            boxes.push_back(box);
        }
        if (focus) {
            Box over = box;
            over.style = {};
            over.style.type = StyleBox::Type::Texture;
            over.style.texture = *paths.at(kFocused);
            over.style.flip_h = button.flip_h;
            over.style.flip_v = button.flip_v;
            over.texture_size = textures.at(kFocused).value_or(Vec2{});
            boxes.push_back(std::move(over));
        }
    }
};

// The entities that are controls, in the order they were made.
std::vector<entt::entity> all_controls(const world::World& world) {
    std::vector<entt::entity> controls;
    for (const auto [entity] : world.ecs().storage<entt::entity>()->each()) {
        if (is_control(world, entity)) {
            controls.push_back(entity);
        }
    }
    std::ranges::sort(controls, [](entt::entity a, entt::entity b) {
        return entt::to_entity(a) < entt::to_entity(b);
    });
    return controls;
}

// Links the controls into trees and makes a node for each, parents before children, with the
// themes each one uses.
void build(Builder& builder, const std::vector<entt::entity>& controls) {
    builder.link(controls);
    builder.break_loops(controls);
    const ThemeLink* project = builder.project_theme();
    std::vector<std::pair<entt::entity, std::size_t>> stack;
    for (const entt::entity entity : std::views::reverse(controls)) {
        if (!builder.parents.contains(entity)) {
            stack.emplace_back(entity, kNone);
        }
    }
    while (!stack.empty()) {
        const auto [entity, parent] = stack.back();
        stack.pop_back();
        const std::size_t index = builder.nodes.size();
        Node& node = builder.nodes.emplace_back();
        node.entity = entity;
        node.parent = parent;
        node.control = &builder.control_of(entity);
        const ThemeLink* themes = parent != kNone ? builder.nodes[parent].themes : project;
        if (const std::string& path = node.control->theme; !path.empty()) {
            std::string error;
            if (const Theme* theme = builder.theme(path, &error); theme != nullptr) {
                themes = &builder.links.emplace_back(
                    ThemeLink{.theme = theme, .path = path, .parent = themes});
            } else {
                builder.problem(entity, "/components/Control/theme", error);
            }
        }
        builder.nodes[index].themes = themes;
        const std::string& direction = node.control->layout_direction;
        builder.nodes[index].rtl = direction == "inherited"
                                       ? parent != kNone && builder.nodes[parent].rtl
                                       : direction == "rtl";
        if (parent != kNone) {
            builder.nodes[parent].kids.push_back(index);
        }
        if (const auto found = builder.children.find(entity); found != builder.children.end()) {
            for (const entt::entity child : std::views::reverse(found->second)) {
                stack.emplace_back(child, index);
            }
        }
    }
}

} // namespace

Layout layout(const world::World& world, text::Fonts& fonts, Vec2 viewport) {
    Layout out;
    out.viewport = viewport;
    Builder builder(world, fonts, out);
    build(builder, all_controls(world));
    std::vector<Node>& nodes = builder.nodes;
    for (Node& node : nodes) {
        builder.start(node);
        out.tree.push_back(node.entity);
    }

    for (int pass = 0; pass < kMaxPasses; ++pass) {
        for (Node& node : std::views::reverse(nodes)) {
            node.minimum = builder.minimum_of(node, std::nullopt);
        }
        for (Node& node : nodes) {
            builder.place(node);
        }
        // A control a container sized whose minimum at that size isn't the one the container
        // used: lay out again with that size.
        bool settled = true;
        for (Node& node : nodes) {
            if (node.depends == Depends::None || !node.assigned) {
                continue;
            }
            const Vec2 actual = builder.minimum_of(node, node.placed.size);
            if (std::abs(actual.x - node.minimum.x) > 0.01F ||
                std::abs(actual.y - node.minimum.y) > 0.01F) {
                node.hint = node.placed.size;
                settled = false;
            }
        }
        if (settled) {
            break;
        }
    }

    for (Node& node : nodes) {
        builder.finish(node);
        out.controls.push_back(node.placed);
    }
    // Then by z: Godot draws a canvas item over those with a lower z_index, the rest in tree
    // order.
    std::ranges::stable_sort(out.controls,
                             [](const Placed& a, const Placed& b) { return a.z < b.z; });
    for (std::size_t i = 0; i < out.controls.size(); ++i) {
        out.index.emplace(out.controls[i].entity, i);
    }
    return out;
}

std::vector<ThemeItem> theme_items(const world::World& world, text::Fonts& fonts,
                                   entt::entity entity) {
    Layout scratch;
    Builder builder(world, fonts, scratch);
    build(builder, all_controls(world));
    const auto node = std::ranges::find(builder.nodes, entity, &Node::entity);
    std::vector<ThemeItem> out;
    if (node == builder.nodes.end()) {
        return out;
    }
    // By kind then name; the first type to use an item gives it.
    std::map<std::pair<std::string, std::string>, ThemeItem, std::less<>> items;
    const auto add = [&](std::string_view kind, const std::string& name, json::Value value,
                         std::string from) {
        std::pair<std::string, std::string> key{std::string(kind), name};
        if (!items.contains(key)) {
            ThemeItem item{.kind = key.first, .name = name, .value = {}, .from = std::move(from)};
            item.value = std::move(value);
            items.emplace(std::move(key), std::move(item));
        }
    };
    for (const std::string_view type : control_classes(world, entity)) {
        const Resolver theme(node->themes, *node->control, type);
        const ThemeItems& known = default_theme(type);
        std::string from;
        for (const auto& [name, fallback] : known.colors) {
            const Vec4 c = theme.color(name, &from);
            add("colors", name, json::Array{decimal(c.x), decimal(c.y), decimal(c.z), decimal(c.w)},
                from);
        }
        for (const auto& [name, fallback] : known.constants) {
            const int value = theme.constant(name, &from);
            add("constants", name, value, from);
        }
        for (const auto& [name, fallback] : known.fonts) {
            std::string font = theme.font(name, &from);
            add("fonts", name, std::move(font), from);
        }
        for (const auto& [name, fallback] : known.font_sizes) {
            const int size = theme.font_size(name, &from);
            add("font_sizes", name, size, from);
        }
        for (const auto& [name, fallback] : known.icons) {
            std::string icon = theme.icon(name, &from);
            add("icons", name, std::move(icon), from);
        }
        for (const auto& [name, fallback] : known.styles) {
            const StyleBox style = theme.style(name, &from);
            add("styles", name, style_box_json(style), from);
        }
        // Items only a theme gives, when one does.
        for (const auto& [name, fallback] : optional_items(type).styles) {
            const StyleBox style = theme.style(name, &from);
            if (from != "default") {
                add("styles", name, style_box_json(style), from);
            }
        }
    }
    out.reserve(items.size());
    for (auto& [key, item] : items) {
        out.push_back(std::move(item));
    }
    return out;
}

} // namespace slash::ui
