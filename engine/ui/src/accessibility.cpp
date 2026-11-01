#include "slash/ui/accessibility.hpp"

#include "internal.hpp"

#include "slash/l10n/l10n.hpp"
#include "slash/world/components.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace slash::ui {
namespace {

// Text as a control shows it, as Godot's Node::atr.
std::string shown(const world::World& world, bool translate, const std::string& text) {
    return translate && !text.empty() ? l10n::tr(world, text) : text;
}

// A popup menu item's text as it shows, as PopupMenu::_atr.
std::string item_shown(const world::World& world, bool owner, const std::string& mode,
                       const std::string& text) {
    const bool translate = mode == "inherit" ? owner : mode == "always";
    return shown(world, translate, text);
}

// The bounds of a rect moved by a transform, as [x, y, w, h].
Vec4 bounds(const Transform& transform, Vec2 size) {
    const std::array<Vec2, 4> corners{transform.apply({0.0F, 0.0F}),
                                      transform.apply({size.x, 0.0F}),
                                      transform.apply({0.0F, size.y}), transform.apply(size)};
    Vec2 low = corners[0];
    Vec2 high = corners[0];
    for (const Vec2 corner : corners) {
        low = {std::min(low.x, corner.x), std::min(low.y, corner.y)};
        high = {std::max(high.x, corner.x), std::max(high.y, corner.y)};
    }
    return {low.x, low.y, high.x - low.x, high.y - low.y};
}

// How many code points UTF-8 text holds.
std::size_t code_points(std::string_view text) {
    return static_cast<std::size_t>(std::ranges::count_if(
        text, [](char c) { return (static_cast<unsigned char>(c) & 0xC0U) != 0x80U; }));
}

// The fields BaseButton's NOTIFICATION_ACCESSIBILITY_UPDATE reads.
struct ButtonFields {
    std::string button_group;
    bool disabled = false;
    bool toggle_mode = false;
    bool pressed = false;
};

std::optional<ButtonFields> base_button(const world::World& world, entt::entity entity) {
    if (const world::Button* button = button_of(world, entity); button != nullptr) {
        return ButtonFields{.button_group = button->button_group,
                            .disabled = button->disabled,
                            .toggle_mode = button->toggle_mode,
                            .pressed = button->button_pressed};
    }
    if (const auto* button = world.ecs().try_get<world::TextureButton>(entity); button != nullptr) {
        return ButtonFields{.button_group = button->button_group,
                            .disabled = button->disabled,
                            .toggle_mode = button->toggle_mode,
                            .pressed = button->button_pressed};
    }
    return std::nullopt;
}

class Builder {
public:
    Builder(const world::World& world, const Layout& layout, bool hidden)
        : world_(world), layout_(layout), hidden_(hidden) {}

    AccessTree build() {
        // Each control, parents before children and children in order, as Node adds them.
        for (const entt::entity entity : layout_.tree) {
            const Placed* placed = layout_.find(entity);
            if (placed == nullptr || (!placed->visible && !hidden_)) {
                continue;
            }
            const std::size_t index = add(control(entity, *placed));
            made_[entity] = index;
            const auto parent = made_.find(placed->parent);
            if (placed->parent != entt::null && parent != made_.end()) {
                tree_.elements[parent->second].children.push_back(index);
            } else {
                tree_.roots.push_back(index);
            }
        }
        // The popup menus open, as windows: a button's under it, a PopupMenu's at the root.
        for (const PopupView& popup : layout_.popups) {
            const std::size_t index = menu(popup);
            const auto owner = made_.find(popup.owner);
            if (!world_.ecs().all_of<world::PopupMenu>(popup.owner) && owner != made_.end()) {
                tree_.elements[owner->second].children.push_back(index);
            } else {
                tree_.roots.push_back(index);
            }
        }
        find_focus();
        return std::move(tree_);
    }

private:
    std::size_t add(AccessElement element) {
        tree_.elements.push_back(std::move(element));
        return tree_.elements.size() - 1;
    }

