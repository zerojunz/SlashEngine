#pragma once

#include "slash/accessibility/accessibility.hpp"
#include "slash/core/json.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The phones' side of accessibility (src/android.cpp, src/ios.mm). AccessKit has no adapters for
// them in C, so each backend keeps elements of the platform's own for the tree's nodes, as the
// web's does: virtual views of an AccessibilityNodeProvider on the game's view for TalkBack
// (engine/accessibility/android/), and UIAccessibilityElements in the game's view for VoiceOver.
// How a node becomes one is worked out here, the same on every platform, so tests read it
// natively.
namespace slash::accessibility {

// A node as an Android virtual view: what AccessibilityNodeInfo gets.
struct AndroidElement {
    // The widget class TalkBack announces it as ("android.widget.Button").
    std::string class_name;
    // What TalkBack reads: a label's or button's text, a text field's contents.
    std::string text;
    // A text field's name and placeholder, or a control's description (setHintText).
    std::string hint;
    std::string tooltip;
    // A range's value as text, read in place of its number (setStateDescription).
    std::string state;
    bool checkable = false;
    bool checked = false;
    bool enabled = true;
    bool focusable = false;
    bool editable = false;
    bool multi_line = false;
    bool scrollable = false;
    bool selected = false;
    bool visible = true;
    // View.ACCESSIBILITY_LIVE_REGION_*: 0 none, 1 polite, 2 assertive.
    int live_region = 0;
    // [value, min, max] (RangeInfo, as floats).
    std::optional<std::array<double, 3>> range;
    // AccessibilityAction names the Java side turns into the framework's actions: "click",
    // "focus", "scroll_forward", "scroll_backward", "scroll_up", "scroll_down", "scroll_left",
    // "scroll_right", "set_progress", "set_text" and "show_on_screen".
    std::vector<std::string> actions;

    bool operator==(const AndroidElement&) const = default;
};

[[nodiscard]] AndroidElement android_element(const Node& node);

// What the Java side does to turn before's virtual views into now's: dom_changes' shape, each
// "set" item with the element's fields by their names above.
[[nodiscard]] json::Value android_changes(const Snapshot& before, const Snapshot& now);

// The actions TalkBack performed, [{"node": "131072", "action": "set_progress", "value": 40}]
// (a "text" for set_text), as Requests on the nodes of sent, the snapshot last sent: a forward
// or backward scroll steps a range or scrolls a scroll view down or up. Actions a node doesn't
// take, and the window's, are dropped.
[[nodiscard]] std::vector<Request> android_requests(const json::Value& queued,
                                                    const Snapshot& sent);

// A node as a UIAccessibilityElement.
struct IosElement {
    // Whether VoiceOver stops on it (isAccessibilityElement): not the window or containers.
    bool element = true;
    std::string label;
    std::string value;
    std::string hint;
    // UIAccessibilityTraits by name: "button", "toggle_button" (iOS 17, a button before),
    // "static_text", "adjustable", "image", "selected", "not_enabled" and
    // "updates_frequently".
    std::vector<std::string> traits;

    bool operator==(const IosElement&) const = default;
};

[[nodiscard]] IosElement ios_element(const Node& node);

// What a VoiceOver gesture on a node asks, as a Request on the nodes of sent: "activate" (a
// double tap) clicks, "increment" and "decrement" (a swipe up or down on an adjustable element)
// step a range, and "scroll_up", "scroll_down", "scroll_left" and "scroll_right" (which
// src/ios.mm makes of three-finger swipes) scroll the nearest scroll view holding the node.
// Nothing when no node there takes it.
[[nodiscard]] std::optional<Request> ios_request(const Snapshot& sent, std::uint64_t node,
                                                 std::string_view gesture);

} // namespace slash::accessibility
