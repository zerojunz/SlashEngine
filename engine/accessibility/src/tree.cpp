#include "slash/accessibility/accessibility.hpp"
#include "slash/ui/ui.hpp"

#include <type_traits>
#include <unordered_map>

namespace slash::accessibility {
namespace {

// A node ID's low 16 bits say what of its entity it is: the control, its open menu, or an item.
constexpr std::uint64_t kControl = 0;
constexpr std::uint64_t kMenu = 1;
constexpr std::uint64_t kFirstItem = 2;
constexpr std::uint64_t kKindBits = 16;
constexpr std::uint64_t kKindMask = (std::uint64_t{1} << kKindBits) - 1;

std::uint64_t element_id(const ui::AccessElement& element) {
    return node_id(element.entity, element.menu, element.item);
}

// The IDs of the controls an element names, as their elements' IDs.
std::vector<std::uint64_t> ids_of(const std::vector<entt::entity>& entities) {
    std::vector<std::uint64_t> ids;
    ids.reserve(entities.size());
    for (const entt::entity entity : entities) {
        ids.push_back(node_id(entity));
    }
    return ids;
}

} // namespace

std::uint64_t node_id(entt::entity entity, bool menu, int item) {
    // Never the window's: entities start at 1 << 16.
    const std::uint64_t base = (static_cast<std::uint64_t>(entt::to_integral(entity)) + 1)
                               << kKindBits;
    if (item >= 0) {
        return base | (kFirstItem + static_cast<std::uint64_t>(item));
    }
    return base | (menu ? kMenu : kControl);
}

std::optional<Target> target_of(std::uint64_t id) {
    if ((id >> kKindBits) == 0) {
        return std::nullopt;
    }
    using Integral = std::underlying_type_t<entt::entity>;
    Target target;
    target.entity = static_cast<entt::entity>(static_cast<Integral>((id >> kKindBits) - 1));
    const std::uint64_t kind = id & kKindMask;
    target.menu = kind == kMenu;
    target.item = kind >= kFirstItem ? static_cast<int>(kind - kFirstItem) : -1;
    return target;
}

Snapshot snapshot(const ui::AccessTree& tree, std::string_view name, double scale) {
    Snapshot out;
    out.nodes.reserve(tree.elements.size() + 1);
    Node window;
    window.role = "window";
    window.name = std::string(name);
    for (const std::size_t root : tree.roots) {
        window.children.push_back(element_id(tree.elements[root]));
    }
    out.nodes.emplace_back(kWindowNode, std::move(window));
    for (const ui::AccessElement& element : tree.elements) {
        Node node;
        node.role = element.role;
        node.name = element.name;
        node.description = element.description;
        node.value = element.value;
        node.placeholder = element.placeholder;
        node.tooltip = element.tooltip;
        node.live = element.live;
        node.popup_type = element.popup_type;
        node.checked = element.checked;
        node.range = element.range;
        node.color = element.color;
        const auto& [x, y, w, h] = element.rect;
        node.bounds = std::array<double, 4>{x * scale, y * scale, (x + w) * scale, (y + h) * scale};
        node.selected = element.selected;
        node.flags.assign(element.flags.begin(), element.flags.end());
        // Menus and their items take what they take; controls also take the common actions.
        if (!element.menu && element.item < 0) {
            node.actions.assign(ui::kCommonActions.begin(), ui::kCommonActions.end());
        }
        node.actions.insert(node.actions.end(), element.actions.begin(), element.actions.end());
        for (const std::size_t child : element.children) {
            node.children.push_back(element_id(tree.elements[child]));
        }
        node.controls = ids_of(element.controls);
        node.described_by = ids_of(element.described_by);
        node.labeled_by = ids_of(element.labeled_by);
        node.flow_to = ids_of(element.flow_to);
        node.radio_group = ids_of(element.radio_group);
        out.nodes.emplace_back(element_id(element), std::move(node));
    }
    if (tree.focus) {
        out.focus = element_id(tree.elements[*tree.focus]);
    }
    return out;
}

Snapshot snapshot(const world::World& world, std::string_view name, double scale) {
    text::Fonts* fonts = ui::fonts(world);
    if (fonts == nullptr) {
        return snapshot(ui::AccessTree{}, name, scale);
    }
    const ui::Layout layout = ui::layout(world, *fonts, ui::settings(world).viewport);
    return snapshot(ui::accessibility_tree(world, layout), name, scale);
}

std::vector<std::size_t> changed(const Snapshot& before, const Snapshot& now) {
    std::unordered_map<std::uint64_t, const Node*> sent;
    sent.reserve(before.nodes.size());
    for (const auto& [id, node] : before.nodes) {
        sent.emplace(id, &node);
    }
    std::vector<std::size_t> out;
    for (std::size_t i = 0; i < now.nodes.size(); ++i) {
        const auto& [id, node] = now.nodes[i];
        const auto found = sent.find(id);
        if (found == sent.end() || !(*found->second == node)) {
            out.push_back(i);
        }
    }
    return out;
}

Result<void> perform(world::World& world, const Request& request, double scale) {
    const std::optional<Target> target = target_of(request.node);
    if (!target || !world.valid(target->entity)) {
        return make_error(ErrorCode::NotFound, "no control has that accessibility node");
    }
    json::Value data = request.data;
    if (request.action == "set_scroll_offset" && data.is_array() && data.as_array().size() == 2 &&
        scale > 0.0) {
        // Back into the viewport's units.
        json::Array point;
        point.emplace_back(data.as_array()[0].as_double() / scale);
        point.emplace_back(data.as_array()[1].as_double() / scale);
        data = json::Value(std::move(point));
    }
    return ui::accessibility_action(world, target->entity, request.action, data, target->item);
}

} // namespace slash::accessibility