    // Node, CanvasItem, Control, then the control's own class, each in its turn, as Godot sends
    // NOTIFICATION_ACCESSIBILITY_UPDATE from the base class down.
    AccessElement control(entt::entity entity, const Placed& placed) {
        AccessElement out;
        out.entity = entity;
        out.rect = bounds(placed.transform, placed.size);
        const auto* control = world_.ecs().try_get<world::Control>(entity);
        const world::Control fallback;
        const world::Control& fields = control != nullptr ? *control : fallback;
        // CanvasItem.
        if (!fields.visible) {
            out.flags.emplace_back("hidden");
        }
        // Control.
        out.name = shown(world_, placed.translate, fields.accessibility_name);
        out.description = shown(world_, placed.translate, fields.accessibility_description);
        out.live = fields.accessibility_live;
        out.tooltip = fields.tooltip_text;
        if (placed.clips) {
            out.flags.emplace_back("clips_children");
        }
        if (mouse_filter_name(world_, entity) == "pass") {
            out.flags.emplace_back("touch_passthrough");
        }
        const auto related = [&](const std::vector<entt::entity>& nodes,
                                 std::vector<entt::entity>& into) {
            for (const entt::entity node : nodes) {
                if (world_.valid(node)) {
                    into.push_back(node);
                }
            }
        };
        related(fields.accessibility_controls_nodes, out.controls);
        related(fields.accessibility_described_by_nodes, out.described_by);
        related(fields.accessibility_labeled_by_nodes, out.labeled_by);
        related(fields.accessibility_flow_to_nodes, out.flow_to);
        // The control's own class.
        const std::vector<std::string_view> classes = control_classes(world_, entity);
        const std::string_view type = classes.empty() ? "Control" : classes.front();
        if (const std::optional<ButtonFields> button = base_button(world_, entity)) {
            base_button_update(out, *button);
            button_update(out, entity, placed, type);
        } else if (type == "Label") {
            out.role = "static_text";
            out.value =
                shown(world_, placed.translate, world_.ecs().get<world::Label>(entity).text);
        } else if (type == "LineEdit") {
            line_edit_update(out, entity, placed);
        } else if (type == "TextEdit" || type == "CodeEdit") {
            text_edit_update(out, entity, placed);
        } else if (type == "ProgressBar") {
            const auto& bar = world_.ecs().get<world::ProgressBar>(entity);
            range_update(out, {.min = bar.min_value,
                               .max = std::max(bar.max_value, bar.min_value),
                               .step = bar.step,
                               .page = 0.0,
                               .value = bar.value,
                               .rounded = false,
                               .allow_greater = bar.allow_greater,
                               .allow_lesser = bar.allow_lesser});
            out.role = "progress_indicator";
        } else if (type == "HSlider" || type == "VSlider") {
            range_update(out, range_of(world_.ecs().get<world::Slider>(entity)));
            out.role = "slider";
        } else if (type == "HScrollBar" || type == "VScrollBar") {
            range_update(out, range_of(world_.ecs().get<world::ScrollBar>(entity)));
            out.role = "scroll_bar";
        } else if (type == "ScrollContainer") {
            out.role = "scroll_view";
            out.actions = {"scroll_down", "scroll_left", "scroll_right", "scroll_up",
                           "set_scroll_offset"};
        } else if (type == "Panel") {
            out.role = "panel";
        } else if (type == "ColorRect") {
            out.color = world_.ecs().get<world::ColorRect>(entity).color;
        } else if (type == "VideoStreamPlayer") {
            out.role = "video";
        }
        return out;
    }

    // BaseButton.
    void base_button_update(AccessElement& out, const ButtonFields& button) {
        out.role = "button";
        out.actions = {"click"};
        if (button.disabled) {
            out.flags.emplace_back("disabled");
        }
        if (button.toggle_mode) {
            out.checked = button.pressed;
        }
        if (!button.button_group.empty()) {
            for (const entt::entity other : layout_.tree) {
                const std::optional<ButtonFields> fields = base_button(world_, other);
                if (fields && fields->button_group == button.button_group) {
                    out.radio_group.push_back(other);
                }
            }
        }
    }

