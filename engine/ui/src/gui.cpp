#include "internal.hpp"

#include "slash/core/text.hpp"
#include "slash/input/input.hpp"
#include "slash/ui/ui.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// The gui system sends input events to controls as Godot's Viewport does
// (scene/main/viewport.cpp: _gui_input_event, _gui_call_input, _gui_find_control_at_pos and
// _update_mouse_over), moves focus as Control does (scene/gui/control.cpp:
// find_next_valid_focus, find_prev_valid_focus and _get_focus_neighbor with the legacy
// strategy, Godot's default) and presses buttons as BaseButton does
// (scene/gui/base_button.cpp). Its state is kept by entity ID, since sessions make entities
// anew when they roll a command back. Drag and drop, tooltips and cursor shapes aren't here.
namespace slash::ui {
namespace {

constexpr std::uint64_t kNever = ~std::uint64_t{0};
// Godot's gui/timers/button_shortcut_feedback_highlight_time.
constexpr double kFeedbackSeconds = 0.2;
constexpr int kMaxNeighborSearch = 512;
constexpr float kNoScore = 1e10F;
constexpr float kEpsilon = 0.00001F; // CMP_EPSILON

// What BaseButton keeps between events (its status), for one button.
struct ButtonStatus {
    bool hovering = false;
    bool press_attempt = false;   // a press started on it and hasn't been released
    bool pressing_inside = false; // and the pointer is still over it
    bool pressed_down_with_focus = false;
    bool disabled = false; // as last seen, to release it once it's disabled
    int touch_index = -1;  // the finger pressing it
    double feedback = 0.0; // seconds its shortcut's highlight has left
};

[[nodiscard]] bool idle(const ButtonStatus& status) {
    return !status.hovering && !status.press_attempt && !status.pressing_inside &&
           !status.pressed_down_with_focus && status.touch_index == -1 && status.feedback <= 0.0;
}

struct GuiState {
    std::uint64_t tick = kNever; // the tick the gui system last ran
    std::vector<UiEvent> events; // that tick's
    std::vector<UiEvent> next;   // emitted between ticks, for the next one
    std::map<std::string, ButtonStatus, std::less<>> buttons;
    std::string key_focus;
    bool hide_focus = false; // focus came from a click, so it isn't drawn
    std::string mouse_focus; // the control a held mouse button pressed
    std::uint16_t mouse_focus_mask = 0;
    std::string mouse_over;
    std::vector<std::string> mouse_over_hierarchy; // it and its parents, outermost first
    std::map<int, std::string> touch_focus;        // the control each finger pressed
    // Sticks holding a focus action, so a held stick moves focus once: (device, axis, action).
    std::set<std::tuple<int, int, std::string>> joy_held;
    std::vector<std::string> presses; // press() calls for the next tick
};

using Handle = std::shared_ptr<GuiState>;

GuiState* state_of(world::World& world) {
    auto* found = world.ecs().ctx().find<Handle>();
    return found != nullptr ? found->get() : nullptr;
}

const GuiState* state_of(const world::World& world) {
    const auto* found = world.ecs().ctx().find<Handle>();
    return found != nullptr ? found->get() : nullptr;
}

// ---- Controls ----

enum class Filter : std::uint8_t { Stop, Pass, Ignore };
enum class Focus : std::uint8_t { None, Click, All };

bool is_button(const world::World& world, entt::entity entity) {
    return world.ecs().any_of<world::Button, world::TextureButton>(entity);
}

bool is_container(const world::World& world, entt::entity entity) {
    return world.ecs()
        .any_of<world::BoxContainer, world::GridContainer, world::MarginContainer,
                world::CenterContainer, world::FlowContainer, world::PanelContainer>(entity);
}

// The mouse filter a control has: its own, or its type's as Godot's constructors set them.
Filter mouse_filter(const world::World& world, entt::entity entity) {
    const auto* control = world.ecs().try_get<world::Control>(entity);
    if (control != nullptr && !control->mouse_filter.empty()) {
        if (control->mouse_filter == "ignore") {
            return Filter::Ignore;
        }
        return control->mouse_filter == "pass" ? Filter::Pass : Filter::Stop;
    }
    const std::vector<std::string_view> classes = control_classes(world, entity);
    const std::string_view type = classes.empty() ? "Control" : classes.front();
    if (type == "Label" || type == "NinePatchRect") {
        return Filter::Ignore;
    }
    if (type == "TextureRect" || (type.ends_with("Container") && type != "PanelContainer")) {
        return Filter::Pass;
    }
    return Filter::Stop;
}

Focus focus_mode(const world::World& world, entt::entity entity) {
    const auto* control = world.ecs().try_get<world::Control>(entity);
    const std::string_view mode = control != nullptr ? control->focus_mode : std::string_view();
    if (mode == "all") {
        return Focus::All;
    }
    if (mode == "click") {
        return Focus::Click;
    }
    if (mode == "none") {
        return Focus::None;
    }
    return is_button(world, entity) ? Focus::All : Focus::None;
}

// The BaseButton fields Button and TextureButton share.
struct Base {
    const std::vector<std::string>* button_mask = nullptr;
    const std::vector<json::Value>* shortcut = nullptr;
    std::string action_mode;
    std::string button_group;
    bool disabled = false;
    bool toggle_mode = false;
    bool button_pressed = false;
    bool keep_pressed_outside = false;
    bool shortcut_feedback = true;
};

template <typename T>
Base base_from(const T& button) {
    return {.button_mask = &button.button_mask,
            .shortcut = &button.shortcut,
            .action_mode = button.action_mode,
            .button_group = button.button_group,
            .disabled = button.disabled,
            .toggle_mode = button.toggle_mode,
            .button_pressed = button.button_pressed,
            .keep_pressed_outside = button.keep_pressed_outside,
            .shortcut_feedback = button.shortcut_feedback};
}

std::optional<Base> base_of(const world::World& world, entt::entity entity) {
    if (!world.valid(entity)) {
        return std::nullopt;
    }
    const auto& ecs = world.ecs();
    if (const auto* button = ecs.try_get<world::Button>(entity); button != nullptr) {
        return base_from(*button);
    }
    if (const auto* button = ecs.try_get<world::TextureButton>(entity); button != nullptr) {
        return base_from(*button);
    }
    return std::nullopt;
}

void set_button_pressed(world::World& world, entt::entity entity, bool pressed) {
    auto& ecs = world.ecs();
    if (auto* button = ecs.try_get<world::Button>(entity); button != nullptr) {
        button->button_pressed = pressed;
    } else if (auto* texture = ecs.try_get<world::TextureButton>(entity); texture != nullptr) {
        texture->button_pressed = pressed;
    }
}

// Controls' parents as the layout links them, the first control to list a child being its
// parent, for what needs no rects: whether a control is visible.
class Parents {
public:
    explicit Parents(const world::World& world) : world_(&world) {
        std::vector<entt::entity> controls;
        for (const auto [entity, control] : world.ecs().view<world::Control>().each()) {
            controls.push_back(entity);
        }
        std::ranges::sort(controls, [](entt::entity a, entt::entity b) {
            return entt::to_entity(a) < entt::to_entity(b);
        });
        for (const entt::entity entity : controls) {
            for (const entt::entity child : world.ecs().get<world::Control>(entity).children) {
                if (child != entity && world.valid(child) && is_control(world, child)) {
                    parents_.try_emplace(child, entity);
                }
            }
        }
    }

