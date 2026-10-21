#pragma once

#include "slash/ui/ui.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

// What the UI sources share and nothing else needs.
namespace slash::ui {

// Godot's default theme: each control type's own items, without its base types'.
[[nodiscard]] const std::map<std::string, ThemeItems, std::less<>>& own_defaults();
// Items a control type uses when a theme sets them but the default theme has none of, as
// Button's hover_pressed style; their values mean nothing.
[[nodiscard]] const ThemeItems& optional_items(std::string_view type);

// Why an icon's path is wrong, or "": not a .png, .bmp or .ktx2 file in the project, a built-in
// icon or "" (theme.cpp).
[[nodiscard]] std::string icon_path_error(const std::string& path);

// The control types an entity's components make it, as Godot's classes: "Label",
// "VBoxContainer" for a vertical BoxContainer and so on, the drawing ones first and containers
// last; "Control" when it has only a Control.
[[nodiscard]] std::vector<std::string_view> control_classes(const world::World& world,
                                                            entt::entity entity);

// The Button an entity has: its Button, or its CheckBox, CheckButton, OptionButton or
// MenuButton, which are buttons too; nullptr for none.
[[nodiscard]] const world::Button* button_of(const world::World& world, entt::entity entity);
[[nodiscard]] world::Button* button_of(world::World& world, entt::entity entity);
// Which component button_of found: "Button", "CheckBox", "CheckButton", "OptionButton" or
// "MenuButton"; "" for none.
[[nodiscard]] std::string_view button_type(const world::World& world, entt::entity entity);

// A Range's fields, as Godot's Range keeps them: max_value at least min_value, page at most the
// span between them.
struct Range {
    double min = 0.0;
    double max = 100.0;
    double step = 1.0;
    double page = 0.0;
    double value = 0.0;
    bool rounded = false;
    bool allow_greater = false;
    bool allow_lesser = false;
};
[[nodiscard]] Range range_of(const world::Slider& slider);
[[nodiscard]] Range range_of(const world::ScrollBar& bar);
// A value as Range::set_value keeps it: rounded to min plus a multiple of step, to a whole
// number if rounded, then kept between min and max less page unless allowed past them.
[[nodiscard]] double range_value(const Range& range, double value);
// How far along its value is, from 0 to 1, as Range::get_as_ratio.
[[nodiscard]] double range_ratio(const Range& range);
// The value a ratio picks, as Range::set_as_ratio.
[[nodiscard]] double range_from_ratio(const Range& range, double ratio);

// How a scroll bar looks from what the gui system keeps: the part the pointer is over and the
// part held, as ScrollBar's highlight, decr_active, incr_active and drag.active.
struct BarLook {
    int highlight = 0; // 0 none, 1 the decrement arrow, 2 the track, 3 the increment arrow
    bool decrement_pressed = false;
    bool increment_pressed = false;
    bool dragging = false;
};
// Which bar: a ScrollBar's own, or a ScrollContainer's horizontal or vertical one.
enum class Bar : std::uint8_t { Own, Horizontal, Vertical };
[[nodiscard]] BarLook bar_look(const world::World& world, entt::entity control, Bar bar);
// The scroll a ScrollContainer asks for, [horizontal, vertical]: the gui system's, which keeps
// fractions of a pixel, while its component's whole pixels still match it; else the
// component's.
[[nodiscard]] std::array<double, 2> wanted_scroll(const world::World& world,
                                                  entt::entity container);

// What the gui system keeps for an open popup menu that the layout needs: the item the pointer
// or keys are on, how far its list is scrolled, and an item to scroll into view, as
// PopupMenu::scroll_to_item asks.
struct PopupState {
    entt::entity owner = entt::null;
    int focused = -1;
    float scroll = 0.0F;
    int scroll_to = -1;
};
// The popup menus open, the last on top: those the gui system keeps, then PopupMenus that are
// visible and it hasn't seen yet (gui.cpp).
[[nodiscard]] std::vector<PopupState> popup_states(const world::World& world);

// Gives the world the gui system and its state (gui.cpp).
void install_gui(world::World& world);
// Whether a control has focus and shows it: focus a click gave is hidden, as Godot's.
[[nodiscard]] bool shows_focus(const world::World& world, entt::entity control);
// Whether a button is pressed, as BaseButton::is_pressed: a toggle button that is on, another
// while a press on it is held.
[[nodiscard]] bool is_pressed(const world::World& world, entt::entity button);
// Whether the mouse is over a control or a control inside it, as Godot's mouse_entered says.
[[nodiscard]] bool is_hovered(const world::World& world, entt::entity control);
// Mistakes in buttons, focus, tooltips and drags: shortcuts that aren't bindings or actions,
// groups on buttons that don't toggle, focus fields that name no control, a Label or a second
// button on a button, tooltips the pointer can't reach and drag previews that aren't root
// controls.
void check_buttons(const world::World& world, std::vector<Problem>& problems);

} // namespace slash::ui