    // Button, then CheckBox, CheckButton, OptionButton and MenuButton.
    void button_update(AccessElement& out, entt::entity entity, const Placed& placed,
                       std::string_view type) {
        const auto& ecs = world_.ecs();
        const world::Button* button = button_of(world_, entity);
        if (button == nullptr) {
            return; // a TextureButton is only a BaseButton
        }
        std::string text = shown(world_, placed.translate, button->text);
        if (const auto* option = ecs.try_get<world::OptionButton>(entity); option != nullptr) {
            // OptionButton shows the selected option's text.
            text.clear();
            if (option->selected >= 0 &&
                static_cast<std::size_t>(option->selected) < option->items.size()) {
                const world::OptionButtonItem& item =
                    option->items[static_cast<std::size_t>(option->selected)];
                text = item_shown(world_, placed.translate, item.auto_translate_mode, item.text);
            }
        }
        if (!text.empty() && out.name.empty()) {
            out.name = text;
        } else if (!text.empty() && out.name != text) {
            out.name += ": " + text;
        } else if (text.empty() && out.name.empty() && !out.tooltip.empty()) {
            out.name = out.tooltip; // fall back to the tooltip
        }
        if (type == "CheckBox") {
            out.role = button->button_group.empty() ? "check_box" : "radio_button";
        } else if (type == "CheckButton") {
            out.role = "check_button";
        } else if (type == "OptionButton") {
            out.popup_type = "list";
        } else if (type == "MenuButton") {
            out.popup_type = "menu";
        }
    }

    void line_edit_update(AccessElement& out, entt::entity entity, const Placed& placed) {
        const auto& line = world_.ecs().get<world::LineEdit>(entity);
        out.role = "text_field";
        const std::string placeholder = shown(world_, placed.translate, line.placeholder_text);
        if (line.text.empty() && ime_text(world_, entity).empty() && !placeholder.empty()) {
            out.placeholder = placeholder;
        }
        if (!placeholder.empty() && out.name.empty()) {
            out.name = placeholder;
        }
        if (!line.editable) {
            out.flags.emplace_back("readonly");
        }
        out.actions = {"set_value"};
        if (line.secret) {
            out.value.clear();
            for (std::size_t i = code_points(line.text); i > 0; --i) {
                out.value += line.secret_character.substr(0, 4);
            }
        } else {
            out.value = line.text;
        }
        if (const auto selected = selection(world_, entity)) {
            out.selection = std::array<int, 2>{selected->first, selected->second};
        } else {
            out.selection = std::array<int, 2>{line.caret_column, line.caret_column};
        }
    }

    void text_edit_update(AccessElement& out, entt::entity entity, const Placed& placed) {
        const auto& edit = world_.ecs().get<world::TextEdit>(entity);
        out.role = "multiline_text_field";
        const std::string placeholder = shown(world_, placed.translate, edit.placeholder_text);
        if (edit.text.empty()) {
            out.placeholder = placeholder;
        }
        if (!placeholder.empty() && out.name.empty()) {
            out.name = placeholder;
        }
        if (!edit.editable) {
            out.flags.emplace_back("readonly");
        }
        out.actions = {"set_value"};
        out.value = edit.text;
    }

    // Range.
    static void range_update(AccessElement& out, const Range& range) {
        out.range = std::array<double, 4>{range_value(range, range.value), range.min, range.max,
                                          range.step > 0.0 ? range.step : 1.0};
        out.actions = {"decrement", "increment", "set_value"};
    }

