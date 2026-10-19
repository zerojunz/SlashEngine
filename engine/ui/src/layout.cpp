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
#include <limits>
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

// What an OptionButton draws besides a Button's: the arrow at its end, as its NOTIFICATION_DRAW
// draws it.
struct OptionWork {
    std::string arrow; // the arrow icon, if it loads
    Vec2 arrow_size;   // its size in pixels
    Vec4 arrow_color{1.0F, 1.0F, 1.0F, 1.0F};
    float arrow_margin = 0.0F;
    std::optional<Vec2> longest; // with fit_to_longest_item, its _cached_size
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

// A scroll bar's theme items, as ScrollBar's theme cache holds them: HScrollBar's or
// VScrollBar's.
struct BarTheme {
    StyleBox scroll;
    StyleBox scroll_focus;
    StyleBox grabber;
    StyleBox grabber_highlight;
    StyleBox grabber_pressed;
    // Its arrows' icons, normal, highlighted and pressed, and their sizes when they load.
    std::array<std::string, 3> decrement;
    std::array<std::string, 3> increment;
    std::array<std::optional<Vec2>, 3> decrement_size;
    std::array<std::optional<Vec2>, 3> increment_size;
    int icon_max_size = 0;
    // Across it: padding_top and padding_bottom for a horizontal bar, padding_left and
    // padding_right for a vertical one.
    int padding_before = 0;
    int padding_after = 0;
    bool vertical = false;
};

// A ScrollBar being laid out.
struct BarWork {
    BarTheme theme;
    Range range;
    float custom_step = -1.0F;
    BarLook look;
    bool focused = false;
};

enum class ScrollMode : std::uint8_t { Disabled, Auto, Always, Never, Reserve };

ScrollMode scroll_mode_of(std::string_view name) {
    if (name == "disabled") {
        return ScrollMode::Disabled;
    }
    if (name == "always") {
        return ScrollMode::Always;
    }
    if (name == "never") {
        return ScrollMode::Never;
    }
    return name == "reserve" ? ScrollMode::Reserve : ScrollMode::Auto;
}

// A ScrollContainer being laid out: its theme items, the scroll it asks for, and its bars and
// scroll as last sorted.
struct ScrollWork {
    const world::ScrollContainer* scroll = nullptr;
    ScrollMode h_mode = ScrollMode::Auto;
    ScrollMode v_mode = ScrollMode::Auto;
    StyleBox panel;
    StyleBox focus;
    std::array<int, 4> bar_margins{}; // scrollbar_margin_left, _top, _right and _bottom
    int h_separation = 0;             // scrollbar_h_separation
    int v_separation = 0;
    BarTheme h;
    BarTheme v;
    BarLook h_look;
    BarLook v_look;
    std::array<double, 2> wanted{};
    bool focus_drawn = false; // draw_focus_border, and it or a control inside it has focus
    ScrollView view;
};

float along(Vec2 v, bool vertical) {
    return vertical ? v.y : v.x;
}

// A scroll bar icon's size, as ScrollBar::_fit_icon_size fits it to icon_max_size, rounded as
// its callers round it; none for an icon that doesn't load.
Vec2 bar_icon(const std::optional<Vec2>& size, int max_size) {
    const Vec2 icon = size.value_or(Vec2{});
    const auto largest = static_cast<int>(std::max(icon.x, icon.y));
    if (max_size <= 0 || largest <= max_size) {
        return {std::round(icon.x), std::round(icon.y)};
    }
    const double scale = static_cast<double>(max_size) / largest;
    return {static_cast<float>(static_cast<int>(icon.x * scale)),
            static_cast<float>(static_cast<int>(icon.y * scale))};
}

// ScrollBar::get_grabber_min_size.
double grabber_minimum(const BarTheme& theme) {
    return along(theme.grabber.minimum_size(), theme.vertical);
}

// ScrollBar::get_minimum_size: its arrows, its track's margins and the smallest grabber along
// it; the widest of them and its padding across.
Vec2 bar_minimum(const BarTheme& theme) {
    const Vec2 increment = bar_icon(theme.increment_size[0], theme.icon_max_size);
    const Vec2 decrement = bar_icon(theme.decrement_size[0], theme.icon_max_size);
    const Vec2 track = theme.scroll.minimum_size();
    const auto padding =
        static_cast<float>(std::max(theme.padding_before, 0) + std::max(theme.padding_after, 0));
    const auto grabber = static_cast<float>(grabber_minimum(theme));
    if (theme.vertical) {
        return {std::max(increment.x, track.x) + padding,
                increment.y + decrement.y + track.y + grabber};
    }
    return {increment.x + decrement.x + track.x + grabber,
            std::max(increment.y, track.y) + padding};
}

// A scroll bar's measures in a rect of its control's, as ScrollBar's get_area_size,
// get_grabber_size and get_grabber_offset find them.
ScrollBarView measure_bar(const BarTheme& theme, const Range& range, float custom_step,
                          Vec2 position, Vec2 size) {
    const bool vertical = theme.vertical;
    ScrollBarView view;
    view.position = position;
    view.size = size;
    view.vertical = vertical;
    view.visible = true;
    view.min_value = range.min;
    view.max_value = range.max;
    view.page = range.page;
    view.step = range.step;
    view.value = range.value;
    view.ratio = range_ratio(range);
    view.custom_step = custom_step;
    view.decrement = along(bar_icon(theme.decrement_size[0], theme.icon_max_size), vertical);
    view.increment = along(bar_icon(theme.increment_size[0], theme.icon_max_size), vertical);
    view.begin = theme.scroll.margin(vertical ? kTop : kLeft);
    // The area takes the icons' own sizes, not their fitted ones, as Godot's does.
    const double grabber = grabber_minimum(theme);
    view.area = static_cast<double>(along(size, vertical)) -
                along(theme.scroll.minimum_size(), vertical) -
                along(theme.increment_size[0].value_or(Vec2{}), vertical) -
                along(theme.decrement_size[0].value_or(Vec2{}), vertical) - grabber;
    // The page's share in single precision, as Godot's.
    const auto span = static_cast<float>(range.max - range.min);
    if (span > 0.0F) {
        const float page = range.page > 0.0 ? static_cast<float>(range.page) : 0.0F;
        view.grabber_size = (static_cast<double>(page / span) * view.area) + grabber;
    }
    view.grabber_offset = view.area * view.ratio;
    return view;
}

// Which of a bar's three icons it shows: pressed, highlighted or normal.
std::size_t bar_icon_state(bool pressed, bool highlighted) {
    if (pressed) {
        return 2;
    }
    return highlighted ? 1 : 0;
}

// What a scroll bar draws, as its NOTIFICATION_DRAW: its decrement arrow, its track, its
// increment arrow and its grabber, in its control's rect.
void scroll_bar_boxes(const BarTheme& theme, const ScrollBarView& view, const BarLook& look,
                      bool focused, std::vector<Box>& boxes) {
    const bool vertical = theme.vertical;
    const std::size_t d = bar_icon_state(look.decrement_pressed, look.highlight == 1);
    const std::size_t i = bar_icon_state(look.increment_pressed, look.highlight == 3);
    const Vec2 decrement = bar_icon(theme.decrement_size.at(d), theme.icon_max_size);
    const Vec2 increment = bar_icon(theme.increment_size.at(i), theme.icon_max_size);
    const auto icon = [&](const std::string& path, const std::optional<Vec2>& texture, Vec2 at,
                          Vec2 size) {
        if (path.empty() || !texture) {
            return;
        }
        Box box{
            .style = {}, .position = view.position + at, .size = size, .texture_size = *texture};
        box.style.type = StyleBox::Type::Texture;
        box.style.texture = path;
        boxes.push_back(std::move(box));
    };
    icon(theme.decrement.at(d), theme.decrement_size.at(d), {}, decrement);
    Vec2 at = vertical ? Vec2{0.0F, decrement.y} : Vec2{decrement.x, 0.0F};
    Vec2 area = view.size;
    (vertical ? area.y : area.x) -= along(increment, vertical) + along(decrement, vertical);
    boxes.push_back({.style = focused ? theme.scroll_focus : theme.scroll,
                     .position = view.position + at,
                     .size = area,
                     .texture_size = {}});
    (vertical ? at.y : at.x) += along(area, vertical);
    icon(theme.increment.at(i), theme.increment_size.at(i), at, increment);
    const auto before = static_cast<float>(std::max(theme.padding_before, 0));
    const auto after = static_cast<float>(std::max(theme.padding_after, 0));
    const auto offset = static_cast<float>(view.grabber_offset);
    const auto length = static_cast<float>(view.grabber_size);
    Box grabber{.style = theme.grabber, .position = {}, .size = {}, .texture_size = {}};
    if (look.dragging) {
        grabber.style = theme.grabber_pressed;
    } else if (look.highlight == 2) {
        grabber.style = theme.grabber_highlight;
    }
    if (vertical) {
        grabber.size = {view.size.x - before - after, length};
        grabber.position = {before, offset + decrement.y + theme.scroll.margin(kTop)};
    } else {
        grabber.size = {length, view.size.y - before - after};
        grabber.position = {offset + decrement.x + theme.scroll.margin(kLeft), before};
    }
    grabber.position = grabber.position + view.position;
    boxes.push_back(std::move(grabber));
}

enum class Kind : std::uint8_t { None, Box, Grid, Margin, Center, Flow, Panel, Scroll };

// Which way a control's minimum size depends on its own size.
enum class Depends : std::uint8_t { None, Width, Height };

struct Node {
    entt::entity entity = entt::null;
    const world::Control* control = &kDefaultControl;
    std::size_t parent = kNone;
    std::size_t canvas = 0; // as Placed::canvas
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
    // An OptionButton: its Button with the selected option's text and icon, and its arrow.
    std::unique_ptr<world::Button> shown_button;
    std::optional<OptionWork> option;
    const world::TextureButton* texture_button = nullptr;
    // TextureButton's textures' sizes, those that load: normal, pressed, hover, disabled and
    // focused.
    std::array<std::optional<Vec2>, 5> button_textures;
    std::optional<ButtonState> button_state;
    std::optional<SliderWork> slider;
    std::optional<BarWork> scroll_bar; // a ScrollBar's
    std::optional<ScrollWork> scroll;  // a ScrollContainer's
    // A LineEdit: a copy with its caret column in its text, its theme items, its shaped text and
    // the gui system's state for it.
    std::optional<world::LineEdit> line_edit;
    std::unique_ptr<LineEditView> edit_view;
    std::optional<Field> field;
    EditState edit;
    // A TextEdit, its theme items, its scroll bars' and how they look, its CodeHighlighter and
    // the gui system's state for it.
    const world::TextEdit* text_edit = nullptr;
    std::unique_ptr<TextEditView> text_view;
    std::unique_ptr<std::array<BarTheme, 2>> text_bars; // horizontal, vertical
    std::array<BarLook, 2> text_bar_looks{};
    const world::CodeHighlighter* highlighter = nullptr;
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
    const ThemeLink* project_link = nullptr; // the project's theme, if it has one that loads
    // A drag's preview, a root control laid out last with its top left at the pointer.
    entt::entity preview = entt::null;
    Vec2 preview_at;
    // The canvases controls draw in, or null for all in the world.
    const world::Canvases* canvases = nullptr;