    [[nodiscard]] bool visible_in_tree(entt::entity entity) const {
        std::unordered_set<entt::entity> seen;
        for (entt::entity at = entity; at != entt::null && seen.insert(at).second;) {
            const auto* control = world_->ecs().try_get<world::Control>(at);
            if (control != nullptr && !control->visible) {
                return false;
            }
            const auto found = parents_.find(at);
            at = found != parents_.end() ? found->second : entt::null;
        }
        return true;
    }

private:
    const world::World* world_;
    std::unordered_map<entt::entity, entt::entity> parents_;
};

// ---- Events ----

bool is_pressed(const input::Event& event) {
    switch (event.type) {
    case input::EventType::Key:
    case input::EventType::MouseButton:
    case input::EventType::ScreenTouch:
    case input::EventType::JoypadButton:
    case input::EventType::Action:
        return event.pressed;
    case input::EventType::JoypadMotion:
        return std::abs(event.axis_value) >= 0.5F;
    case input::EventType::MouseMotion:
    case input::EventType::ScreenDrag:
        break;
    }
    return false;
}

bool is_pointer(const input::Event& event) {
    return event.type == input::EventType::MouseButton ||
           event.type == input::EventType::MouseMotion ||
           event.type == input::EventType::ScreenTouch ||
           event.type == input::EventType::ScreenDrag;
}

bool is_scroll(const input::Event& event) {
    using input::MouseButton;
    return event.type == input::EventType::MouseButton &&
           (event.button == MouseButton::WheelUp || event.button == MouseButton::WheelDown ||
            event.button == MouseButton::WheelLeft || event.button == MouseButton::WheelRight);
}

std::uint16_t mask_bit(input::MouseButton button) {
    const auto index = static_cast<int>(button);
    return index > 0 ? static_cast<std::uint16_t>(1U << static_cast<unsigned>(index - 1)) : 0;
}

std::string_view mask_name(input::MouseButton button) {
    switch (button) {
    case input::MouseButton::Left:
        return "left";
    case input::MouseButton::Right:
        return "right";
    case input::MouseButton::Middle:
        return "middle";
    default:
        return {};
    }
}

// Records a UI event and emits it as a world event: in this tick's list while the gui system
// has run this tick, else in the next tick's.
void emit(world::World& world, GuiState& state, std::string_view kind, const std::string& id,
          std::optional<bool> toggled = std::nullopt) {
    if (id.empty()) {
        return;
    }
    const UiEvent event{.kind = std::string(kind), .id = id, .toggled_on = toggled.value_or(false)};
    (state.tick == world.tick() ? state.events : state.next).push_back(event);
    json::Value data;
    if (toggled) {
        data = json::Object{{"toggled_on", *toggled}};
    }
    world.emit(std::string(kind), id, std::move(data));
}

// A button's status, made with its disabled state as it is now.
ButtonStatus& status_of(const world::World& world, GuiState& state, entt::entity entity) {
    const std::string id(world.id_of(entity));
    auto [found, made] = state.buttons.try_emplace(id);
    if (made) {
        const std::optional<Base> base = base_of(world, entity);
        found->second.disabled = base && base->disabled;
    }
    return found->second;
}

// ---- Focus ----

// BaseButton's NOTIFICATION_FOCUS_EXIT.
void button_focus_exit(world::World& world, GuiState& state, const std::string& id) {
    const auto found = state.buttons.find(id);
    if (found == state.buttons.end()) {
        return;
    }
    ButtonStatus& status = found->second;
    if (status.press_attempt) {
        status.press_attempt = false;
        status.touch_index = -1;
    }
    if (status.pressed_down_with_focus) {
        status.pressed_down_with_focus = false;
        emit(world, state, "button_up", id);
    }
}

void release(world::World& world, GuiState& state) {
    if (state.key_focus.empty()) {
        return;
    }
    const std::string old = std::move(state.key_focus);
    state.key_focus.clear();
    emit(world, state, "focus_exited", old);
    button_focus_exit(world, state, old);
}

// Viewport::_gui_control_grab_focus.
void grab(world::World& world, GuiState& state, const std::string& id, bool hide) {
    if (state.key_focus == id) {
        state.hide_focus = hide;
        return;
    }
    release(world, state);
    state.key_focus = id;
    state.hide_focus = hide;
    emit(world, state, "focus_entered", id);
}

// ---- Buttons ----

// BaseButton::_unpress_group: a toggle button in a group stays pressed, and the group's other
// buttons are released.
void unpress_group(world::World& world, GuiState& state, entt::entity entity) {
    const std::optional<Base> base = base_of(world, entity);
    if (!base || base->button_group.empty()) {
        return;
    }
    if (base->toggle_mode) {
        set_button_pressed(world, entity, true);
    }
    std::vector<entt::entity> group;
    const auto collect = [&](const auto& view) {
        for (const auto [other, button] : view.each()) {
            if (other != entity && button.button_group == base->button_group) {
                group.push_back(other);
            }
        }
    };
    collect(world.ecs().view<world::Button>());
    collect(world.ecs().view<world::TextureButton>());
    std::ranges::sort(group, [](entt::entity a, entt::entity b) {
        return entt::to_entity(a) < entt::to_entity(b);
    });
    for (const entt::entity other : group) {
        const std::optional<Base> each = base_of(world, other);
        if (!each || !each->toggle_mode || !each->button_pressed) {
            continue;
        }
        set_button_pressed(world, other, false);
        if (!each->disabled) {
            emit(world, state, "toggled", std::string(world.id_of(other)), false);
        }
    }
}

// Toggles a toggle button, or just presses one that isn't, as the end of a click and
// BaseButton::press do.
void fire(world::World& world, GuiState& state, entt::entity entity) {
    const std::optional<Base> base = base_of(world, entity);
    if (!base || base->disabled) {
        return;
    }
    const std::string id(world.id_of(entity));
    if (base->toggle_mode) {
        set_button_pressed(world, entity, !base->button_pressed);
        unpress_group(world, state, entity);
        // In a group, the button pressed stays on.
        const std::optional<Base> now = base_of(world, entity);
        emit(world, state, "toggled", id, now && now->button_pressed);
    }
    emit(world, state, "pressed", id);
}

// Whether one of a button's shortcuts matches an event, as Shortcut::matches_event: bindings
// with exactly the event's modifiers, or actions the event is bound to.
bool shortcut_matches(const input::Map* map, const std::vector<json::Value>& shortcut,
                      const input::Event& event) {
    return std::ranges::any_of(shortcut, [&](const json::Value& entry) {
        const json::Value* type = entry.is_object() ? entry.find("type") : nullptr;
        if (type != nullptr && type->is_string() && type->as_string() == "action") {
            const json::Value* action = entry.find("action");
            return map != nullptr && action != nullptr && action->is_string() &&
                   map->find_event(action->as_string(), event, true).has_value();
        }
        if (!input::validate_event(entry, input::Use::Binding).empty()) {
            return false;
        }
        const auto binding = input::parse_event(entry, input::Use::Binding);
        return binding && input::match(binding.value(), event, input::kDefaultDeadzone, true);
    });
}

// ---- Rects ----

struct Rect2 {
    Vec2 position;
    Vec2 size;

