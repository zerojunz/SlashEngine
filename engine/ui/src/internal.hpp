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

// The control types an entity's components make it, as Godot's classes: "Label",
// "VBoxContainer" for a vertical BoxContainer and so on, the drawing ones first and containers
// last; "Control" when it has only a Control.
[[nodiscard]] std::vector<std::string_view> control_classes(const world::World& world,
                                                            entt::entity entity);

// Gives the world the gui system and its state (gui.cpp).
void install_gui(world::World& world);
// Whether a control has focus and shows it: focus a click gave is hidden, as Godot's.
[[nodiscard]] bool shows_focus(const world::World& world, entt::entity control);
// Mistakes in buttons and focus: shortcuts that aren't bindings or actions, groups on buttons
// that don't toggle, focus fields that name no control, a Label or a second button on a button.
void check_buttons(const world::World& world, std::vector<Problem>& problems);

} // namespace slash::ui
