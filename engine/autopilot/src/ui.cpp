// What the player reads and can click on screen, after the Godot addon's lib/ui.gd (Autopilot,
// Apache 2.0): texts of the controls shown, and the buttons, check boxes, option buttons, sliders
// and text fields an agent can work, with where each one's middle is in the window.

#include "slash/ui/ui.hpp"

#include "internal.hpp"

#include "slash/command/session.hpp"
#include "slash/world/components.hpp"
#include "slash/world/world.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace slash::autopilot::detail {

namespace {

// The window's controls as laid out now.
ui::Layout layout_of(const world::World& world) {
    text::Fonts* fonts = ui::fonts(world);
    SLASH_ASSERT(fonts != nullptr, "sessions install ui");
    return ui::layout(world, *fonts, ui::settings(world).viewport);
}

// Whether a control shows: visible, not cut away by a clip and at least partly in the window.
bool shows(const ui::Placed& placed, Vec2 viewport) {
    if (!placed.visible) {
        return false;
    }
    const Vec2 middle = placed.transform.apply(placed.size * 0.5F);
    if (placed.clip) {
        const Vec4& clip = *placed.clip;
        if (middle.x < clip.x || middle.y < clip.y || middle.x > clip.z || middle.y > clip.w) {
            return false;
        }
    }
    return middle.x >= 0.0F && middle.y >= 0.0F && middle.x <= viewport.x && middle.y <= viewport.y;
}

// The layer a control draws in, as the addon names a control's owner: its CanvasLayer's ID, or
// "Screen".
std::string owner_of(const world::World& world, const ui::Layout& layout,
                     const ui::Placed& placed) {
    if (placed.canvas < layout.canvases.size() &&
        layout.canvases[placed.canvas].layer != entt::null) {
        if (const std::string_view id = world.id_of(layout.canvases[placed.canvas].layer);
            !id.empty()) {
            return std::string(id);
        }
    }
    return "Screen";
}

// The lines a label shows, joined with newlines.
std::string shown_text(const ui::LabelText& label) {
    std::string out;
    const std::size_t last = std::min(label.last, label.gutters_from);
    for (std::size_t i = label.first; i < last && i < label.text.lines.size(); ++i) {
        const text::Line& line = label.text.lines[i];
        if (!out.empty()) {
            out += '\n';
        }
        out += label.source.substr(line.start, line.end - line.start);
    }
    return out;
}

const world::Button* button_of(const world::World& world, entt::entity entity) {
    const auto& ecs = world.ecs();
    if (const auto* b = ecs.try_get<world::Button>(entity); b != nullptr) {
        return b;
    }
    if (const auto* b = ecs.try_get<world::CheckBox>(entity); b != nullptr) {
        return b;
    }
    if (const auto* b = ecs.try_get<world::CheckButton>(entity); b != nullptr) {
        return b;
    }
    if (const auto* b = ecs.try_get<world::OptionButton>(entity); b != nullptr) {
        return b;
    }
    if (const auto* b = ecs.try_get<world::MenuButton>(entity); b != nullptr) {
        return b;
    }
    return nullptr;
}

} // namespace

json::Value screen(command::Session& session, const json::Value& args) {
    const world::World& world = session.world();
    const ui::Layout layout = layout_of(world);
    const auto& ecs = world.ecs();
    json::Value texts = json::Object{};
    for (const ui::Placed& placed : layout.controls) {
        const std::string_view id = world.id_of(placed.entity);
        if (id.empty() || !shows(placed, layout.viewport)) {
            continue;
        }
        std::string text;
        if (const ui::LabelText* label = layout.label(placed.entity); label != nullptr) {
            text = shown_text(*label);
        } else if (const auto percent = layout.percentages.find(placed.entity);
                   percent != layout.percentages.end()) {
            text = percent->second.source;
        } else if (const auto* edit = ecs.try_get<world::LineEdit>(placed.entity);
                   edit != nullptr) {
            text = edit->text;
        } else if (const auto* area = ecs.try_get<world::TextEdit>(placed.entity);
                   area != nullptr) {
            text = area->text;
        }
        if (text.empty()) {
            continue;
        }
        texts[owner_of(world, layout, placed) + "." + std::string(id)] = std::move(text);
    }
    // The items of open popup menus read as the menu's texts.
    for (const ui::PopupView& popup : layout.popups) {
        const std::string_view id = world.id_of(popup.owner);
        for (std::size_t i = 0; i < popup.items.size(); ++i) {
            if (!popup.items[i].text.empty()) {
                texts["Popup." + std::string(id) + "." + std::to_string(i)] = popup.items[i].text;
            }
        }
    }
    json::Value out;
    out["texts"] = std::move(texts);
    if (const json::Value* with_ui = args.find("ui");
        with_ui != nullptr && with_ui->is_bool() && with_ui->as_bool()) {
        out["ui"] = ui_list(session);
    }
    return out;
}