    [[nodiscard]] Vec2 end() const { return {position.x + size.x, position.y + size.y}; }
    [[nodiscard]] Vec2 center() const {
        return {position.x + (size.x * 0.5F), position.y + (size.y * 0.5F)};
    }
};

float dot(Vec2 a, Vec2 b) {
    return (a.x * b.x) + (a.y * b.y);
}

float cross(Vec2 a, Vec2 b) {
    return (a.x * b.y) - (a.y * b.x);
}

std::optional<Transform> inverse(const Transform& t) {
    const float det = (t.a * t.d) - (t.b * t.c);
    if (det == 0.0F) {
        return std::nullopt;
    }
    const float inv = 1.0F / det;
    Transform r{.a = t.d * inv, .b = -t.b * inv, .c = -t.c * inv, .d = t.a * inv};
    if (t.translation_only()) {
        r = {};
    }
    r.tx = -((r.a * t.tx) + (r.c * t.ty));
    r.ty = -((r.b * t.tx) + (r.d * t.ty));
    return r;
}

// Control::get_global_rect: its position, and its size scaled.
Rect2 global_rect(const Placed& placed) {
    const Transform& t = placed.transform;
    if (t.translation_only()) {
        return {{t.tx, t.ty}, placed.size};
    }
    const float det = (t.a * t.d) - (t.b * t.c);
    const Vec2 scale{std::hypot(t.a, t.b), (det < 0.0F ? -1.0F : 1.0F) * std::hypot(t.c, t.d)};
    return {{t.tx, t.ty}, {placed.size.x * scale.x, placed.size.y * scale.y}};
}

// ---- Sending events to controls ----

// One tick's work: the layout to find controls in, and whether the event being sent was handled.
class Gui {
public:
    Gui(world::World& world, GuiState& state, const Layout& layout)
        : world_(world), state_(state), layout_(layout), map_(input::map(std::as_const(world))) {
        for (const entt::entity entity : layout.tree) {
            const Placed* placed = layout.find(entity);
            if (placed == nullptr) {
                continue;
            }
            if (placed->parent == entt::null) {
                roots_.push_back(entity);
            } else {
                kids_[placed->parent].push_back(entity);
            }
        }
    }

    // Viewport::push_input, from the mouse over on.
    void send(const input::Event& event) {
        handled_ = false;
        if (event.type == input::EventType::MouseButton ||
            event.type == input::EventType::MouseMotion) {
            update_mouse_over(event.position);
        }
        switch (event.type) {
        case input::EventType::MouseButton:
            mouse_button(event);
            break;
        case input::EventType::MouseMotion:
            mouse_motion(event);
            break;
        case input::EventType::ScreenTouch:
            screen_touch(event);
            break;
        case input::EventType::ScreenDrag:
            screen_drag(event);
            break;
        case input::EventType::Key:
        case input::EventType::JoypadButton:
        case input::EventType::JoypadMotion:
        case input::EventType::Action:
            action_event(event);
            break;
        }
        if (!handled_ &&
            (event.type == input::EventType::Key || event.type == input::EventType::JoypadButton)) {
            shortcut_input(event);
        }
    }

private:
    [[nodiscard]] std::string id(entt::entity entity) const {
        return std::string(world_.id_of(entity));
    }
    [[nodiscard]] entt::entity find(const std::string& id) const {
        return id.empty() ? entt::null : world_.find(id);
    }
    [[nodiscard]] const Placed* placed(entt::entity entity) const {
        return entity == entt::null ? nullptr : layout_.find(entity);
    }
    [[nodiscard]] entt::entity parent(entt::entity entity) const {
        const Placed* found = placed(entity);
        return found != nullptr ? found->parent : entt::null;
    }
    [[nodiscard]] bool visible(entt::entity entity) const {
        const Placed* found = placed(entity);
        return found != nullptr && found->visible;
    }
    [[nodiscard]] bool focusable(entt::entity entity) const {
        return visible(entity) && focus_mode(world_, entity) != Focus::None;
    }
    [[nodiscard]] const std::vector<entt::entity>& kids(entt::entity entity) const {
        static const std::vector<entt::entity> kNone;
        const auto found = kids_.find(entity);
        return found != kids_.end() ? found->second : kNone;
    }

    // A viewport point in a control's rect, or nullopt if the control has no area.
    [[nodiscard]] std::optional<Vec2> to_local(entt::entity entity, Vec2 point) const {
        const Placed* found = placed(entity);
        if (found == nullptr) {
            return std::nullopt;
        }
        const auto local = inverse(found->transform);
        return local ? std::optional<Vec2>(local->apply(point)) : std::nullopt;
    }

    // Control::has_point, or BaseButton's, which reaches click_margin past its rect.
    [[nodiscard]] bool has_point(entt::entity entity, std::optional<Vec2> local) const {
        const Placed* found = placed(entity);
        if (found == nullptr || !local) {
            return false;
        }
        float margin = 0.0F;
        const bool button = is_button(world_, entity);
        if (const auto state = layout_.buttons.find(entity); state != layout_.buttons.end()) {
            margin = state->second.click_margin;
        }
        const Rect2 rect{{-margin, -margin},
                         {found->size.x + (2.0F * margin), found->size.y + (2.0F * margin)}};
        if (button && (rect.size.x <= 0.0F || rect.size.y <= 0.0F)) {
            return false;
        }
        return local->x >= rect.position.x && local->y >= rect.position.y &&
               local->x < rect.end().x && local->y < rect.end().y;
    }

    // Viewport::gui_find_control: the last control in tree order under a point that the mouse
    // doesn't pass by.
    [[nodiscard]] entt::entity find_control(Vec2 point) const {
        for (const entt::entity root : std::views::reverse(roots_)) {
            if (!visible(root)) {
                continue;
            }
            if (const entt::entity found = find_at(root, point); found != entt::null) {
                return found;
            }
        }
        return entt::null;
    }

    [[nodiscard]] entt::entity find_at(entt::entity entity, Vec2 point) const {
        const Placed* found = placed(entity);
        if (found == nullptr || !found->visible) {
            return entt::null;
        }
        const auto local = inverse(found->transform);
        if (!local) {
            return entt::null;
        }
        const std::vector<entt::entity>& children = kids(entity);
        for (const entt::entity child : std::views::reverse(children)) {
            if (const entt::entity hit = find_at(child, point); hit != entt::null) {
                return hit;
            }
        }
        if (mouse_filter(world_, entity) == Filter::Ignore || world_.id_of(entity).empty()) {
            return entt::null;
        }
        return has_point(entity, local->apply(point)) ? entity : entt::null;
    }

