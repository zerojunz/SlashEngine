#pragma once

#include "slash/core/json.hpp"
#include "slash/core/math.hpp"
#include "slash/core/result.hpp"
#include "slash/ui/ui.hpp"
#include "slash/world/world.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The accessibility tree, as Godot 4.5's controls describe themselves to a screen reader in
// NOTIFICATION_ACCESSIBILITY_UPDATE: each control's role, name, value, states and the actions it
// takes, in the controls' tree. A platform's accessibility backend hands it to the system's
// screen reader; agents read it to check a game can be played without seeing it.
//
//   const ui::Layout placed = ui::layout(world, *ui::fonts(world), viewport);
//   const ui::AccessTree tree = ui::accessibility_tree(world, placed);
//   ui::accessibility_action(world, button, "click");
namespace slash::ui {

// Whether a world's controls serve a screen reader, as Godot's
// SceneTree::is_accessibility_enabled: Settings::accessibility_support is "always", or "auto"
// while a screen reader is active. Serving one, focus_mode "accessibility" takes focus.
[[nodiscard]] bool accessibility_enabled(const world::World& world);

// One element of the tree: a control, an open popup menu or one of its items.
struct AccessElement {
    // The control; for a menu, the PopupMenu, OptionButton or MenuButton whose menu it is, and
    // for an item, that menu's.
    entt::entity entity = entt::null;
    bool menu = false; // an open popup menu (role "menu")
    int item = -1;     // a menu item's index; -1 for the rest
    // Godot's AccessibilityRole in lower case without ROLE_: "container", "button",
    // "check_box", "radio_button", "check_button", "static_text", "text_field",
    // "multiline_text_field", "slider", "scroll_bar", "progress_indicator", "scroll_view",
    // "panel", "video", "menu", "menu_item", "menu_item_check_box" or "menu_item_radio".
    std::string role = "container";
    std::string name;
    std::string description;
    std::string value; // a Label's text or a text field's (a secret one's as its characters)
    std::string placeholder;
    std::string tooltip;
    std::string live = "off";                    // "off", "polite" or "assertive"
    std::string popup_type;                      // "list" or "menu" for a button that opens one
    std::optional<bool> checked;                 // a toggle button's or check item's state
    std::optional<std::array<double, 4>> range;  // a Range's [value, min, max, step]
    std::optional<Vec4> color;                   // a ColorRect's
    std::optional<std::array<int, 2>> selection; // a text field's, [from, to] in characters
    Vec4 rect;                                   // its bounds in the viewport, [x, y, w, h]
    bool selected = false;                       // a menu item hovered or moved to
    // Its AccessibilityFlags that are set: "hidden", "disabled", "readonly", "clips_children"
    // and "touch_passthrough".
    std::vector<std::string_view> flags;
    // The actions it takes besides those every control takes (kCommonActions).
    std::vector<std::string_view> actions;
    std::vector<std::size_t> children; // elements in the tree, in order
    // Related controls, as accessibility_update_add_related_*.
    std::vector<entt::entity> controls;
    std::vector<entt::entity> described_by;
    std::vector<entt::entity> labeled_by;
    std::vector<entt::entity> flow_to;
    std::vector<entt::entity> radio_group; // the other buttons in its button_group
};

// The actions every control takes, as Godot's Control adds them.
inline constexpr std::array<std::string_view, 5> kCommonActions{"focus", "blur", "show_tooltip",
                                                                "hide_tooltip", "scroll_into_view"};

struct AccessTree {
    std::vector<AccessElement> elements;
    std::vector<std::size_t> roots;   // root controls, then the PopupMenus open
    std::optional<std::size_t> focus; // the element with focus: the focus owner, or a menu's item
};

// The accessibility tree of a world's controls as laid out. Controls hidden in the tree (their
// own or a parent's visible off) are left out unless hidden is set; open popup menus are
// children of the buttons that opened them, or roots for PopupMenus.
[[nodiscard]] AccessTree accessibility_tree(const world::World& world, const Layout& layout,
                                            bool hidden = false);

// Does what a screen reader asks of a control (or of an open popup menu's item, given its
// index), as the callables Godot's controls add: "click" (a button presses at the next tick, as
// ui::press; an item is picked, as ui::select_item), "focus", "blur", "show_tooltip",
// "hide_tooltip", "scroll_into_view", "increment" and "decrement" (a Range by its step),
// "set_value" (a Range's number, or a text field's text), "scroll_up", "scroll_down",
// "scroll_left" and "scroll_right" (a ScrollContainer by a page, or an eighth of one with data
// "item") and "set_scroll_offset" (data [x, y]). Fails when the element doesn't take it.
Result<void> accessibility_action(world::World& world, entt::entity entity, std::string_view action,
                                  const json::Value& data = {}, int item = -1);

// What a screen reader would struggle with, as Godot's accessibility configuration warnings:
// a control that can take focus with no name, an accessibility_name holding the control's class
// name or a control character, and related nodes that aren't controls.
[[nodiscard]] std::vector<Problem> accessibility_problems(const world::World& world,
                                                          const AccessTree& tree);

} // namespace slash::ui
