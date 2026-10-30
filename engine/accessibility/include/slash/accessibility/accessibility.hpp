#pragma once

#include "slash/core/json.hpp"
#include "slash/core/math.hpp"
#include "slash/core/result.hpp"
#include "slash/ui/accessibility.hpp"
#include "slash/world/world.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct SDL_Window;
union SDL_Event;

// The platform side of accessibility, as Godot's AccessibilityDriverAccessKit: AccessKit's
// adapters hand the controls' accessibility tree (ui::accessibility_tree) to UI Automation on
// Windows, the macOS accessibility API and AT-SPI on Linux, so Narrator, NVDA, VoiceOver and
// Orca read a game's controls, and what they ask of them goes to ui::accessibility_action.
//
//   // Before the window is first shown, as Windows needs:
//   auto backend = accessibility::Backend::attach(window, "My Game");
//   // After each tick:
//   if (backend) backend->update(world);
namespace slash::accessibility {

// Whether this build has AccessKit: cargo built it (cmake/Dependencies.cmake).
[[nodiscard]] bool available();

// The window's node, the tree's root.
inline constexpr std::uint64_t kWindowNode = 1;

// What a node stands for: a control, an open popup menu (menu) or one of its items (item).
struct Target {
    entt::entity entity = entt::null;
    bool menu = false;
    int item = -1;
};

// The node ID of an element of ui::accessibility_tree, the same while its entity lives.
[[nodiscard]] std::uint64_t node_id(entt::entity entity, bool menu = false, int item = -1);
// What a node ID stands for; nothing for the window's or an ID no element has.
[[nodiscard]] std::optional<Target> target_of(std::uint64_t id);

// One node as AccessKit gets it: an element of the tree, or the window, with the elements it
// names as node IDs and its bounds in the pixels the system measures in.
struct Node {
    // ui::AccessElement's role, or "window".
    std::string role = "container";
    std::string name;
    std::string description;
    std::string value;
    std::string placeholder;
    std::string tooltip;
    std::string live = "off";
    std::string popup_type;
    std::optional<bool> checked;
    std::optional<std::array<double, 4>> range; // [value, min, max, step]
    std::optional<Vec4> color;
    std::optional<std::array<double, 4>> bounds; // [x0, y0, x1, y1]
    bool selected = false;
    bool busy = false;
    std::vector<std::string> flags;
    // Every action it takes: the common ones (ui::kCommonActions), then its own.
    std::vector<std::string> actions;
    std::vector<std::uint64_t> children;
    std::vector<std::uint64_t> controls;
    std::vector<std::uint64_t> described_by;
    std::vector<std::uint64_t> labeled_by;
    std::vector<std::uint64_t> flow_to;
    std::vector<std::uint64_t> radio_group;

    bool operator==(const Node&) const = default;
};

// The nodes of a tree, the window's first, and the node with focus (the window's when no
// control has it).
struct Snapshot {
    std::vector<std::pair<std::uint64_t, Node>> nodes;
    std::uint64_t focus = kWindowNode;
};

// A tree's nodes under a window named name. scale turns the viewport's units into the pixels
// the system measures in: the window's pixel density on Windows and Linux, 1 on macOS.
[[nodiscard]] Snapshot snapshot(const ui::AccessTree& tree, std::string_view name,
                                double scale = 1.0);
// The world's controls laid out at the viewport (ui::Settings::viewport), as a snapshot.
[[nodiscard]] Snapshot snapshot(const world::World& world, std::string_view name,
                                double scale = 1.0);

// The nodes of now that before lacks or has otherwise, as indexes into now.nodes: what a tree
// update sends.
[[nodiscard]] std::vector<std::size_t> changed(const Snapshot& before, const Snapshot& now);

// What a screen reader asked: an action of ui::accessibility_action on a node, with its data
// (a value, [x, y] for set_scroll_offset in the system's pixels, or "item" to scroll by one).
struct Request {
    std::uint64_t node = 0;
    std::string action;
    json::Value data;
};

// Does what a request asks of its node's control or menu item. scale is the snapshot's, to
// turn scroll offsets back into the viewport's units.
Result<void> perform(world::World& world, const Request& request, double scale = 1.0);

// AccessKit's adapter on a game's window, as Godot's driver makes one per window: the
// subclassing adapter on Windows' HWND and macOS's NSWindow, and the AT-SPI adapter on Linux.
class Backend {
public:
    // The adapter for a window, which on Windows must not have been shown yet. Null where this
    // build has no AccessKit or the platform no adapter.
    static std::unique_ptr<Backend> attach(SDL_Window* window, std::string name);

    Backend() = default;
    Backend(const Backend&) = delete;
    Backend& operator=(const Backend&) = delete;
    virtual ~Backend() = default;

    // Does what screen readers asked since the last call, sets ui::Settings::
    // screen_reader_active from the system, and sends the nodes that changed while an
    // assistive technology reads the tree. Call it after each tick, on the window's thread.
    virtual void update(world::World& world) = 0;
    // Tells the adapter of the window's focus and place, from SDL's window events.
    virtual void handle_event(const SDL_Event& event) = 0;
    // Whether an assistive technology has asked for the tree.
    [[nodiscard]] virtual bool active() const = 0;
};

} // namespace slash::accessibility