    // PopupMenu: the menu, then an element per item.
    std::size_t menu(const PopupView& popup) {
        AccessElement out;
        out.entity = popup.owner;
        out.menu = true;
        out.role = "menu";
        out.rect = {popup.position.x, popup.position.y, popup.size.x, popup.size.y};
        const bool standalone = world_.ecs().all_of<world::PopupMenu>(popup.owner);
        if (standalone) {
            out.name = world_.id_of(popup.owner); // Node's name
        }
        const std::size_t index = add(std::move(out));
        menus_[popup.owner] = index;
        const std::vector<world::PopupMenuItem> items = popup_items(world_, popup.owner);
        const Placed* owner = layout_.find(popup.owner);
        const bool translate = owner == nullptr || owner->translate;
        for (std::size_t i = 0; i < items.size(); ++i) {
            const world::PopupMenuItem& item = items[i];
            AccessElement element;
            element.entity = popup.owner;
            element.item = static_cast<int>(i);
            element.role = "menu_item";
            if (item.checkable == "check" || item.checkable == "radio") {
                element.role =
                    item.checkable == "check" ? "menu_item_check_box" : "menu_item_radio";
                element.checked = item.checked;
            }
            element.actions = {"click"};
            element.selected = static_cast<int>(i) == popup.focused;
            element.name = item_shown(world_, translate, item.auto_translate_mode, item.text);
            if (item.disabled) {
                element.flags.emplace_back("disabled");
            }
            element.tooltip = item.tooltip;
            if (i < popup.items.size()) {
                const PopupItemView& view = popup.items[i];
                element.rect = {popup.position.x + popup.list_position.x,
                                popup.position.y + popup.list_position.y + view.y - popup.scroll,
                                popup.list_size.x,
                                view.height + static_cast<float>(popup.v_separation)};
            }
            const std::size_t at = add(std::move(element));
            tree_.elements[index].children.push_back(at);
        }
        return index;
    }

    // SceneTree::_process_accessibility_changes: the popup open on top has focus, on its item
    // hovered or moved to; else the control with focus.
    void find_focus() {
        if (!layout_.popups.empty()) {
            const PopupView& top = layout_.popups.back();
            const std::size_t menu = menus_.at(top.owner);
            tree_.focus = menu;
            if (top.focused >= 0 &&
                static_cast<std::size_t>(top.focused) < tree_.elements[menu].children.size()) {
                tree_.focus = tree_.elements[menu].children[static_cast<std::size_t>(top.focused)];
            }
            return;
        }
        if (const auto found = made_.find(focus_owner(world_)); found != made_.end()) {
            tree_.focus = found->second;
        }
    }

    const world::World& world_;
    const Layout& layout_;
    bool hidden_ = false;
    AccessTree tree_;
    std::unordered_map<entt::entity, std::size_t> made_;
    std::unordered_map<entt::entity, std::size_t> menus_;
};

// The Range a control is, as Godot's Range: a Slider's, ScrollBar's or ProgressBar's.
std::optional<Range> range_at(const world::World& world, entt::entity entity) {
    const auto& ecs = world.ecs();
    if (const auto* slider = ecs.try_get<world::Slider>(entity); slider != nullptr) {
        return range_of(*slider);
    }
    if (const auto* bar = ecs.try_get<world::ScrollBar>(entity); bar != nullptr) {
        return range_of(*bar);
    }
    if (const auto* bar = ecs.try_get<world::ProgressBar>(entity); bar != nullptr) {
        Range range{.min = bar->min_value,
                    .max = std::max(bar->max_value, bar->min_value),
                    .step = bar->step,
                    .page = 0.0,
                    .value = 0.0,
                    .rounded = false,
                    .allow_greater = bar->allow_greater,
                    .allow_lesser = bar->allow_lesser};
        range.value = range_value(range, bar->value);
        return range;
    }
    return std::nullopt;
}

// Range::set_value: kept in range, then written to the component.
void set_range(world::World& world, entt::entity entity, const Range& range, double value) {
    auto& ecs = world.ecs();
    const double kept = range_value(range, value);
    if (auto* slider = ecs.try_get<world::Slider>(entity); slider != nullptr) {
        slider->value = kept;
    } else if (auto* bar = ecs.try_get<world::ScrollBar>(entity); bar != nullptr) {
        bar->value = kept;
    } else if (auto* progress = ecs.try_get<world::ProgressBar>(entity); progress != nullptr) {
        progress->value = kept;
    }
}

Error no_action(std::string_view action, std::string_view what) {
    return Error{ErrorCode::InvalidArgument,
                 std::string(what) + " doesn't take \"" + std::string(action) + "\""};
}

// A ScrollContainer's scroll actions, as ScrollContainer::_accessibility_action_scroll_*.
Result<void> scroll_action(world::World& world, entt::entity entity, std::string_view action,
                           const json::Value& data) {
    text::Fonts* found = fonts(world);
    if (found == nullptr) {
        return Error{ErrorCode::InvalidArgument, "the world has no UI installed"};
    }
    const Layout laid = layout(world, *found, settings(world).viewport);
    const auto view = laid.scrolls.find(entity);
    if (view == laid.scrolls.end()) {
        return Error{ErrorCode::InvalidArgument,
                     "the ScrollContainer isn't laid out: it is hidden"};
    }
    std::array<std::optional<double>, 2> to;
    if (action == "set_scroll_offset") {
        if (!data.is_array() || data.size() != 2 || !data.as_array()[0].is_number() ||
            !data.as_array()[1].is_number()) {
            return Error{ErrorCode::InvalidArgument, "set_scroll_offset takes data [x, y]"};
        }
        to = {data.as_array()[0].as_double(), data.as_array()[1].as_double()};
    } else {
        // A page, or an eighth of one (ScrollBar::PAGE_DIVISOR) for the item unit.
        const bool item = data.is_string() && data.as_string() == "item";
        const bool vertical = action == "scroll_up" || action == "scroll_down";
        const ScrollBarView& bar = vertical ? view->second.v : view->second.h;
        const double step = item ? bar.page / 8.0 : bar.page;
        const double sign = action == "scroll_up" || action == "scroll_left" ? -1.0 : 1.0;
        to.at(vertical ? 1 : 0) = bar.value + (sign * step);
    }
    if (!scroll_container_to(world, entity, to)) {
        return Error{ErrorCode::InvalidArgument, "the ScrollContainer has no entity ID"};
    }
    return {};
}

} // namespace

