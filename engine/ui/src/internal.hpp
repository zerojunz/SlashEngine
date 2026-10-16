#pragma once

#include "slash/ui/ui.hpp"

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

// Why an icon's path is wrong, or "": not a .png or .bmp file in the project, a built-in icon or
// "" (theme.cpp).
[[nodiscard]] std::string icon_path_error(const std::string& path);

// The control types an entity's components make it, as Godot's classes: "Label",
// "VBoxContainer" for a vertical BoxContainer and so on, the drawing ones first and containers
// last; "Control" when it has only a Control.
[[nodiscard]] std::vector<std::string_view> control_classes(const world::World& world,
                                                            entt::entity entity);

// The Button an entity has: its Button, or its CheckBox or CheckButton, which are buttons too;
// nullptr for none.
[[nodiscard]] const world::Button* button_of(const world::World& world, entt::entity entity);
[[nodiscard]] world::Button* button_of(world::World& world, entt::entity entity);
// Which component button_of found: "Button", "CheckBox" or "CheckButton"; "" for none.
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
// A value as Range::set_value keeps it: rounded to min plus a multiple of step, to a whole
// number if rounded, then kept between min and max less page unless allowed past them.
[[nodiscard]] double range_value(const Range& range, double value);
// How far along its value is, from 0 to 1, as Range::get_as_ratio.
[[nodiscard]] double range_ratio(const Range& range);
// The value a ratio picks, as Range::set_as_ratio.
[[nodiscard]] double range_from_ratio(const Range& range, double ratio);

// Gives the world the gui system and its state (gui.cpp).
void install_gui(world::World& world);
// Whether a control has focus and shows it: focus a click gave is hidden, as Godot's.
[[nodiscard]] bool shows_focus(const world::World& world, entt::entity control);
// Whether a button is pressed, as BaseButton::is_pressed: a toggle button that is on, another
// while a press on it is held.
[[nodiscard]] bool is_pressed(const world::World& world, entt::entity button);
// Whether the mouse is over a control or a control inside it, as Godot's mouse_entered says.
[[nodiscard]] bool is_hovered(const world::World& world, entt::entity control);
// Mistakes in buttons and focus: shortcuts that aren't bindings or actions, groups on buttons
// that don't toggle, focus fields that name no control, a Label or a second button on a button.
void check_buttons(const world::World& world, std::vector<Problem>& problems);

} // namespace slash::ui
