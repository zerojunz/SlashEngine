#include "slash/accessibility/accesskit.hpp"

#include "internal.hpp"

#include "slash/core/log.hpp"
#include "slash/ui/ui.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace slash::accessibility {
namespace {

// Godot's role map (AccessibilityDriverAccessKit's constructor), by ui::AccessElement's names.
accesskit_role role_of(std::string_view role) {
    static const std::unordered_map<std::string_view, accesskit_role> roles{
        {"container", ACCESSKIT_ROLE_GENERIC_CONTAINER},
        {"button", ACCESSKIT_ROLE_BUTTON},
        {"check_box", ACCESSKIT_ROLE_CHECK_BOX},
        {"radio_button", ACCESSKIT_ROLE_RADIO_BUTTON},
        {"check_button", ACCESSKIT_ROLE_SWITCH},
        {"static_text", ACCESSKIT_ROLE_LABEL},
        {"text_field", ACCESSKIT_ROLE_TEXT_INPUT},
        {"multiline_text_field", ACCESSKIT_ROLE_MULTILINE_TEXT_INPUT},
        {"slider", ACCESSKIT_ROLE_SLIDER},
        {"scroll_bar", ACCESSKIT_ROLE_SCROLL_BAR},
        {"progress_indicator", ACCESSKIT_ROLE_PROGRESS_INDICATOR},
        {"scroll_view", ACCESSKIT_ROLE_SCROLL_VIEW},
        {"panel", ACCESSKIT_ROLE_PANE},
        {"video", ACCESSKIT_ROLE_VIDEO},
        {"menu", ACCESSKIT_ROLE_MENU},
        {"menu_item", ACCESSKIT_ROLE_MENU_ITEM},
        {"menu_item_check_box", ACCESSKIT_ROLE_MENU_ITEM_CHECK_BOX},
        {"menu_item_radio", ACCESSKIT_ROLE_MENU_ITEM_RADIO},
        {"window", ACCESSKIT_ROLE_WINDOW},
    };
    const auto found = roles.find(role);
    return found != roles.end() ? found->second : ACCESSKIT_ROLE_UNKNOWN;
}

// The actions the controls take, by ui::accessibility_action's names, as Godot's action map.
constexpr std::array<std::pair<std::string_view, accesskit_action>, 14> kActions{{
    {"click", ACCESSKIT_ACTION_CLICK},
    {"focus", ACCESSKIT_ACTION_FOCUS},
    {"blur", ACCESSKIT_ACTION_BLUR},
    {"show_tooltip", ACCESSKIT_ACTION_SHOW_TOOLTIP},
    {"hide_tooltip", ACCESSKIT_ACTION_HIDE_TOOLTIP},
    {"scroll_into_view", ACCESSKIT_ACTION_SCROLL_INTO_VIEW},
    {"increment", ACCESSKIT_ACTION_INCREMENT},
    {"decrement", ACCESSKIT_ACTION_DECREMENT},
    {"set_value", ACCESSKIT_ACTION_SET_VALUE},
    {"scroll_up", ACCESSKIT_ACTION_SCROLL_UP},
    {"scroll_down", ACCESSKIT_ACTION_SCROLL_DOWN},
    {"scroll_left", ACCESSKIT_ACTION_SCROLL_LEFT},
    {"scroll_right", ACCESSKIT_ACTION_SCROLL_RIGHT},
    {"set_scroll_offset", ACCESSKIT_ACTION_SET_SCROLL_OFFSET},
}};

std::uint8_t channel(float value) {
    return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F));
}

// What an adapter's handlers share with the backend: they may run on another thread.
struct Shared {
    std::string name;
    std::mutex mutex;
    std::vector<Request> requests;    // under mutex
    std::atomic<bool> resend{true};   // the next update sends every node
    std::atomic<bool> reading{false}; // activated and not deactivated
};

// The tree an adapter starts with when an assistive technology first asks, as Godot's
// _accessibility_initial_tree_update_callback: the window, busy until the first update.
accesskit_tree_update* initial_tree(void* userdata) {
    auto* shared = static_cast<Shared*>(userdata);
    shared->resend = true;
    shared->reading = true;
    Snapshot snapshot;
    Node window;
    window.role = "window";
    window.name = shared->name;
    window.busy = true;
    snapshot.nodes.emplace_back(kWindowNode, std::move(window));
    return make_update(snapshot, nullptr, true);
}

#if defined(__linux__)
void deactivated(void* userdata) {
    static_cast<Shared*>(userdata)->reading = false;
}
#endif

// Queued for the next update, as Godot's driver defers each action's callable.
void action_requested(accesskit_action_request* request, void* userdata) {
    auto* shared = static_cast<Shared*>(userdata);
    if (std::optional<Request> ours = request_of(*request); ours) {
        const std::lock_guard lock(shared->mutex);
        shared->requests.push_back(std::move(ours).value());
    }
    accesskit_action_request_free(request);
}