AccessTree accessibility_tree(const world::World& world, const Layout& layout, bool hidden) {
    return Builder(world, layout, hidden).build();
}

Result<void> accessibility_action(world::World& world, entt::entity entity, std::string_view action,
                                  const json::Value& data, int item) {
    if (!world.valid(entity)) {
        return Error{ErrorCode::InvalidArgument, "no such entity"};
    }
    if (item >= 0) {
        // PopupMenu::_accessibility_action_click.
        if (action != "click") {
            return no_action(action, "a menu item");
        }
        if (!select_item(world, entity, item)) {
            return Error{ErrorCode::InvalidArgument,
                         "the item can't be picked: it doesn't exist, is a separator or is "
                         "disabled"};
        }
        return {};
    }
    if (!is_control(world, entity)) {
        return Error{ErrorCode::InvalidArgument, "it isn't a control"};
    }
    if (action == "focus") {
        if (!grab_focus(world, entity)) {
            return Error{ErrorCode::InvalidArgument,
                         "it can't take focus: it is hidden, has no ID or its focus_mode is "
                         "none (or accessibility while no screen reader is served)"};
        }
        return {};
    }
    if (action == "blur") {
        if (focus_owner(world) == entity) {
            release_focus(world);
        }
        return {};
    }
    if (action == "show_tooltip") {
        show_tooltip(world, entity);
        return {};
    }
    if (action == "hide_tooltip") {
        cancel_tooltip(world);
        return {};
    }
    if (action == "scroll_into_view") {
        (void)scroll_into_view(world, entity);
        return {};
    }
    auto& ecs = world.ecs();
    if (action == "click") {
        const std::optional<ButtonFields> button = base_button(world, entity);
        if (!button) {
            return no_action(action, "a control that isn't a button");
        }
        if (button->disabled) {
            return Error{ErrorCode::InvalidArgument, "the button is disabled"};
        }
        press(world, entity);
        return {};
    }
    if (action == "increment" || action == "decrement" ||
        (action == "set_value" && data.is_number())) {
        const std::optional<Range> range = range_at(world, entity);
        if (!range) {
            return no_action(action, "a control that isn't a Range");
        }
        // Range::_accessibility_action_inc and _dec step by the step, or 1 without one.
        const double step = range->step > 0.0 ? range->step : 1.0;
        double value = 0.0;
        if (action == "set_value") {
            value = data.as_double();
        } else {
            value = range->value + (action == "increment" ? step : -step);
        }
        set_range(world, entity, *range, value);
        return {};
    }
    if (action == "set_value") {
        // LineEdit::_accessibility_action_set_value and TextEdit's: set_text.
        if (!data.is_string()) {
            return Error{ErrorCode::InvalidArgument,
                         "set_value takes a number for a Range or text for a text field"};
        }
        if (auto* line = ecs.try_get<world::LineEdit>(entity); line != nullptr) {
            line->text = data.as_string();
            line->caret_column = 0; // as LineEdit::set_text leaves it
            return {};
        }
        if (auto* edit = ecs.try_get<world::TextEdit>(entity); edit != nullptr) {
            edit->text = data.as_string();
            edit->caret_line = 0; // as TextEdit::set_text leaves it
            edit->caret_column = 0;
            return {};
        }
        return no_action(action, "a control that isn't a Range or text field");
    }
    if (action == "scroll_up" || action == "scroll_down" || action == "scroll_left" ||
        action == "scroll_right" || action == "set_scroll_offset") {
        if (!ecs.all_of<world::ScrollContainer>(entity)) {
            return no_action(action, "a control that isn't a ScrollContainer");
        }
        return scroll_action(world, entity, action, data);
    }
    return Error{ErrorCode::InvalidArgument, "unknown action \"" + std::string(action) + "\""};
}

