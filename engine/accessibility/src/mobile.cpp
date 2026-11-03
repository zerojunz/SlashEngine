#include "slash/accessibility/mobile.hpp"

#include "changes.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace slash::accessibility {
namespace {

bool takes(const Node& node, std::string_view action) {
    return std::ranges::find(node.actions, action) != node.actions.end();
}

bool has_flag(const Node& node, std::string_view flag) {
    return std::ranges::find(node.flags, flag) != node.flags.end();
}

bool is_field(const Node& node) {
    return node.role == "text_field" || node.role == "multiline_text_field";
}

// Godot's roles as the widgets TalkBack knows, as AccessKit's Android adapter names them.
std::string_view android_class(const Node& node) {
    if (node.role == "button" && node.checked) {
        return "android.widget.ToggleButton";
    }
    constexpr std::array<std::pair<std::string_view, std::string_view>, 17> kClasses{{
        {"button", "android.widget.Button"},
        {"check_box", "android.widget.CheckBox"},
        {"radio_button", "android.widget.RadioButton"},
        {"check_button", "android.widget.Switch"},
        {"static_text", "android.widget.TextView"},
        {"text_field", "android.widget.EditText"},
        {"multiline_text_field", "android.widget.EditText"},
        {"slider", "android.widget.SeekBar"},
        {"scroll_bar", "android.widget.SeekBar"},
        {"progress_indicator", "android.widget.ProgressBar"},
        {"scroll_view", "android.widget.ScrollView"},
        {"panel", "android.view.ViewGroup"},
        {"video", "android.widget.ImageView"},
        {"menu", "android.widget.ListView"},
        {"menu_item", "android.widget.Button"},
        {"menu_item_check_box", "android.widget.CheckBox"},
        {"menu_item_radio", "android.widget.RadioButton"},
    }};
    for (const auto& [role, name] : kClasses) {
        if (role == node.role) {
            return name;
        }
    }
    return "android.view.View";
}

// A node's name and another text, as one hint.
std::string joined(const std::string& first, const std::string& second) {
    if (first.empty() || second.empty()) {
        return first.empty() ? second : first;
    }
    return first + ", " + second;
}

const Node* find_node(const Snapshot& sent, std::uint64_t id) {
    const auto found =
        std::ranges::find_if(sent.nodes, [id](const auto& entry) { return entry.first == id; });
    return found != sent.nodes.end() ? &found->second : nullptr;
}

// The node and the nodes holding it, nearest first.
std::vector<std::pair<std::uint64_t, const Node*>> ancestry(const Snapshot& sent,
                                                            std::uint64_t id) {
    std::unordered_map<std::uint64_t, std::uint64_t> parents;
    for (const auto& [parent, node] : sent.nodes) {
        for (const std::uint64_t child : node.children) {
            parents.emplace(child, parent);
        }
    }
    std::vector<std::pair<std::uint64_t, const Node*>> out;
    std::uint64_t at = id;
    while (const Node* node = find_node(sent, at)) {
        out.emplace_back(at, node);
        const auto parent = parents.find(at);
        if (parent == parents.end() || out.size() > sent.nodes.size()) {
            break;
        }
        at = parent->second;
    }
    return out;
}

// A scroll in direction (scroll_up, ...) for the nearest node from id up that takes it.
std::optional<Request> scroll_request(const Snapshot& sent, std::uint64_t id,
                                      std::string_view direction) {
    for (const auto& [at, node] : ancestry(sent, id)) {
        if (at != kWindowNode && takes(*node, direction)) {
            return Request{.node = at, .action = std::string(direction), .data = {}};
        }
    }
    return std::nullopt;
}

std::string text_field(const json::Value& object, std::string_view key) {
    const json::Value* found = object.find(key);
    return found != nullptr && found->is_string() ? found->as_string() : std::string();
}

} // namespace

