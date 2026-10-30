#pragma once

// AccessKit's side of the conversion, in builds that have it (accessibility::available()):
// what the backend sends and takes, for tests to read back with AccessKit's own getters.

#include "slash/accessibility/accessibility.hpp"

#include <accesskit.h>
#include <cstddef>
#include <optional>
#include <vector>

namespace slash::accessibility {

// A node as AccessKit's, as Godot's driver fills one: its role (Godot's role map), label,
// description, value, placeholder, tooltip, live setting, popup, toggled state, numbers, color,
// bounds, states, actions, children and related nodes. The caller owns it.
[[nodiscard]] accesskit_node* make_node(const Node& node);

// A tree update of a snapshot's nodes at the given indexes (every node when nodes is null) and
// its focus; with tree_info, it names the window's node as the root and SlashEngine as the
// toolkit, as a first update must. The caller owns it.
[[nodiscard]] accesskit_tree_update*
make_update(const Snapshot& snapshot, const std::vector<std::size_t>* nodes, bool tree_info);

// A screen reader's request as ours: its action's name and data. Nothing for actions the
// controls don't take.
[[nodiscard]] std::optional<Request> request_of(const accesskit_action_request& request);

} // namespace slash::accessibility
