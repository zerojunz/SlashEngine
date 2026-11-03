#include "slash/accessibility/web.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <unordered_map>

namespace slash::accessibility {
namespace {

// Godot's roles (ui::AccessElement::role) as WAI-ARIA's, as a browser's accessibility tree maps
// AccessKit's to its platform's; the window is the application a game's keys go to.
std::string_view aria_role(std::string_view role) {
    constexpr std::array<std::pair<std::string_view, std::string_view>, 19> kRoles{{
        {"window", "application"},
        {"container", ""},
        {"button", "button"},
        {"check_box", "checkbox"},
        {"radio_button", "radio"},
        {"check_button", "switch"},
        {"static_text", ""},
        {"text_field", "textbox"},
        {"multiline_text_field", "textbox"},
        {"slider", "slider"},
        {"scroll_bar", "scrollbar"},
        {"progress_indicator", "progressbar"},
        {"scroll_view", "group"},
        {"panel", "group"},
        {"video", "img"},
        {"menu", "menu"},
        {"menu_item", "menuitem"},
        {"menu_item_check_box", "menuitemcheckbox"},
        {"menu_item_radio", "menuitemradio"},
    }};
    for (const auto& [godot, aria] : kRoles) {
        if (godot == role) {
            return aria;
        }
    }
    return {};
}

// A number as ARIA's attributes take it: whole numbers without a point.
std::string number_text(double value) {
    if (std::abs(value) < 1e15 && value == static_cast<double>(static_cast<std::int64_t>(value))) {
        return std::to_string(static_cast<std::int64_t>(value));
    }
    return json::write(json::Value(value), json::Style::Compact);
}

std::string id_list(const std::vector<std::uint64_t>& ids) {
    std::string out;
    for (const std::uint64_t id : ids) {
        if (!out.empty()) {
            out += ' ';
        }
        out += dom_id(id);
    }
    return out;
}

bool has_flag(const Node& node, std::string_view flag) {
    return std::ranges::find(node.flags, flag) != node.flags.end();
}

std::string text_of(std::uint64_t id) {
    return std::to_string(id);
}

json::Value element_json(std::uint64_t id, const Node& node) {
    const DomElement element = dom_element(node);
    json::Object attributes;
    for (const auto& [name, value] : element.attributes) {
        attributes.emplace(name, value);
    }
    json::Array bounds;
    if (node.bounds) {
        for (const double edge : *node.bounds) {
            bounds.emplace_back(edge);
        }
    }
    json::Array children;
    for (const std::uint64_t child : node.children) {
        children.emplace_back(text_of(child));
    }
    json::Object out;
    out.emplace("id", text_of(id));
    out.emplace("role", element.role);
    out.emplace("attributes", std::move(attributes));
    out.emplace("text", element.text);
    out.emplace("bounds", std::move(bounds));
    out.emplace("children", std::move(children));
    return {std::move(out)};
}

} // namespace

std::string dom_id(std::uint64_t node) {
    return "slash-a11y-" + std::to_string(node);
}

DomElement dom_element(const Node& node) {
    DomElement out;
    out.role = std::string(aria_role(node.role));
    const auto set = [&](std::string name, std::string value) {
        out.attributes.emplace_back(std::move(name), std::move(value));
    };
    const bool text = node.role == "static_text";
    const bool field = node.role == "text_field" || node.role == "multiline_text_field";
    // A label reads its text; a field its contents, named by its label; the rest by their names.
    if (text) {
        out.text = !node.value.empty() ? node.value : node.name;
    } else {
        if (field) {
            out.text = node.value;
        }
        if (!node.name.empty()) {
            set("aria-label", node.name);
        }
    }
    if (!node.description.empty()) {
        set("aria-description", node.description);
    }
    if (!node.tooltip.empty()) {
        set("title", node.tooltip);
    }
    if (!node.placeholder.empty()) {
        set("aria-placeholder", node.placeholder);
    }
    if (node.role == "multiline_text_field") {
        set("aria-multiline", "true");
    }
    if (node.live != "off" && !node.live.empty()) {
        set("aria-live", node.live);
    }
    if (node.popup_type == "list") {
        set("aria-haspopup", "listbox");
    } else if (node.popup_type == "menu") {
        set("aria-haspopup", "menu");
    }
    if (node.checked) {
        // A toggle button is pressed; the rest are checked.
        set(out.role == "button" ? "aria-pressed" : "aria-checked",
            *node.checked ? "true" : "false");
    }
    if (node.range) {
        const auto& [value, low, high, step] = *node.range;
        (void)step;
        set("aria-valuenow", number_text(value));
        set("aria-valuemin", number_text(low));
        set("aria-valuemax", number_text(high));
        if (!node.value.empty() && !field) {
            set("aria-valuetext", node.value);
        }
    }
    if (node.busy) {
        set("aria-busy", "true");
    }
    if (has_flag(node, "hidden")) {
        set("aria-hidden", "true");
    }
    if (has_flag(node, "disabled")) {
        set("aria-disabled", "true");
    }
    if (has_flag(node, "readonly")) {
        set("aria-readonly", "true");
    }
    for (const auto& [name, ids] : {std::pair{"aria-controls", &node.controls},
                                    std::pair{"aria-describedby", &node.described_by},
                                    std::pair{"aria-labelledby", &node.labeled_by},
                                    std::pair{"aria-flowto", &node.flow_to}}) {
        if (!ids->empty()) {
            set(name, id_list(*ids));
        }
    }
    // What the page passes back: a click, and focus for controls that take it.
    std::string actions;
    for (const std::string_view action : {std::string_view("click"), std::string_view("focus")}) {
        if (std::ranges::find(node.actions, action) != node.actions.end()) {
            actions += actions.empty() ? "" : " ";
            actions += action;
        }
    }
    if (!actions.empty()) {
        set("data-actions", actions);
    }
    std::ranges::sort(out.attributes);
    return out;
}

json::Value dom_changes(const Snapshot& before, const Snapshot& now) {
    std::unordered_map<std::uint64_t, const Node*> kept;
    kept.reserve(now.nodes.size());
    for (const auto& [id, node] : now.nodes) {
        kept.emplace(id, &node);
    }
    json::Array removed;
    for (const auto& [id, node] : before.nodes) {
        if (!kept.contains(id)) {
            removed.emplace_back(text_of(id));
        }
    }
    json::Array set;
    for (const std::size_t at : changed(before, now)) {
        set.push_back(element_json(now.nodes[at].first, now.nodes[at].second));
    }
    json::Object out;
    if (!removed.empty()) {
        out.emplace("remove", std::move(removed));
    }
    if (!set.empty()) {
        out.emplace("set", std::move(set));
    }
    if (before.nodes.empty() || before.focus != now.focus) {
        out.emplace("focus", text_of(now.focus));
    }
    return {std::move(out)};
}

std::vector<Request> dom_requests(const json::Value& queued) {
    std::vector<Request> out;
    if (!queued.is_array()) {
        return out;
    }
    for (const json::Value& item : queued.as_array()) {
        const json::Value* node = item.find("node");
        const json::Value* action = item.find("action");
        if (node == nullptr || action == nullptr || !node->is_string() || !action->is_string() ||
            (action->as_string() != "click" && action->as_string() != "focus")) {
            continue;
        }
        const std::string& text = node->as_string();
        std::uint64_t id = 0;
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), id);
        if (error != std::errc{} || end != text.data() + text.size() || id == kWindowNode) {
            continue;
        }
        out.push_back(Request{.node = id, .action = action->as_string(), .data = {}});
    }
    return out;
}

} // namespace slash::accessibility