AndroidElement android_element(const Node& node) {
    AndroidElement out;
    out.class_name = std::string(android_class(node));
    const bool field = is_field(node);
    // TalkBack reads a widget's text; a field's name goes with its placeholder as its hint.
    if (node.role == "static_text") {
        out.text = !node.value.empty() ? node.value : node.name;
        out.hint = node.description;
    } else if (field) {
        out.text = node.value;
        out.hint = joined(node.name, node.placeholder);
        if (!node.description.empty()) {
            out.hint = joined(out.hint, node.description);
        }
    } else {
        out.text = node.name;
        out.hint = node.description;
    }
    out.tooltip = node.tooltip;
    if (node.range && !field) {
        const auto& [value, low, high, step] = *node.range;
        (void)step;
        out.range = std::array{value, low, high};
        out.state = node.value;
    }
    if (node.checked) {
        out.checkable = true;
        out.checked = *node.checked;
    }
    out.enabled = !has_flag(node, "disabled");
    out.focusable = takes(node, "focus");
    out.editable = field && !has_flag(node, "readonly");
    out.multi_line = node.role == "multiline_text_field";
    out.selected = node.selected;
    out.visible = !has_flag(node, "hidden");
    if (node.live == "polite") {
        out.live_region = 1;
    } else if (node.live == "assertive") {
        out.live_region = 2;
    }

    const auto add = [&](std::string_view name) { out.actions.emplace_back(name); };
    if (takes(node, "click")) {
        add("click");
    }
    if (takes(node, "focus")) {
        add("focus");
    }
    const bool steps = takes(node, "increment") || takes(node, "decrement");
    const bool scrolls = takes(node, "scroll_down") || takes(node, "scroll_up") ||
                         takes(node, "scroll_left") || takes(node, "scroll_right");
    if (steps || scrolls) {
        out.scrollable = true;
        add("scroll_forward");
        add("scroll_backward");
    }
    for (const std::string_view direction :
         {std::string_view("scroll_up"), std::string_view("scroll_down"),
          std::string_view("scroll_left"), std::string_view("scroll_right")}) {
        if (takes(node, direction)) {
            add(direction);
        }
    }
    if (takes(node, "set_value")) {
        if (node.range && !field) {
            add("set_progress");
        } else if (out.editable) {
            add("set_text");
        }
    }
    if (takes(node, "scroll_into_view")) {
        add("show_on_screen");
    }
    return out;
}

json::Value android_changes(const Snapshot& before, const Snapshot& now) {
    return detail::changes(before, now, [](const Node& node) {
        const AndroidElement element = android_element(node);
        json::Object out;
        out.emplace("class_name", element.class_name);
        out.emplace("text", element.text);
        out.emplace("hint", element.hint);
        out.emplace("tooltip", element.tooltip);
        out.emplace("state", element.state);
        out.emplace("checkable", element.checkable);
        out.emplace("checked", element.checked);
        out.emplace("enabled", element.enabled);
        out.emplace("focusable", element.focusable);
        out.emplace("editable", element.editable);
        out.emplace("multi_line", element.multi_line);
        out.emplace("scrollable", element.scrollable);
        out.emplace("selected", element.selected);
        out.emplace("visible", element.visible);
        out.emplace("live_region", element.live_region);
        if (element.range) {
            out.emplace("range",
                        json::Array{(*element.range)[0], (*element.range)[1], (*element.range)[2]});
        }
        json::Array actions;
        for (const std::string& action : element.actions) {
            actions.emplace_back(action);
        }
        out.emplace("actions", std::move(actions));
        return out;
    });
}

