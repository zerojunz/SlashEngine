#include "internal.hpp"

#include "slash/core/text.hpp"
#include "slash/render/image.hpp"
#include "slash/ui/ui.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Themes as Godot's Theme resource (scene/resources/theme.cpp) and its default theme
// (scene/theme/default_theme.cpp).
namespace slash::ui {
namespace {

// Each control type and the type it is built on, as Godot's classes.
constexpr std::array<std::pair<std::string_view, std::string_view>, 39> kTypes{{
    {"Control", ""},
    {"Label", "Control"},
    {"TooltipLabel", "Label"},
    {"BaseButton", "Control"},
    {"Button", "BaseButton"},
    {"CheckBox", "Button"},
    {"CheckButton", "Button"},
    {"OptionButton", "Button"},
    {"MenuButton", "Button"},
    {"TextureButton", "BaseButton"},
    {"LineEdit", "Control"},
    {"TextEdit", "Control"},
    {"CodeEdit", "TextEdit"},
    {"ColorRect", "Control"},
    {"Panel", "Control"},
    {"TextureRect", "Control"},
    {"NinePatchRect", "Control"},
    {"Range", "Control"},
    {"ProgressBar", "Range"},
    {"Slider", "Range"},
    {"HSlider", "Slider"},
    {"VSlider", "Slider"},
    {"ScrollBar", "Range"},
    {"HScrollBar", "ScrollBar"},
    {"VScrollBar", "ScrollBar"},
    {"Container", "Control"},
    {"BoxContainer", "Container"},
    {"HBoxContainer", "BoxContainer"},
    {"VBoxContainer", "BoxContainer"},
    {"GridContainer", "Container"},
    {"MarginContainer", "Container"},
    {"CenterContainer", "Container"},
    {"FlowContainer", "Container"},
    {"HFlowContainer", "FlowContainer"},
    {"VFlowContainer", "FlowContainer"},
    {"PanelContainer", "Container"},
    {"ScrollContainer", "Container"},
    // Windows in Godot, not controls.
    {"PopupMenu", ""},
    {"TooltipPanel", ""},
}};

constexpr std::array<std::string_view, 6> kKinds{"colors",     "constants", "fonts",
                                                 "font_sizes", "icons",     "styles"};

// Godot's make_flat_stylebox: a rounded rect with content margins and smooth corners.
StyleBox flat_style(Vec4 color, std::array<float, 4> margins, int corner_radius,
                    bool draw_center = true, int border_width = 0) {
    StyleBox style;
    style.bg_color = color;
    style.content_margin = margins;
    style.corner_radius.fill(corner_radius);
    style.anti_aliasing = true;
    style.corner_detail = std::min(static_cast<int>(std::ceil(1.5 * corner_radius)), 6);
    style.draw_center = draw_center;
    style.border_width.fill(border_width);
    return style;
}

StyleBox flat_style(Vec4 color, float margin, int corner_radius, bool draw_center = true,
                    int border_width = 0) {
    return flat_style(color, {margin, margin, margin, margin}, corner_radius, draw_center,
                      border_width);
}

} // namespace

// Godot's default theme, each type's own items.
const std::map<std::string, ThemeItems, std::less<>>& own_defaults() {
    static const std::map<std::string, ThemeItems, std::less<>> kThemes = [] {
        constexpr int kSeparation = 4;
        constexpr float kMargin = 4.0F;
        constexpr int kCornerRadius = 3;
        const Vec4 style_normal_color{0.1F, 0.1F, 0.1F, 0.6F};
        const Vec4 style_hover_color{0.225F, 0.225F, 0.225F, 0.6F};
        const Vec4 style_pressed_color{0.0F, 0.0F, 0.0F, 0.6F};
        const Vec4 style_disabled_color{0.1F, 0.1F, 0.1F, 0.3F};
        const Vec4 style_focus_color{1.0F, 1.0F, 1.0F, 0.75F};
        const Vec4 style_progress_color{1.0F, 1.0F, 1.0F, 0.4F};
        const Vec4 control_font_color{0.875F, 0.875F, 0.875F, 1.0F};
        const Vec4 control_font_hover_color{0.95F, 0.95F, 0.95F, 1.0F};
        const Vec4 control_font_focus_color{0.95F, 0.95F, 0.95F, 1.0F};
        const Vec4 control_font_pressed_color{1.0F, 1.0F, 1.0F, 1.0F};
        const Vec4 control_font_disabled_color{0.875F, 0.875F, 0.875F, 0.5F};
        const Vec4 white{1.0F, 1.0F, 1.0F, 1.0F};
        std::map<std::string, ThemeItems, std::less<>> themes;
        themes["BaseButton"].constants = {{"click_margin", 0}};
        ThemeItems& button = themes["Button"];
        // The focus outline sits just outside the button, so it doesn't cover its content.
        StyleBox focus = flat_style(style_focus_color, kMargin, kCornerRadius, false, 2);
        focus.expand_margin.fill(2.0F);
        button.styles = {{"normal", flat_style(style_normal_color, kMargin, kCornerRadius)},
                         {"hover", flat_style(style_hover_color, kMargin, kCornerRadius)},
                         {"pressed", flat_style(style_pressed_color, kMargin, kCornerRadius)},
                         {"disabled", flat_style(style_disabled_color, kMargin, kCornerRadius)},
                         {"focus", focus}};
        button.colors = {{"font_color", control_font_color},
                         {"font_pressed_color", control_font_pressed_color},
                         {"font_hover_color", control_font_hover_color},
                         {"font_focus_color", control_font_focus_color},
                         {"font_hover_pressed_color", control_font_pressed_color},
                         {"font_disabled_color", control_font_disabled_color},
                         {"font_outline_color", {0.0F, 0.0F, 0.0F, 1.0F}},
                         {"icon_normal_color", white},
                         {"icon_pressed_color", white},
                         {"icon_hover_color", white},
                         {"icon_hover_pressed_color", white},
                         {"icon_focus_color", white},
                         {"icon_disabled_color", {1.0F, 1.0F, 1.0F, 0.4F}}};
        button.constants = {{"outline_size", 0},
                            {"h_separation", kSeparation},
                            {"icon_max_width", 0},
                            {"align_to_largest_stylebox", 0},
                            {"line_spacing", 0}};
        button.fonts = {{"font", ""}};
        button.font_sizes = {{"font_size", text::kDefaultFontSize}};
        // Check boxes and check buttons draw no box, only their text and an icon that shows
        // whether they are on.
        const auto check_items = [&](std::array<float, 4> margins) {
            ThemeItems items;
            StyleBox empty;
            empty.type = StyleBox::Type::Empty;
            empty.content_margin = margins;
            for (const std::string_view state :
                 {"normal", "pressed", "disabled", "hover", "hover_pressed"}) {
                items.styles.emplace(state, empty);
            }
            items.styles.emplace("focus", focus);
            items.colors = {{"font_color", control_font_color},
                            {"font_pressed_color", control_font_pressed_color},
                            {"font_hover_color", control_font_hover_color},
                            {"font_hover_pressed_color", control_font_pressed_color},
                            {"font_focus_color", control_font_focus_color},
                            {"font_disabled_color", control_font_disabled_color},
                            {"font_outline_color", {0.0F, 0.0F, 0.0F, 1.0F}}};
            items.constants = {
                {"h_separation", kSeparation}, {"check_v_offset", 0}, {"outline_size", 0}};
            return items;
        };
        ThemeItems& check_box = themes["CheckBox"] = check_items({4.0F, 4.0F, 4.0F, 4.0F});
        check_box.icons = {{"checked", "builtin:checked"},
                           {"checked_disabled", "builtin:checked_disabled"},
                           {"unchecked", "builtin:unchecked"},
                           {"unchecked_disabled", "builtin:unchecked_disabled"},
                           {"radio_checked", "builtin:radio_checked"},
                           {"radio_checked_disabled", "builtin:radio_checked_disabled"},
                           {"radio_unchecked", "builtin:radio_unchecked"},
                           {"radio_unchecked_disabled", "builtin:radio_unchecked_disabled"}};
        check_box.colors.emplace("checkbox_checked_color", white);
        check_box.colors.emplace("checkbox_unchecked_color", white);
        ThemeItems& check_button = themes["CheckButton"] = check_items({6.0F, 4.0F, 6.0F, 4.0F});
        check_button.icons = {
            {"checked", "builtin:toggle_on"},
            {"checked_disabled", "builtin:toggle_on_disabled"},
            {"unchecked", "builtin:toggle_off"},
            {"unchecked_disabled", "builtin:toggle_off_disabled"},
            {"checked_mirrored", "builtin:toggle_on_mirrored"},
            {"checked_disabled_mirrored", "builtin:toggle_on_disabled_mirrored"},
            {"unchecked_mirrored", "builtin:toggle_off_mirrored"},
            {"unchecked_disabled_mirrored", "builtin:toggle_off_disabled_mirrored"}};
        check_button.colors.emplace("button_checked_color", white);
        check_button.colors.emplace("button_unchecked_color", white);
        // Option buttons: wider margins, and an arrow at the end.
        ThemeItems& option = themes["OptionButton"];
        const std::array<float, 4> option_margins{2.0F * kMargin, kMargin, 2.0F * kMargin, kMargin};
        for (const auto& [state, color] :
             {std::pair{"normal", style_normal_color}, std::pair{"hover", style_hover_color},
              std::pair{"pressed", style_pressed_color},
              std::pair{"disabled", style_disabled_color}}) {
            option.styles.emplace(state, flat_style(color, option_margins, kCornerRadius));
            option.styles.emplace(std::string(state) + "_mirrored",
                                  flat_style(color, option_margins, kCornerRadius));
        }
        option.styles.emplace("focus", focus);
        option.icons = {{"arrow", "builtin:option_button_arrow"}};
        option.colors = {{"font_color", control_font_color},
                         {"font_pressed_color", control_font_pressed_color},
                         {"font_hover_color", control_font_hover_color},
                         {"font_hover_pressed_color", control_font_pressed_color},
                         {"font_focus_color", control_font_focus_color},
                         {"font_disabled_color", control_font_disabled_color},
                         {"font_outline_color", {0.0F, 0.0F, 0.0F, 1.0F}}};
        option.constants = {{"h_separation", kSeparation},
                            {"arrow_margin", 4},
                            {"outline_size", 0},
                            {"modulate_arrow", 0}};
        option.fonts = {{"font", ""}};
        option.font_sizes = {{"font_size", text::kDefaultFontSize}};
        // Menu buttons are flat buttons, dimmer when disabled.
        ThemeItems& menu_button = themes["MenuButton"];
        menu_button.styles = button.styles;
        menu_button.colors = {{"font_color", control_font_color},
                              {"font_pressed_color", control_font_pressed_color},
                              {"font_hover_color", control_font_hover_color},
                              {"font_focus_color", control_font_focus_color},
                              {"font_disabled_color", {1.0F, 1.0F, 1.0F, 0.3F}},
                              {"font_outline_color", {0.0F, 0.0F, 0.0F, 1.0F}}};
        menu_button.constants = {{"h_separation", kSeparation}, {"outline_size", 0}};
        menu_button.fonts = {{"font", ""}};
        menu_button.font_sizes = {{"font_size", text::kDefaultFontSize}};
        // Popup menus always have a border, so they stand out from what is under them, and
        // separate items with a thin line.
        ThemeItems& popup = themes["PopupMenu"];
        StyleBox popup_panel =
            flat_style({0.25F, 0.25F, 0.25F, 1.0F}, kMargin, kCornerRadius, true, 2);
        popup_panel.border_color = {0.175F, 0.175F, 0.175F, 1.0F};
        StyleBox separator;
        separator.type = StyleBox::Type::Line;
        separator.color = {0.5F, 0.5F, 0.5F, 1.0F};
        separator.content_margin = {kMargin, 0.0F, kMargin, 0.0F};
        popup.styles = {{"panel", popup_panel},
                        {"hover", flat_style({0.4F, 0.4F, 0.4F, 1.0F}, kMargin, kCornerRadius)},
                        {"separator", separator},
                        {"labeled_separator_left", separator},
                        {"labeled_separator_right", separator}};
        popup.icons = check_box.icons;
        popup.colors = {{"font_color", control_font_color},
                        {"font_hover_color", control_font_color},
                        {"font_disabled_color", {0.4F, 0.4F, 0.4F, 0.8F}},
                        {"font_separator_color", control_font_color},
                        {"font_outline_color", {0.0F, 0.0F, 0.0F, 1.0F}},
                        {"font_separator_outline_color", {0.0F, 0.0F, 0.0F, 1.0F}}};
        popup.constants = {{"indent", 10},
                           {"h_separation", kSeparation},
                           {"v_separation", kSeparation},
                           {"item_start_padding", 2},
                           {"item_end_padding", 2},
                           {"icon_max_width", 0},
                           {"outline_size", 0},
                           {"separator_outline_size", 0}};
        popup.fonts = {{"font", ""}, {"font_separator", ""}};
        popup.font_sizes = {{"font_size", text::kDefaultFontSize},
                            {"font_separator_size", text::kDefaultFontSize}};
        // A line along the bottom tells a LineEdit from a Button.
        ThemeItems& line_edit = themes["LineEdit"];
        StyleBox line_normal = flat_style(style_normal_color, kMargin, kCornerRadius);
        line_normal.border_width[3] = 2;
        line_normal.border_color = style_pressed_color;
        StyleBox line_read_only = flat_style(style_disabled_color, kMargin, kCornerRadius);
        line_read_only.border_width[3] = 2;
        line_read_only.border_color = {0.0F, 0.0F, 0.0F, 0.3F};
        line_edit.styles = {
            {"normal", line_normal}, {"read_only", line_read_only}, {"focus", focus}};
        line_edit.colors = {{"font_color", control_font_color},
                            {"font_selected_color", control_font_pressed_color},
                            {"font_uneditable_color", control_font_disabled_color},
                            {"font_placeholder_color", {0.875F, 0.875F, 0.875F, 0.6F}},
                            {"font_outline_color", {0.0F, 0.0F, 0.0F, 1.0F}},
                            {"caret_color", control_font_hover_color},
                            {"selection_color", {0.5F, 0.5F, 0.5F, 1.0F}},
                            {"clear_button_color", control_font_color},
                            {"clear_button_color_pressed", control_font_pressed_color}};
        line_edit.constants = {
            {"minimum_character_width", 4}, {"outline_size", 0}, {"caret_width", 1}};
        line_edit.fonts = {{"font", ""}};
        line_edit.font_sizes = {{"font_size", text::kDefaultFontSize}};
        ThemeItems& text_edit = themes["TextEdit"];
        text_edit.styles = {
            {"normal", line_normal}, {"read_only", line_read_only}, {"focus", focus}};
        text_edit.colors = {{"background_color", {0.0F, 0.0F, 0.0F, 0.0F}},
                            {"font_color", control_font_color},
                            {"font_selected_color", {0.0F, 0.0F, 0.0F, 0.0F}},
                            {"font_readonly_color", control_font_disabled_color},
                            {"font_placeholder_color", {0.875F, 0.875F, 0.875F, 0.6F}},
                            {"font_outline_color", {0.0F, 0.0F, 0.0F, 1.0F}},
                            {"selection_color", {0.5F, 0.5F, 0.5F, 1.0F}},
                            {"current_line_color", {0.25F, 0.25F, 0.26F, 0.8F}},
                            {"caret_color", control_font_color},
                            {"caret_background_color", {0.0F, 0.0F, 0.0F, 1.0F}},
                            {"word_highlighted_color", {0.5F, 0.5F, 0.5F, 0.25F}},
                            {"search_result_color", {0.3F, 0.3F, 0.3F, 1.0F}},
                            {"search_result_border_color", {0.3F, 0.3F, 0.3F, 0.4F}}};
        text_edit.constants = {
            {"line_spacing", 4}, {"outline_size", 0}, {"caret_width", 1}, {"wrap_offset", 10}};
        text_edit.fonts = {{"font", ""}};
        text_edit.font_sizes = {{"font_size", text::kDefaultFontSize}};
        // CodeEdit: TextEdit's, with its gutters' icons and colors.
        ThemeItems& code_edit = themes["CodeEdit"];
        code_edit.icons = {{"breakpoint", "builtin:breakpoint"},
                           {"bookmark", "builtin:bookmark"},
                           {"executing_line", "builtin:arrow_right"}};
        code_edit.colors = {{"bookmark_color", {0.5F, 0.64F, 1.0F, 0.8F}},
                            {"breakpoint_color", {0.9F, 0.29F, 0.3F, 1.0F}},
                            {"executing_line_color", {0.98F, 0.89F, 0.27F, 1.0F}},
                            {"line_number_color", {0.67F, 0.67F, 0.67F, 0.4F}},
                            {"word_highlighted_color", {0.8F, 0.9F, 0.9F, 0.15F}},
                            {"line_length_guideline_color", {0.3F, 0.5F, 0.8F, 0.1F}}};
        // Scroll bars: a track and a rounded grabber, with no arrows.
        const StyleBox grabber = flat_style(style_progress_color, 4.0F, 10);
        const StyleBox grabber_highlight = flat_style(style_focus_color, 4.0F, 10);
        const StyleBox grabber_pressed =
            flat_style({style_focus_color.x * 0.75F, style_focus_color.y * 0.75F,
                        style_focus_color.z * 0.75F, style_focus_color.w},
                       4.0F, 10);
        themes["ScrollBar"].constants = {{"icon_max_size", 0}};
        for (const std::string_view type : {"HScrollBar", "VScrollBar"}) {
            ThemeItems& bar = themes[std::string(type)];
            const bool vertical = type == "VScrollBar";
            bar.styles = {{"scroll", flat_style(style_normal_color,
                                                vertical ? std::array{4.0F, 0.0F, 4.0F, 0.0F}
                                                         : std::array{0.0F, 4.0F, 0.0F, 4.0F},
                                                10)},
                          {"scroll_focus", focus},
                          {"grabber", grabber},
                          {"grabber_highlight", grabber_highlight},
                          {"grabber_pressed", grabber_pressed}};
            for (const std::string_view icon : {"increment", "decrement"}) {
                for (const std::string_view state : {"", "_highlight", "_pressed"}) {
                    bar.icons.emplace(std::string(icon) + std::string(state), "");
                }
            }
            bar.constants = vertical ? std::map<std::string, int, std::less<>>{{"padding_left", 0},
                                                                               {"padding_right", 0}}
                                     : std::map<std::string, int, std::less<>>{
                                           {"padding_top", 0}, {"padding_bottom", 0}};
        }
        // Scroll containers draw nothing of their own but a focus outline, if asked to.
        ThemeItems& scroll = themes["ScrollContainer"];
        StyleBox empty_panel;
        empty_panel.type = StyleBox::Type::Empty;
        StyleBox scroll_focus = flat_style(style_focus_color, kMargin, kCornerRadius, false, 2);
        scroll_focus.border_color = style_focus_color;
        scroll_focus.expand_margin.fill(4.0F);
        scroll.styles = {{"panel", empty_panel}, {"focus", scroll_focus}};
        scroll.constants = {{"scrollbar_margin_left", -1},  {"scrollbar_margin_top", -1},
                            {"scrollbar_margin_right", -1}, {"scrollbar_margin_bottom", -1},
                            {"scrollbar_h_separation", 0},  {"scrollbar_v_separation", 0}};
        // Sliders: a track, the part of it before the grabber filled in, and a round grabber.
        for (const std::string_view type : {"HSlider", "VSlider"}) {
            ThemeItems& slider = themes[std::string(type)];
            slider.styles = {{"slider", flat_style(style_normal_color, kMargin, 4)},
                             {"grabber_area", flat_style(style_progress_color, kMargin, 4)},
                             {"grabber_area_highlight", flat_style(style_focus_color, kMargin, 4)}};
            slider.icons = {
                {"grabber", "builtin:slider_grabber"},
                {"grabber_highlight", "builtin:slider_grabber_hl"},
                {"grabber_disabled", "builtin:slider_grabber_disabled"},
                {"tick", type == "HSlider" ? "builtin:hslider_tick" : "builtin:vslider_tick"}};
            slider.constants = {{"center_grabber", 0},
                                {"grabber_offset", 0},
                                {"grabber_max_size", 0},
                                {"tick_offset", 0},
                                {"tick_max_size", 0}};
        }
        ThemeItems& label = themes["Label"];
        label.colors = {{"font_color", {1.0F, 1.0F, 1.0F, 1.0F}},
                        {"font_shadow_color", {0.0F, 0.0F, 0.0F, 0.0F}},
                        {"font_outline_color", {0.0F, 0.0F, 0.0F, 1.0F}}};
        label.constants = {{"line_spacing", 3},    {"paragraph_spacing", 0},
                           {"outline_size", 0},    {"shadow_offset_x", 1},
                           {"shadow_offset_y", 1}, {"shadow_outline_size", 1}};
        label.fonts = {{"font", ""}};
        label.font_sizes = {{"font_size", text::kDefaultFontSize}};
        // Tooltips: a Label in a see-through panel.
        ThemeItems& tooltip_label = themes["TooltipLabel"];
        tooltip_label.colors = {{"font_color", control_font_color},
                                {"font_shadow_color", {0.0F, 0.0F, 0.0F, 0.0F}},
                                {"font_outline_color", {0.0F, 0.0F, 0.0F, 1.0F}}};
        tooltip_label.constants = {
            {"shadow_offset_x", 1}, {"shadow_offset_y", 1}, {"outline_size", 0}};
        themes["TooltipPanel"].styles = {
            {"panel", flat_style({0.0F, 0.0F, 0.0F, 0.5F},
                                 {2.0F * kMargin, 0.5F * kMargin, 2.0F * kMargin, 0.5F * kMargin},
                                 kCornerRadius)}};
        themes["Panel"].styles = {{"panel", flat_style(style_normal_color, 0.0F, 3)}};
        themes["PanelContainer"].styles = themes["Panel"].styles;
        ThemeItems& bar = themes["ProgressBar"];
        bar.styles = {{"background", flat_style(style_disabled_color, 2.0F, 6)},
                      {"fill", flat_style(style_progress_color, 2.0F, 6)}};
        bar.fonts = {{"font", ""}};
        bar.font_sizes = {{"font_size", text::kDefaultFontSize}};
        bar.colors = {{"font_color", control_font_hover_color},
                      {"font_outline_color", {0.0F, 0.0F, 0.0F, 1.0F}}};
        bar.constants = {{"outline_size", 0}};
        for (const std::string_view box : {"BoxContainer", "HBoxContainer", "VBoxContainer"}) {
            themes[std::string(box)].constants = {{"separation", kSeparation}};
        }
        themes["MarginContainer"].constants = {
            {"margin_left", 0}, {"margin_top", 0}, {"margin_right", 0}, {"margin_bottom", 0}};
        for (const std::string_view grid :
             {"GridContainer", "FlowContainer", "HFlowContainer", "VFlowContainer"}) {
            themes[std::string(grid)].constants = {{"h_separation", kSeparation},
                                                   {"v_separation", kSeparation}};
        }
        return themes;
    }();
    return kThemes;
}

const ThemeItems& optional_items(std::string_view type) {
    static const ThemeItems kNone;
    // Button's hover_pressed style is used when a theme has one, else pressed is, and a state's
    // _mirrored style in place of its own when the button lays out right to left, as Godot's.
    static const ThemeItems kButton = [] {
        ThemeItems items;
        StyleBox empty;
        empty.type = StyleBox::Type::Empty;
        for (const std::string_view state :
             {"hover_pressed", "normal", "hover", "pressed", "disabled"}) {
            if (state == "hover_pressed") {
                items.styles.emplace(state, empty);
            }
            items.styles.emplace(std::string(state) + "_mirrored", empty);
        }
        return items;
    }();
    const std::vector<std::string_view> bases = base_types(type);
    return std::ranges::find(bases, std::string_view("Button")) != bases.end() ? kButton : kNone;
}

namespace {

// Adds the items of from that into doesn't have.
void merge(ThemeItems& into, const ThemeItems& from) {
    into.colors.insert(from.colors.begin(), from.colors.end());
    into.constants.insert(from.constants.begin(), from.constants.end());
    into.fonts.insert(from.fonts.begin(), from.fonts.end());
    into.font_sizes.insert(from.font_sizes.begin(), from.font_sizes.end());
    into.icons.insert(from.icons.begin(), from.icons.end());
    into.styles.insert(from.styles.begin(), from.styles.end());
}

// The names a kind of item has.
std::vector<std::string_view> names_of(const ThemeItems& items, std::string_view kind) {
    std::vector<std::string_view> names;
    const auto add = [&](const auto& map) {
        for (const auto& [name, value] : map) {
            names.emplace_back(name);
        }
    };
    if (kind == "colors") {
        add(items.colors);
    } else if (kind == "constants") {
        add(items.constants);
    } else if (kind == "fonts") {
        add(items.fonts);
    } else if (kind == "font_sizes") {
        add(items.font_sizes);
    } else if (kind == "icons") {
        add(items.icons);
    } else {
        add(items.styles);
    }
    return names;
}

std::string quoted(std::string_view text) {
    return "\"" + std::string(text) + "\"";
}

std::string with_hint(std::string message, std::string_view name,
                      const std::vector<std::string_view>& candidates) {
    if (const std::string_view hint = closest_match(name, candidates); !hint.empty()) {
        message += "; did you mean " + quoted(hint) + "?";
    }
    return message;
}

json::Value object_of(json::Value items, std::string_view description) {
    json::Value schema;
    schema["type"] = "object";
    schema["additionalProperties"] = std::move(items);
    schema["description"] = description;
    return schema;
}

std::string font_path_error(const std::string& path) {
    if (path.empty()) {
        return "";
    }
    if (!is_project_path(path)) {
        return "expected a path inside the project folder, separated by '/', with no \".\" or "
               "\"..\" parts";
    }
    if (!text::is_font_path(path)) {
        return "expected a .ttf, .otf or .font.json file";
    }
    return "";
}

Vec4 vec4(const json::Value& value) {
    const json::Array& channels = value.as_array();
    return {
        static_cast<float>(channels[0].as_double()), static_cast<float>(channels[1].as_double()),
        static_cast<float>(channels[2].as_double()), static_cast<float>(channels[3].as_double())};
}

} // namespace

// Why an icon's path is wrong: not a PNG, BMP or KTX2 file in the project, nor a built-in icon.
std::string icon_path_error(const std::string& path) {
    if (path.empty()) {
        return "";
    }
    if (path.starts_with(kBuiltinPrefix)) {
        const std::string_view name = std::string_view(path).substr(kBuiltinPrefix.size());
        if (builtin_icon(path) != nullptr) {
            return "";
        }
        return with_hint(quoted(name) + " isn't a built-in icon", name, builtin_icon_names());
    }
    if (!is_project_path(path)) {
        return "expected a path inside the project folder, separated by '/', with no \".\" or "
               "\"..\" parts, or a built-in icon such as \"builtin:checked\"";
    }
    if (!path.ends_with(".png") && !path.ends_with(".bmp") && !path.ends_with(".ktx2")) {
        return "expected a .png, .bmp or .ktx2 file";
    }
    return "";
}

std::vector<std::string_view> base_types(std::string_view type) {
    std::vector<std::string_view> out;
    bool more = !type.empty();
    while (more) {
        more = false;
        for (const auto& [name, base] : kTypes) {
            if (name == type) {
                out.push_back(name);
                type = base;
                more = !type.empty();
                break;
            }
        }
    }
    return out;
}

const std::vector<std::string_view>& control_types() {
    static const std::vector<std::string_view> kNames = [] {
        std::vector<std::string_view> names;
        names.reserve(kTypes.size());
        for (const auto& [name, base] : kTypes) {
            names.push_back(name);
        }
        return names;
    }();
    return kNames;
}

const ThemeItems& default_theme(std::string_view type) {
    static const std::map<std::string, ThemeItems, std::less<>> kMerged = [] {
        std::map<std::string, ThemeItems, std::less<>> merged;
        for (const auto& [name, base] : kTypes) {
            ThemeItems& items = merged[std::string(name)];
            for (const std::string_view each : base_types(name)) {
                if (const auto own = own_defaults().find(each); own != own_defaults().end()) {
                    merge(items, own->second);
                }
            }
        }
        return merged;
    }();
    const auto found = kMerged.find(type);
    return found != kMerged.end() ? found->second : kMerged.find("Control")->second;
}

json::Value theme_schema() {
    json::Value color;
    color["type"] = "array";
    color["items"]["type"] = "number";
    color["items"]["minimum"] = 0;
    color["minItems"] = 4;
    color["maxItems"] = 4;
    json::Value constant;
    constant["type"] = "integer";
    json::Value font;
    font["type"] = "string";
    json::Value icon;
    icon["type"] = "string";
    json::Value font_size;
    font_size["type"] = "integer";
    font_size["minimum"] = 1;
    font_size["maximum"] = text::kMaxFontSize;

    json::Value type;
    type["type"] = "object";
    type["additionalProperties"] = false;
    json::Value& items = type["properties"];
    items["base_type"]["type"] = "string";
    items["base_type"]["description"] =
        "Makes this type a variation of another, e.g. \"Label\": controls whose "
        "theme_type_variation names it use its items, then the base type's.";
    items["colors"] = object_of(color, "Colors by name, as [r, g, b, a] from 0 to 1.");
    items["constants"] = object_of(constant, "Constants by name, e.g. \"separation\": 8.");
    items["fonts"] = object_of(font, "Fonts by name: a .ttf, .otf or .font.json file; \"\" is "
                                     "the built-in font.");
    items["font_sizes"] = object_of(font_size, "Font sizes by name, in pixels.");
    items["icons"] =
        object_of(icon, "Icons by name: a .png, .bmp or .ktx2 file, a built-in icon such as "
                        "\"builtin:radio_checked\", or \"\" for none.");
    items["styles"] = object_of(style_box_schema(), "Style boxes by name.");

    json::Value schema;
    schema["$schema"] = "https://json-schema.org/draft/2020-12/schema";
    schema["title"] = "SlashEngine theme";
    schema["description"] = "Theme items by control type, as Godot's Theme. A control uses its "
                            "theme_override_* items, then its own and its parents' themes, the "
                            "project's theme and the default theme.";
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["required"] = json::Array{"format"};
    json::Value& properties = schema["properties"];
    properties["format"]["const"] = 1;
    properties["format"]["description"] = "Format version. Must be 1.";
    properties["default_font"]["type"] = "string";
    properties["default_font"]["description"] =
        "The font of every type in this theme that names none: a .ttf, .otf or .font.json file.";
    properties["default_font_size"] = font_size;
    properties["default_font_size"]["description"] =
        "The font size of every type in this theme that names none.";
    properties["types"] = object_of(type, "Items by type: a control type such as \"Label\" or "
                                          "\"PanelContainer\", or a variation with a base_type.");
    return schema;
}

std::vector<json::ValidationError> validate_theme(const json::Value& doc) {
    std::vector<json::ValidationError> errors = json::validate(doc, theme_schema());
    if (!errors.empty()) {
        return errors;
    }
    if (const json::Value* font = doc.find("default_font"); font != nullptr) {
        if (std::string error = font_path_error(font->as_string()); !error.empty()) {
            errors.push_back({"/default_font", std::move(error)});
        }
    }
    const json::Value* types = doc.find("types");
    if (types == nullptr) {
        return errors;
    }
    const json::Object& all = types->as_object();
    for (const auto& [name, type] : all) {
        const std::string path = "/types/" + json::pointer_escape(name);
        // Down the variations to the control type they lead to.
        std::string_view native;
        std::set<std::string_view> seen;
        std::string_view at = name;
        std::string at_path = path;
        while (true) {
            if (!base_types(at).empty()) {
                native = at;
                break;
            }
            const auto found = all.find(at);
            const json::Value* base =
                found != all.end() ? found->second.find("base_type") : nullptr;
            if (found == all.end() || base == nullptr) {
                if (at == name) {
                    std::string message = quoted(name) + " isn't a control type";
                    if (closest_match(name, control_types()).empty()) {
                        message += "; give it a base_type to make it a variation";
                    } else {
                        message = with_hint(std::move(message), name, control_types());
                    }
                    errors.push_back({path, std::move(message)});
                }
                break;
            }
            if (!seen.insert(at).second) {
                errors.push_back({path + "/base_type",
                                  "the base types of " + quoted(name) + " go round in a loop"});
                break;
            }
            const std::string& next = base->as_string();
            if (base_types(next).empty() && !all.contains(next)) {
                std::vector<std::string_view> candidates = control_types();
                for (const auto& [other, value] : all) {
                    candidates.emplace_back(other);
                }
                errors.push_back(
                    {at_path + "/base_type",
                     with_hint(quoted(next) + " isn't a control type or a type in this theme", next,
                               candidates)});
                break;
            }
            at = next;
            at_path = "/types/" + json::pointer_escape(next);
        }
        ThemeItems known = default_theme(native.empty() ? "Control" : native);
        merge(known, optional_items(native));
        for (const std::string_view kind : kKinds) {
            const json::Value* items = type.find(kind);
            if (items == nullptr) {
                continue;
            }
            const std::vector<std::string_view> names = names_of(known, kind);
            for (const auto& [item, value] : items->as_object()) {
                const std::string item_path =
                    path + "/" + std::string(kind) + "/" + json::pointer_escape(item);
                if (!native.empty() && std::ranges::find(names, item) == names.end()) {
                    std::string message = std::string(native) + " has no " + std::string(kind) +
                                          " item " + quoted(item);
                    if (names.empty()) {
                        message += "; it has none";
                    } else {
                        message = with_hint(std::move(message), item, names);
                    }
                    errors.push_back({item_path, std::move(message)});
                    continue;
                }
                if (kind == "styles") {
                    for (json::ValidationError& error : validate_style_box(value)) {
                        errors.push_back({item_path + error.path, std::move(error.message)});
                    }
                } else if (kind == "fonts") {
                    if (std::string error = font_path_error(value.as_string()); !error.empty()) {
                        errors.push_back({item_path, std::move(error)});
                    }
                } else if (kind == "icons") {
                    if (std::string error = icon_path_error(value.as_string()); !error.empty()) {
                        errors.push_back({item_path, std::move(error)});
                    }
                }
            }
        }
    }
    return errors;
}

Result<Theme> parse_theme(const json::Value& doc) {
    if (const auto errors = validate_theme(doc); !errors.empty()) {
        const json::ValidationError& first = errors.front();
        return make_error(ErrorCode::InvalidArgument,
                          (first.path.empty() ? std::string("root") : first.path) + ": " +
                              first.message);
    }
    Theme out;
    if (const json::Value* font = doc.find("default_font"); font != nullptr) {
        out.default_font = font->as_string();
    }
    if (const json::Value* size = doc.find("default_font_size"); size != nullptr) {
        out.default_font_size = static_cast<int>(size->as_int());
    }
    const json::Value* types = doc.find("types");
    if (types == nullptr) {
        return out;
    }
    for (const auto& [name, value] : types->as_object()) {
        Theme::Type& type = out.types[name];
        if (const json::Value* base = value.find("base_type"); base != nullptr) {
            type.base_type = base->as_string();
        }
        if (const json::Value* items = value.find("colors"); items != nullptr) {
            for (const auto& [item, color] : items->as_object()) {
                type.items.colors.emplace(item, vec4(color));
            }
        }
        if (const json::Value* items = value.find("constants"); items != nullptr) {
            for (const auto& [item, constant] : items->as_object()) {
                type.items.constants.emplace(item, static_cast<int>(constant.as_int()));
            }
        }
        if (const json::Value* items = value.find("fonts"); items != nullptr) {
            for (const auto& [item, font] : items->as_object()) {
                type.items.fonts.emplace(item, font.as_string());
            }
        }
        if (const json::Value* items = value.find("font_sizes"); items != nullptr) {
            for (const auto& [item, size] : items->as_object()) {
                type.items.font_sizes.emplace(item, static_cast<int>(size.as_int()));
            }
        }
        if (const json::Value* items = value.find("icons"); items != nullptr) {
            for (const auto& [item, icon] : items->as_object()) {
                type.items.icons.emplace(item, icon.as_string());
            }
        }
        if (const json::Value* items = value.find("styles"); items != nullptr) {
            for (const auto& [item, style] : items->as_object()) {
                type.items.styles.emplace(item, parse_style_box(style).value());
            }
        }
    }
    return out;
}

void add_asset_types(asset::Assets& assets) {
    text::add_asset_types(assets);
    assets.add_type<render::Image>("texture", {".png", ".bmp", ".ktx2"},
                                   [](std::string_view bytes, std::string_view /*path*/) {
                                       return render::decode_image(bytes);
                                   });
    assets.add_type<Theme>("theme", {std::string(kThemeSuffix)},
                           [](std::string_view text, std::string_view /*path*/) -> Result<Theme> {
                               auto doc = json::parse(text);
                               if (!doc) {
                                   return doc.error();
                               }
                               return parse_theme(doc.value());
                           });
}

} // namespace slash::ui