// What an update's factory needs: AccessKit calls it inside update_if_active.
struct Pending {
    const world::World* world = nullptr;
    Shared* shared = nullptr;
    Snapshot* sent = nullptr;
    double scale = 1.0;
};

accesskit_tree_update* build_update(void* userdata) {
    auto* pending = static_cast<Pending*>(userdata);
    Snapshot now = snapshot(*pending->world, pending->shared->name, pending->scale);
    accesskit_tree_update* update = nullptr;
    if (pending->shared->resend.exchange(false)) {
        update = make_update(now, nullptr, true);
    } else {
        const std::vector<std::size_t> nodes = changed(*pending->sent, now);
        update = make_update(now, &nodes, false);
    }
    *pending->sent = std::move(now);
    return update;
}

#if defined(_WIN32)
using Adapter = accesskit_windows_subclassing_adapter;
#elif defined(__APPLE__)
using Adapter = accesskit_macos_subclassing_adapter;
#elif defined(__linux__)
using Adapter = accesskit_unix_adapter;
#else
// No adapter here: CMake builds AccessKit for Windows, macOS and Linux only.
using Adapter = void;
#endif

class AccessKitBackend final : public Backend {
public:
    AccessKitBackend(SDL_Window* window, std::string name)
        : window_(window), shared_(std::make_unique<Shared>()) {
        shared_->name = std::move(name);
        const SDL_PropertiesID properties = SDL_GetWindowProperties(window);
#ifdef _WIN32
        auto* hwnd = static_cast<HWND>(
            SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
        if (hwnd != nullptr && !IsWindowVisible(hwnd)) {
            adapter_ = accesskit_windows_subclassing_adapter_new(hwnd, &initial_tree, shared_.get(),
                                                                 &action_requested, shared_.get());
        }
#elif defined(__APPLE__)
        void* ns_window =
            SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
        if (ns_window != nullptr) {
            // SDL gives the window, not its content view, the keyboard focus.
            static std::once_flag forwarder;
            std::call_once(forwarder, [] {
                accesskit_macos_add_focus_forwarder_to_window_class("SDL3Window");
            });
            adapter_ = accesskit_macos_subclassing_adapter_for_window(
                ns_window, &initial_tree, shared_.get(), &action_requested, shared_.get());
        }
#elif defined(__linux__)
        (void)properties;
        adapter_ = accesskit_unix_adapter_new(&initial_tree, shared_.get(), &action_requested,
                                              shared_.get(), &deactivated, shared_.get());
        place_window();
#else
        (void)properties;
#endif
    }

    ~AccessKitBackend() override {
        if (adapter_ == nullptr) {
            return;
        }
#ifdef _WIN32
        accesskit_windows_subclassing_adapter_free(adapter_);
#elif defined(__APPLE__)
        accesskit_macos_subclassing_adapter_free(adapter_);
#elif defined(__linux__)
        accesskit_unix_adapter_free(adapter_);
#endif
    }

    AccessKitBackend(const AccessKitBackend&) = delete;
    AccessKitBackend& operator=(const AccessKitBackend&) = delete;

    [[nodiscard]] bool attached() const { return adapter_ != nullptr; }

    void update(world::World& world) override {
        std::vector<Request> requests;
        {
            const std::lock_guard lock(shared_->mutex);
            requests.swap(shared_->requests);
        }
        const double scale = window_scale();
        for (const Request& request : requests) {
            if (const Result<void> done = perform(world, request, scale); !done) {
                log::warn("a screen reader's request failed",
                          {{"action", request.action}, {"error", done.error().message}});
            }
        }
        if (ui::Settings settings = ui::settings(world);
            settings.screen_reader_active != screen_reader_running()) {
            settings.screen_reader_active = !settings.screen_reader_active;
            ui::set_settings(world, settings);
        }
        Pending pending{.world = &world, .shared = shared_.get(), .sent = &sent_, .scale = scale};
#ifdef _WIN32
        if (accesskit_windows_queued_events* events =
                accesskit_windows_subclassing_adapter_update_if_active(adapter_, &build_update,
                                                                       &pending);
            events != nullptr) {
            accesskit_windows_queued_events_raise(events);
        }
#elif defined(__APPLE__)
        if (accesskit_macos_queued_events* events =
                accesskit_macos_subclassing_adapter_update_if_active(adapter_, &build_update,
                                                                     &pending);
            events != nullptr) {
            accesskit_macos_queued_events_raise(events);
        }
#elif defined(__linux__)
        accesskit_unix_adapter_update_if_active(adapter_, &build_update, &pending);
#else
        (void)pending;
#endif
    }

    void handle_event(const SDL_Event& event) override {
        if (event.type == SDL_EVENT_WINDOW_FOCUS_GAINED ||
            event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
            const bool focused = event.type == SDL_EVENT_WINDOW_FOCUS_GAINED;
#ifdef __APPLE__
            if (accesskit_macos_queued_events* events =
                    accesskit_macos_subclassing_adapter_update_view_focus_state(adapter_, focused);
                events != nullptr) {
                accesskit_macos_queued_events_raise(events);
            }
#elif defined(__linux__)
            accesskit_unix_adapter_update_window_focus_state(adapter_, focused);
#else
            // Windows' subclassing adapter follows the window's focus itself.
            (void)focused;
#endif
        } else if (event.type == SDL_EVENT_WINDOW_MOVED || event.type == SDL_EVENT_WINDOW_RESIZED) {
            place_window();
        }
    }

    [[nodiscard]] bool active() const override { return shared_->reading; }

private:
    // The viewport's units in the pixels AccessKit measures in: physical pixels on Windows and
    // Linux, points on macOS, as the window's controls are laid out in points.
    [[nodiscard]] double window_scale() const {
#ifdef __APPLE__
        return 1.0;
#else
        const float density = SDL_GetWindowPixelDensity(window_);
        return density > 0.0F ? static_cast<double>(density) : 1.0;
#endif
    }

    // AT-SPI places the tree in the window's bounds on the screen, which X11 gives and Wayland
    // doesn't, as Godot's accessibility_set_window_rect.
    void place_window() {
#if defined(__linux__)
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;
        int top = 0;
        int left = 0;
        int bottom = 0;
        int right = 0;
        if (!SDL_GetWindowPosition(window_, &x, &y) ||
            !SDL_GetWindowSize(window_, &width, &height)) {
            return;
        }
        (void)SDL_GetWindowBordersSize(window_, &top, &left, &bottom, &right);
        const accesskit_rect outer{.x0 = static_cast<double>(x - left),
                                   .y0 = static_cast<double>(y - top),
                                   .x1 = static_cast<double>(x + width + right),
                                   .y1 = static_cast<double>(y + height + bottom)};
        const accesskit_rect inner{.x0 = static_cast<double>(x),
                                   .y0 = static_cast<double>(y),
                                   .x1 = static_cast<double>(x + width),
                                   .y1 = static_cast<double>(y + height)};
        accesskit_unix_adapter_set_root_window_bounds(adapter_, outer, inner);
#endif
    }

    // Whether the system's screen reader runs, as Godot's
    // DisplayServer::accessibility_screen_reader_active; on Linux, whether AT-SPI reads.
    [[nodiscard]] bool screen_reader_running() const {
#ifdef _WIN32
        BOOL running = FALSE;
        return SystemParametersInfoW(SPI_GETSCREENREADER, 0, &running, 0) != FALSE &&
               running != FALSE;
#elif defined(__APPLE__)
        return voice_over_running();
#else
        return shared_->reading;
#endif
    }

    [[maybe_unused]] SDL_Window* window_ = nullptr; // unread on macOS
    Adapter* adapter_ = nullptr;
    std::unique_ptr<Shared> shared_;
    Snapshot sent_;
};

} // namespace

