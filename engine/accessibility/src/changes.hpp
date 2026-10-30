#pragma once

#include "slash/accessibility/accessibility.hpp"
#include "slash/core/json.hpp"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

// What the backends that keep their own elements share (src/dom.cpp, src/mobile.cpp): the page's
// and Android's code get the changes as JSON, with node IDs as strings, since JavaScript's numbers
// lose integers past 2^53.
namespace slash::accessibility::detail {

inline std::string id_text(std::uint64_t id) {
    return std::to_string(id);
}

// A number as ARIA's attributes and VoiceOver's values take it: whole numbers without a point.
inline std::string number_text(double value) {
    if (std::abs(value) < 1e15 && value == static_cast<double>(static_cast<std::int64_t>(value))) {
        return std::to_string(static_cast<std::int64_t>(value));
    }
    return json::write(json::Value(value), json::Style::Compact);
}

// A node ID read back from its string; nothing for any other text.
inline std::optional<std::uint64_t> id_of(const std::string& text) {
    std::uint64_t id = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), id);
    if (text.empty() || error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return id;
}

// What turns before's elements into now's:
//
//   {"remove": ["65536"], "set": [{"id": "1", "bounds": [...], "children": [...], ...}],
//    "focus": "131072"}
//
// "set" holds every node new or changed, each with its bounds, its children in order and the
// fields fields(node) gives; keys with nothing to say are left out.
template <class Fields>
json::Value changes(const Snapshot& before, const Snapshot& now, Fields fields) {
    std::unordered_map<std::uint64_t, const Node*> kept;
    kept.reserve(now.nodes.size());
    for (const auto& [id, node] : now.nodes) {
        kept.emplace(id, &node);
    }
    json::Array removed;
    for (const auto& [id, node] : before.nodes) {
        if (!kept.contains(id)) {
            removed.emplace_back(id_text(id));
        }
    }
    json::Array set;
    for (const std::size_t at : changed(before, now)) {
        const auto& [id, node] = now.nodes[at];
        json::Object item = fields(node);
        json::Array bounds;
        if (node.bounds) {
            for (const double edge : *node.bounds) {
                bounds.emplace_back(edge);
            }
        }
        json::Array children;
        for (const std::uint64_t child : node.children) {
            children.emplace_back(id_text(child));
        }
        item.insert_or_assign("id", id_text(id));
        item.insert_or_assign("bounds", std::move(bounds));
        item.insert_or_assign("children", std::move(children));
        set.emplace_back(std::move(item));
    }
    json::Object out;
    if (!removed.empty()) {
        out.emplace("remove", std::move(removed));
    }
    if (!set.empty()) {
        out.emplace("set", std::move(set));
    }
    if (before.nodes.empty() || before.focus != now.focus) {
        out.emplace("focus", id_text(now.focus));
    }
    return {std::move(out)};
}

} // namespace slash::accessibility::detail
