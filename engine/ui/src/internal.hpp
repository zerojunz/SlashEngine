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

// The control types an entity's components make it, as Godot's classes: "Label",
// "VBoxContainer" for a vertical BoxContainer and so on, the drawing ones first and containers
// last; "Control" when it has only a Control.
[[nodiscard]] std::vector<std::string_view> control_classes(const world::World& world,
                                                            entt::entity entity);

} // namespace slash::ui