    // Viewport::_update_mouse_over: mouse_exited for the controls the mouse left, from the
    // innermost, then mouse_entered for the ones it is over, from the outermost.
    void update_mouse_over(Vec2 point) {
        const entt::entity over = find_control(point);
        const std::string over_id = over != entt::null ? id(over) : std::string();
        auto& hierarchy = state_.mouse_over_hierarchy;
        if (over_id == state_.mouse_over && (over != entt::null || hierarchy.empty())) {
            return;
        }
        std::string common;
        std::vector<std::string> entering;
        for (entt::entity at = over; at != entt::null; at = parent(at)) {
            const Filter filter = mouse_filter(world_, at);
            if (filter != Filter::Ignore && !world_.id_of(at).empty()) {
                std::string at_id = id(at);
                if (std::ranges::find(hierarchy, at_id) != hierarchy.end()) {
                    common = std::move(at_id);
                    break;
                }
                entering.push_back(std::move(at_id));
            }
            if (filter == Filter::Stop) {
                break;
            }
        }
        if (!state_.mouse_over.empty() || !hierarchy.empty()) {
            drop_mouse_over(world_, state_, common);
        }
        if (over != entt::null) {
            state_.mouse_over = over_id;
            for (const std::string& each : std::views::reverse(entering)) {
                hierarchy.push_back(each);
                const entt::entity entity = find(each);
                if (is_button(world_, entity)) {
                    status_of(world_, state_, entity).hovering = true;
                }
                emit(world_, state_, "mouse_entered", each);
            }
        }
    }

public:
    // Viewport::_drop_mouse_over: the controls the mouse is over after until leave it.
    static void drop_mouse_over(world::World& world, GuiState& state, const std::string& until) {
        state.mouse_over.clear();
        auto& hierarchy = state.mouse_over_hierarchy;
        std::size_t keep = 0;
        if (!until.empty()) {
            const auto found = std::ranges::find(hierarchy, until);
            keep = found != hierarchy.end()
                       ? static_cast<std::size_t>(found - hierarchy.begin()) + 1
                       : 0;
        }
        while (hierarchy.size() > keep) {
            const std::string left = std::move(hierarchy.back());
            hierarchy.pop_back();
            if (const auto found = state.buttons.find(left); found != state.buttons.end()) {
                found->second.hovering = false;
            }
            if (world.find(left) != entt::null) {
                emit(world, state, "mouse_exited", left);
            }
        }
    }

private:
    // Viewport::_gui_call_input: the control, then its parents until one stops the pointer.
    void call_input(entt::entity entity, const input::Event& event) {
        const bool pointer = is_pointer(event);
        const bool scroll = is_scroll(event);
        for (entt::entity at = entity; at != entt::null; at = parent(at)) {
            const Filter filter = mouse_filter(world_, at);
            if (filter != Filter::Ignore) {
                gui_input(at, event);
            }
            if (filter == Filter::Stop && pointer && !scroll) {
                handled_ = true;
                break;
            }
            if (handled_) {
                break;
            }
        }
    }

    void gui_input(entt::entity entity, const input::Event& event) {
        if (is_button(world_, entity) && !world_.id_of(entity).empty()) {
            button_input(entity, event);
        }
    }

    [[nodiscard]] bool is_action(const input::Event& event, std::string_view action,
                                 bool exact) const {
        return map_ != nullptr && map_->find_event(action, event, exact).has_value();
    }

    [[nodiscard]] bool action_pressed(const input::Event& event, std::string_view action,
                                      bool allow_echo, bool exact) const {
        if (map_ == nullptr) {
            return false;
        }
        const auto found = map_->find_event(action, event, exact);
        return found && found->match.pressed && (allow_echo || !event.echo);
    }

    // BaseButton::gui_input.
    void button_input(entt::entity entity, const input::Event& event) {
        if (event.device == input::kEmulatedDevice) {
            return;
        }
        const std::optional<Base> base = base_of(world_, entity);
        if (!base) {
            return;
        }
        ButtonStatus& status = status_of(world_, state_, entity);
        if (event.type == input::EventType::ScreenTouch) {
            if (status.touch_index == -1) {
                if (event.pressed) {
                    status.touch_index = event.index;
                    status.press_attempt = true;
                    status.pressing_inside = has_point(entity, to_local(entity, event.position));
                    on_action_event(entity, event);
                }
            } else if (event.index == status.touch_index && !event.pressed) {
                status.touch_index = -1;
                on_action_event(entity, event);
            }
        }
        if (event.type == input::EventType::ScreenDrag) {
            ButtonStatus& now = status_of(world_, state_, entity);
            if (event.index == now.touch_index && now.press_attempt) {
                now.pressing_inside = has_point(entity, to_local(entity, event.position));
            }
        }
        const bool ui_accept = is_action(event, "ui_accept", true) && !event.echo;
        const bool masked = event.type == input::EventType::MouseButton &&
                            !mask_name(event.button).empty() &&
                            std::ranges::find(*base->button_mask, mask_name(event.button)) !=
                                base->button_mask->end();
        if (masked || ui_accept) {
            on_action_event(entity, event);
        } else if (event.type == input::EventType::MouseMotion) {
            ButtonStatus& now = status_of(world_, state_, entity);
            if (now.press_attempt) {
                now.pressing_inside = has_point(entity, to_local(entity, event.position));
            }
        }
    }

    // BaseButton::on_action_event.
    void on_action_event(entt::entity entity, const input::Event& event) {
        const std::optional<Base> base = base_of(world_, entity);
        if (!base) {
            return;
        }
        const std::string button_id = id(entity);
        const bool pressed = is_pressed(event);
        const bool accept_event = event.type != input::EventType::MouseButton &&
                                  event.type != input::EventType::ScreenTouch &&
                                  event.type != input::EventType::ScreenDrag;
        {
            ButtonStatus& status = status_of(world_, state_, entity);
            const bool touch_press_inside = event.type == input::EventType::ScreenTouch &&
                                            event.pressed && status.pressing_inside;
            if (pressed && (accept_event || status.hovering || touch_press_inside)) {
                status.press_attempt = true;
                status.pressing_inside = true;
                if (!status.pressed_down_with_focus) {
                    status.pressed_down_with_focus = true;
                    if (!base->disabled) {
                        emit(world_, state_, "button_down", button_id);
                    }
                }
            }
        }
        ButtonStatus& status = status_of(world_, state_, entity);
        const bool on_press = base->action_mode == "press";
        if (status.press_attempt && status.pressing_inside && pressed == on_press) {
            if (base->toggle_mode && on_press) {
                status.press_attempt = false;
                status.pressing_inside = false;
                status.touch_index = -1;
            }
            fire(world_, state_, entity);
        }
        if (!pressed) {
            ButtonStatus& now = status_of(world_, state_, entity);
            now.press_attempt = false;
            now.pressing_inside = false;
            if (now.pressed_down_with_focus) {
                now.pressed_down_with_focus = false;
                if (!base->disabled) {
                    emit(world_, state_, "button_up", button_id);
                }
            }
        }
    }

