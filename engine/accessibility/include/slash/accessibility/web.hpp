#pragma once

#include "slash/accessibility/accessibility.hpp"
#include "slash/core/json.hpp"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The web's side of accessibility (src/web.cpp). Browsers have no AccessKit adapter, so a web
// game's backend mirrors the tree as a DOM of WAI-ARIA elements over its canvas, out of the
// pointer's way and unseen, which the page's screen reader reads as it reads any page; a click or
// focus a screen reader gives an element comes back as a Request. What changes in the DOM is
// worked out here, the same on every platform, so tests read it natively.
namespace slash::accessibility {

// A node as a DOM element: its ARIA role ("" for a generic element), its attributes (aria-*,
// title and data-actions, the actions it takes) and its text, a label's or a text field's.
struct DomElement {
    std::string role;
    std::vector<std::pair<std::string, std::string>> attributes;
    std::string text;

    bool operator==(const DomElement&) const = default;
};

[[nodiscard]] DomElement dom_element(const Node& node);

// The element ID of a node in the page: "slash-a11y-<node ID>".
[[nodiscard]] std::string dom_id(std::uint64_t node);

// What the page does to turn before's DOM into now's, as JSON:
//
//   {"remove": ["65536"],
//    "set": [{"id": "1", "role": "application", "attributes": {"aria-label": "Pause"},
//             "text": "", "bounds": [0, 0, 400, 300], "children": ["131072"]}],
//    "focus": "131072"}
//
// IDs are node IDs as strings; "remove" lists the nodes before had and now lacks, "set" every
// node new or changed (bounds [x0, y0, x1, y1] as the snapshot's), each with its children in
// order, and "focus" the node with focus when it moved. Keys with nothing to say are left out.
[[nodiscard]] json::Value dom_changes(const Snapshot& before, const Snapshot& now);

// The requests the page queued, [{"node": "131072", "action": "click"}, ...]: those naming a
// node and an action of the page's ("click" and "focus") as Requests, the rest dropped.
[[nodiscard]] std::vector<Request> dom_requests(const json::Value& queued);

} // namespace slash::accessibility