std::vector<Problem> accessibility_problems(const world::World& world, const AccessTree& tree) {
    std::vector<Problem> problems;
    for (const AccessElement& element : tree.elements) {
        if (element.menu || element.item >= 0) {
            continue;
        }
        const std::string id(world.id_of(element.entity));
        if (id.empty()) {
            continue;
        }
        const auto* control = world.ecs().try_get<world::Control>(element.entity);
        const std::string name = control != nullptr ? control->accessibility_name : std::string();
        // Control::get_accessibility_configuration_warnings, on the name as set.
        if (!name.empty()) {
            for (const std::string_view type : control_classes(world, element.entity)) {
                if (name.find(type) != std::string::npos) {
                    problems.push_back({id, "/components/Control/accessibility_name",
                                        "accessibility_name must not include the control's "
                                        "class name, \"" +
                                            std::string(type) + "\""});
                    break;
                }
            }
            if (std::ranges::any_of(name, [](char c) {
                    return static_cast<unsigned char>(c) < 0x20U || c == 0x7F;
                })) {
                problems.push_back({id, "/components/Control/accessibility_name",
                                    "accessibility_name must not include a control character"});
            }
        }
        // A control a screen reader stops at with nothing to call it.
        const std::string_view focus = focus_mode_name(world, element.entity);
        std::string trimmed = element.name;
        std::erase_if(trimmed, [](char c) { return c == ' ' || c == '\t' || c == '\n'; });
        if (focus != "none" && trimmed.empty() && element.role != "static_text") {
            problems.push_back({id, "/components/Control/accessibility_name",
                                "a screen reader has no name for this " + element.role +
                                    ": give it text, accessibility_name or tooltip_text"});
        }
        if (control != nullptr) {
            const auto check = [&](const std::vector<entt::entity>& nodes, std::string_view field) {
                for (std::size_t i = 0; i < nodes.size(); ++i) {
                    if (!is_control(world, nodes[i])) {
                        problems.push_back(
                            {id,
                             "/components/Control/" + std::string(field) + "/" + std::to_string(i),
                             "isn't a control"});
                    }
                }
            };
            check(control->accessibility_controls_nodes, "accessibility_controls_nodes");
            check(control->accessibility_described_by_nodes, "accessibility_described_by_nodes");
            check(control->accessibility_labeled_by_nodes, "accessibility_labeled_by_nodes");
            check(control->accessibility_flow_to_nodes, "accessibility_flow_to_nodes");
        }
    }
    return problems;
}

} // namespace slash::ui