    // The mouse button part of Viewport::_gui_input_event.
    void mouse_button(const input::Event& event) {
        const std::uint16_t bit = mask_bit(event.button);
        if (event.pressed) {
            if (state_.mouse_focus_mask != 0 && (state_.mouse_focus_mask & bit) == 0) {
                if (state_.mouse_focus.empty()) {
                    return;
                }
                state_.mouse_focus_mask |= bit;
            } else {
                const entt::entity over = find_control(event.position);
                if (over == entt::null) {
                    // A click on nothing hides focus, though the focus stays.
                    if (!state_.key_focus.empty() && event.button == input::MouseButton::Left) {
                        state_.hide_focus = true;
                    }
                    return;
                }
                state_.mouse_focus = id(over);
                state_.mouse_focus_mask |= bit;
            }
            const entt::entity focus = find(state_.mouse_focus);
            if (event.button == input::MouseButton::Left) {
                // The first focusable control up from the one clicked takes focus, hidden.
                for (entt::entity at = focus; at != entt::null; at = parent(at)) {
                    if (focusable(at)) {
                        const std::string at_id = id(at);
                        if (!at_id.empty() &&
                            std::ranges::find(state_.mouse_over_hierarchy, at_id) !=
                                state_.mouse_over_hierarchy.end()) {
                            grab(world_, state_, at_id, true);
                        }
                        break;
                    }
                    if (mouse_filter(world_, at) == Filter::Stop) {
                        break;
                    }
                }
            }
            if (focus != entt::null) {
                call_input(focus, event);
            }
            return;
        }
        state_.mouse_focus_mask &= static_cast<std::uint16_t>(~bit);
        if (state_.mouse_focus.empty()) {
            return;
        }
        const entt::entity focus = find(state_.mouse_focus);
        if (state_.mouse_focus_mask == 0) {
            state_.mouse_focus.clear();
        }
        if (focus != entt::null) {
            call_input(focus, event);
        }
    }

    void mouse_motion(const input::Event& event) {
        const entt::entity over =
            !state_.mouse_focus.empty() ? find(state_.mouse_focus) : find_control(event.position);
        if (over != entt::null) {
            call_input(over, event);
        }
    }

    void screen_touch(const input::Event& event) {
        if (event.pressed) {
            const entt::entity over = find_control(event.position);
            if (over != entt::null) {
                state_.touch_focus[event.index] = id(over);
                call_input(over, event);
            }
            return;
        }
        const auto found = state_.touch_focus.find(event.index);
        if (found == state_.touch_focus.end()) {
            return;
        }
        const entt::entity over = find(found->second);
        state_.touch_focus.erase(found);
        if (over != entt::null) {
            call_input(over, event);
        }
    }

    void screen_drag(const input::Event& event) {
        const auto found = state_.touch_focus.find(event.index);
        const entt::entity over =
            found != state_.touch_focus.end() ? find(found->second) : find_control(event.position);
        if (over != entt::null) {
            call_input(over, event);
        }
    }

    // Keys, gamepads and actions: the control with focus first, then the ui_* actions move
    // focus.
    void action_event(const input::Event& event) {
        // A stick moves focus once per push, as is_action_just_pressed_by_event.
        std::set<std::string_view> just_pushed;
        static constexpr std::array<std::string_view, 6> kMoves{
            "ui_focus_next", "ui_focus_prev", "ui_up", "ui_left", "ui_right", "ui_down"};
        if (event.type == input::EventType::JoypadMotion && map_ != nullptr) {
            for (const std::string_view action : kMoves) {
                const auto found = map_->find_event(action, event, false);
                if (!found) {
                    continue;
                }
                const auto key = std::make_tuple(event.device, static_cast<int>(event.axis),
                                                 std::string(action));
                if (!found->match.pressed) {
                    state_.joy_held.erase(key);
                } else if (state_.joy_held.insert(key).second) {
                    just_pushed.insert(action);
                }
            }
        }
        if (const entt::entity focus = find(state_.key_focus); focus != entt::null) {
            gui_input(focus, event);
            if (handled_) {
                return;
            }
        }
        entt::entity from = find(state_.key_focus);
        if (from == entt::null) {
            const auto first =
                std::ranges::find_if(roots_, [this](entt::entity root) { return visible(root); });
            from = first != roots_.end() ? *first : entt::null;
        }
        if (from == entt::null || !is_pressed(event)) {
            return;
        }
        entt::entity next = entt::null;
        bool show_focus = false;
        for (const std::string_view action : kMoves) {
            const bool moved =
                event.type == input::EventType::JoypadMotion
                    ? action_pressed(event, action, false, false) && just_pushed.contains(action)
                    : action_pressed(event, action, true, true);
            if (!moved) {
                continue;
            }
            show_focus = true;
            if (action == "ui_focus_next") {
                next = find_next_valid_focus(from);
            } else if (action == "ui_focus_prev") {
                next = find_prev_valid_focus(from);
            } else if (action == "ui_up") {
                next = focus_neighbor(from, 1, 0);
            } else if (action == "ui_left") {
                next = focus_neighbor(from, 0, 0);
            } else if (action == "ui_right") {
                next = focus_neighbor(from, 2, 0);
            } else {
                next = focus_neighbor(from, 3, 0);
            }
        }
        if (next != entt::null && !world_.id_of(next).empty()) {
            grab(world_, state_, id(next), false);
            handled_ = true;
        } else if (show_focus && state_.hide_focus && !state_.key_focus.empty()) {
            // Shown even if it didn't move, as feedback.
            state_.hide_focus = false;
        }
    }

    // BaseButton::shortcut_input, for each button from the last in tree order until one takes
    // the event.
    void shortcut_input(const input::Event& event) {
        if (!is_pressed(event) || event.echo) {
            return;
        }
        for (const entt::entity entity : std::views::reverse(layout_.tree)) {
            const std::optional<Base> base = base_of(world_, entity);
            if (!base || base->shortcut->empty() || world_.id_of(entity).empty() ||
                !visible(entity) || !shortcut_matches(map_, *base->shortcut, event)) {
                continue;
            }
            fire(world_, state_, entity);
            handled_ = true;
            if (base->shortcut_feedback) {
                status_of(world_, state_, entity).feedback = kFeedbackSeconds;
            }
            return;
        }
    }

    // ---- Moving focus ----

    // _next_control: the next visible sibling, or its parent's.
    [[nodiscard]] entt::entity next_control(entt::entity from) const {
        const entt::entity up = parent(from);
        if (up == entt::null) {
            return entt::null;
        }
        const std::vector<entt::entity>& siblings = kids(up);
        auto at = std::ranges::find(siblings, from);
        if (at != siblings.end()) {
            for (++at; at != siblings.end(); ++at) {
                if (visible(*at)) {
                    return *at;
                }
            }
        }
        return next_control(up);
    }