bool available() {
    return true;
}

accesskit_node* make_node(const Node& node) {
    accesskit_node* out = accesskit_node_new(role_of(node.role));
    if (!node.name.empty()) {
        accesskit_node_set_label(out, node.name.c_str());
    }
    if (!node.description.empty()) {
        accesskit_node_set_description(out, node.description.c_str());
    }
    if (!node.value.empty()) {
        accesskit_node_set_value(out, node.value.c_str());
    }
    if (!node.placeholder.empty()) {
        accesskit_node_set_placeholder(out, node.placeholder.c_str());
    }
    if (!node.tooltip.empty()) {
        accesskit_node_set_tooltip(out, node.tooltip.c_str());
    }
    if (node.live == "polite") {
        accesskit_node_set_live(out, ACCESSKIT_LIVE_POLITE);
    } else if (node.live == "assertive") {
        accesskit_node_set_live(out, ACCESSKIT_LIVE_ASSERTIVE);
    }
    if (node.popup_type == "list") {
        accesskit_node_set_has_popup(out, ACCESSKIT_HAS_POPUP_LISTBOX);
    } else if (node.popup_type == "menu") {
        accesskit_node_set_has_popup(out, ACCESSKIT_HAS_POPUP_MENU);
    }
    if (node.checked) {
        accesskit_node_set_toggled(out, *node.checked ? ACCESSKIT_TOGGLED_TRUE
                                                      : ACCESSKIT_TOGGLED_FALSE);
    }
    if (node.range) {
        const auto& [value, min, max, step] = *node.range;
        accesskit_node_set_numeric_value(out, value);
        accesskit_node_set_min_numeric_value(out, min);
        accesskit_node_set_max_numeric_value(out, max);
        if (step > 0.0) {
            accesskit_node_set_numeric_value_step(out, step);
        }
    }
    if (node.color) {
        const auto& [r, g, b, a] = *node.color;
        accesskit_node_set_color_value(
            out, {.red = channel(r), .green = channel(g), .blue = channel(b), .alpha = channel(a)});
    }
    if (node.bounds) {
        const auto& [x0, y0, x1, y1] = *node.bounds;
        accesskit_node_set_bounds(out, {.x0 = x0, .y0 = y0, .x1 = x1, .y1 = y1});
    }
    if (node.selected) {
        accesskit_node_set_selected(out, true);
    }
    if (node.busy) {
        accesskit_node_set_busy(out);
    }
    for (const std::string& flag : node.flags) {
        if (flag == "hidden") {
            accesskit_node_set_hidden(out);
        } else if (flag == "disabled") {
            accesskit_node_set_disabled(out);
        } else if (flag == "readonly") {
            accesskit_node_set_read_only(out);
        } else if (flag == "clips_children") {
            accesskit_node_set_clips_children(out);
        } else if (flag == "touch_passthrough") {
            accesskit_node_set_touch_transparent(out);
        }
    }
    for (const std::string& action : node.actions) {
        for (const auto& [name, code] : kActions) {
            if (name == action) {
                accesskit_node_add_action(out, code);
            }
        }
    }
    for (const std::uint64_t child : node.children) {
        accesskit_node_push_child(out, child);
    }
    for (const std::uint64_t id : node.controls) {
        accesskit_node_push_controlled(out, id);
    }
    for (const std::uint64_t id : node.described_by) {
        accesskit_node_push_described_by(out, id);
    }
    for (const std::uint64_t id : node.labeled_by) {
        accesskit_node_push_labelled_by(out, id);
    }
    for (const std::uint64_t id : node.flow_to) {
        accesskit_node_push_flow_to(out, id);
    }
    for (const std::uint64_t id : node.radio_group) {
        accesskit_node_push_to_radio_group(out, id);
    }
    return out;
}