    // The canvas a root control draws in.
    [[nodiscard]] std::size_t canvas_of_root(entt::entity entity) const {
        if (canvases == nullptr) {
            return 0;
        }
        return entity == preview ? canvases->all().size() : canvases->of(entity);
    }

    // Whether a canvas shows: a hidden CanvasLayer hides its controls.
    [[nodiscard]] bool canvas_visible(std::size_t canvas) const {
        return canvases == nullptr || canvas >= canvases->all().size() ||
               canvases->all()[canvas].visible;
    }

    // What places a root control in its canvas: a CanvasLayer's transform, or none for the
    // world's controls, which stay on the screen, and for a drag's preview.
    [[nodiscard]] std::optional<Transform> canvas_transform(std::size_t canvas) const {
        if (canvases == nullptr || canvas >= canvases->all().size()) {
            return std::nullopt;
        }
        const world::Canvas& in = canvases->all()[canvas];
        const world::Affine2D& t = in.transform;
        if (in.layer == entt::null || t == world::Affine2D{}) {
            return std::nullopt;
        }
        return Transform{
            .a = t.x.x, .b = t.x.y, .c = t.y.x, .d = t.y.y, .tx = t.origin.x, .ty = t.origin.y};
    }

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
        } else if (const auto* scroll = ecs.try_get<world::ScrollContainer>(entity);
                   scroll != nullptr) {
            node.kind = Kind::Scroll;
            start_scroll(node, *scroll, resolver("ScrollContainer"));
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
            if (const auto* option = ecs.try_get<world::OptionButton>(entity); option != nullptr) {
                start_option(node, *option, resolver(type));
            } else {
                start_button_node(node, *button, resolver(type), type);
            }
        }
        if (const auto* slider = ecs.try_get<world::Slider>(entity); slider != nullptr) {
            start_slider(node, *slider, resolver(slider->vertical ? "VSlider" : "HSlider"));
        }
        if (const auto* bar = ecs.try_get<world::ScrollBar>(entity); bar != nullptr) {
            BarWork& work = node.scroll_bar.emplace();
            work.theme =
                bar_theme(resolver(bar->vertical ? "VScrollBar" : "HScrollBar"), bar->vertical);
            work.range = range_of(*bar);
            work.custom_step = bar->custom_step;
            work.look = bar_look(world, entity, Bar::Own);
            work.focused = shows_focus(world, entity);
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

    ButtonWork& start_button_node(Node& node, const world::Button& button, const Resolver& theme,
                                  std::string_view type) {
        const DrawMode mode = draw_mode(world, node.entity);
        node.button = start_button(button, theme, mode, shows_focus(world, node.entity), node.rtl);
        node.button_state = ButtonState{
            .mode = mode, .click_margin = static_cast<float>(theme.constant("click_margin"))};
        ButtonWork& work = *node.button;
        if (!button.icon.empty()) {
            std::string error;
            work.icon = texture_size(button.icon, &error);
            // An OptionButton's icons are its options', which start_option checks.
            if (!work.icon && type != "OptionButton") {
                problem(node.entity, "/components/" + std::string(type) + "/icon", error);
            }
        }
        if (type == "CheckBox" || type == "CheckButton") {
            start_check(node, work, button, theme, type == "CheckButton");
        }
        if (const auto* menu = world.ecs().try_get<world::MenuButton>(node.entity);
            menu != nullptr) {
            check_item_icons(node.entity, "MenuButton", menu->items);
        }
        if (work.text.autowrap == text::Autowrap::Off) {
            shape(fonts, work.text, std::nullopt);
        } else {
            node.depends = Depends::Width;
        }
        return work;
    }

    // An OptionButton: a Button showing the selected option's text and icon, with room kept
    // for its arrow (NOTIFICATION_THEME_CHANGED) and, with fit_to_longest_item, the size of its
    // longest option (_refresh_size_cache).
    void start_option(Node& node, const world::OptionButton& option, const Resolver& theme) {
        node.shown_button =
            std::make_unique<world::Button>(static_cast<const world::Button&>(option));
        world::Button& shown = *node.shown_button;
        const bool selected =
            option.selected >= 0 && std::cmp_less(option.selected, option.items.size());
        shown.text = selected ? option.items[static_cast<std::size_t>(option.selected)].text : "";
        shown.icon = selected ? option.items[static_cast<std::size_t>(option.selected)].icon : "";
        ButtonWork& work = start_button_node(node, shown, theme, "OptionButton");
        OptionWork& made = node.option.emplace();
        const std::string arrow = theme.icon("arrow");
        if (const auto size = texture_size(arrow); size) {
            made.arrow = arrow;
            made.arrow_size = *size;
            work.internal.at(node.rtl ? 0 : 1) = size->x;
        }
        made.arrow_margin = static_cast<float>(theme.constant("arrow_margin"));
        if (theme.constant("modulate_arrow") != 0) {
            std::string_view color = "font_color";
            switch (draw_mode(world, node.entity)) {
            case DrawMode::Pressed:
                color = "font_pressed_color";
                break;
            case DrawMode::Hover:
                color = "font_hover_color";
                break;
            case DrawMode::HoverPressed:
                color = "font_hover_pressed_color";
                break;
            case DrawMode::Disabled:
                color = "font_disabled_color";
                break;
            case DrawMode::Normal:
                if (has_focus(world, node.entity)) {
                    color = "font_focus_color";
                }
                break;
            }
            made.arrow_color = theme.color(std::string(color));
        }
        check_item_icons(node.entity, "OptionButton", option.items);
        if (!option.fit_to_longest_item) {
            return;
        }
        // Each option measured as Button::get_minimum_size_for_text_and_icon measures it: an
        // option with no text takes the text shown.
        Vec2 longest = theme.style("normal").minimum_size();
        for (const world::OptionButtonItem& item : option.items) {
            world::Button each_button = shown;
            if (!item.text.empty()) {
                each_button.text = item.text;
            }
            each_button.icon = item.icon;
            ButtonWork each = work;
            each.button = &each_button;
            each.icon = item.icon.empty() ? std::nullopt : texture_size(item.icon);
            each.text.out.source = each_button.text;
            each.text.shaped = false;
            shape(fonts, each.text, std::nullopt);
            longest = max(longest, button_minimum(each));
        }
        made.longest = longest;
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

    // A scroll bar's theme items, as ScrollBar's theme cache reads them.
    BarTheme bar_theme(const Resolver& theme, bool vertical) {
        BarTheme made;
        made.vertical = vertical;
        made.scroll = theme.style("scroll");
        made.scroll_focus = theme.style("scroll_focus");
        made.grabber = theme.style("grabber");
        made.grabber_highlight = theme.style("grabber_highlight");
        made.grabber_pressed = theme.style("grabber_pressed");
        static constexpr std::array<std::string_view, 3> kStates{"", "_highlight", "_pressed"};
        for (std::size_t i = 0; i < kStates.size(); ++i) {
            made.decrement.at(i) = theme.icon("decrement" + std::string(kStates.at(i)));
            made.increment.at(i) = theme.icon("increment" + std::string(kStates.at(i)));
            if (!made.decrement.at(i).empty()) {
                made.decrement_size.at(i) = texture_size(made.decrement.at(i));
            }
            if (!made.increment.at(i).empty()) {
                made.increment_size.at(i) = texture_size(made.increment.at(i));
            }
        }
        made.icon_max_size = theme.constant("icon_max_size");
        made.padding_before = theme.constant(vertical ? "padding_left" : "padding_top");
        made.padding_after = theme.constant(vertical ? "padding_right" : "padding_bottom");
        return made;
    }

    // A ScrollContainer's theme items and the scroll it asks for. Its bars take HScrollBar's and
    // VScrollBar's items from the themes it uses, not its overrides, as Godot's bars are
    // controls of their own.
    void start_scroll(Node& node, const world::ScrollContainer& scroll, const Resolver& theme) {
        ScrollWork& work = node.scroll.emplace();
        work.scroll = &scroll;
        work.h_mode = scroll_mode_of(scroll.horizontal_scroll_mode);
        work.v_mode = scroll_mode_of(scroll.vertical_scroll_mode);
        work.panel = theme.style("panel");
        work.focus = theme.style("focus");
        work.bar_margins = {
            theme.constant("scrollbar_margin_left"), theme.constant("scrollbar_margin_top"),
            theme.constant("scrollbar_margin_right"), theme.constant("scrollbar_margin_bottom")};
        work.h_separation = theme.constant("scrollbar_h_separation");
        work.v_separation = theme.constant("scrollbar_v_separation");
        static const world::Control kNoOverrides;
        work.h = bar_theme(Resolver(node.themes, kNoOverrides, "HScrollBar"), false);
        work.v = bar_theme(Resolver(node.themes, kNoOverrides, "VScrollBar"), true);
        work.h_look = bar_look(world, node.entity, Bar::Horizontal);
        work.v_look = bar_look(world, node.entity, Bar::Vertical);
        work.wanted = wanted_scroll(world, node.entity);
        if (scroll.draw_focus_border) {
            // ScrollContainer::_gui_focus_changed: it or a control inside it shows focus.
            const entt::entity owner = focus_owner(world);
            bool inside = false;
            for (entt::entity at = owner; at != entt::null && !inside;) {
                inside = at == node.entity;
                const auto found = parents.find(at);
                at = found != parents.end() ? found->second : entt::null;
            }
            work.focus_drawn = inside && shows_focus(world, owner);
        }
        node.panel = work.panel;
        // With one axis disabled, the scroll bar of the other may take room in its minimum size,
        // when its children don't fit its size that way.
        if (work.h_mode == ScrollMode::Disabled && work.v_mode == ScrollMode::Auto) {
            node.depends = Depends::Height;
        } else if (work.v_mode == ScrollMode::Disabled && work.h_mode == ScrollMode::Auto) {
            node.depends = Depends::Width;
        }
    }

    // ScrollContainer::_get_margins: its panel's margins, or its focus style's where they are
    // bigger when it draws the focus border.
    static std::array<float, 4> scroll_margins(const ScrollWork& work) {
        std::array<float, 4> margins{};
        for (std::size_t side = 0; side < margins.size(); ++side) {
            margins.at(side) = work.panel.margin(side);
            if (work.scroll->draw_focus_border) {
                margins.at(side) = std::max(margins.at(side), work.focus.margin(side));
            }
        }
        return margins;
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
        view->word_highlighted_color = theme.color("word_highlighted_color");
        view->search_result_color = theme.color("search_result_color");
        view->search_result_border_color = theme.color("search_result_border_color");
        view->line_spacing = theme.constant("line_spacing");
        view->outline_size = theme.constant("outline_size");
        view->caret_width = theme.constant("caret_width");
        view->wrap_offset = theme.constant("wrap_offset");
        static const world::Control kNoOverrides;
        auto bars = std::make_unique<std::array<BarTheme, 2>>();
        bars->at(0) = bar_theme(Resolver(node.themes, kNoOverrides, "HScrollBar"), false);
        bars->at(1) = bar_theme(Resolver(node.themes, kNoOverrides, "VScrollBar"), true);
        view->h_bar_minimum = bar_minimum(bars->at(0));
        view->v_bar_minimum = bar_minimum(bars->at(1));
        view->rtl = node.rtl;
        node.text_edit = &edit;
        node.text_view = std::move(view);
        node.text_bars = std::move(bars);
        node.text_bar_looks = {bar_look(world, node.entity, Bar::Horizontal),
                               bar_look(world, node.entity, Bar::Vertical)};
        node.highlighter = world.ecs().try_get<world::CodeHighlighter>(node.entity);
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
        case Kind::Scroll:
            return scroll_minimum(*node.scroll, biggest, size.value_or(Vec2{}));
        }
        return {};
    }

    // ScrollContainer::_get_minimum_size: nothing on an axis it scrolls; on a disabled one its
    // largest child, and the other axis's bar when it shows; and its margins.
    static Vec2 scroll_minimum(const ScrollWork& work, Vec2 largest, Vec2 size) {
        const bool v_show = work.v_mode == ScrollMode::Always ||
                            work.v_mode == ScrollMode::Reserve ||
                            (work.v_mode == ScrollMode::Auto && largest.y > size.y);
        const bool h_show = work.h_mode == ScrollMode::Always ||
                            work.h_mode == ScrollMode::Reserve ||
                            (work.h_mode == ScrollMode::Auto && largest.x > size.x);
        Vec2 minimum;
        if (work.h_mode == ScrollMode::Disabled) {
            minimum.x = largest.x;
            if (v_show) {
                minimum.x += bar_minimum(work.v).x + static_cast<float>(work.h_separation);
            }
        }
        if (work.v_mode == ScrollMode::Disabled) {
            minimum.y = largest.y;
            if (h_show) {
                minimum.y += bar_minimum(work.h).y + static_cast<float>(work.v_separation);
            }
        }
        const std::array<float, 4> margins = scroll_margins(work);
        return {minimum.x + margins[kLeft] + margins[kRight],
                minimum.y + margins[kTop] + margins[kBottom]};
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
            Vec2 own = button_minimum(work);
            // OptionButton::get_minimum_size: its longest option's size, then room for the
            // arrow beside the content.
            if (node.option) {
                const OptionWork& option = *node.option;
                own = option.longest.value_or(own);
                if (!option.arrow.empty()) {
                    const Vec2 padding = work.largest;
                    Vec2 content{own.x - padding.x, own.y - padding.y};
                    content.x +=
                        option.arrow_size.x + static_cast<float>(std::max(0, work.h_separation));
                    content.y = std::max(content.y, option.arrow_size.y);
                    own = {content.x + padding.x, content.y + padding.y};
                }
            }
            minimum = max(minimum, own);
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
        if (node.scroll_bar) {
            minimum = max(minimum, bar_minimum(node.scroll_bar->theme));
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
    void sort(Node& node, Vec2 size) {
        switch (node.kind) {
        case Kind::None:
            return;
        case Kind::Scroll:
            if (node.scroll) {
                sort_scroll(node, *node.scroll, size);
            }
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

    // ScrollContainer::_reposition_children, with _update_scrollbars and
    // _update_scrollbar_position: which bars show and where, how far it scrolls, and its
    // children at their minimum sizes, or the visible size on an axis they expand on, moved back
    // by the scroll.
    void sort_scroll(Node& node, ScrollWork& work, Vec2 size) {
        const world::ScrollContainer& scroll = *work.scroll;
        const bool rtl = node.rtl;
        const std::array<float, 4> margins = scroll_margins(work);
        const Vec2 inner{size.x - margins[kLeft] - margins[kRight],
                         size.y - margins[kTop] - margins[kBottom]};
        const std::vector<std::size_t> kids = sorted(node);
        Vec2 largest;
        for (const std::size_t kid : kids) {
            largest = max(largest, nodes[kid].minimum);
        }
        const Vec2 hmin = bar_minimum(work.h);
        const Vec2 vmin = bar_minimum(work.v);
        const auto shows = [](ScrollMode mode, float content, float room) {
            return mode == ScrollMode::Always ||
                   ((mode == ScrollMode::Auto || mode == ScrollMode::Reserve) && content > room);
        };
        const bool h_visible = shows(work.h_mode, largest.x, inner.x);
        const bool v_visible = shows(work.v_mode, largest.y, inner.y);
        // Each bar's range is its largest child's size, of which the room in view is a page.
        const auto range = [](float content, float page, double wanted) {
            Range made{.min = 0.0,
                       .max = std::max(static_cast<double>(content), 0.0),
                       .step = 0.0,
                       .page = 0.0,
                       .value = 0.0,
                       .rounded = false,
                       .allow_greater = false,
                       .allow_lesser = false};
            made.page = std::clamp(static_cast<double>(page), 0.0, made.max - made.min);
            made.value = range_value(made, wanted);
            return made;
        };
        const Range h_range =
            range(largest.x, v_visible ? inner.x - vmin.x : inner.x, work.wanted[0]);
        const Range v_range =
            range(largest.y, h_visible ? inner.y - hmin.y : inner.y, work.wanted[1]);

        // The bars go along its far edges, inside its margins or scrollbar_margin_*, each grown
        // to its minimum size and mirrored right to left as Control::_size_changed places it.
        const auto edge = [&](std::size_t side, float fallback) {
            return static_cast<float>(work.bar_margins.at(side) < 0 ? static_cast<int>(fallback)
                                                                    : work.bar_margins.at(side));
        };
        const float left = edge(kLeft, rtl ? margins[kRight] : margins[kLeft]);
        const float right = edge(kRight, rtl ? margins[kLeft] : margins[kRight]);
        const float top = edge(kTop, margins[kTop]);
        const float bottom = edge(kBottom, work.panel.margin(kBottom));
        const Vec2 h_size = h_visible ? hmin : Vec2{};
        const Vec2 v_size = v_visible ? vmin : Vec2{};
        const auto bar_rect = [&](Vec2 from, Vec2 to, Vec2 minimum) {
            Rect r{from, {std::max(to.x - from.x, minimum.x), std::max(to.y - from.y, minimum.y)}};
            if (rtl) {
                r.position.x = size.x - r.position.x - r.size.x;
            }
            return r;
        };
        const Rect h_rect = bar_rect({left, size.y - h_size.y - bottom},
                                     {size.x - right - v_size.x, size.y - bottom}, hmin);
        const Rect v_rect = bar_rect({size.x - v_size.x - right, top},
                                     {size.x - right, size.y - h_size.y - bottom}, vmin);
        work.view = {};
        work.view.h = measure_bar(work.h, h_range, scroll.scroll_horizontal_custom_step,
                                  h_rect.position, h_rect.size);
        work.view.h.visible = h_visible;
        work.view.v = measure_bar(work.v, v_range, scroll.scroll_vertical_custom_step,
                                  v_rect.position, v_rect.size);
        work.view.v.visible = v_visible;

        // The room its children have: its rect less its margins and the bars it shows or keeps
        // room for.
        Vec2 room = inner;
        Vec2 offset{margins[kLeft], margins[kTop]};
        if (h_visible || work.h_mode == ScrollMode::Reserve) {
            auto height = static_cast<int>(hmin.y + static_cast<float>(work.v_separation));
            if (work.bar_margins[kBottom] >= 0) {
                const int scroll_margin = work.bar_margins[kBottom] + height;
                if (static_cast<float>(scroll_margin) > margins[kBottom]) {
                    height = static_cast<int>(static_cast<float>(scroll_margin) - margins[kBottom]);
                }
            }
            room.y -= static_cast<float>(height);
        }
        if (v_visible || work.v_mode == ScrollMode::Reserve) {
            auto width = static_cast<int>(vmin.x + static_cast<float>(work.h_separation));
            if (work.bar_margins[kRight] >= 0) {
                const int scroll_margin = work.bar_margins[kRight] + width;
                if (static_cast<float>(scroll_margin) > margins[kRight]) {
                    width = static_cast<int>(static_cast<float>(scroll_margin) - margins[kRight]);
                }
            }
            room.x -= static_cast<float>(width);
            if (rtl) {
                offset.x += static_cast<float>(width);
            }
        }
        // get_h_scroll and get_v_scroll: whole pixels.
        const Vec2 scrolled{static_cast<float>(static_cast<int>(h_range.value)),
                            static_cast<float>(static_cast<int>(v_range.value))};
        for (const std::size_t kid : kids) {
            const Node& child = nodes[kid];
            const Vec2 m = child.minimum;
            Rect r{{-scrolled.x, -scrolled.y}, m};
            if (flags_of(child.control->size_flags_horizontal).expand) {
                r.size.x = std::max(room.x, m.x);
            }
            if (flags_of(child.control->size_flags_vertical).expand) {
                r.size.y = std::max(room.y, m.y);
            }
            r.position = {std::floor(r.position.x + offset.x), std::floor(r.position.y + offset.y)};
            fit(kid, r, rtl);
        }

        // _get_local_visible_rect: its rect less the bars that show.
        const float side = v_visible ? v_rect.size.x : 0.0F;
        const float below = h_visible ? h_rect.size.y : 0.0F;
        work.view.visible_position = {rtl ? side : 0.0F, 0.0F};
        work.view.visible_size = {size.x - side, size.y - below};
        work.view.scroll = scrolled;
        work.view.scroll_max = {
            static_cast<float>(std::max(h_range.max - h_range.page - h_range.min, 0.0)),
            static_cast<float>(std::max(v_range.max - v_range.page - v_range.min, 0.0))};
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
        const bool previewing = parent == nullptr && node.entity == preview;
        if (previewing) {
            // set_drag_preview's set_position, at the pointer.
            left = preview_at.x;
            top = preview_at.y;
        }
        Placed& placed = node.placed;
        placed.entity = node.entity;
        placed.parent = parent != nullptr ? parent->entity : entt::null;
        placed.canvas = node.canvas;
        placed.position = {left, top};
        placed.size = {width, height};
        placed.minimum_size = minimum;
        const Transform local = local_transform(control, placed.position);
        if (parent != nullptr) {
            placed.transform = parent->placed.transform * local;
        } else if (const std::optional<Transform> canvas = canvas_transform(node.canvas)) {
            // A root of a CanvasLayer's is in the layer's space, as Godot's
            // get_global_transform_with_canvas.
            placed.transform = *canvas * local;
        } else {
            placed.transform = local;
        }
        placed.visible = (control.visible || previewing) &&
                         (parent != nullptr ? parent->placed.visible : canvas_visible(node.canvas));
        placed.modulate =
            parent != nullptr ? times(parent->placed.modulate, control.modulate) : control.modulate;
        placed.z = std::clamp(
            control.z_index + (parent != nullptr && control.z_as_relative ? parent->placed.z : 0),
            -kMaxZ, kMaxZ);
        placed.texture_filter = control.texture_filter != "inherit" || parent == nullptr
                                    ? control.texture_filter
                                    : parent->placed.texture_filter;
        // The clip it draws in, as RendererCanvasCull finds each item's final_clip_rect: its
        // parent's, cut to its own rect on screen when it clips, rounded to whole pixels.
        placed.clips = control.clip_contents || node.kind == Kind::Scroll;
        placed.clip = parent != nullptr ? parent->placed.clip : std::nullopt;
        if (placed.clips) {
            float x0 = std::numeric_limits<float>::max();
            float y0 = x0;
            float x1 = std::numeric_limits<float>::lowest();
            float y1 = x1;
            for (const Vec2 corner :
                 {Vec2{}, Vec2{width, 0.0F}, Vec2{0.0F, height}, Vec2{width, height}}) {
                const Vec2 at = placed.transform.apply(corner);
                x0 = std::min(x0, at.x);
                y0 = std::min(y0, at.y);
                x1 = std::max(x1, at.x);
                y1 = std::max(y1, at.y);
            }
            if (placed.clip) {
                x0 = std::max(x0, placed.clip->x);
                y0 = std::max(y0, placed.clip->y);
                x1 = std::min(x1, placed.clip->z);
                y1 = std::min(y1, placed.clip->w);
            }
            const float w = std::max(x1 - x0, 0.0F);
            const float h = std::max(y1 - y0, 0.0F);
            const Vec2 at{std::round(x0), std::round(y0)};
            placed.clip = Vec4{at.x, at.y, at.x + std::round(w), at.y + std::round(h)};
        }
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
        if (node.option && !node.option->arrow.empty()) {
            const OptionWork& option = *node.option;
            const Vec2 arrow = option.arrow_size;
            const float y = std::trunc(std::abs((size.y - arrow.y) / 2.0F));
            const float x = node.rtl ? option.arrow_margin : size.x - arrow.x - option.arrow_margin;
            Box box{.style = {}, .position = {x, y}, .size = arrow, .texture_size = arrow};
            box.style.type = StyleBox::Type::Texture;
            box.style.texture = option.arrow;
            box.style.modulate_color = option.arrow_color;
            boxes.push_back(std::move(box));
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
        if (node.scroll_bar) {
            const BarWork& work = *node.scroll_bar;
            ScrollBarView view =
                measure_bar(work.theme, work.range, work.custom_step, Vec2{}, size);
            scroll_bar_boxes(work.theme, view, work.look, work.focused, boxes);
            out.scroll_bars.emplace(node.entity, view);
        }
        if (node.scroll) {
            const ScrollWork& work = *node.scroll;
            std::vector<Box> over;
            if (work.view.h.visible) {
                scroll_bar_boxes(work.h, work.view.h, work.h_look, false, over);
            }
            if (work.view.v.visible) {
                scroll_bar_boxes(work.v, work.view.v, work.v_look, false, over);
            }
            if (work.focus_drawn) {
                over.push_back(
                    {.style = work.focus, .position = {}, .size = size, .texture_size = {}});
            }
            std::erase_if(over, [&](Box& box) { return !keep(box); });
            if (!over.empty()) {
                out.over_children.emplace(node.entity, std::move(over));
            }
            out.scrolls.emplace(node.entity, work.view);
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
        area.draw(edit, shown, has_focus(world, node.entity), node.highlighter, draw, view);
        view.error = area.error();
        // Its scroll bars over the rest, as children of its own.
        for (const bool vertical : {true, false}) {
            const TextArea::BarPlace place = area.bar_place(vertical);
            if (!place.visible) {
                continue;
            }
            const BarTheme& bar = node.text_bars->at(vertical ? 1 : 0);
            const Range range{.min = 0.0,
                              .max = place.max,
                              .step = 0.0,
                              .page = place.page,
                              .value = place.value,
                              .rounded = false,
                              .allow_greater = false,
                              .allow_lesser = false};
            ScrollBarView& measured = vertical ? view.v_bar : view.h_bar;
            measured = measure_bar(bar, range, -1.0F, place.position, place.size);
            scroll_bar_boxes(bar, measured, node.text_bar_looks.at(vertical ? 1 : 0), false,
                             draw.over);
        }
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
            Box drawn{.style = {},
                      .position = at,
                      .size = {std::abs(size.x), std::abs(size.y)},
                      .texture_size = texture};
            drawn.style.type = StyleBox::Type::Texture;
            drawn.style.texture = path;
            drawn.style.flip_h = size.x < 0.0F;
            drawn.style.flip_v = size.y < 0.0F;
            boxes.push_back(std::move(drawn));
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

    // ---- Popup menus ----

    // Reports the items' icons that don't load.
    template <typename Item>
    void check_item_icons(entt::entity entity, std::string_view type,
                          const std::vector<Item>& items) {
        for (std::size_t i = 0; i < items.size(); ++i) {
            std::string error;
            if (!items[i].icon.empty() && !texture_size(items[i].icon, &error)) {
                problem(entity,
                        "/components/" + std::string(type) + "/items/" + std::to_string(i) +
                            "/icon",
                        error);
            }
        }
    }

    // PopupMenus' themes and icons that don't load, open or not.
    void check_popup_menus() {
        std::vector<std::pair<entt::entity, const world::PopupMenu*>> menus;
        for (const auto [entity, menu] : world.ecs().view<world::PopupMenu>().each()) {
            menus.emplace_back(entity, &menu);
        }
        std::ranges::sort(menus, [](const auto& a, const auto& b) {
            return entt::to_entity(a.first) < entt::to_entity(b.first);
        });
        for (const auto& [entity, menu] : menus) {
            std::string error;
            if (!menu->theme.empty() && theme(menu->theme, &error) == nullptr) {
                problem(entity, "/components/PopupMenu/theme", error);
            }
            check_item_icons(entity, "PopupMenu", menu->items);
        }
    }

    // A popup menu's theme items, as PopupMenu's theme cache holds them.
    struct PopupTheme {
        StyleBox panel;
        StyleBox hover;
        StyleBox separator;
        StyleBox labeled_left;
        StyleBox labeled_right;
        // checked, radio_checked, checked_disabled and radio_checked_disabled, as
        // PopupMenu::_draw_items orders them; then the unchecked ones. Sizes are 0 for icons that
        // don't load.
        std::array<std::string, 4> check;
        std::array<std::string, 4> uncheck;
        std::array<Vec2, 4> check_size{};
        std::array<Vec2, 4> uncheck_size{};
        std::string font;
        std::string font_separator;
        int font_size = text::kDefaultFontSize;
        int font_separator_size = text::kDefaultFontSize;
        Vec4 font_color;
        Vec4 font_hover_color;
        Vec4 font_disabled_color;
        Vec4 font_separator_color;
        Vec4 font_outline_color;
        Vec4 font_separator_outline_color;
        int indent = 0;
        int h_separation = 0;
        int v_separation = 0;
        int item_start_padding = 0;
        int item_end_padding = 0;
        int icon_max_width = 0;
        int outline_size = 0;
        int separator_outline_size = 0;
    };

    // One item of a popup menu being laid out: its text shaped as PopupMenu::_shape_item shapes
    // it, its icon's size and its height.
    struct PopupItemWork {
        LabelWork text;
        Vec2 text_size;           // TextLine::get_size; none for no text
        std::optional<Vec2> icon; // its icon's size, fitted to icon_max_width, if it loads
        Vec2 icon_texture;        // and its texture's
        float height = 0.0F;
    };

    PopupTheme popup_theme(const Resolver& theme) {
        static constexpr std::array<std::string_view, 4> kChecked{
            "checked", "radio_checked", "checked_disabled", "radio_checked_disabled"};
        static constexpr std::array<std::string_view, 4> kUnchecked{
            "unchecked", "radio_unchecked", "unchecked_disabled", "radio_unchecked_disabled"};
        PopupTheme look;
        look.panel = theme.style("panel");
        look.hover = theme.style("hover");
        look.separator = theme.style("separator");
        look.labeled_left = theme.style("labeled_separator_left");
        look.labeled_right = theme.style("labeled_separator_right");
        for (std::size_t i = 0; i < kChecked.size(); ++i) {
            look.check.at(i) = theme.icon(std::string(kChecked.at(i)));
            look.check_size.at(i) = texture_size(look.check.at(i)).value_or(Vec2{});
            look.uncheck.at(i) = theme.icon(std::string(kUnchecked.at(i)));
            look.uncheck_size.at(i) = texture_size(look.uncheck.at(i)).value_or(Vec2{});
        }
        look.font = theme.font("font");
        look.font_separator = theme.font("font_separator");
        look.font_size = std::clamp(theme.font_size("font_size"), 1, text::kMaxFontSize);
        look.font_separator_size =
            std::clamp(theme.font_size("font_separator_size"), 1, text::kMaxFontSize);
        look.font_color = theme.color("font_color");
        look.font_hover_color = theme.color("font_hover_color");
        look.font_disabled_color = theme.color("font_disabled_color");
        look.font_separator_color = theme.color("font_separator_color");
        look.font_outline_color = theme.color("font_outline_color");
        look.font_separator_outline_color = theme.color("font_separator_outline_color");
        look.indent = theme.constant("indent");
        look.h_separation = theme.constant("h_separation");
        look.v_separation = theme.constant("v_separation");
        look.item_start_padding = theme.constant("item_start_padding");
        look.item_end_padding = theme.constant("item_end_padding");
        look.icon_max_width = theme.constant("icon_max_width");
        look.outline_size = theme.constant("outline_size");
        look.separator_outline_size = theme.constant("separator_outline_size");
        return look;
    }

    // PopupMenu::_get_item_height, in whole pixels as its ints keep it: the tallest of its text
    // (a line of the font when it has none), its icon or check box, and a separator's lines.
    static float popup_item_height(const PopupTheme& theme, const world::PopupMenuItem& item,
                                   const PopupItemWork& work) {
        float icon = std::trunc(work.icon.value_or(Vec2{}).y);
        if (item.checkable != "none" && !item.separator) {
            icon = std::max(icon, std::max(theme.check_size[0].y, theme.check_size[1].y));
        }
        float text = std::trunc(work.text_size.y);
        if (text == 0.0F && !item.separator) {
            text = std::trunc(work.text.line_height);
        }
        float separator = 0.0F;
        if (item.separator) {
            separator = std::trunc(std::max(theme.separator.minimum_size().y,
                                            std::max(theme.labeled_left.minimum_size().y,
                                                     theme.labeled_right.minimum_size().y)));
        }
        return std::max(separator, std::max(text, icon));
    }

    // An open popup menu, as PopupMenu lays out and draws its window: its contents' minimum
    // size (_get_contents_minimum_size); its rect, as OptionButton's and MenuButton's show_popup
    // and Popup::_popup_adjust_rect place it, in whole pixels as a window's; its list scrolled
    // inside the panel (scroll_to_item); and its items (_draw_items).
    // The themes a control's own items come from: its node's, or a PopupMenu's own theme over
    // the project's.
    const ThemeLink* themes_of(entt::entity entity) {
        if (const auto node = std::ranges::find(nodes, entity, &Node::entity);
            node != nodes.end()) {
            return node->themes;
        }
        if (const auto* menu = world.ecs().try_get<world::PopupMenu>(entity);
            menu != nullptr && !menu->theme.empty()) {
            if (const Theme* loaded = theme(menu->theme, nullptr); loaded != nullptr) {
                return &links.emplace_back(
                    ThemeLink{.theme = loaded, .path = menu->theme, .parent = project_link});
            }
        }
        return project_link;
    }

    // Viewport::_gui_show_tooltip_at: the tooltip panel at its contents' minimum size, the
    // position offset below and right of where the pointer came to rest; where it would leave
    // the viewport it goes the other way, or hugs the edge.
    void tooltip_view(const Tooltip& shown) {
        if (!world.valid(shown.owner) || shown.text.empty()) {
            return;
        }
        const ThemeLink* link = themes_of(shown.owner);
        const Resolver label_theme(link, kDefaultControl, "TooltipLabel");
        const StyleBox panel = Resolver(link, kDefaultControl, "TooltipPanel").style("panel");
        world::Label label;
        label.text = shown.text;
        LabelWork work = start_label(label, label_theme, false);
        shape(fonts, work, std::nullopt);
        // PopupPanel::_get_contents_minimum_size, ceiled.
        const Vec2 contents = label_minimum(work) + panel.minimum_size();
        const Vec2 size{std::ceil(contents.x), std::ceil(contents.y)};
        const Vec2 offset = settings(world).tooltip_position_offset;
        const Vec2 rest = shown.position;
        const Vec2 room{std::trunc(out.viewport.x), std::trunc(out.viewport.y)};
        Vec2 position = rest + offset;
        const auto fit = [](float& at, float rest_at, float length, float offset_at, float end) {
            if (at + length > end) {
                at = rest_at - length - offset_at;
                if (at < 0.0F) {
                    at = end - length;
                }
            } else if (at < 0.0F) {
                at = 0.0F;
            }
        };
        fit(position.x, rest.x, size.x, offset.x, room.x);
        fit(position.y, rest.y, size.y, offset.y, room.y);
        TooltipView& view = out.tooltip.emplace();
        view.owner = shown.owner;
        view.text = shown.text;
        view.position = {std::trunc(position.x), std::trunc(position.y)};
        view.size = size;
        view.panel.push_back({.style = panel, .position = {}, .size = size, .texture_size = {}});
        // The label fills the panel inside its margins.
        const Vec2 margin{panel.margin(kLeft), panel.margin(kTop)};
        finish_label(fonts, work, size - margin - Vec2{panel.margin(kRight), panel.margin(kBottom)},
                     false);
        for (Vec2& pen : work.out.pens) {
            pen = pen + margin;
        }
        view.label = std::move(work.out);
    }

    // The drag going on: its preview control was placed at the pointer; a text field's text is
    // a Label in the field's font, colors and outline with no margins, as LineEdit's and
    // TextEdit's get_drag_data make it.
    void drag_view(const Drag& going) {
        DragView& view = out.drag.emplace();
        view.from = going.from;
        view.data = going.data;
        view.over = going.over;
        view.position = going.position;
        if (std::ranges::any_of(nodes, [&](const Node& node) {
                return node.entity == going.preview && node.parent == kNone;
            })) {
            view.preview = going.preview;
        }
        if (going.preview_text.empty() || !world.valid(going.from)) {
            return;
        }
        const auto node = std::ranges::find(nodes, going.from, &Node::entity);
        if (node == nodes.end()) {
            return;
        }
        const bool field = world.ecs().all_of<world::LineEdit>(going.from);
        const Resolver source(node->themes, *node->control, field ? "LineEdit" : "TextEdit");
        world::Label label;
        label.text = going.preview_text;
        LabelWork work =
            start_label(label, Resolver(project_link, kDefaultControl, "Label"), false);
        work.out.font = source.font("font");
        work.out.font_size = std::clamp(source.font_size("font_size"), 1, text::kMaxFontSize);
        work.out.outline_size = std::max(source.constant("outline_size"), 0);
        work.out.font_color = source.color("font_color");
        work.out.outline_color = source.color("font_outline_color");
        shape(fonts, work, std::nullopt);
        finish_label(fonts, work, label_minimum(work), false);
        view.text = std::move(work.out);
    }

    void popup(const PopupState& state) {
        const auto& ecs = world.ecs();
        const entt::entity owner = state.owner;
        if (!world.valid(owner)) {
            return;
        }
        enum class From : std::uint8_t { Popup, Option, Menu };
        From from = From::Popup;
        const ThemeLink* link = project_link;
        bool rtl = false;
        Vec2 origin; // where it opens: a PopupMenu's position, or its button's top left
        Vec2 button; // the button's size in the viewport
        if (const auto* menu = ecs.try_get<world::PopupMenu>(owner); menu != nullptr) {
            if (!menu->theme.empty()) {
                if (const Theme* loaded = theme(menu->theme, nullptr); loaded != nullptr) {
                    link = &links.emplace_back(
                        ThemeLink{.theme = loaded, .path = menu->theme, .parent = project_link});
                }
            }
            rtl = menu->layout_direction == "rtl";
            origin = menu->position;
        } else {
            const auto node = std::ranges::find(nodes, owner, &Node::entity);
            if (node == nodes.end() || !node->placed.visible) {
                return;
            }
            from = ecs.all_of<world::OptionButton>(owner) ? From::Option : From::Menu;
            link = node->themes;
            rtl = node->rtl;
            // Control::get_screen_rect.
            const Transform& t = node->placed.transform;
            origin = t.apply({});
            button = {node->placed.size.x * std::hypot(t.a, t.b),
                      node->placed.size.y * std::hypot(t.c, t.d)};
        }
        const PopupTheme look = popup_theme(Resolver(link, kDefaultControl, "PopupMenu"));
        const std::vector<world::PopupMenuItem> items = popup_items(world, owner);
        std::vector<PopupItemWork> works(items.size());
        for (std::size_t i = 0; i < items.size(); ++i) {
            const world::PopupMenuItem& item = items[i];
            PopupItemWork& work = works[i];
            LabelText& text = work.text.out;
            text.font = item.separator ? look.font_separator : look.font;
            text.font_size = item.separator ? look.font_separator_size : look.font_size;
            text.source = item.text;
            work.text.direction = rtl ? text::Direction::Rtl : text::Direction::Ltr;
            shape(fonts, work.text, std::nullopt);
            if (!item.text.empty()) {
                work.text_size = {text.text.width, work.text.line_height};
            }
            // _get_item_icon_size: no wider than icon_max_width, keeping its aspect.
            if (auto icon = texture_size(item.icon); icon) {
                work.icon_texture = *icon;
                const auto max_width = static_cast<float>(look.icon_max_width);
                if (look.icon_max_width > 0 && icon->x > max_width) {
                    *icon = {max_width, icon->y * max_width / icon->x};
                }
                work.icon = icon;
            }
            work.height = popup_item_height(look, item, work);
        }

        // Its contents' minimum size: the panel's, the scroll bar's width, and the widest item
        // with the widest icon and a check box; each item's height and v_separation.
        const BarTheme bar = bar_theme(Resolver(link, kDefaultControl, "VScrollBar"), true);
        const float bar_width = bar_minimum(bar).x;
        const auto v_separation = static_cast<float>(look.v_separation);
        Vec2 minimum = look.panel.minimum_size();
        minimum.x += bar_width;
        float widest = 0.0F;
        float icon_width = 0.0F;
        bool has_check = false;
        float total = 0.0F; // the list's height, as _get_items_total_height
        for (std::size_t i = 0; i < items.size(); ++i) {
            icon_width = std::max(icon_width, works[i].icon.value_or(Vec2{}).x);
            widest = std::max(widest, static_cast<float>(items[i].indent * look.indent) +
                                          works[i].text_size.x);
            has_check = has_check || (items[i].checkable != "none" && !items[i].separator);
            total += works[i].height + v_separation;
        }
        minimum.y += total;
        minimum.x += widest + icon_width +
                     static_cast<float>(look.item_start_padding + look.item_end_padding);
        if (has_check) {
            minimum.x += std::trunc(std::max(look.check_size[0].x, look.check_size[1].x)) +
                         static_cast<float>(look.h_separation);
        }
        minimum.y = std::ceil(std::min(minimum.y, std::trunc(out.viewport.y)));

        // Its rect, in whole pixels: an OptionButton's list is at least as wide as the button.
        const Vec2 least{std::trunc(minimum.x), std::trunc(minimum.y)};
        Vec2 size = least;
        Vec2 position{std::trunc(origin.x), std::trunc(origin.y)};
        if (from == From::Option) {
            size.x = std::max(std::trunc(button.x), least.x);
            position.y = std::trunc(origin.y + button.y);
        } else if (from == From::Menu) {
            const float x =
                rtl ? origin.x + button.x - std::max(std::trunc(button.x), least.x) : origin.x;
            position = {std::trunc(x), std::trunc(origin.y + button.y)};
        }
        const Vec2 parent{std::trunc(out.viewport.x), std::trunc(out.viewport.y)};
        if (parent.x > 0.0F && parent.y > 0.0F) {
            position.x = std::max(std::min(position.x, parent.x - size.x), 0.0F);
            position.y = std::max(std::min(position.y, parent.y - size.y), 0.0F);
            size = {std::min(size.x, parent.x), std::min(size.y, parent.y)};
        }

        // The list scrolls inside the panel's margins, beside a scroll bar when it is taller
        // than its room, as the ScrollContainer in PopupMenu's window.
        PopupView& view = out.popups.emplace_back();
        view.owner = owner;
        view.position = position;
        view.size = size;
        view.rtl = rtl;
        view.v_separation = look.v_separation;
        const std::array<float, 4> margins{look.panel.margin(kLeft), look.panel.margin(kTop),
                                           look.panel.margin(kRight), look.panel.margin(kBottom)};
        view.margins = margins;
        const Vec2 room{std::max(size.x - margins[kLeft] - margins[kRight], 0.0F),
                        std::max(size.y - margins[kTop] - margins[kBottom], 0.0F)};
        const bool scrolls = total > room.y;
        view.bar_width = scrolls ? bar_width : 0.0F;
        view.list_position = {margins[kLeft] + (rtl ? view.bar_width : 0.0F), margins[kTop]};
        view.list_size = {std::max(room.x - view.bar_width, 0.0F), room.y};
        view.scroll_max = scrolls ? total - room.y : 0.0F;
        // Each item's top in the list (_ofs_cache): half a separation before the first.
        std::vector<float> tops(items.size());
        float top = 0.0F;
        for (std::size_t i = 0; i < items.size(); ++i) {
            top += i > 0 ? v_separation : v_separation / 2.0F;
            tops[i] = top;
            top += works[i].height;
        }
        float scroll = state.scroll;
        if (state.scroll_to >= 0 && std::cmp_less(state.scroll_to, items.size())) {
            const auto index = static_cast<std::size_t>(state.scroll_to);
            const float item_y = std::trunc(tops[index]);
            const float relative = item_y - std::trunc(scroll);
            if (relative < 0.0F) {
                scroll = item_y;
            } else if (relative + works[index].height > std::trunc(room.y)) {
                scroll = item_y + works[index].height - std::trunc(room.y);
            }
        }
        view.scroll = scrolls ? std::round(std::clamp(scroll, 0.0F, view.scroll_max)) : 0.0F;
        view.focused =
            state.focused >= 0 && std::cmp_less(state.focused, items.size()) ? state.focused : -1;
        for (std::size_t i = 0; i < items.size(); ++i) {
            view.items.push_back({.y = tops[i],
                                  .height = works[i].height,
                                  .text = items[i].text,
                                  .selectable = !items[i].separator && !items[i].disabled});
        }
        view.panel.push_back(
            {.style = look.panel, .position = {}, .size = size, .texture_size = {}});
        popup_items_draw(view, look, items, works, tops);
        if (scrolls) {
            const Range range{.min = 0.0,
                              .max = static_cast<double>(total),
                              .step = 0.0,
                              .page = static_cast<double>(room.y),
                              .value = static_cast<double>(view.scroll),
                              .rounded = false,
                              .allow_greater = false,
                              .allow_lesser = false};
            const Vec2 at{rtl ? margins[kLeft] : margins[kLeft] + room.x - bar_width,
                          margins[kTop]};
            scroll_bar_boxes(bar, measure_bar(bar, range, -1.0F, at, {bar_width, room.y}),
                             BarLook{}, false, view.bar);
        }
    }

    // PopupMenu::_draw_items: each item's hover box, separator lines, check box, icon and text,
    // in the popup's rect, its list scrolled.
    static void popup_items_draw(PopupView& view, const PopupTheme& look,
                                 const std::vector<world::PopupMenuItem>& items,
                                 std::vector<PopupItemWork>& works,
                                 const std::vector<float>& tops) {
        const bool rtl = view.rtl;
        const float display_width = view.list_size.x;
        const Vec2 base{view.list_position.x, view.list_position.y - view.scroll};
        const auto h_separation = static_cast<float>(look.h_separation);
        const auto texture = [&](const std::string& path, Vec2 texture_size, Vec2 at, Vec2 size) {
            if (path.empty() || texture_size.x <= 0.0F || texture_size.y <= 0.0F) {
                return;
            }
            Box box{.style = {}, .position = base + at, .size = size, .texture_size = texture_size};
            box.style.type = StyleBox::Type::Texture;
            box.style.texture = path;
            view.boxes.push_back(std::move(box));
        };
        const auto style = [&](const StyleBox& box_style, Vec2 at, Vec2 size) {
            view.boxes.push_back(
                {.style = box_style, .position = base + at, .size = size, .texture_size = {}});
        };
        // The widest icon and whether any item has a check box give the offsets of the rest.
        float icon_ofs = 0.0F;
        bool has_check = false;
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (items[i].separator) {
                continue;
            }
            icon_ofs = std::max(icon_ofs, works[i].icon.value_or(Vec2{}).x);
            has_check = has_check || items[i].checkable != "none";
        }
        if (icon_ofs > 0.0F) {
            icon_ofs += h_separation;
        }
        float check_ofs = 0.0F;
        if (has_check) {
            for (std::size_t k = 0; k < look.check_size.size(); ++k) {
                check_ofs =
                    std::max({check_ofs, look.check_size.at(k).x, look.uncheck_size.at(k).x});
            }
            check_ofs += h_separation;
        }
        for (std::size_t i = 0; i < items.size(); ++i) {
            const world::PopupMenuItem& item = items[i];
            PopupItemWork& work = works[i];
            const float h = work.height;
            const Vec2 text_size = work.text_size;
            const Vec2 icon_size = work.icon.value_or(Vec2{});
            Vec2 at{0.0F, tops[i]};
            if (std::cmp_equal(view.focused, i)) {
                const int half_separation = look.v_separation / 2; // whole pixels, as Godot's
                style(look.hover, {at.x, at.y - static_cast<float>(half_separation)},
                      {display_width, h + static_cast<float>(look.v_separation)});
            }
            at.x += static_cast<float>(item.indent * look.indent);
            if (item.separator) {
                if (!item.text.empty() || work.icon) {
                    auto content = static_cast<int>(text_size.x + (h_separation * 2.0F));
                    if (work.icon) {
                        content = static_cast<int>(static_cast<float>(content) + icon_size.x +
                                                   h_separation);
                    }
                    const auto center = static_cast<int>(display_width / 2.0F);
                    const int left = center - (content / 2);
                    const int right = center + (content / 2);
                    if (static_cast<float>(left) > at.x) {
                        const float sep_h = std::trunc(look.labeled_left.minimum_size().y);
                        const float sep_ofs = std::floor((h - sep_h) / 2.0F);
                        style(look.labeled_left, {at.x, at.y + sep_ofs},
                              {std::max(0.0F, static_cast<float>(left) - at.x), sep_h});
                    }
                    if (static_cast<float>(right) < display_width) {
                        const float sep_h = std::trunc(look.labeled_right.minimum_size().y);
                        const float sep_ofs = std::floor((h - sep_h) / 2.0F);
                        style(look.labeled_right, {static_cast<float>(right), at.y + sep_ofs},
                              {std::max(0.0F, display_width - static_cast<float>(right)), sep_h});
                    }
                } else {
                    const float sep_h = std::trunc(look.separator.minimum_size().y);
                    const float sep_ofs = std::floor((h - sep_h) / 2.0F);
                    style(look.separator, {at.x, at.y + sep_ofs}, {display_width, sep_h});
                }
            } else {
                at.x += static_cast<float>(look.item_start_padding);
            }
            if (item.checkable != "none" && !item.separator) {
                const std::size_t k =
                    (item.checkable == "radio" ? 1U : 0U) + (item.disabled ? 2U : 0U);
                const std::string& path = item.checked ? look.check.at(k) : look.uncheck.at(k);
                const Vec2 icon = item.checked ? look.check_size.at(k) : look.uncheck_size.at(k);
                const float x = rtl ? display_width - at.x - icon.x : at.x;
                texture(path, icon, {x, at.y + std::floor((h - icon.y) / 2.0F)}, icon);
            }
            auto separator_ofs = static_cast<int>((display_width - text_size.x) / 2.0F);
            if (work.icon) {
                const float icon_y = std::floor((h - icon_size.y) / 2.0F);
                Vec2 icon_at;
                if (item.separator) {
                    separator_ofs = static_cast<int>(static_cast<float>(separator_ofs) -
                                                     ((icon_size.x + h_separation) / 2.0F));
                    if (rtl) {
                        icon_at = {display_width - at.x - static_cast<float>(separator_ofs) -
                                       icon_size.x,
                                   at.y};
                    } else {
                        icon_at = {at.x + static_cast<float>(separator_ofs), at.y};
                        separator_ofs = static_cast<int>(static_cast<float>(separator_ofs) +
                                                         icon_size.x + h_separation);
                    }
                } else {
                    icon_at = rtl ? Vec2{display_width - at.x - check_ofs - icon_size.x, at.y}
                                  : Vec2{at.x + check_ofs, at.y};
                }
                texture(item.icon, work.icon_texture, {icon_at.x, icon_at.y + icon_y}, icon_size);
            }
            if (item.text.empty()) {
                continue;
            }
            const float text_y = at.y + std::floor((h - text_size.y) / 2.0F);
            Vec2 text_at;
            LabelText& label = work.text.out;
            if (item.separator) {
                text_at = {static_cast<float>(separator_ofs), text_y};
                label.font_color = look.font_separator_color;
                label.outline_color = look.font_separator_outline_color;
                label.outline_size = std::max(look.separator_outline_size, 0);
            } else {
                at.x += icon_ofs + check_ofs;
                text_at =
                    rtl ? Vec2{display_width - text_size.x - at.x, text_y} : Vec2{at.x, text_y};
                if (item.disabled) {
                    label.font_color = look.font_disabled_color;
                } else {
                    label.font_color =
                        std::cmp_equal(view.focused, i) ? look.font_hover_color : look.font_color;
                }
                label.outline_color = look.font_outline_color;
                label.outline_size = std::max(look.outline_size, 0);
            }
            label.shadow_color = {0.0F, 0.0F, 0.0F, 0.0F};
            label.first = 0;
            label.last = std::min<std::size_t>(label.text.lines.size(), 1);
            label.pens = {base + text_at + Vec2{0.0F, label.text.metrics.ascent}};
            label.stretch = {0.0F};
            view.labels.push_back(std::move(label));
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
    builder.project_link = project;
    std::vector<std::pair<entt::entity, std::size_t>> stack;
    // The roots by canvas, then in the order they were made.
    std::vector<entt::entity> roots;
    for (const entt::entity entity : controls) {
        if (!builder.parents.contains(entity) && entity != builder.preview) {
            roots.push_back(entity);
        }
    }
    std::ranges::stable_sort(roots, {},
                             [&](entt::entity entity) { return builder.canvas_of_root(entity); });
    for (const entt::entity entity : std::views::reverse(roots)) {
        stack.emplace_back(entity, kNone);
    }
    // A drag's preview comes last, over the other roots, as set_drag_preview's move_to_front.
    if (builder.preview != entt::null && !builder.parents.contains(builder.preview) &&
        std::ranges::find(controls, builder.preview) != controls.end()) {
        stack.emplace(stack.begin(), builder.preview, kNone);
    }
    while (!stack.empty()) {
        const auto [entity, parent] = stack.back();
        stack.pop_back();
        const std::size_t index = builder.nodes.size();
        Node& node = builder.nodes.emplace_back();
        node.entity = entity;
        node.parent = parent;
        node.canvas =
            parent != kNone ? builder.nodes[parent].canvas : builder.canvas_of_root(entity);
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
    const std::optional<Drag> dragging = drag(world);
    if (dragging) {
        builder.preview = dragging->preview;
        builder.preview_at = dragging->position;
    }
    const world::Tree2D tree(world);
    const world::Canvases canvases = world::canvases(world, tree, viewport);
    out.canvases = canvases.all();
    builder.canvases = &canvases;
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
    builder.check_popup_menus();
    for (const PopupState& state : popup_states(world)) {
        builder.popup(state);
    }
    if (const Tooltip shown = tooltip(world); shown.owner != entt::null) {
        builder.tooltip_view(shown);
    }
    if (dragging) {
        builder.drag_view(*dragging);
    }
    // Then by canvas, then z: Godot draws a canvas item over those of its canvas with a lower
    // z_index, the rest in tree order.
    std::ranges::stable_sort(out.controls, [](const Placed& a, const Placed& b) {
        return a.canvas != b.canvas ? a.canvas < b.canvas : a.z < b.z;
    });
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