    // _prev_control: the last visible control in its subtree.
    [[nodiscard]] entt::entity prev_control(entt::entity from) const {
        const std::vector<entt::entity>& children = kids(from);
        for (const entt::entity child : std::views::reverse(children)) {
            if (visible(child)) {
                return prev_control(child);
            }
        }
        return from;
    }

    [[nodiscard]] entt::entity root_of(entt::entity entity) const {
        std::unordered_set<entt::entity> seen;
        while (parent(entity) != entt::null && seen.insert(entity).second) {
            entity = parent(entity);
        }
        return entity;
    }

    [[nodiscard]] std::size_t root_index(entt::entity root) const {
        return static_cast<std::size_t>(std::ranges::find(roots_, root) - roots_.begin());
    }

    [[nodiscard]] const world::Control& control_of(entt::entity entity) const {
        static const world::Control kDefault{};
        const auto* control = world_.ecs().try_get<world::Control>(entity);
        return control != nullptr ? *control : kDefault;
    }

    // A focus field's control if it can take focus, else nullopt to search on; entt::null if
    // it names no control.
    [[nodiscard]] std::optional<entt::entity> named(entt::entity field) const {
        if (field == entt::null) {
            return std::nullopt;
        }
        if (!world_.valid(field) || !is_control(world_, field)) {
            return entt::entity{entt::null};
        }
        return focusable(field) ? std::optional(field) : std::nullopt;
    }

    // Control::find_next_valid_focus: focus_next, else the next control in tree order with
    // focus_mode "all", going round the roots.
    [[nodiscard]] entt::entity find_next_valid_focus(entt::entity self) const {
        if (const auto chosen = named(control_of(self).focus_next)) {
            return *chosen;
        }
        entt::entity from = self;
        std::unordered_set<entt::entity> checked{from};
        std::optional<std::size_t> window_next;
        while (true) {
            entt::entity next_child = entt::null;
            for (const entt::entity child : kids(from)) {
                if (visible(child)) {
                    next_child = child;
                    break;
                }
            }
            if (next_child == entt::null) {
                next_child = next_control(from);
                if (next_child == entt::null) {
                    // Round the roots, from the one this control is in.
                    const entt::entity root = root_of(self);
                    if (!window_next) {
                        window_next = root_index(root);
                    }
                    next_child = root;
                    const std::size_t count = roots_.size();
                    for (std::size_t i = 1; i <= count; ++i) {
                        const std::size_t at = (*window_next + i) % count;
                        if (visible(roots_[at])) {
                            window_next = at;
                            next_child = roots_[at];
                            break;
                        }
                    }
                }
            }
            if (next_child == entt::null) {
                return entt::null;
            }
            if (focus_mode(world_, next_child) == Focus::All) {
                return next_child;
            }
            if (!checked.insert(next_child).second) {
                return entt::null;
            }
            from = next_child;
        }
    }

    // Control::find_prev_valid_focus.
    [[nodiscard]] entt::entity find_prev_valid_focus(entt::entity self) const {
        if (const auto chosen = named(control_of(self).focus_previous)) {
            return *chosen;
        }
        entt::entity from = self;
        std::unordered_set<entt::entity> checked{from};
        std::optional<std::size_t> window_prev;
        while (true) {
            entt::entity prev_child = entt::null;
            const entt::entity up = parent(from);
            if (up == entt::null) {
                if (!window_prev) {
                    window_prev = root_index(from);
                }
                const std::size_t count = roots_.size();
                for (std::size_t i = 1; i <= count; ++i) {
                    const std::size_t at = (*window_prev + (count * 2) - i) % count;
                    if (visible(roots_[at])) {
                        window_prev = at;
                        prev_child = prev_control(roots_[at]);
                        break;
                    }
                }
                if (prev_child == entt::null) {
                    prev_child = prev_control(from);
                }
            } else {
                const std::vector<entt::entity>& siblings = kids(up);
                auto at = std::ranges::find(siblings, from);
                while (at != siblings.begin()) {
                    --at;
                    if (visible(*at)) {
                        prev_child = *at;
                        break;
                    }
                }
                prev_child = prev_child == entt::null ? up : prev_control(prev_child);
            }
            if (focus_mode(world_, prev_child) == Focus::All) {
                return prev_child;
            }
            if (!checked.insert(prev_child).second) {
                return entt::null;
            }
            from = prev_child;
        }
    }

    // Control::_get_focus_neighbor: the side's focus_neighbor field, else the closest control
    // with focus_mode "all" that way in the same root, as Godot's legacy strategy measures.
    // Sides are left, top, right and bottom.
    [[nodiscard]] entt::entity focus_neighbor(entt::entity from, std::size_t side,
                                              int count) const {
        if (count >= kMaxNeighborSearch) {
            return entt::null;
        }
        const world::Control& control = control_of(from);
        const std::array<entt::entity, 4> fields{
            control.focus_neighbor_left, control.focus_neighbor_top, control.focus_neighbor_right,
            control.focus_neighbor_bottom};
        if (const entt::entity field = fields.at(side); field != entt::null) {
            if (!world_.valid(field) || !is_control(world_, field)) {
                return entt::null;
            }
            return focusable(field) ? field : focus_neighbor(field, side, count + 1);
        }
        const Placed* found = placed(from);
        if (found == nullptr) {
            return entt::null;
        }
        static constexpr std::array<Vec2, 4> kDirections{Vec2{-1.0F, 0.0F}, Vec2{0.0F, -1.0F},
                                                         Vec2{1.0F, 0.0F}, Vec2{0.0F, 1.0F}};
        const Vec2 dir = kDirections.at(side);
        const Rect2 rect = global_rect(*found);
        const float min = std::max(dot(dir, rect.position), dot(dir, rect.end()));
        float score = kNoScore;
        entt::entity closest = entt::null;
        find_neighbor(from, dir, root_of(from), rect, min, score, closest);
        return closest;
    }

    // Control::_window_find_focus_neighbor.
    void find_neighbor(entt::entity self, Vec2 dir, entt::entity at, const Rect2& rect, float min,
                       float& score, entt::entity& closest) const {
        const bool in_container = is_container(world_, at) && contains(at, self);
        if (at != self && focus_mode(world_, at) == Focus::All && !in_container &&
            !world_.id_of(at).empty()) {
            const float each = legacy_score(dir, at, rect, min);
            if (each != kNoScore) {
                if (each < score || closest == entt::null) {
                    score = each;
                    closest = at;
                } else if (each == score) {
                    // The one most in line with the direction.
                    const Vec2 center = rect.center();
                    const Vec2 a = global_rect(*placed(at)).center();
                    const Vec2 b = global_rect(*placed(closest)).center();
                    if (std::abs(cross(dir, {a.x - center.x, a.y - center.y})) <
                        std::abs(cross(dir, {b.x - center.x, b.y - center.y}))) {
                        closest = at;
                    }
                }
            }
        }
        for (const entt::entity child : kids(at)) {
            if (visible(child)) {
                find_neighbor(self, dir, child, rect, min, score, closest);
            }
        }
    }