std::vector<Request> android_requests(const json::Value& queued, const Snapshot& sent) {
    std::vector<Request> out;
    if (!queued.is_array()) {
        return out;
    }
    for (const json::Value& item : queued.as_array()) {
        if (!item.is_object()) {
            continue;
        }
        const std::uint64_t id = detail::id_of(text_field(item, "node")).value_or(kWindowNode);
        const Node* node = find_node(sent, id);
        if (node == nullptr || id == kWindowNode) {
            continue;
        }
        const std::string action = text_field(item, "action");
        Request request{.node = id, .action = {}, .data = {}};
        const bool direct = action == "click" || action == "focus" || action == "scroll_up" ||
                            action == "scroll_down" || action == "scroll_left" ||
                            action == "scroll_right";
        if (direct) {
            request.action = action;
        } else if (action == "show_on_screen") {
            request.action = "scroll_into_view";
        } else if (action == "scroll_forward" || action == "scroll_backward") {
            // A range steps; a scroll view shows what is further down, or right.
            const bool forward = action == "scroll_forward";
            if (takes(*node, forward ? "increment" : "decrement")) {
                request.action = forward ? "increment" : "decrement";
            } else if (takes(*node, forward ? "scroll_down" : "scroll_up")) {
                request.action = forward ? "scroll_down" : "scroll_up";
            } else {
                request.action = forward ? "scroll_right" : "scroll_left";
            }
        } else if (action == "set_progress") {
            const json::Value* value = item.find("value");
            if (value == nullptr || !value->is_number() || !node->range) {
                continue;
            }
            request.action = "set_value";
            request.data = *value;
        } else if (action == "set_text") {
            const json::Value* text = item.find("text");
            if (text == nullptr || !text->is_string() || !is_field(*node) ||
                has_flag(*node, "readonly")) {
                continue;
            }
            request.action = "set_value";
            request.data = *text;
        }
        if (!request.action.empty() && takes(*node, request.action)) {
            out.push_back(std::move(request));
        }
    }
    return out;
}

IosElement ios_element(const Node& node) {
    IosElement out;
    const bool field = is_field(node);
    constexpr std::array<std::string_view, 6> kGroups{"window",      "container", "panel",
                                                      "scroll_view", "menu",      "scroll_bar"};
    out.element =
        std::ranges::find(kGroups, node.role) == kGroups.end() && !has_flag(node, "hidden");
    if (node.role == "static_text") {
        out.label = !node.value.empty() ? node.value : node.name;
    } else if (field) {
        out.label = !node.name.empty() ? node.name : node.placeholder;
        out.value = node.value;
    } else {
        out.label = node.name;
        if (node.range) {
            out.value = !node.value.empty() ? node.value : detail::number_text((*node.range)[0]);
        }
    }
    out.hint = !node.description.empty() ? node.description : node.tooltip;

    const auto add = [&](std::string_view trait) { out.traits.emplace_back(trait); };
    constexpr std::array<std::string_view, 7> kButtons{
        "button",    "check_box",           "radio_button",   "check_button",
        "menu_item", "menu_item_check_box", "menu_item_radio"};
    if (std::ranges::find(kButtons, node.role) != kButtons.end()) {
        add("button");
        if (node.role == "check_box" || node.role == "check_button" ||
            (node.role == "button" && node.checked)) {
            add("toggle_button");
        }
    } else if (node.role == "static_text") {
        add("static_text");
    } else if (node.role == "video") {
        add("image");
    }
    if (takes(node, "increment") || takes(node, "decrement")) {
        add("adjustable");
    }
    if (node.selected || (node.checked && *node.checked)) {
        add("selected");
    }
    if (has_flag(node, "disabled")) {
        add("not_enabled");
    }
    if (node.role == "progress_indicator" || (node.live != "off" && !node.live.empty())) {
        add("updates_frequently");
    }
    return out;
}

std::optional<Request> ios_request(const Snapshot& sent, std::uint64_t node,
                                   std::string_view gesture) {
    const Node* found = node != kWindowNode ? find_node(sent, node) : nullptr;
    if (found == nullptr) {
        return std::nullopt;
    }
    if (gesture == "scroll_up" || gesture == "scroll_down" || gesture == "scroll_left" ||
        gesture == "scroll_right") {
        return scroll_request(sent, node, gesture);
    }
    const std::string_view action = gesture == "activate" ? std::string_view("click") : gesture;
    if ((action == "click" || action == "increment" || action == "decrement") &&
        takes(*found, action)) {
        return Request{.node = node, .action = std::string(action), .data = {}};
    }
    return std::nullopt;
}

} // namespace slash::accessibility