json::Array ui_list(command::Session& session) {
    const world::World& world = session.world();
    const ui::Layout layout = layout_of(world);
    const auto& ecs = world.ecs();
    json::Array elements;
    for (const ui::Placed& placed : layout.controls) {
        const entt::entity e = placed.entity;
        const std::string_view id = world.id_of(e);
        if (id.empty() || !shows(placed, layout.viewport)) {
            continue;
        }
        json::Value item;
        std::string label;
        bool enabled = true;
        if (const auto* option = ecs.try_get<world::OptionButton>(e); option != nullptr) {
            item["kind"] = "combo";
            json::Array options;
            for (const world::OptionButtonItem& entry : option->items) {
                if (!entry.separator) {
                    options.emplace_back(entry.text);
                }
            }
            item["options"] = std::move(options);
            if (option->selected >= 0 &&
                static_cast<std::size_t>(option->selected) < option->items.size()) {
                item["selected"] = option->items[static_cast<std::size_t>(option->selected)].text;
            }
            label = option->text;
            enabled = !option->disabled;
        } else if (ecs.any_of<world::CheckBox, world::CheckButton>(e)) {
            const world::Button* button = button_of(world, e);
            item["kind"] = "checkbox";
            item["checked"] = button->button_pressed;
            label = button->text;
            enabled = !button->disabled;
        } else if (const world::Button* button = button_of(world, e); button != nullptr) {
            // A toggle button reads as a check box, as the addon's kind_of.
            const bool toggle = button->toggle_mode && !ecs.all_of<world::MenuButton>(e);
            item["kind"] = toggle ? "checkbox" : "button";
            if (toggle) {
                item["checked"] = button->button_pressed;
            }
            label = button->text;
            enabled = !button->disabled;
        } else if (const auto* texture = ecs.try_get<world::TextureButton>(e); texture != nullptr) {
            item["kind"] = texture->toggle_mode ? "checkbox" : "button";
            if (texture->toggle_mode) {
                item["checked"] = texture->button_pressed;
            }
            enabled = !texture->disabled;
        } else if (const auto* slider = ecs.try_get<world::Slider>(e); slider != nullptr) {
            item["kind"] = "slider";
            item["value"] = r2(slider->value);
            item["min"] = r2(slider->min_value);
            item["max"] = r2(slider->max_value);
            enabled = slider->editable;
        } else if (const auto* edit = ecs.try_get<world::LineEdit>(e); edit != nullptr) {
            item["kind"] = "text_input";
            item["value"] = edit->text;
            if (!edit->placeholder_text.empty()) {
                item["placeholder"] = edit->placeholder_text;
            }
            label = edit->placeholder_text;
            enabled = edit->editable;
        } else if (const auto* area = ecs.try_get<world::TextEdit>(e); area != nullptr) {
            item["kind"] = "text_input";
            item["value"] = area->text;
            if (!area->placeholder_text.empty()) {
                item["placeholder"] = area->placeholder_text;
            }
            label = area->placeholder_text;
            enabled = area->editable;
        } else {
            continue;
        }
        if (label.empty()) {
            if (const auto* control = ecs.try_get<world::Control>(e);
                control != nullptr && !control->accessibility_name.empty()) {
                label = control->accessibility_name;
            } else {
                label = std::string(id);
            }
        }
        const Vec2 middle = placed.transform.apply(placed.size * 0.5F);
        item["id"] = id;
        item["name"] =
            std::string(id.substr(id.rfind('/') == std::string_view::npos ? 0 : id.rfind('/') + 1));
        item["owner"] = owner_of(world, layout, placed);
        item["label"] = std::move(label);
        item["enabled"] = enabled;
        item["path"] = id;
        item["class"] = std::string(ui::control_class(world, e));
        json::Value at;
        at["x"] = static_cast<std::int64_t>(std::lround(middle.x));
        at["y"] = static_cast<std::int64_t>(std::lround(middle.y));
        item["screen"] = std::move(at);
        elements.push_back(std::move(item));
    }
    return elements;
}

std::optional<Vec2> control_center(command::Session& session, entt::entity control) {
    const world::World& world = session.world();
    const ui::Layout layout = layout_of(world);
    const ui::Placed* placed = layout.find(control);
    if (placed == nullptr || !shows(*placed, layout.viewport)) {
        return std::nullopt;
    }
    return placed->transform.apply(placed->size * 0.5F);
}

} // namespace slash::autopilot::detail