    // Control::_focus_strategy_legacy: the squared gap between the rects, for a candidate
    // that reaches past the control's far edge.
    [[nodiscard]] float legacy_score(Vec2 dir, entt::entity candidate, const Rect2& rect,
                                     float min) const {
        const Rect2 r_c = global_rect(*placed(candidate));
        const float max = std::max(dot(dir, r_c.position), dot(dir, r_c.end()));
        if (max <= min + kEpsilon) {
            return kNoScore;
        }
        const Vec2 a = r_c.center();
        const Vec2 b = rect.center();
        const Vec2 gap{std::abs(a.x - b.x) - (0.5F * r_c.size.x) - (0.5F * rect.size.x),
                       std::abs(a.y - b.y) - (0.5F * r_c.size.y) - (0.5F * rect.size.y)};
        float squared = 0.0F;
        if (gap.x > 0.0F) {
            squared += gap.x * gap.x;
        }
        if (gap.y > 0.0F) {
            squared += gap.y * gap.y;
        }
        return squared;
    }

    [[nodiscard]] bool contains(entt::entity ancestor, entt::entity entity) const {
        std::unordered_set<entt::entity> seen;
        for (entt::entity at = parent(entity); at != entt::null && seen.insert(at).second;
             at = parent(at)) {
            if (at == ancestor) {
                return true;
            }
        }
        return false;
    }