accesskit_tree_update* make_update(const Snapshot& snapshot, const std::vector<std::size_t>* nodes,
                                   bool tree_info) {
    const std::size_t count = nodes != nullptr ? nodes->size() : snapshot.nodes.size();
    accesskit_tree_update* update =
        accesskit_tree_update_with_capacity_and_focus(count, snapshot.focus);
    if (tree_info) {
        accesskit_tree_info* info = accesskit_tree_info_new(kWindowNode);
        accesskit_tree_info_set_toolkit_name(info, "SlashEngine");
        accesskit_tree_update_set_tree_info(update, info);
    }
    const auto push = [&](std::size_t index) {
        const auto& [id, node] = snapshot.nodes[index];
        accesskit_tree_update_push_node(update, id, make_node(node));
    };
    if (nodes != nullptr) {
        for (const std::size_t index : *nodes) {
            push(index);
        }
    } else {
        for (std::size_t index = 0; index < snapshot.nodes.size(); ++index) {
            push(index);
        }
    }
    return update;
}

std::optional<Request> request_of(const accesskit_action_request& request) {
    Request out;
    out.node = request.target_node;
    for (const auto& [name, code] : kActions) {
        if (code == request.action) {
            out.action = std::string(name);
        }
    }
    if (out.action.empty()) {
        return std::nullopt;
    }
    if (request.data.has_value) {
        const accesskit_action_data& data = request.data.value;
        switch (data.tag) {
        case ACCESSKIT_ACTION_DATA_VALUE:
            out.data = json::Value(std::string(data.value != nullptr ? data.value : ""));
            break;
        case ACCESSKIT_ACTION_DATA_NUMERIC_VALUE:
            out.data = json::Value(data.numeric_value);
            break;
        case ACCESSKIT_ACTION_DATA_SCROLL_UNIT:
            // A page is what the scroll actions do with no data.
            if (data.scroll_unit == ACCESSKIT_SCROLL_UNIT_ITEM) {
                out.data = json::Value("item");
            }
            break;
        case ACCESSKIT_ACTION_DATA_SET_SCROLL_OFFSET:
            out.data = json::Value(json::Array{json::Value(data.set_scroll_offset.x),
                                               json::Value(data.set_scroll_offset.y)});
            break;
        default:
            break;
        }
    }
    return out;
}

std::unique_ptr<Backend> Backend::attach(SDL_Window* window, std::string name) {
    if (window == nullptr) {
        return nullptr;
    }
    auto backend = std::make_unique<AccessKitBackend>(window, std::move(name));
    if (!backend->attached()) {
        log::warn("no screen reader: AccessKit couldn't attach to the window");
        return nullptr;
    }
    return backend;
}

} // namespace slash::accessibility