    world::World& world_;
    GuiState& state_;
    const Layout& layout_;
    const input::Map* map_;
    std::vector<entt::entity> roots_;
    std::unordered_map<entt::entity, std::vector<entt::entity>> kids_;
    bool handled_ = false;
};

// Drops what refers to controls that went away, were hidden or can no longer have it, as
// Viewport::_gui_hide_control and _gui_remove_control, BaseButton's visibility and
// set_disabled do, and runs shortcut highlights down.
void upkeep(world::World& world, GuiState& state, double tick_seconds) {
    const bool tracking = !state.key_focus.empty() || !state.mouse_focus.empty() ||
                          !state.mouse_over_hierarchy.empty() || !state.touch_focus.empty() ||
                          !state.buttons.empty();
    if (!tracking) {
        return;
    }
    const Parents parents(world);
    const auto shown = [&](const std::string& id) {
        const entt::entity entity = world.find(id);
        return entity != entt::null && is_control(world, entity) && parents.visible_in_tree(entity);
    };
    // A control that was destroyed goes quietly: its entity.destroyed event says so.
    if (!state.key_focus.empty()) {
        const entt::entity focus = world.find(state.key_focus);
        if (focus == entt::null || !is_control(world, focus)) {
            state.key_focus.clear();
        } else if (!shown(state.key_focus) || focus_mode(world, focus) == Focus::None) {
            release(world, state);
        }
    }
    if (!state.mouse_focus.empty() && !shown(state.mouse_focus)) {
        state.mouse_focus.clear();
        state.mouse_focus_mask = 0;
    }
    auto& hierarchy = state.mouse_over_hierarchy;
    for (std::size_t i = 0; i < hierarchy.size(); ++i) {
        if (!shown(hierarchy[i])) {
            const std::string until = i > 0 ? hierarchy[i - 1] : std::string();
            Gui::drop_mouse_over(world, state, until);
            break;
        }
    }
    std::erase_if(state.touch_focus, [&](const auto& each) { return !shown(each.second); });
    for (auto it = state.buttons.begin(); it != state.buttons.end();) {
        const std::string& id = it->first;
        ButtonStatus& status = it->second;
        const entt::entity entity = world.find(id);
        const std::optional<Base> base =
            entity != entt::null ? base_of(world, entity) : std::nullopt;
        if (!base) {
            it = state.buttons.erase(it);
            continue;
        }
        const bool hidden = !parents.visible_in_tree(entity);
        if (hidden) {
            status.hovering = false;
            status.touch_index = -1;
        }
        if (hidden || (base->disabled && !status.disabled)) {
            status.press_attempt = false;
            status.pressing_inside = false;
            if (status.pressed_down_with_focus) {
                status.pressed_down_with_focus = false;
                emit(world, state, "button_up", id);
            }
        }
        status.disabled = base->disabled;
        if (status.feedback > 0.0) {
            status.feedback -= tick_seconds;
            if (status.feedback <= 1e-9) {
                status.feedback = 0.0;
            }
        }
        ++it;
    }
}

void run(world::World& world, double tick_seconds) {
    GuiState* state = state_of(world);
    if (state == nullptr) {
        return;
    }
    state->tick = world.tick();
    state->events = std::move(state->next);
    state->next.clear();
    upkeep(world, *state, tick_seconds);
    const std::span<const input::Event> events = input::events(std::as_const(world));
    const std::vector<std::string> presses = std::move(state->presses);
    state->presses.clear();
    if (!events.empty()) {
        if (text::Fonts* found = fonts(world); found != nullptr) {
            const Layout placed = layout(world, *found, settings(world).viewport);
            Gui gui(world, *state, placed);
            for (const input::Event& event : events) {
                gui.send(event);
            }
        }
    }
    for (const std::string& id : presses) {
        if (const entt::entity entity = world.find(id); entity != entt::null) {
            fire(world, *state, entity);
        }
    }
    std::erase_if(state->buttons, [](const auto& each) { return idle(each.second); });
}

} // namespace

// ---- Public ----

void install_gui(world::World& world) {
    if (state_of(world) != nullptr) {
        return;
    }
    world.ecs().ctx().emplace<Handle>(std::make_shared<GuiState>());
    world.add_system("gui", run,
                     {.doc = "Sends input events to controls: hover, focus and button presses.",
                      .priority = -900});
}

std::string_view draw_mode_name(DrawMode mode) {
    switch (mode) {
    case DrawMode::Normal:
        return "normal";
    case DrawMode::Pressed:
        return "pressed";
    case DrawMode::Hover:
        return "hover";
    case DrawMode::Disabled:
        return "disabled";
    case DrawMode::HoverPressed:
        return "hover_pressed";
    }
    return "normal";
}

std::span<const UiEvent> events(const world::World& world) {
    const GuiState* state = state_of(world);
    return state != nullptr ? std::span<const UiEvent>(state->events) : std::span<const UiEvent>();
}

bool was_pressed(const world::World& world, entt::entity button) {
    const std::string_view id = world.id_of(button);
    return !id.empty() && std::ranges::any_of(events(world), [&](const UiEvent& event) {
        return event.kind == "pressed" && event.id == id;
    });
}

void press(world::World& world, entt::entity button) {
    GuiState* state = state_of(world);
    if (state != nullptr && !world.id_of(button).empty() && base_of(world, button)) {
        state->presses.emplace_back(world.id_of(button));
    }
}

// BaseButton::get_draw_mode.
DrawMode draw_mode(const world::World& world, entt::entity button) {
    const std::optional<Base> base = base_of(world, button);
    if (!base) {
        return DrawMode::Normal;
    }
    if (base->disabled) {
        return DrawMode::Disabled;
    }
    ButtonStatus status;
    if (const GuiState* state = state_of(world); state != nullptr) {
        if (const auto found = state->buttons.find(world.id_of(button));
            found != state->buttons.end()) {
            status = found->second;
        }
    }
    if (status.feedback > 0.0) {
        return DrawMode::HoverPressed;
    }
    const bool pressed = base->toggle_mode && base->button_pressed;
    if (!status.press_attempt && status.hovering) {
        return pressed ? DrawMode::HoverPressed : DrawMode::Hover;
    }
    bool pressing = pressed;
    if (status.press_attempt) {
        pressing = status.pressing_inside || base->keep_pressed_outside;
        if (pressed) {
            pressing = !pressing;
        }
    }
    return pressing ? DrawMode::Pressed : DrawMode::Normal;
}

entt::entity focus_owner(const world::World& world) {
    const GuiState* state = state_of(world);
    return state != nullptr && !state->key_focus.empty() ? world.find(state->key_focus)
                                                         : entt::null;
}

bool grab_focus(world::World& world, entt::entity control) {
    GuiState* state = state_of(world);
    if (state == nullptr || !world.valid(control) || !is_control(world, control) ||
        world.id_of(control).empty() || focus_mode(world, control) == Focus::None ||
        !Parents(world).visible_in_tree(control)) {
        return false;
    }
    grab(world, *state, std::string(world.id_of(control)), false);
    return true;
}

void release_focus(world::World& world) {
    if (GuiState* state = state_of(world); state != nullptr) {
        release(world, *state);
    }
}

entt::entity hovered(const world::World& world) {
    const GuiState* state = state_of(world);
    return state != nullptr && !state->mouse_over.empty() ? world.find(state->mouse_over)
                                                          : entt::null;
}

bool shows_focus(const world::World& world, entt::entity control) {
    const GuiState* state = state_of(world);
    return state != nullptr && !state->hide_focus && !state->key_focus.empty() &&
           state->key_focus == world.id_of(control);
}

Snapshot save(const world::World& world) {
    const GuiState* state = state_of(world);
    if (state == nullptr) {
        return {};
    }
    return {std::make_shared<const GuiState>(*state)};
}

void restore(world::World& world, const Snapshot& snapshot) {
    GuiState* state = state_of(world);
    if (state == nullptr || snapshot.data == nullptr) {
        return;
    }
    *state = *static_cast<const GuiState*>(snapshot.data.get());
}

void check_buttons(const world::World& world, std::vector<Problem>& problems) {
    const auto& ecs = world.ecs();
    const input::Map* map = input::map(world);
    const input::Map defaults = input::Map::with_defaults();
    const input::Map& actions = map != nullptr ? *map : defaults;
    std::vector<std::pair<std::string, entt::entity>> entities;
    for (const auto [entity] : ecs.storage<entt::entity>()->each()) {
        if (is_control(world, entity) && !world.id_of(entity).empty()) {
            entities.emplace_back(world.id_of(entity), entity);
        }
    }
    std::ranges::sort(entities);
    const auto check_shortcut = [&](const std::string& id, std::string_view type,
                                    const std::vector<json::Value>& shortcut) {
        for (std::size_t i = 0; i < shortcut.size(); ++i) {
            const std::string path =
                "/components/" + std::string(type) + "/shortcut/" + std::to_string(i);
            const json::Value& entry = shortcut[i];
            const json::Value* kind = entry.is_object() ? entry.find("type") : nullptr;
            if (kind == nullptr || !kind->is_string() || kind->as_string() != "action") {
                for (json::ValidationError& error :
                     input::validate_event(entry, input::Use::Binding)) {
                    problems.push_back(
                        {.id = id, .path = path + error.path, .message = std::move(error.message)});
                }
                continue;
            }
            for (const auto& [key, value] : entry.as_object()) {
                if (key != "type" && key != "action") {
                    problems.push_back({.id = id,
                                        .path = path + "/" + json::pointer_escape(key),
                                        .message = "an action shortcut has only \"type\" and "
                                                   "\"action\""});
                }
            }
            const json::Value* action = entry.find("action");
            if (action == nullptr || !action->is_string()) {
                problems.push_back(
                    {.id = id, .path = path, .message = "an action shortcut names an \"action\""});
            } else if (!actions.has_action(action->as_string())) {
                std::string message = "no action \"" + action->as_string() + "\"";
                const std::vector<std::string_view> names = actions.names();
                if (const std::string_view hint = closest_match(action->as_string(), names);
                    !hint.empty()) {
                    message += "; did you mean \"" + std::string(hint) + "\"?";
                }
                problems.push_back(
                    {.id = id, .path = path + "/action", .message = std::move(message)});
            }
        }
    };
    const auto check_group = [&](const std::string& id, std::string_view type, const auto& button) {
        if (!button.button_group.empty() && !button.toggle_mode) {
            problems.push_back(
                {.id = id,
                 .path = "/components/" + std::string(type) + "/button_group",
                 .message = "button groups are for toggle buttons; set toggle_mode too"});
        }
    };
    for (const auto& [id, entity] : entities) {
        const auto* button = ecs.try_get<world::Button>(entity);
        const auto* texture = ecs.try_get<world::TextureButton>(entity);
        if (button != nullptr) {
            check_shortcut(id, "Button", button->shortcut);
            check_group(id, "Button", *button);
        }
        if (texture != nullptr) {
            check_shortcut(id, "TextureButton", texture->shortcut);
            check_group(id, "TextureButton", *texture);
        }
        if (button != nullptr && texture != nullptr) {
            problems.push_back({.id = id,
                                .path = "/components/TextureButton",
                                .message = "an entity is one button; it has a Button too"});
        }
        if ((button != nullptr || texture != nullptr) && ecs.all_of<world::Label>(entity)) {
            problems.push_back(
                {.id = id,
                 .path = "/components/Label",
                 .message = "a button draws its own text; give it to the button, or put the "
                            "Label in a child"});
        }
        const auto* control = ecs.try_get<world::Control>(entity);
        if (control == nullptr) {
            continue;
        }
        const std::array<std::pair<std::string_view, entt::entity>, 6> fields{{
            {"focus_neighbor_left", control->focus_neighbor_left},
            {"focus_neighbor_top", control->focus_neighbor_top},
            {"focus_neighbor_right", control->focus_neighbor_right},
            {"focus_neighbor_bottom", control->focus_neighbor_bottom},
            {"focus_next", control->focus_next},
            {"focus_previous", control->focus_previous},
        }};
        for (const auto& [field, target] : fields) {
            if (target != entt::null && (!world.valid(target) || !is_control(world, target))) {
                problems.push_back({.id = id,
                                    .path = "/components/Control/" + std::string(field),
                                    .message = "names no control"});
            }
        }
    }
}

} // namespace slash::ui
