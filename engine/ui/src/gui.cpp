#include "internal.hpp"
#include "line_edit.hpp"
#include "text_edit.hpp"

#include "slash/core/text.hpp"
#include "slash/input/input.hpp"
#include "slash/text/text.hpp"
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
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// The gui system sends input events to controls as Godot's Viewport does
// (scene/main/viewport.cpp: _gui_input_event, _gui_call_input, _gui_find_control_at_pos and
// _update_mouse_over), moves focus as Control does (scene/gui/control.cpp:
// find_next_valid_focus, find_prev_valid_focus and _get_focus_neighbor with the legacy
// strategy, Godot's default) and presses buttons as BaseButton does
// (scene/gui/base_button.cpp), edits text as LineEdit and TextEdit do (scene/gui/line_edit.cpp,
// and text_editor.cpp for TextEdit), and scrolls as ScrollBar and ScrollContainer do
// (scene/gui/scroll_bar.cpp and scroll_container.cpp). Its state is kept by entity ID, since
// sessions make entities anew when they roll a command back. Drag and drop, tooltips, context
// menus and cursor shapes aren't here.
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

constexpr double kSliderRepeatDelay = 0.5; // Slider's DEFAULT_GAMEPAD_EVENT_DELAY_MS, in seconds

// What Slider keeps between events, for one slider: its drag (its grab) and the value it last
// had, so a change from anywhere emits value_changed, as Range::set_value does.
struct SliderStatus {
    bool active = false;                // being dragged
    double pos = 0.0;                   // where along it the drag started
    double uvalue = 0.0;                // its ratio then
    double before = 0.0;                // its ratio before the drag, for drag_ended's value_changed
    double known = 0.0;                 // its value as last seen
    bool known_set = false;             // known has been seen
    bool repeating = false;             // a gamepad direction steps it while held
    bool repeat_rtl = false;            // and it runs from the right
    double repeat = kSliderRepeatDelay; // seconds until it steps again
};

// What ScrollBar keeps between events, for a ScrollBar or one of a ScrollContainer's bars: the
// part the pointer is over, the arrow held down and its grabber's drag.
struct BarStatus {
    int highlight = 0; // as BarLook's
    bool decrement_pressed = false;
    bool increment_pressed = false;
    bool dragging = false;
    double pos_at_click = 0.0;   // where along its area the drag started
    double value_at_click = 0.0; // its ratio then
};

// A ScrollBar's: as above, and the value it last had, so a change from anywhere emits
// value_changed.
struct ScrollBarStatus {
    BarStatus bar;
    double known = 0.0;
    bool known_set = false;
};

// A ScrollContainer's: its bars', its scroll as its bars' values, in fractions of a pixel, and a
// touch drag, as ScrollContainer's drag_* members.
struct ScrollStatus {
    BarStatus h;
    BarStatus v;
    std::array<double, 2> value{}; // horizontal, vertical
    bool touching = false;         // drag_touching
    bool deaccel = false;          // drag_touching_deaccel: let go, and gliding to a stop
    bool beyond_deadzone = false;
    Vec2 speed;
    Vec2 accum;
    Vec2 last_accum;
    Vec2 from;
    double time_since_motion = 0.0;
};

[[nodiscard]] bool idle(const BarStatus& status) {
    return status.highlight == 0 && !status.decrement_pressed && !status.increment_pressed &&
           !status.dragging;
}

[[nodiscard]] bool idle(const ButtonStatus& status) {
    return !status.hovering && !status.press_attempt && !status.pressing_inside &&
           !status.pressed_down_with_focus && status.touch_index == -1 && status.feedback <= 0.0;
}

// Godot's gui/timers/incremental_search_max_interval_msec, in seconds.
constexpr double kSearchInterval = 2.0;
// PopupMenu ignores a release this soon after it opened under a held button, so the press that
// opened it doesn't pick an item.
constexpr double kGrabbedClickSeconds = 0.4;
// InputEvent::DEVICE_ID_INTERNAL: the releases a control gets when it loses the mouse.
constexpr int kInternalDevice = -2;

// What PopupMenu keeps between events, for one open popup menu: a PopupMenu's, or an
// OptionButton's or MenuButton's list.
struct PopupStatus {
    std::string owner; // the entity ID of the PopupMenu, OptionButton or MenuButton
    int focused = -1;  // mouse_over: the item the pointer or keys are on
    double scroll = 0.0;
    int scroll_to = -1;             // an item to scroll into view, as scroll_to_item asks
    double opened = 0.0;            // popup_time_msec, in seconds of world time
    std::uint16_t initial_mask = 0; // initial_button_mask: the mouse buttons held as it opened
    bool grabbed = false;           // during_grabbed_click: opened under a held button
    bool scrolling = false;         // is_scrolling: a press landed beside the items
    bool closing = false;           // _close_pressed: hidden at the end of the tick
    bool mouse_inside = false;      // the mouse is over it, as NOTIFICATION_WM_MOUSE_ENTER
    std::u32string search;          // search_string
    double search_time = -1e9;      // search_time_msec, in seconds
};

struct GuiState {
    std::uint64_t tick = kNever; // the tick the gui system last ran
    double now = 0.0;            // seconds of world time this tick
    std::vector<UiEvent> events; // that tick's
    std::vector<UiEvent> next;   // emitted between ticks, for the next one
    std::map<std::string, ButtonStatus, std::less<>> buttons;
    std::string key_focus;
    std::string mouse_focus; // the control a held mouse button pressed
    std::string mouse_over;
    std::vector<std::string> mouse_over_hierarchy; // it and its parents, outermost first
    std::map<int, std::string> touch_focus;        // the control each finger pressed
    // Sticks holding a focus action, so a held stick moves focus once: (device, axis, action).
    std::set<std::tuple<int, int, std::string>> joy_held;
    std::vector<std::string> presses;                             // press() calls for the next tick
    std::map<std::string, EditState, std::less<>> edits;          // each LineEdit's
    std::map<std::string, TextEditState, std::less<>> text_edits; // each TextEdit's
    std::map<std::string, SliderStatus, std::less<>> sliders;     // each Slider's
    std::map<std::string, ScrollBarStatus, std::less<>> scroll_bars; // each ScrollBar's
    std::map<std::string, ScrollStatus, std::less<>> scrolls;        // each ScrollContainer's
    std::string followed;  // the focus follow_focus last scrolled to
    std::string clipboard; // the world's clipboard, which a backend shares with the system's
    std::vector<PopupStatus> popups;                  // the popup menus open, the last on top
    std::vector<std::pair<std::string, int>> selects; // select_item() calls for the next tick
    std::uint16_t mouse_focus_mask = 0;
    std::uint16_t mouse_mask = 0; // the mouse buttons held, as Input::get_mouse_button_mask
    bool hide_focus = false;      // focus came from a click, so it isn't drawn
    bool focus_by_arrow = false;  // an arrow key is moving focus, so a LineEdit won't edit
    // A popup opened, so the controls lose the mouse as their window loses focus.
    bool drop_mouse_focus = false;
    bool popups_changed = false; // a popup opened or closed, so the layout is out of date
};

// The clipboard the platform gives, kept apart from GuiState so snapshots leave it alone.
struct ClipboardHolder {
    ClipboardBackend backend;
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

// A ScrollContainer's bars have no entities, so the gui system names one by its container's ID
// and the bar's name, as Godot names them _h_scroll and _v_scroll. IDs have no '/'.
constexpr std::string_view kHBar = "/h_scroll";
constexpr std::string_view kVBar = "/v_scroll";

std::string bar_id(std::string_view owner, Bar bar) {
    std::string out(owner);
    if (bar != Bar::Own) {
        out += bar == Bar::Horizontal ? kHBar : kVBar;
    }
    return out;
}

// The control an ID the gui system keeps names: itself, or a bar's container.
std::string_view owner_of(std::string_view id) {
    return id.substr(0, id.find('/'));
}

Bar bar_named(std::string_view id) {
    if (id.ends_with(kHBar)) {
        return Bar::Horizontal;
    }
    return id.ends_with(kVBar) ? Bar::Vertical : Bar::Own;
}

// The status of the bar an ID names, if the gui system keeps one: const in a const state.
template <typename State>
auto* bar_status(State& state, std::string_view id) {
    using Status = std::conditional_t<std::is_const_v<State>, const BarStatus, BarStatus>;
    Status* none = nullptr;
    const Bar bar = bar_named(id);
    if (bar == Bar::Own) {
        const auto found = state.scroll_bars.find(id);
        return found != state.scroll_bars.end() ? &found->second.bar : none;
    }
    const auto found = state.scrolls.find(owner_of(id));
    if (found == state.scrolls.end()) {
        return none;
    }
    return bar == Bar::Horizontal ? &found->second.h : &found->second.v;
}

// ---- Controls ----

enum class Filter : std::uint8_t { Stop, Pass, Ignore };
enum class Focus : std::uint8_t { None, Click, All };

bool is_button(const world::World& world, entt::entity entity) {
    return button_of(world, entity) != nullptr ||
           (world.valid(entity) && world.ecs().all_of<world::TextureButton>(entity));
}

bool is_container(const world::World& world, entt::entity entity) {
    return world.ecs()
        .any_of<world::BoxContainer, world::GridContainer, world::MarginContainer,
                world::CenterContainer, world::FlowContainer, world::PanelContainer,
                world::ScrollContainer>(entity);
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
    // MenuButton's constructor sets FOCUS_ACCESSIBILITY: keys don't move focus to it.
    if (world.ecs().all_of<world::MenuButton>(entity)) {
        return Focus::None;
    }
    return is_button(world, entity) ||
                   world.ecs().any_of<world::LineEdit, world::TextEdit, world::Slider>(entity)
               ? Focus::All
               : Focus::None;
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
    if (const world::Button* button = button_of(world, entity); button != nullptr) {
        return base_from(*button);
    }
    if (const auto* button = world.ecs().try_get<world::TextureButton>(entity); button != nullptr) {
        return base_from(*button);
    }
    return std::nullopt;
}

void set_button_pressed(world::World& world, entt::entity entity, bool pressed) {
    if (world::Button* button = button_of(world, entity); button != nullptr) {
        button->button_pressed = pressed;
    } else if (auto* texture = world.ecs().try_get<world::TextureButton>(entity);
               texture != nullptr) {
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
    case input::EventType::Ime:
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
    const UiEvent event{.kind = std::string(kind),
                        .id = id,
                        .toggled_on = toggled.value_or(false),
                        .text = {},
                        .from_line = 0,
                        .to_line = 0,
                        .value = 0.0,
                        .value_changed = false,
                        .index = -1,
                        .item_id = -1};
    (state.tick == world.tick() ? state.events : state.next).push_back(event);
    json::Value data;
    if (toggled) {
        data = json::Object{{"toggled_on", *toggled}};
    }
    world.emit(std::string(kind), id, std::move(data));
}

// A slider's value_changed, or its drag_ended with whether the drag changed its value.
void emit_value(world::World& world, GuiState& state, std::string_view kind, const std::string& id,
                double value, bool changed = false) {
    if (id.empty()) {
        return;
    }
    const bool ended = kind == "drag_ended";
    const UiEvent event{.kind = std::string(kind),
                        .id = id,
                        .toggled_on = false,
                        .text = {},
                        .from_line = 0,
                        .to_line = 0,
                        .value = ended ? 0.0 : value,
                        .value_changed = ended && changed,
                        .index = -1,
                        .item_id = -1};
    (state.tick == world.tick() ? state.events : state.next).push_back(event);
    world.emit(std::string(kind), id,
               ended ? json::Object{{"value_changed", changed}} : json::Object{{"value", value}});
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

// ---- Text fields ----

// Godot's LineEdit: a third click this soon after a double click, this near it, selects all.
constexpr double kTripleClickSeconds = 0.6;
constexpr float kTripleClickTolerance = 5.0F;

// Records a LineEdit event that carries text, as emit does.
void emit_text(world::World& world, GuiState& state, std::string_view kind, const std::string& id,
               std::string_view key, const std::string& text) {
    if (id.empty()) {
        return;
    }
    const UiEvent event{.kind = std::string(kind),
                        .id = id,
                        .toggled_on = false,
                        .text = text,
                        .from_line = 0,
                        .to_line = 0,
                        .value = 0.0,
                        .value_changed = false,
                        .index = -1,
                        .item_id = -1};
    (state.tick == world.tick() ? state.events : state.next).push_back(event);
    world.emit(std::string(kind), id, json::Object{{std::string(key), text}});
}

// Brings a LineEdit's state up to its component: text changed from outside (a command or
// gameplay code) starts the history again as set_text does, from the text it had to the text it
// has, and drops the selection.
void sync_edit(const world::LineEdit& edit, EditState& s) {
    if (s.known && s.known_text == edit.text) {
        return;
    }
    const std::string old = s.known ? s.known_text : std::string();
    s.selection = {};
    s.scroll_offset = 0.0F;
    s.undo.clear();
    s.undo_pos = -1;
    s.undo.push_back({.text = old, .caret_column = 0, .scroll_offset = 0.0F});
    if (edit.text != old) {
        s.undo.push_back({.text = edit.text,
                          .caret_column = static_cast<int>(to_utf32(edit.text).size()),
                          .scroll_offset = 0.0F});
    }
    s.known_text = edit.text;
    s.known = true;
}

// Godot's String::strip_escapes: without control characters, which a LineEdit can't show.
std::u32string strip_escapes(std::u32string text) {
    std::erase_if(text, [](char32_t c) { return c < 0x20U; });
    return text;
}

// One LineEdit being worked on, as Godot's LineEdit methods work on its members: its text as
// characters, its caret column in the component and the rest in its EditState. With a layout it
// shapes its text as the layout does, to scroll, hit test and find words and characters.
class Editor {
public:
    Editor(world::World& world, GuiState& state, entt::entity entity, const Layout* layout)
        : world_(world), state_(state), id_(world.id_of(entity)),
          edit_(world.ecs().get<world::LineEdit>(entity)), s_(state.edits[id_]) {
        sync_edit(edit_, s_);
        text_ = to_utf32(edit_.text);
        edit_.caret_column = std::clamp(edit_.caret_column, 0, length());
        if (layout == nullptr) {
            return;
        }
        const auto view = layout->line_edits.find(entity);
        const Placed* placed = layout->find(entity);
        text::Fonts* found = fonts(world);
        if (view != layout->line_edits.end() && placed != nullptr && found != nullptr) {
            field_.emplace(*found, view->second, placed->size);
            field_->shape(edit_, s_);
            field_->fit(edit_, s_);
        }
    }

    [[nodiscard]] const world::LineEdit& edit() const { return edit_; }
    [[nodiscard]] EditState& state() { return s_; }
    [[nodiscard]] const std::u32string& text() const { return text_; }
    [[nodiscard]] int length() const { return static_cast<int>(text_.size()); }
    [[nodiscard]] int caret() const { return edit_.caret_column; }
    [[nodiscard]] bool editing() const { return s_.editing; }
    [[nodiscard]] const Field* field() const { return field_ ? &*field_ : nullptr; }

    // ---- Editing ----

    // _edit, once it has focus: whether it started editing.
    bool start_editing(bool show_keyboard) {
        if (!edit_.editable || s_.editing) {
            return false;
        }
        if (edit_.select_all_on_focus) {
            if ((state_.mouse_focus_mask & mask_bit(input::MouseButton::Left)) != 0) {
                s_.pending_select_all = true; // when the mouse button is up
            } else {
                select_all();
            }
        }
        s_.editing = true;
        validate_caret();
        if (show_keyboard && !s_.pending_select_all) {
            show_virtual_keyboard();
        }
        return true;
    }

    void unedit() {
        if (!s_.editing) {
            return;
        }
        s_.editing = false;
        validate_caret();
        apply_ime();
        set_caret_column(caret());
        s_.show_keyboard = false;
        if (edit_.deselect_on_focus_loss_enabled) {
            deselect();
        }
    }

    void show_virtual_keyboard() { s_.show_keyboard = edit_.virtual_keyboard_enabled; }

    // _validate_caret_can_draw and _reset_caret_blink_timer: the caret shows again.
    void validate_caret() {
        if (edit_.caret_blink) {
            s_.draw_caret = true;
            s_.blink_timer = 0.0;
        }
    }
    void reset_blink() {
        if (edit_.caret_blink) {
            s_.draw_caret = true;
            if (edit_.caret_force_displayed || (s_.editing && state_.key_focus == id_)) {
                s_.blink_timer = 0.0;
            }
        }
    }

    // ---- Input method ----

    void cancel_ime() {
        if (s_.ime_text.empty()) {
            return;
        }
        s_.ime_text.clear();
        s_.ime_selection_start = 0;
        s_.ime_selection_length = 0;
        shape();
    }

    // Commits what the input method is composing. Godot's apply_ime doesn't emit text_changed;
    // this does, as for typing.
    void apply_ime() {
        if (s_.ime_text.empty()) {
            return;
        }
        const std::u32string composed = s_.ime_text;
        cancel_ime();
        insert_text_at_caret(composed);
        s_.text_changed_dirty = true;
    }

    // NOTIFICATION_OS_IME_UPDATE.
    void ime_update(std::u32string text, int selection_start, int selection_length) {
        if (s_.ime_text == text && s_.ime_selection_start == selection_start &&
            s_.ime_selection_length == selection_length) {
            return;
        }
        s_.ime_text = std::move(text);
        s_.ime_selection_start = selection_start;
        s_.ime_selection_length = selection_length;
        if (!s_.ime_text.empty()) {
            selection_delete();
        }
        shape();
        set_caret_column(caret());
    }

    // ---- Caret ----

    void set_caret_column(int column) {
        edit_.caret_column = std::clamp(column, 0, length());
        if (field_) {
            field_->fit(edit_, s_);
        }
    }

    void set_caret_at_pixel_pos(float x) {
        if (field_) {
            set_caret_column(field_->column_at(edit_, s_, static_cast<float>(static_cast<int>(x))));
        }
    }

    [[nodiscard]] std::vector<std::uint32_t> word_breaks() const {
        return field_ ? field_->line().word_breaks() : std::vector<std::uint32_t>();
    }

    // The word start before a column, or 0; the word end after one, or the text's end.
    [[nodiscard]] int word_left(int column) const {
        const std::vector<std::uint32_t> words = word_breaks();
        if (words.empty() || column <= static_cast<int>(words.front())) {
            return 0;
        }
        for (int i = static_cast<int>(words.size()) - 2; i >= 0; i -= 2) {
            if (static_cast<int>(words[static_cast<std::size_t>(i)]) < column) {
                return static_cast<int>(words[static_cast<std::size_t>(i)]);
            }
        }
        return column;
    }
    [[nodiscard]] int word_right(int column) const {
        const std::vector<std::uint32_t> words = word_breaks();
        if (words.empty() || column >= static_cast<int>(words.back())) {
            return length();
        }
        for (std::size_t i = 1; i < words.size(); i += 2) {
            if (static_cast<int>(words[i]) > column) {
                return static_cast<int>(words[i]);
            }
        }
        return column;
    }

    [[nodiscard]] int next_character(int column) const {
        if (!field_) {
            return std::min(column + 1, length());
        }
        return static_cast<int>(field_->line().next_character(static_cast<std::uint32_t>(column)));
    }
    [[nodiscard]] int previous_character(int column) const {
        if (!field_) {
            return std::max(column - 1, 0);
        }
        return static_cast<int>(
            field_->line().previous_character(static_cast<std::uint32_t>(column)));
    }

    void move_caret_left(bool select, bool by_word) {
        if (s_.selection.enabled && !select) {
            set_caret_column(s_.selection.begin);
            deselect();
            return;
        }
        shift_selection_check_pre(select);
        if (by_word) {
            set_caret_column(word_left(caret()));
        } else if (edit_.caret_mid_grapheme) {
            set_caret_column(caret() - 1);
        } else {
            set_caret_column(previous_character(caret()));
        }
        shift_selection_check_post(select);
        reset_blink();
    }

    void move_caret_right(bool select, bool by_word) {
        if (s_.selection.enabled && !select) {
            set_caret_column(s_.selection.end);
            deselect();
            return;
        }
        shift_selection_check_pre(select);
        if (by_word) {
            set_caret_column(word_right(caret()));
        } else if (edit_.caret_mid_grapheme) {
            set_caret_column(caret() + 1);
        } else {
            set_caret_column(next_character(caret()));
        }
        shift_selection_check_post(select);
        reset_blink();
    }

    void move_caret_to(bool select, int column) {
        shift_selection_check_pre(select);
        set_caret_column(column);
        shift_selection_check_post(select);
    }

    void swap_input_direction() {
        s_.input_direction = s_.input_direction == text::Direction::Ltr ? text::Direction::Rtl
                                                                        : text::Direction::Ltr;
        set_caret_column(caret());
    }

    // ---- Selection ----

    void deselect() { s_.selection = {}; }

    void selection_fill_at_caret() {
        if (!edit_.selecting_enabled) {
            return;
        }
        EditState::Selection& selection = s_.selection;
        selection.begin = caret();
        selection.end = selection.start_column;
        if (selection.end < selection.begin) {
            std::swap(selection.begin, selection.end);
        }
        selection.enabled = selection.begin != selection.end;
    }

    void select_all() {
        if (!edit_.selecting_enabled) {
            return;
        }
        if (text_.empty()) {
            set_caret_column(0);
            return;
        }
        s_.selection.begin = 0;
        s_.selection.end = length();
        s_.selection.enabled = true;
    }

    void select(int from, int to) {
        if (!edit_.selecting_enabled) {
            return;
        }
        if (from == 0 && to == 0) {
            deselect();
            return;
        }
        from = std::clamp(from, 0, length());
        if (to < 0 || to > length()) {
            to = length();
        }
        if (from >= to) {
            return;
        }
        EditState::Selection& selection = s_.selection;
        selection.enabled = true;
        selection.begin = from;
        selection.end = to;
        selection.creating = false;
        selection.double_click = false;
    }

    void shift_selection_check_pre(bool shift) {
        if (!s_.selection.enabled && shift) {
            s_.selection.start_column = caret();
        }
        if (!shift) {
            deselect();
        }
    }
    void shift_selection_check_post(bool shift) {
        if (shift) {
            selection_fill_at_caret();
        }
    }

    [[nodiscard]] std::u32string selected_text() const {
        const EditState::Selection& selection = s_.selection;
        if (!selection.enabled) {
            return {};
        }
        return text_.substr(static_cast<std::size_t>(selection.begin),
                            static_cast<std::size_t>(selection.end - selection.begin));
    }

    // ---- Changing the text ----

    void insert_text_at_caret(std::u32string inserted) {
        if (edit_.max_length > 0) {
            const int available = std::max(edit_.max_length - length(), 0);
            if (static_cast<int>(inserted.size()) > available) {
                emit_text(world_, state_, "text_change_rejected", id_, "rejected_substring",
                          to_utf8(inserted.substr(static_cast<std::size_t>(available))));
                inserted.resize(static_cast<std::size_t>(available));
            }
        }
        const auto at = static_cast<std::size_t>(caret());
        const auto count = static_cast<int>(inserted.size());
        set_text(text_.substr(0, at) + inserted + text_.substr(at));
        if (field_) {
            const text::Direction direction = field_->line().dominant_direction(
                static_cast<std::uint32_t>(at), static_cast<std::uint32_t>(caret() + count));
            if (direction != text::Direction::Auto) {
                s_.input_direction = direction;
            }
        }
        set_caret_column(caret() + count);
        if (!s_.ime_text.empty()) {
            shape();
        }
    }

    void delete_text(int from, int to) {
        if (from < 0 || from > to || to > length()) {
            return;
        }
        set_text(text_.substr(0, static_cast<std::size_t>(from)) +
                 text_.substr(static_cast<std::size_t>(to)));
        set_caret_column(caret() - std::clamp(caret() - from, 0, to - from));
        s_.text_changed_dirty = true;
    }

    void selection_delete() {
        if (s_.selection.enabled) {
            delete_text(s_.selection.begin, s_.selection.end);
        }
        deselect();
    }

    void delete_char() {
        if (text_.empty() || caret() == 0) {
            return;
        }
        int offset = 1;
        if (!edit_.caret_mid_grapheme && edit_.backspace_deletes_composite_character_enabled) {
            offset = caret() - previous_character(caret());
        }
        const auto at = static_cast<std::size_t>(caret());
        set_text(text_.substr(0, at - static_cast<std::size_t>(offset)) + text_.substr(at));
        set_caret_column(caret() - offset);
        text_changed();
    }

    void backspace(bool word, bool all_to_left) {
        if (!edit_.editable) {
            return;
        }
        if (s_.selection.enabled) {
            selection_delete();
            return;
        }
        if (caret() == 0) {
            return;
        }
        if (all_to_left) {
            set_text(text_.substr(static_cast<std::size_t>(caret())));
            set_caret_column(0);
            text_changed();
            return;
        }
        if (word) {
            const int from = word_left(caret());
            delete_text(from, caret());
            set_caret_column(from);
        } else {
            delete_char();
        }
    }

    void delete_forward(bool word, bool all_to_right) {
        if (!edit_.editable) {
            return;
        }
        if (s_.selection.enabled) {
            selection_delete();
            return;
        }
        if (caret() == length()) {
            return;
        }
        if (all_to_right) {
            set_text(text_.substr(0, static_cast<std::size_t>(caret())));
            text_changed();
            return;
        }
        if (word) {
            delete_text(caret(), word_right(caret()));
            set_caret_column(caret());
        } else if (edit_.caret_mid_grapheme) {
            set_caret_column(caret() + 1);
            delete_char();
        } else {
            const int from = caret();
            set_caret_column(next_character(caret()));
            delete_text(from, caret());
        }
    }

    void clear() {
        const bool was_empty = text_.empty();
        clear_internal();
        clear_redo();
        if (!was_empty) {
            emit_text_change();
        }
        if (s_.editing) {
            show_virtual_keyboard();
        }
    }

    // ---- Clipboard ----

    void copy_text() {
        if (s_.selection.enabled && !edit_.secret) {
            set_clipboard(world_, to_utf8(selected_text()));
        }
    }

    void cut_text() {
        if (edit_.editable && s_.selection.enabled && !edit_.secret) {
            set_clipboard(world_, to_utf8(selected_text()));
            selection_delete();
        }
    }

    void paste_text() {
        if (!edit_.editable) {
            return;
        }
        const std::u32string pasted = strip_escapes(to_utf32(clipboard(world_)));
        if (pasted.empty()) {
            return;
        }
        const std::u32string before = text_;
        if (s_.selection.enabled) {
            selection_delete();
        }
        insert_text_at_caret(pasted);
        if (text_ != before) {
            s_.text_changed_dirty = true;
        }
    }

    // ---- History ----

    // _text_changed: text_changed, and what was typed becomes a step to undo.
    void text_changed() {
        emit_text_change();
        clear_redo();
    }

    void emit_text_change() {
        emit_text(world_, state_, "text_changed", id_, "text", edit_.text);
        s_.text_changed_dirty = false;
    }

    [[nodiscard]] bool has_undo() const {
        return s_.undo_pos < 0 ? s_.undo.size() > 1 : s_.undo_pos > 0;
    }
    [[nodiscard]] bool has_redo() const {
        return s_.undo_pos >= 0 && s_.undo_pos != static_cast<int>(s_.undo.size()) - 1;
    }

    void undo() {
        if (!edit_.editable || !has_undo()) {
            return;
        }
        if (s_.undo_pos < 0) {
            s_.undo_pos = static_cast<int>(s_.undo.size()) - 1;
        }
        deselect();
        --s_.undo_pos;
        restore_operation();
    }

    void redo() {
        if (!edit_.editable || !has_redo()) {
            return;
        }
        deselect();
        ++s_.undo_pos;
        restore_operation();
    }

private:
    void shape() {
        if (field_) {
            field_->shape(edit_, s_);
        }
    }

    // text = ...; _shape(), with the component and what the state last saw kept in step.
    void set_text(std::u32string text) {
        text_ = std::move(text);
        edit_.text = to_utf8(text_);
        s_.known_text = edit_.text;
        shape();
    }

    void clear_internal() {
        deselect();
        clear_undo_stack();
        edit_.caret_column = 0;
        s_.scroll_offset = 0.0F;
        set_text({});
    }

    void create_undo_state() {
        s_.undo.push_back(
            {.text = edit_.text, .caret_column = caret(), .scroll_offset = s_.scroll_offset});
    }

    // _clear_redo: the history ends at the text as it is.
    void clear_redo() {
        create_undo_state();
        if (s_.undo_pos < 0) {
            return;
        }
        s_.undo.resize(static_cast<std::size_t>(s_.undo_pos) + 1);
        s_.undo_pos = -1;
        create_undo_state();
    }

    void clear_undo_stack() {
        s_.undo.clear();
        s_.undo_pos = -1;
        create_undo_state();
    }

    void restore_operation() {
        const EditState::Operation operation = s_.undo[static_cast<std::size_t>(s_.undo_pos)];
        s_.scroll_offset = operation.scroll_offset;
        set_text(to_utf32(operation.text));
        set_caret_column(operation.caret_column);
        emit_text_change();
    }

    world::World& world_;
    GuiState& state_;
    std::string id_;
    world::LineEdit& edit_;
    EditState& s_;
    std::u32string text_;
    std::optional<Field> field_;
};

// LineEdit's NOTIFICATION_FOCUS_ENTER: it starts editing, unless arrow keys moved focus to it.
// Godot emits editing_toggled even when it was already editing or can't edit; this emits it
// only when editing starts.
void line_edit_focus_enter(world::World& world, GuiState& state, const std::string& id) {
    const entt::entity entity = world.find(id);
    if (entity == entt::null || !world.ecs().all_of<world::LineEdit>(entity) ||
        state.focus_by_arrow) {
        return;
    }
    Editor editor(world, state, entity, nullptr);
    if (editor.start_editing(editor.edit().virtual_keyboard_show_on_focus)) {
        emit(world, state, "editing_toggled", id, true);
    }
}

// NOTIFICATION_FOCUS_EXIT.
void line_edit_focus_exit(world::World& world, GuiState& state, const std::string& id) {
    const entt::entity entity = world.find(id);
    if (entity == entt::null || !world.ecs().all_of<world::LineEdit>(entity)) {
        return;
    }
    Editor editor(world, state, entity, nullptr);
    if (editor.editing()) {
        editor.unedit();
        emit(world, state, "editing_toggled", id, false);
    } else if (editor.edit().deselect_on_focus_loss_enabled) {
        editor.deselect();
    }
}

// ---- Multi-line text fields ----

// Records the signals a TextEdit emitted at once: text_set, and lines_edited_from with its lines.
void emit_text_edit(world::World& world, GuiState& state, const std::string& id,
                    const std::vector<TextEditSignal>& signals) {
    for (const TextEditSignal& signal : signals) {
        if (signal.kind != "lines_edited_from") {
            emit(world, state, signal.kind, id);
            continue;
        }
        const UiEvent event{.kind = signal.kind,
                            .id = id,
                            .toggled_on = false,
                            .text = {},
                            .from_line = signal.from_line,
                            .to_line = signal.to_line,
                            .value = 0.0,
                            .value_changed = false,
                            .index = -1,
                            .item_id = -1};
        (state.tick == world.tick() ? state.events : state.next).push_back(event);
        world.emit(signal.kind, id,
                   json::Object{{"from_line", signal.from_line}, {"to_line", signal.to_line}});
    }
}

// A TextEdit's lines shaped at the size and with the theme a layout gave it, or nullopt.
std::optional<TextArea> text_area(world::World& world, const Layout& layout, entt::entity entity,
                                  const TextEditState& s) {
    const auto view = layout.text_edits.find(entity);
    const Placed* placed = layout.find(entity);
    text::Fonts* found = fonts(world);
    const auto* edit = world.ecs().try_get<world::TextEdit>(entity);
    if (view == layout.text_edits.end() || placed == nullptr || found == nullptr ||
        edit == nullptr) {
        return std::nullopt;
    }
    return text_area_for(*found, view->second, placed->size, *edit, s);
}

// TextEdit's NOTIFICATION_FOCUS_ENTER.
void text_edit_focus_entered(world::World& world, GuiState& state, const std::string& id) {
    const entt::entity entity = world.find(id);
    const auto* edit =
        entity != entt::null ? world.ecs().try_get<world::TextEdit>(entity) : nullptr;
    if (edit != nullptr) {
        text_edit_focus_enter(*edit, state.text_edits[id]);
    }
}

// NOTIFICATION_FOCUS_EXIT. Committing a composition needs the text laid out.
void text_edit_focus_exited(world::World& world, GuiState& state, const std::string& id) {
    const entt::entity entity = world.find(id);
    auto* edit = entity != entt::null ? world.ecs().try_get<world::TextEdit>(entity) : nullptr;
    const auto found = state.text_edits.find(id);
    if (edit == nullptr || found == state.text_edits.end()) {
        return;
    }
    TextEditState& s = found->second;
    std::vector<TextEditSignal> signals;
    const TextEditContext context{.world = &world,
                                  .map = input::map(std::as_const(world)),
                                  .now = 0.0,
                                  .focused = false,
                                  .signals = &signals};
    std::optional<Layout> placed;
    if (text::Fonts* fonts_found = fonts(world); fonts_found != nullptr && !s.ime_text.empty()) {
        placed.emplace(layout(world, *fonts_found, settings(world).viewport));
    }
    std::optional<TextArea> area =
        placed ? text_area(world, *placed, entity, s) : std::optional<TextArea>();
    text_edit_focus_exit(context, *edit, s, area ? &*area : nullptr);
    emit_text_edit(world, state, id, signals);
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
    line_edit_focus_exit(world, state, old);
    text_edit_focus_exited(world, state, old);
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
    line_edit_focus_enter(world, state, id);
    text_edit_focus_entered(world, state, id);
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
    collect(world.ecs().view<world::CheckBox>());
    collect(world.ecs().view<world::CheckButton>());
    collect(world.ecs().view<world::OptionButton>());
    collect(world.ecs().view<world::MenuButton>());
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

// BaseButton::set_pressed: a toggle button set on or off without a press, emitting toggled if
// that changed it.
void set_toggled(world::World& world, GuiState& state, entt::entity entity, bool pressed) {
    const std::optional<Base> base = base_of(world, entity);
    if (!base || !base->toggle_mode || base->button_pressed == pressed) {
        return;
    }
    set_button_pressed(world, entity, pressed);
    if (pressed) {
        unpress_group(world, state, entity);
    }
    emit(world, state, "toggled", std::string(world.id_of(entity)), pressed);
}

// ---- Popup menus ----

// What shows an entity's popup menu.
enum class PopupKind : std::uint8_t { None, Menu, Option, MenuButton };

PopupKind popup_kind(const world::World& world, entt::entity entity) {
    if (!world.valid(entity)) {
        return PopupKind::None;
    }
    const auto& ecs = world.ecs();
    if (ecs.all_of<world::PopupMenu>(entity)) {
        return PopupKind::Menu;
    }
    if (ecs.all_of<world::OptionButton>(entity)) {
        return PopupKind::Option;
    }
    return ecs.all_of<world::MenuButton>(entity) ? PopupKind::MenuButton : PopupKind::None;
}

PopupStatus* popup_status(GuiState& state, std::string_view owner) {
    const auto found = std::ranges::find(state.popups, owner, &PopupStatus::owner);
    return found != state.popups.end() ? &*found : nullptr;
}

// A popup menu's event: item_selected and item_focused with {"index"}, id_focused with {"id"},
// and id_pressed and index_pressed with both.
void emit_item(world::World& world, GuiState& state, std::string_view kind, const std::string& id,
               int index, int item) {
    if (id.empty()) {
        return;
    }
    const bool has_id = kind == "id_focused" || kind == "id_pressed" || kind == "index_pressed";
    const bool has_index = kind != "id_focused";
    const UiEvent event{.kind = std::string(kind),
                        .id = id,
                        .toggled_on = false,
                        .text = {},
                        .from_line = 0,
                        .to_line = 0,
                        .value = 0.0,
                        .value_changed = false,
                        .index = has_index ? index : -1,
                        .item_id = has_id ? item : -1};
    (state.tick == world.tick() ? state.events : state.next).push_back(event);
    json::Object data;
    if (has_id) {
        data.emplace("id", item);
    }
    if (has_index) {
        data.emplace("index", index);
    }
    world.emit(std::string(kind), id, std::move(data));
}

// Window::hide on an open popup menu, and what its popup_hide does: a PopupMenu emits it; a
// MenuButton emits it and is released (_popup_visibility_changed); an OptionButton is released.
void hide_popup(world::World& world, GuiState& state, const std::string& owner) {
    const auto found = std::ranges::find(state.popups, owner, &PopupStatus::owner);
    if (found == state.popups.end()) {
        return;
    }
    state.popups.erase(found);
    state.popups_changed = true;
    const entt::entity entity = world.find(owner);
    switch (popup_kind(world, entity)) {
    case PopupKind::Menu:
        world.ecs().get<world::PopupMenu>(entity).visible = false;
        emit(world, state, "popup_hide", owner);
        break;
    case PopupKind::MenuButton:
        emit(world, state, "popup_hide", owner);
        set_toggled(world, state, entity, false);
        break;
    case PopupKind::Option:
        set_toggled(world, state, entity, false);
        break;
    case PopupKind::None:
        break;
    }
}

// The first item that isn't disabled, as OptionButton's and MenuButton's show_popup look for
// one; -1 for none.
int first_enabled(const std::vector<world::PopupMenuItem>& items) {
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (!items[i].disabled) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// Popup::popup and what leads to it. An OptionButton's list starts on the option shown, or the
// first that isn't disabled: focused when keys opened it, else only scrolled to. A MenuButton
// emits about_to_popup and, opened by keys, focuses its first item that isn't disabled, and is
// pressed as its popup's about_to_popup does. A PopupMenu becomes visible and emits
// about_to_popup. Any other popup closes, and the controls lose the mouse, as the viewport's
// window loses focus to the popup's.
void show_popup(world::World& world, GuiState& state, entt::entity entity, bool by_mouse) {
    const std::string owner(world.id_of(entity));
    const PopupKind kind = popup_kind(world, entity);
    if (owner.empty() || kind == PopupKind::None || popup_status(state, owner) != nullptr) {
        return;
    }
    const std::vector<world::PopupMenuItem> items = popup_items(world, entity);
    PopupStatus status;
    status.owner = owner;
    int start = -1;
    if (kind == PopupKind::Option) {
        start = world.ecs().get<world::OptionButton>(entity).selected;
        if (start < 0 || !std::cmp_less(start, items.size()) ||
            items[static_cast<std::size_t>(start)].disabled) {
            start = first_enabled(items);
        }
    } else if (kind == PopupKind::MenuButton) {
        emit(world, state, "about_to_popup", owner);
        start = by_mouse ? -1 : first_enabled(items);
    } else {
        world.ecs().get<world::PopupMenu>(entity).visible = true;
        emit(world, state, "about_to_popup", owner);
    }
    status.scroll_to = start;
    status.focused = by_mouse ? -1 : start;
    // NOTIFICATION_POST_POPUP.
    status.opened = state.now;
    status.initial_mask = state.mouse_mask;
    status.grabbed = state.mouse_mask != 0;
    for (PopupStatus& other : state.popups) {
        other.closing = true;
    }
    state.popups.push_back(std::move(status));
    state.popups_changed = true;
    state.drop_mouse_focus = true;
    if (kind == PopupKind::MenuButton) {
        set_toggled(world, state, entity, true);
    }
}

// PopupMenu::activate_item, and what its signals do. The popup hides, unless a PopupMenu's
// hide_on_item_selection or hide_on_checkable_item_selection keeps it; then a PopupMenu or
// MenuButton emits id_pressed and index_pressed, and an OptionButton selects the option and
// emits item_selected, unless it was selected already and allow_reselect is off (_select).
void activate_item(world::World& world, GuiState& state, entt::entity entity, int index) {
    const std::vector<world::PopupMenuItem> items = popup_items(world, entity);
    if (index < 0 || !std::cmp_less(index, items.size())) {
        return;
    }
    const auto at = static_cast<std::size_t>(index);
    const world::PopupMenuItem& item = items[at];
    if (item.separator) {
        return;
    }
    const std::string owner(world.id_of(entity));
    const int id = item_id(item, at);
    const PopupKind kind = popup_kind(world, entity);
    bool hide = true;
    if (kind == PopupKind::Menu) {
        const auto& menu = world.ecs().get<world::PopupMenu>(entity);
        hide = item.checkable != "none" ? menu.hide_on_checkable_item_selection
                                        : menu.hide_on_item_selection;
    }
    if (hide) {
        hide_popup(world, state, owner);
    }
    if (kind == PopupKind::Option) {
        auto& option = world.ecs().get<world::OptionButton>(entity);
        if (index == option.selected && !option.allow_reselect) {
            return;
        }
        option.selected = index;
        emit_item(world, state, "item_selected", owner, index, id);
        return;
    }
    emit_item(world, state, "id_pressed", owner, index, id);
    emit_item(world, state, "index_pressed", owner, index, id);
}

// The keys moving to an item: id_focused, or an OptionButton's item_focused (_focused), and the
// item scrolled into view.
void focus_item(world::World& world, GuiState& state, entt::entity entity, int index) {
    const std::string owner(world.id_of(entity));
    PopupStatus* status = popup_status(state, owner);
    const std::vector<world::PopupMenuItem> items = popup_items(world, entity);
    if (status == nullptr || index < 0 || !std::cmp_less(index, items.size())) {
        return;
    }
    status->focused = index;
    status->scroll_to = index;
    const int id = item_id(items[static_cast<std::size_t>(index)], static_cast<std::size_t>(index));
    if (popup_kind(world, entity) == PopupKind::Option) {
        emit_item(world, state, "item_focused", owner, index, id);
    } else {
        emit_item(world, state, "id_focused", owner, index, id);
    }
}

// OptionButton::pressed and MenuButton::pressed: the popup hides if it's open, else shows.
void press_popup_button(world::World& world, GuiState& state, entt::entity entity, bool by_mouse) {
    const std::string owner(world.id_of(entity));
    if (popup_status(state, owner) != nullptr) {
        hide_popup(world, state, owner);
    } else {
        show_popup(world, state, entity, by_mouse);
    }
}

// Toggles a toggle button, or just presses one that isn't, as the end of a click and
// BaseButton::press do; an OptionButton or MenuButton opens or closes its popup between toggled
// and pressed, as BaseButton::_pressed calls pressed().
void fire(world::World& world, GuiState& state, entt::entity entity, bool by_mouse = false) {
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
    if (const std::string_view type = button_type(world, entity);
        type == "OptionButton" || type == "MenuButton") {
        press_popup_button(world, state, entity, by_mouse);
    }
    emit(world, state, "pressed", id);
}

// ---- Sliders ----

// Godot's Slider: a held gamepad direction steps it again after half a second, then 20 times a
// second.
constexpr double kRepeatDelay = kSliderRepeatDelay;
constexpr double kRepeatRate = 1.0 / 20.0;

// Range::set_value on a slider: its value kept in range, written and value_changed emitted when
// it changes; with quiet, written only, as with set_block_signals.
void set_slider_value(world::World& world, GuiState& state, entt::entity entity, double value,
                      bool quiet = false) {
    auto* slider = world.ecs().try_get<world::Slider>(entity);
    if (slider == nullptr) {
        return;
    }
    const Range range = range_of(*slider);
    const double next = range_value(range, value);
    if (next == range.value || (std::isnan(next) && std::isnan(range.value))) {
        return;
    }
    slider->value = next;
    const std::string id(world.id_of(entity));
    SliderStatus& status = state.sliders[id];
    status.known = next;
    status.known_set = true;
    if (!quiet) {
        emit_value(world, state, "value_changed", id, next);
    }
}

[[nodiscard]] Range slider_range(const world::World& world, entt::entity entity) {
    const auto* slider = world.ecs().try_get<world::Slider>(entity);
    return slider != nullptr ? range_of(*slider) : Range{};
}

// Range::set_as_ratio.
void set_slider_ratio(world::World& world, GuiState& state, entt::entity entity, double ratio,
                      bool quiet = false) {
    set_slider_value(world, state, entity, range_from_ratio(slider_range(world, entity), ratio),
                     quiet);
}

// Keeps each slider's value in range and on its step, as Range::set_value does, and emits
// value_changed for each one whose value changed since the gui system last saw it, by anything
// (a command, gameplay code); and drags end on sliders that can no longer be dragged.
void track_sliders(world::World& world, GuiState& state) {
    std::set<std::string, std::less<>> seen;
    for (auto [entity, slider] : world.ecs().view<world::Slider>().each()) {
        const double value = range_of(slider).value;
        if (value != slider.value && !(std::isnan(value) && std::isnan(slider.value))) {
            slider.value = value;
        }
        const std::string_view id = world.id_of(entity);
        if (id.empty()) {
            continue;
        }
        seen.emplace(id);
        SliderStatus& status = state.sliders[std::string(id)];
        if (!status.known_set) {
            status.known = value;
            status.known_set = true;
        } else if (value != status.known && !(std::isnan(value) && std::isnan(status.known))) {
            status.known = value;
            emit_value(world, state, "value_changed", std::string(id), value);
        }
        if (!slider.editable) {
            status.active = false;
        }
    }
    std::erase_if(state.sliders, [&](const auto& each) { return !seen.contains(each.first); });
}

// Slider's NOTIFICATION_INTERNAL_PROCESS: a held gamepad direction steps it on repeat.
void repeat_sliders(world::World& world, GuiState& state, double tick_seconds) {
    for (auto& [id, status] : state.sliders) {
        if (!status.repeating) {
            continue;
        }
        const entt::entity entity = world.find(id);
        const auto* slider =
            entity != entt::null ? world.ecs().try_get<world::Slider>(entity) : nullptr;
        if (slider == nullptr) {
            status.repeating = false;
            continue;
        }
        const auto released = [&](std::string_view action) {
            return input::is_action_just_released(world, action);
        };
        if (released("ui_left") || released("ui_right") || released("ui_up") ||
            released("ui_down")) {
            status.repeat = kRepeatDelay;
            status.repeating = false;
            continue;
        }
        status.repeat -= tick_seconds;
        if (status.repeat > 0.0) {
            continue;
        }
        status.repeat += kRepeatRate;
        const double step = slider->step;
        const bool rtl = status.repeat_rtl;
        const auto held = [&](std::string_view action) {
            return input::is_action_pressed(world, action);
        };
        const auto nudge = [&](double by) {
            const double value = range_of(*world.ecs().try_get<world::Slider>(entity)).value;
            set_slider_value(world, state, entity, value + by);
        };
        if (!slider->vertical) {
            if (held("ui_left")) {
                nudge(rtl ? step : -step);
            }
            if (held("ui_right")) {
                nudge(rtl ? -step : step);
            }
        } else {
            if (held("ui_down")) {
                nudge(-step);
            }
            if (held("ui_up")) {
                nudge(step);
            }
        }
    }
}

// ---- Scroll bars ----

constexpr double kPageDivisor = 8.0; // ScrollBar::PAGE_DIVISOR: the wheel moves an eighth page
constexpr float kDeceleration =
    1000.0F;                         // a touch glide slows by this many pixels a second, a second
constexpr double kSpeedSample = 0.1; // seconds a touch drag's speed is measured over

// Math::is_equal_approx.
bool approx(double a, double b) {
    if (a == b) {
        return true;
    }
    const double tolerance = std::max(0.00001 * std::abs(a), 0.00001);
    return std::abs(a - b) < tolerance;
}

// A ScrollContainer bar's range, as the layout set it, holding a value.
Range view_range(const ScrollBarView& view, double value) {
    return {.min = view.min_value,
            .max = view.max_value,
            .step = view.step,
            .page = view.page,
            .value = value,
            .rounded = false,
            .allow_greater = false,
            .allow_lesser = false};
}

// A ScrollContainer's scroll as it is now: the gui state's, while the component's whole pixels
// still match it, else the component's.
std::array<double, 2> scroll_now(const world::World& world, const GuiState* state,
                                 entt::entity entity) {
    const auto* scroll = world.ecs().try_get<world::ScrollContainer>(entity);
    if (scroll == nullptr) {
        return {};
    }
    std::array<double, 2> now{static_cast<double>(scroll->scroll_horizontal),
                              static_cast<double>(scroll->scroll_vertical)};
    const std::string_view id = world.id_of(entity);
    if (state == nullptr || id.empty()) {
        return now;
    }
    if (const auto found = state->scrolls.find(id); found != state->scrolls.end()) {
        for (std::size_t axis = 0; axis < now.size(); ++axis) {
            const double value = found->second.value.at(axis);
            if (std::isfinite(value) &&
                static_cast<double>(static_cast<int>(value)) == now.at(axis)) {
                now.at(axis) = value;
            }
        }
    }
    return now;
}

// One axis of a ScrollContainer's scroll set as its bar's Range::set_value keeps it: kept in the
// gui state, and in whole pixels in the component, as get_h_scroll and get_v_scroll give it.
void set_scroll(world::World& world, GuiState& state, entt::entity entity, std::size_t axis,
                const ScrollBarView& view, double value) {
    auto* scroll = world.ecs().try_get<world::ScrollContainer>(entity);
    const std::string id(world.id_of(entity));
    if (scroll == nullptr || id.empty() || !std::isfinite(value)) {
        return;
    }
    const std::array<double, 2> now = scroll_now(world, &state, entity);
    ScrollStatus& status = state.scrolls[id];
    status.value = now;
    status.value.at(axis) = range_value(view_range(view, now.at(axis)), value);
    (axis == 0 ? scroll->scroll_horizontal : scroll->scroll_vertical) =
        static_cast<int>(status.value.at(axis));
}

// ScrollContainer::_cancel_drag: a touch drag or glide stops, and scroll_ended says so if it
// had begun.
void cancel_drag(world::World& world, GuiState& state, const std::string& id,
                 ScrollStatus& status) {
    status.touching = false;
    status.deaccel = false;
    status.speed = {};
    status.accum = {};
    status.last_accum = {};
    status.from = {};
    if (status.beyond_deadzone) {
        status.beyond_deadzone = false;
        emit(world, state, "scroll_ended", id);
    }
}

// Takes in scrolls set from outside the gui system (a command, gameplay code) as
// ScrollContainer::set_h_scroll does, stopping a touch drag, and drops the state of containers
// that went away.
void absorb_scrolls(world::World& world, GuiState& state) {
    for (auto it = state.scrolls.begin(); it != state.scrolls.end();) {
        const entt::entity entity = world.find(it->first);
        const auto* scroll =
            entity != entt::null ? world.ecs().try_get<world::ScrollContainer>(entity) : nullptr;
        if (scroll == nullptr) {
            it = state.scrolls.erase(it);
            continue;
        }
        ScrollStatus& status = it->second;
        bool outside = false;
        const std::array<int, 2> written{scroll->scroll_horizontal, scroll->scroll_vertical};
        for (std::size_t axis = 0; axis < written.size(); ++axis) {
            const double value = status.value.at(axis);
            if (!std::isfinite(value) || static_cast<int>(value) != written.at(axis)) {
                status.value.at(axis) = written.at(axis);
                outside = true;
            }
        }
        if (outside && status.touching) {
            cancel_drag(world, state, it->first, status);
        }
        ++it;
    }
}

// Range::set_value on a ScrollBar: its value kept in range, written and value_changed emitted
// when it changes.
void set_bar_value(world::World& world, GuiState& state, entt::entity entity, double value) {
    auto* bar = world.ecs().try_get<world::ScrollBar>(entity);
    if (bar == nullptr) {
        return;
    }
    const Range range = range_of(*bar);
    const double next = range_value(range, value);
    if (next == range.value || (std::isnan(next) && std::isnan(range.value))) {
        return;
    }
    bar->value = next;
    const std::string id(world.id_of(entity));
    ScrollBarStatus& status = state.scroll_bars[id];
    status.known = next;
    status.known_set = true;
    emit_value(world, state, "value_changed", id, next);
}

// As track_sliders, for ScrollBars: each value kept in range and value_changed emitted for a
// change from anywhere. A hidden bar lets go of its arrows and drag, as
// NOTIFICATION_VISIBILITY_CHANGED.
void track_scroll_bars(world::World& world, GuiState& state) {
    std::set<std::string, std::less<>> seen;
    std::optional<Parents> parents;
    for (auto [entity, bar] : world.ecs().view<world::ScrollBar>().each()) {
        const double value = range_of(bar).value;
        if (value != bar.value && !(std::isnan(value) && std::isnan(bar.value))) {
            bar.value = value;
        }
        const std::string_view id = world.id_of(entity);
        if (id.empty()) {
            continue;
        }
        seen.emplace(id);
        ScrollBarStatus& status = state.scroll_bars[std::string(id)];
        if (!status.known_set) {
            status.known = value;
            status.known_set = true;
        } else if (value != status.known && !(std::isnan(value) && std::isnan(status.known))) {
            status.known = value;
            emit_value(world, state, "value_changed", std::string(id), value);
        }
        if (!idle(status.bar)) {
            if (!parents) {
                parents.emplace(world);
            }
            if (!parents->visible_in_tree(entity)) {
                status.bar = {};
            }
        }
    }
    std::erase_if(state.scroll_bars, [&](const auto& each) { return !seen.contains(each.first); });
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

// What the pointer is on: a control, or one of a ScrollContainer's bars.
struct Target {
    entt::entity entity = entt::null;
    Bar bar = Bar::Own;
};

// One tick's work: the layout to find controls in, laid out again when a popup opens or closes,
// and whether the event being sent was handled.
class Gui {
public:
    Gui(world::World& world, GuiState& state, text::Fonts& fonts, double now)
        : world_(world), state_(state), fonts_(fonts),
          layout_(layout(world, fonts, settings(world).viewport)),
          map_(input::map(std::as_const(world))), now_(now),
          last_mouse_(input::mouse_position(world)) {
        index_tree();
        settle_popups();
    }

    // What a popup opening or closing leaves to do once an event is handled: the controls lose
    // the mouse as their window loses focus (Viewport::_drop_mouse_focus), and the layout catches
    // up.
    void after_event() {
        if (state_.drop_mouse_focus) {
            state_.drop_mouse_focus = false;
            drop_mouse_focus();
        }
        if (state_.popups_changed) {
            relayout();
        }
    }

    // Once a tick, after its events: popup lists scroll to the items asked for, and a MenuButton
    // whose menu is open hands it to another the mouse is on, with switch_on_hover, in the same
    // parent (MenuButton's NOTIFICATION_INTERNAL_PROCESS).
    void process_popups() {
        if (std::ranges::any_of(state_.popups,
                                [](const PopupStatus& status) { return status.scroll_to >= 0; })) {
            relayout();
        }
        const entt::entity hovered = find(std::string(owner_of(state_.mouse_over)));
        const auto* other = hovered != entt::null && bar_named(state_.mouse_over) == Bar::Own
                                ? world_.ecs().try_get<world::MenuButton>(hovered)
                                : nullptr;
        if (other == nullptr || !other->switch_on_hover || other->disabled) {
            return;
        }
        for (const PopupStatus& status : state_.popups) {
            const entt::entity menu = find(status.owner);
            const auto* own =
                menu != entt::null ? world_.ecs().try_get<world::MenuButton>(menu) : nullptr;
            if (own == nullptr || !own->switch_on_hover || menu == hovered || status.closing) {
                continue;
            }
            const entt::entity up = parent(menu);
            const entt::entity other_up = parent(hovered);
            if (!(up != entt::null && contains(up, hovered)) &&
                !(other_up != entt::null && contains(other_up, menu))) {
                continue;
            }
            const std::string owner = status.owner;
            hide_popup(world_, state_, owner);
            // pressed(), not a press: no toggle and no pressed signal. Not opened by the mouse,
            // so its first item is focused, then unfocused.
            press_popup_button(world_, state_, hovered, false);
            if (PopupStatus* opened = popup_status(state_, id(hovered)); opened != nullptr) {
                opened->focused = -1;
            }
            after_event();
            return;
        }
    }

    // What ScrollContainers do once a tick, after its events: one that follows focus scrolls to
    // a control that got it, a finger let go glides on (NOTIFICATION_INTERNAL_PROCESS), and each
    // scroll stays in its bars' ranges, written back in whole pixels (_update_scrollbars).
    void process_scrolls(double tick_seconds) {
        if (state_.key_focus != state_.followed) {
            state_.followed = state_.key_focus;
            if (const entt::entity focus = find(state_.key_focus); focus != entt::null) {
                follow_focus(focus);
            }
        }
        for (const auto& [entity, view] : layout_.scrolls) {
            const auto* scroll = world_.ecs().try_get<world::ScrollContainer>(entity);
            if (scroll == nullptr || world_.id_of(entity).empty()) {
                continue;
            }
            if (const auto found = state_.scrolls.find(world_.id_of(entity));
                found != state_.scrolls.end() && found->second.touching) {
                glide(entity, *scroll, view, found->second, tick_seconds);
            }
            settle(entity, view);
        }
    }

    // Viewport::push_input, from the mouse over on: an open popup menu takes what isn't a left
    // press outside it, as Viewport::_sub_windows_forward_input sends events to the window with
    // focus.
    void send(const input::Event& event) {
        handled_ = false;
        track_sticks(event);
        if (event.type == input::EventType::MouseButton) {
            const std::uint16_t bit = mask_bit(event.button);
            state_.mouse_mask = event.pressed
                                    ? static_cast<std::uint16_t>(state_.mouse_mask | bit)
                                    : static_cast<std::uint16_t>(state_.mouse_mask & ~bit);
        }
        if (event.type == input::EventType::MouseButton ||
            event.type == input::EventType::MouseMotion) {
            last_mouse_ = event.position;
            update_mouse_over(event.position);
        }
        if (forward_to_popup(event)) {
            after_event();
            return;
        }
        dispatch(event);
        after_event();
    }

private:
    void dispatch(const input::Event& event) {
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
        case input::EventType::Ime:
            ime_event(event);
            break;
        }
        if (!handled_ &&
            (event.type == input::EventType::Key || event.type == input::EventType::JoypadButton)) {
            shortcut_input(event);
        }
        if (!handled_ && event.type == input::EventType::Key) {
            unhandled_key(event);
        }
    }

    void index_tree() {
        roots_.clear();
        kids_.clear();
        for (const entt::entity entity : layout_.tree) {
            const Placed* placed = layout_.find(entity);
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

    void relayout() {
        layout_ = layout(world_, fonts_, settings(world_).viewport);
        index_tree();
        areas_.clear();
        state_.popups_changed = false;
        settle_popups();
    }

    // A popup list laid out with an item to scroll to keeps the scroll it came to.
    void settle_popups() {
        for (PopupStatus& status : state_.popups) {
            if (status.scroll_to < 0) {
                continue;
            }
            if (const PopupView* view = popup_view(status.owner); view != nullptr) {
                status.scroll = view->scroll;
                status.scroll_to = -1;
            }
        }
    }

    // A stick pushed to a focus action, as is_action_just_pressed_by_event: it moves focus once
    // a push.
    void track_sticks(const input::Event& event) {
        static constexpr std::array<std::string_view, 6> kMoves{
            "ui_focus_next", "ui_focus_prev", "ui_up", "ui_left", "ui_right", "ui_down"};
        just_pushed_.clear();
        if (event.type != input::EventType::JoypadMotion || map_ == nullptr) {
            return;
        }
        for (const std::string_view action : kMoves) {
            const auto found = map_->find_event(action, event, false);
            if (!found) {
                continue;
            }
            const auto key =
                std::make_tuple(event.device, static_cast<int>(event.axis), std::string(action));
            if (!found->match.pressed) {
                state_.joy_held.erase(key);
            } else if (state_.joy_held.insert(key).second) {
                just_pushed_.insert(action);
            }
        }
    }

    [[nodiscard]] const PopupView* popup_view(std::string_view owner) const {
        const entt::entity entity = owner.empty() ? entt::null : world_.find(owner);
        if (entity == entt::null) {
            return nullptr;
        }
        const auto found = std::ranges::find(layout_.popups, entity, &PopupView::owner);
        return found != layout_.popups.end() ? &*found : nullptr;
    }

    // The popup with focus: the last one open that isn't closing.
    PopupStatus* top_popup() {
        for (PopupStatus& status : std::views::reverse(state_.popups)) {
            if (!status.closing) {
                return &status;
            }
        }
        return nullptr;
    }

    // A left press outside the popup with focus closes it at the end of the tick, as its parent
    // window taking focus does (Popup::_parent_focused), and goes on to the controls; anything
    // else goes to it.
    bool forward_to_popup(const input::Event& event) {
        PopupStatus* top = top_popup();
        if (top == nullptr) {
            return false;
        }
        const PopupView* view = popup_view(top->owner);
        if (event.type == input::EventType::MouseButton && event.pressed &&
            event.button == input::MouseButton::Left &&
            (view == nullptr || !inside(view->size, event.position - view->position))) {
            top->closing = true;
            return false;
        }
        if (view != nullptr) {
            popup_input(top->owner, *view, event);
        }
        return true;
    }

    // Rect2::has_point.
    static bool has(Vec2 position, Vec2 size, Vec2 point) {
        return point.x >= position.x && point.y >= position.y && point.x < position.x + size.x &&
               point.y < position.y + size.y;
    }

    // PopupMenu::_get_mouse_over: the item at a viewport point, in the list as the scroll
    // container cuts it; -1 for none.
    static int item_at(const PopupView& view, Vec2 point) {
        const Vec2 room{view.size.x - view.margins[0] - view.margins[2],
                        view.size.y - view.margins[1] - view.margins[3]};
        const Vec2 local = point - view.position - Vec2{view.margins[0], view.margins[1]};
        const auto separation = static_cast<float>(view.v_separation);
        float total = 0.0F;
        for (const PopupItemView& item : view.items) {
            total += item.height + separation;
        }
        // The list's rect in the scroll container, which fills it at least, cut to it.
        const float left = view.list_position.x - view.margins[0];
        const float x0 = std::max(0.0F, left);
        const float x1 = std::min(room.x, left + view.list_size.x);
        const float y0 = std::max(0.0F, -view.scroll);
        const float y1 = std::min(room.y, std::max(total, room.y) - view.scroll);
        if (local.x < x0 || local.y < y0 || local.x >= x1 || local.y >= y1) {
            return -1;
        }
        const float y = local.y + view.scroll;
        float bottom = 0.0F;
        for (std::size_t i = 0; i < view.items.size(); ++i) {
            bottom += separation;
            bottom += view.items[i].height;
            if (bottom > y) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    // PopupMenu::_mouse_over_update: the item under the pointer, or none over a separator or a
    // disabled item.
    static void mouse_over_update(PopupStatus& status, const PopupView& view, Vec2 point) {
        const int over = item_at(view, point);
        status.focused =
            over >= 0 && view.items[static_cast<std::size_t>(over)].selectable ? over : -1;
    }

    // PopupMenu::_input_from_window: its own handling, then Popup's, which closes it on
    // ui_cancel.
    void popup_input(const std::string& owner, const PopupView& view, const input::Event& event) {
        popup_menu_input(owner, view, event);
        if (action_pressed(event, "ui_cancel", false, true)) {
            if (PopupStatus* status = popup_status(state_, owner); status != nullptr) {
                status->closing = true;
            }
        }
    }

    // PopupMenu::_input_from_window_internal. Submenus, item shortcuts and holding a gamepad
    // direction aren't here.
    void popup_menu_input(const std::string& owner, const PopupView& view,
                          const input::Event& event) {
        const entt::entity entity = find(owner);
        if (entity == entt::null) {
            return;
        }
        const auto status = [&] { return popup_status(state_, owner); };
        const std::vector<world::PopupMenuItem> items = popup_items(world_, entity);
        const int count = static_cast<int>(items.size());
        const auto selectable = [&](int i) {
            const world::PopupMenuItem& item = items[static_cast<std::size_t>(i)];
            return !item.separator && !item.disabled;
        };
        if (!items.empty()) {
            const bool joypad = event.type == input::EventType::JoypadMotion ||
                                event.type == input::EventType::JoypadButton;
            const auto just = [&](std::string_view action) {
                return event.type == input::EventType::JoypadButton ? !event.echo
                                                                    : just_pushed_.contains(action);
            };
            // The next selectable item one way, going round; -1 for none.
            const auto next = [&](int step) {
                const int focused = status()->focused;
                int from = focused + step;
                if (from >= count || from < 0) {
                    from = step > 0 ? 0 : count - 1;
                }
                for (int i = from; i >= 0 && i < count; i += step) {
                    if (selectable(i)) {
                        return i;
                    }
                }
                for (int i = step > 0 ? 0 : count - 1; step > 0 ? i < from : i >= from; i += step) {
                    if (selectable(i)) {
                        return i;
                    }
                }
                return -1;
            };
            if (is_action(event, "ui_down", true) && is_pressed(event)) {
                if (joypad && !just("ui_down")) {
                    return;
                }
                if (const int to = next(1); to >= 0) {
                    focus_item(world_, state_, entity, to);
                }
            } else if (is_action(event, "ui_up", true) && is_pressed(event)) {
                if (joypad && !just("ui_up")) {
                    return;
                }
                if (const int to = next(-1); to >= 0) {
                    focus_item(world_, state_, entity, to);
                }
            } else if (is_action(event, "ui_accept", true) && is_pressed(event)) {
                const int over = status()->focused;
                if (over >= 0 && over < count && !items[static_cast<std::size_t>(over)].separator) {
                    activate_item(world_, state_, entity, over);
                    return;
                }
            }
        }

        // The items' area: the panel inside its margins, less the scroll bar (twice in right to
        // left, as Godot's).
        Vec2 area_position = view.position;
        Vec2 area_size = view.size;
        if (view.bar_width > 0.0F) {
            if (view.rtl) {
                area_position.x += view.bar_width;
                area_size.x -= view.bar_width;
            }
            area_size.x -= view.bar_width;
        }
        area_position = area_position + Vec2{view.margins[0], view.margins[1]};
        area_size =
            area_size - Vec2{view.margins[0] + view.margins[2], view.margins[1] + view.margins[3]};
        const Vec2 point = event.position;

        if (event.type == input::EventType::MouseButton) {
            PopupStatus& s = *status();
            // A release of the left button, or of any button held as it opened, picks an item,
            // so a press can open it and its release pick one.
            if (event.button == input::MouseButton::Left ||
                (s.initial_mask & mask_bit(event.button)) != 0) {
                if (event.pressed) {
                    s.grabbed = false;
                    s.scrolling = view.rtl ? point.x < area_position.x - area_size.x
                                           : point.x > area_size.x + area_position.x;
                    if (!has(area_position, area_size, point)) {
                        return;
                    }
                    mouse_over_update(s, view, point);
                    return;
                }
                if (s.scrolling) {
                    s.scrolling = false;
                    return;
                }
                const bool was_grabbed = s.grabbed;
                s.grabbed = false;
                s.initial_mask = 0;
                if (!has(area_position, area_size, point) ||
                    (was_grabbed && now_ - s.opened < kGrabbedClickSeconds)) {
                    return;
                }
                const int over = item_at(view, point);
                if (over < 0) {
                    if (!was_grabbed) {
                        hide_popup(world_, state_, owner);
                    }
                    return;
                }
                if (selectable(over)) {
                    activate_item(world_, state_, entity, over);
                }
                return;
            }
            // The wheel scrolls the list by an eighth of its page, or a quarter over its scroll
            // bar, as ScrollContainer and ScrollBar do.
            if (event.pressed && view.scroll_max > 0.0F &&
                (event.button == input::MouseButton::WheelUp ||
                 event.button == input::MouseButton::WheelDown)) {
                const Vec2 room{view.size.x - view.margins[0] - view.margins[2],
                                view.size.y - view.margins[1] - view.margins[3]};
                const Vec2 container = view.position + Vec2{view.margins[0], view.margins[1]};
                if (!has(container, room, point)) {
                    return;
                }
                const Vec2 bar{view.rtl ? container.x : container.x + room.x - view.bar_width,
                               container.y};
                const auto page = static_cast<double>(view.list_size.y);
                const double by =
                    has(bar, {view.bar_width, room.y}, point) ? page / 4.0 : page / kPageDivisor;
                const double sign = event.button == input::MouseButton::WheelUp ? -1.0 : 1.0;
                s.scroll = std::clamp(static_cast<double>(view.scroll) +
                                          (sign * by * static_cast<double>(event.factor)),
                                      0.0, static_cast<double>(view.scroll_max));
                s.scroll_to = -1;
            }
            return;
        }

        if (event.type == input::EventType::MouseMotion) {
            // Godot skips motion with no velocity.
            if (event.relative && *event.relative == Vec2{}) {
                return;
            }
            PopupStatus& s = *status();
            if (s.focused == -1 && !has(area_position, area_size, point)) {
                return;
            }
            mouse_over_update(s, view, point);
            return;
        }

        // Typing the start of an item's text moves to the next one that starts so, the letters
        // typed within two seconds of each other making up what it looks for.
        const auto* menu = world_.ecs().try_get<world::PopupMenu>(entity);
        const bool allow_search = menu == nullptr || menu->allow_search;
        if (allow_search && event.type == input::EventType::Key && event.unicode >= 0x20U &&
            event.pressed && !items.empty()) {
            PopupStatus& s = *status();
            const double diff = now_ - s.search_time;
            s.search_time = now_;
            if (diff > kSearchInterval) {
                s.search.clear();
            }
            if (s.search != std::u32string(1, event.unicode)) {
                s.search += event.unicode;
            }
            const std::string wanted = text::to_upper(to_utf8(s.search));
            const int over = s.focused;
            for (int i = over + 1; i <= count; ++i) {
                if (i == count) {
                    if (over <= 0) {
                        break;
                    }
                    i = 0;
                }
                if (i == over) {
                    break;
                }
                if (text::to_upper(items[static_cast<std::size_t>(i)].text).starts_with(wanted)) {
                    focus_item(world_, state_, entity, i);
                    break;
                }
            }
        }
    }

    // Viewport::_drop_mouse_focus: the control a held mouse button pressed gets a release for
    // each button, and loses the mouse.
    void drop_mouse_focus() {
        const Target focus = target(state_.mouse_focus);
        const std::uint16_t mask = state_.mouse_focus_mask;
        state_.mouse_focus.clear();
        state_.mouse_focus_mask = 0;
        if (focus.entity == entt::null) {
            return;
        }
        for (const input::MouseButton button :
             {input::MouseButton::Left, input::MouseButton::Right, input::MouseButton::Middle}) {
            if ((mask & mask_bit(button)) == 0) {
                continue;
            }
            input::Event release;
            release.type = input::EventType::MouseButton;
            release.device = kInternalDevice;
            release.button = button;
            release.pressed = false;
            release.position = last_mouse_;
            if (focus.bar != Bar::Own) {
                bar_input(focus.entity, focus.bar, release);
            } else {
                gui_input(focus.entity, release);
            }
        }
    }

    [[nodiscard]] std::string id(entt::entity entity) const {
        return std::string(world_.id_of(entity));
    }
    [[nodiscard]] entt::entity find(const std::string& id) const {
        return id.empty() ? entt::null : world_.find(id);
    }
    // The control or bar an ID the gui system keeps names.
    [[nodiscard]] Target target(std::string_view id) const {
        const Bar bar = bar_named(id);
        const std::string_view owner = owner_of(id);
        const entt::entity entity = owner.empty() ? entt::null : world_.find(owner);
        if (bar != Bar::Own && !layout_.scrolls.contains(entity)) {
            return {};
        }
        return {.entity = entity, .bar = bar};
    }
    [[nodiscard]] std::string target_id(Target target) const {
        return target.entity == entt::null ? std::string() : bar_id(id(target.entity), target.bar);
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
    [[nodiscard]] Target find_control(Vec2 point) const {
        for (const entt::entity root : std::views::reverse(roots_)) {
            if (!visible(root)) {
                continue;
            }
            if (const Target found = find_at(root, point); found.entity != entt::null) {
                return found;
            }
        }
        return {};
    }

    // Viewport::_gui_find_control_at_pos. A control that clips lets the pointer reach its
    // children only inside its rect, and a ScrollContainer's bars come before its children, as
    // its last ones.
    [[nodiscard]] Target find_at(entt::entity entity, Vec2 point) const {
        const Placed* found = placed(entity);
        if (found == nullptr || !found->visible) {
            return {};
        }
        const auto local = inverse(found->transform);
        if (!local) {
            return {};
        }
        const Vec2 at = local->apply(point);
        if (!found->clips || inside(found->size, at)) {
            if (const auto scroll = layout_.scrolls.find(entity);
                scroll != layout_.scrolls.end() && !world_.id_of(entity).empty()) {
                for (const Bar bar : {Bar::Vertical, Bar::Horizontal}) {
                    const ScrollBarView& view =
                        bar == Bar::Vertical ? scroll->second.v : scroll->second.h;
                    if (view.visible && inside(view.size, at - view.position)) {
                        return {.entity = entity, .bar = bar};
                    }
                }
            }
            const std::vector<entt::entity>& children = kids(entity);
            for (const entt::entity child : std::views::reverse(children)) {
                if (const Target hit = find_at(child, point); hit.entity != entt::null) {
                    return hit;
                }
            }
        }
        if (mouse_filter(world_, entity) == Filter::Ignore || world_.id_of(entity).empty()) {
            return {};
        }
        return has_point(entity, at) ? Target{.entity = entity, .bar = Bar::Own} : Target{};
    }

    // Rect2::has_point for a rect at the origin.
    static bool inside(Vec2 size, Vec2 point) {
        return point.x >= 0.0F && point.y >= 0.0F && point.x < size.x && point.y < size.y;
    }

    // Viewport::_update_mouse_over: mouse_exited for the controls the mouse left, from the
    // innermost, then mouse_entered for the ones it is over, from the outermost.
    void update_mouse_over(Vec2 point) {
        // Over an open popup, the controls lose the mouse and the popup has it; a popup the mouse
        // leaves loses its focused item (NOTIFICATION_WM_MOUSE_EXIT).
        const PopupView* over_popup = nullptr;
        for (const PopupView& view : std::views::reverse(layout_.popups)) {
            if (inside(view.size, point - view.position)) {
                over_popup = &view;
                break;
            }
        }
        for (PopupStatus& status : state_.popups) {
            const bool now_inside =
                over_popup != nullptr && find(status.owner) == over_popup->owner;
            if (status.mouse_inside && !now_inside) {
                status.focused = -1;
            }
            status.mouse_inside = now_inside;
        }
        if (over_popup != nullptr) {
            if (!state_.mouse_over.empty() || !state_.mouse_over_hierarchy.empty()) {
                drop_mouse_over(world_, state_, {});
            }
            return;
        }
        const Target target = find_control(point);
        const entt::entity over = target.entity;
        const std::string over_id = target_id(target);
        auto& hierarchy = state_.mouse_over_hierarchy;
        if (over_id == state_.mouse_over && (over != entt::null || hierarchy.empty())) {
            return;
        }
        std::string common;
        std::vector<std::string> entering;
        if (target.bar != Bar::Own) {
            // A ScrollContainer's bar stops the pointer, so it is all the mouse is over.
            if (std::ranges::find(hierarchy, over_id) != hierarchy.end()) {
                common = over_id;
            } else {
                entering.push_back(over_id);
            }
        }
        for (entt::entity at = target.bar == Bar::Own ? over : entt::null; at != entt::null;
             at = parent(at)) {
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
                if (bar_named(each) != Bar::Own) {
                    continue; // a container's bar has no entity to tell
                }
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
            // ScrollBar's NOTIFICATION_MOUSE_EXIT.
            if (BarStatus* bar = bar_status(state, left); bar != nullptr) {
                bar->highlight = 0;
            }
            if (bar_named(left) == Bar::Own && world.find(left) != entt::null) {
                emit(world, state, "mouse_exited", left);
            }
        }
    }

private:
    // Viewport::_gui_call_input: the control, then its parents until one stops the pointer. A
    // ScrollContainer's bar takes the event and stops it there.
    void call_input(Target target, const input::Event& event) {
        if (target.bar != Bar::Own) {
            bar_input(target.entity, target.bar, event);
            handled_ = true;
            return;
        }
        const entt::entity entity = target.entity;
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
        if (is_line_edit(entity) && event.type != input::EventType::ScreenTouch &&
            event.type != input::EventType::ScreenDrag) {
            line_edit_input(entity, event);
        }
        if (is_text_edit(entity)) {
            text_edit_event(entity, event);
        }
        if (world_.ecs().all_of<world::Slider>(entity) && !world_.id_of(entity).empty()) {
            slider_input(entity, event);
        }
        if (world_.ecs().all_of<world::ScrollBar>(entity) && !world_.id_of(entity).empty()) {
            bar_input(entity, Bar::Own, event);
        }
        if (world_.ecs().all_of<world::ScrollContainer>(entity) && !world_.id_of(entity).empty()) {
            scroll_input(entity, event);
        }
    }

    [[nodiscard]] bool is_text_edit(entt::entity entity) const {
        return entity != entt::null && world_.ecs().all_of<world::TextEdit>(entity) &&
               !world_.id_of(entity).empty();
    }

    // A TextEdit's lines shaped once a tick and kept in step by the editing: shaped again if
    // something else changed its text.
    TextArea* area_of(entt::entity entity, const TextEditState& s) {
        const std::string& text = world_.ecs().get<world::TextEdit>(entity).text;
        if (const auto found = areas_.find(entity);
            found != areas_.end() && found->second.text == text) {
            return found->second.area.get();
        }
        std::optional<TextArea> made = text_area(world_, layout_, entity, s);
        if (!made) {
            return nullptr;
        }
        CachedArea& cached = areas_[entity];
        cached.area = std::make_unique<TextArea>(std::move(*made));
        cached.text = text;
        return cached.area.get();
    }

    [[nodiscard]] TextEditContext text_context(const std::string& id,
                                               std::vector<TextEditSignal>& signals) const {
        return {.world = &world_,
                .map = map_,
                .now = now_,
                .focused = state_.key_focus == id,
                .signals = &signals};
    }

    // Edits a TextEdit with one of the text_edit_* calls, then records its signals.
    template <typename Call>
    void edit_text(entt::entity entity, Call&& call) {
        const std::string id = this->id(entity);
        TextEditState& s = state_.text_edits[id];
        TextArea* area = area_of(entity, s);
        if (area == nullptr) {
            return;
        }
        auto& edit = world_.ecs().get<world::TextEdit>(entity);
        std::vector<TextEditSignal> signals;
        const TextEditContext context = text_context(id, signals);
        std::forward<Call>(call)(context, edit, s, *area);
        areas_[entity].text = edit.text;
        emit_text_edit(world_, state_, id, signals);
    }

    // TextEdit::gui_input.
    void text_edit_event(entt::entity entity, const input::Event& event) {
        const Vec2 local = to_local(entity, event.position).value_or(Vec2{});
        Vec2 relative;
        if (event.relative) {
            const Vec2 origin = to_local(entity, {}).value_or(Vec2{});
            relative = to_local(entity, *event.relative).value_or(Vec2{}) - origin;
        }
        edit_text(entity, [&](const TextEditContext& context, world::TextEdit& edit,
                              TextEditState& s, TextArea& area) {
            if (text_edit_input(context, edit, s, area, event, local, relative)) {
                handled_ = true;
            }
        });
    }

    [[nodiscard]] bool is_line_edit(entt::entity entity) const {
        return entity != entt::null && world_.ecs().all_of<world::LineEdit>(entity) &&
               !world_.id_of(entity).empty();
    }

    void toggled(entt::entity entity, bool on) {
        emit(world_, state_, "editing_toggled", id(entity), on);
    }

    // LineEdit::gui_input. Clicks and drags place the caret and select; with focus, keys and
    // actions edit. Action events edit as keys do, so agents can send ui_text_* actions.
    void line_edit_input(entt::entity entity, const input::Event& event) {
        Editor editor(world_, state_, entity, &layout_);
        const world::LineEdit& edit = editor.edit();
        EditState& s = editor.state();
        if (event.type == input::EventType::MouseButton) {
            line_edit_click(entity, editor, event);
            return;
        }
        if (event.type == input::EventType::MouseMotion) {
            const Vec2 at = to_local(entity, event.position).value_or(Vec2{});
            const Field* field = editor.field();
            if (edit.editable && !editor.text().empty() && edit.clear_button_enabled) {
                s.clear_pressing_inside =
                    s.clear_press_attempt && field != nullptr && field->over_clear_button(edit, at);
            }
            if ((event.button_mask & mask_bit(input::MouseButton::Left)) != 0 &&
                s.selection.creating) {
                editor.set_caret_at_pixel_pos(at.x);
                editor.selection_fill_at_caret();
                if (s.selection.double_click) {
                    // Whole words.
                    const std::vector<std::uint32_t> words = editor.word_breaks();
                    for (std::size_t i = 0; i + 1 < words.size(); i += 2) {
                        const auto start = static_cast<int>(words[i]);
                        const auto end = static_cast<int>(words[i + 1]);
                        if (start <= s.selection.begin && end >= s.selection.begin) {
                            s.selection.begin = start;
                        }
                        if (start <= s.selection.end && end >= s.selection.end) {
                            s.selection.end = end;
                            s.selection.enabled = true;
                            break;
                        }
                    }
                    editor.set_caret_column(editor.caret() < s.selection.start_column
                                                ? s.selection.begin
                                                : s.selection.end);
                }
            }
            return;
        }
        const bool key = event.type == input::EventType::Key;
        if (!key && event.type != input::EventType::Action &&
            event.type != input::EventType::JoypadButton) {
            return;
        }
        if (edit.editable && !editor.editing() &&
            action_pressed(event, "ui_text_submit", false, false)) {
            if (editor.start_editing(true)) {
                toggled(entity, true);
            }
            handled_ = true;
            return;
        }
        if (edit.shortcut_keys_enabled) {
            if (is_action(event, "ui_copy", true)) {
                editor.copy_text();
                handled_ = true;
                return;
            }
            if (is_action(event, "ui_text_select_all", true)) {
                editor.select(0, -1);
                handled_ = true;
                return;
            }
            if (is_action(event, "ui_cut", true)) {
                if (edit.editable) {
                    editor.cut_text();
                } else {
                    editor.copy_text();
                }
                handled_ = true;
                return;
            }
        }
        if (!editor.editing() || event.type == input::EventType::JoypadButton) {
            return;
        }
        if (!s.ime_text.empty()) {
            // The input method has the keys while it composes; typed text commits its own.
            if (key && event.unicode == 0) {
                handled_ = true;
                return;
            }
            editor.cancel_ime();
        }
        if (!event.pressed) {
            return;
        }
        handled_ = true;
        // Enter and keypad Enter by default; not ui_accept, which has Space.
        if (action_pressed(event, "ui_text_submit", false, false)) {
            emit_text(world_, state_, "text_submitted", id(entity), "text", edit.text);
            if (!edit.keep_editing_on_text_submit) {
                editor.unedit();
                toggled(entity, false);
            }
            return;
        }
        if (is_action(event, "ui_cancel", false)) {
            editor.unedit();
            toggled(entity, false);
            return;
        }
        if (edit.shortcut_keys_enabled) {
            if (is_action(event, "ui_paste", true)) {
                editor.paste_text();
                return;
            }
            if (is_action(event, "ui_undo", true)) {
                editor.undo();
                return;
            }
            if (is_action(event, "ui_redo", true)) {
                editor.redo();
                return;
            }
        }
        if (is_action(event, "ui_text_backspace_all_to_left", true)) {
            editor.backspace(false, true);
            return;
        }
        if (is_action(event, "ui_text_backspace_word", true)) {
            editor.backspace(true, false);
            return;
        }
        if (is_action(event, "ui_text_backspace", true)) {
            editor.backspace(false, false);
            return;
        }
        if (is_action(event, "ui_text_delete_all_to_right", true)) {
            editor.delete_forward(false, true);
            return;
        }
        if (is_action(event, "ui_text_delete_word", true)) {
            editor.delete_forward(true, false);
            return;
        }
        if (is_action(event, "ui_text_delete", true)) {
            editor.delete_forward(false, false);
            return;
        }
        // Caret moves match without shift, which selects.
        input::Event moved = event;
        const bool shift = moved.modifiers.shift;
        moved.modifiers.shift = false;
        if (is_action(moved, "ui_text_caret_word_left", true)) {
            editor.move_caret_left(shift, true);
            return;
        }
        if (is_action(moved, "ui_text_caret_left", true)) {
            editor.move_caret_left(shift, false);
            return;
        }
        if (is_action(moved, "ui_text_caret_word_right", true)) {
            editor.move_caret_right(shift, true);
            return;
        }
        if (is_action(moved, "ui_text_caret_right", true)) {
            editor.move_caret_right(shift, false);
            return;
        }
        // Up is Home and Down is End.
        if (is_action(moved, "ui_text_caret_up", true) ||
            is_action(moved, "ui_text_caret_line_start", true) ||
            is_action(moved, "ui_text_caret_page_up", true)) {
            editor.move_caret_to(shift, 0);
            return;
        }
        if (is_action(moved, "ui_text_caret_down", true) ||
            is_action(moved, "ui_text_caret_line_end", true) ||
            is_action(moved, "ui_text_caret_page_down", true)) {
            editor.move_caret_to(shift, editor.length());
            return;
        }
        if (is_action(moved, "ui_swap_input_direction", true)) {
            editor.swap_input_direction();
            return;
        }
        editor.reset_blink();
        // Typed characters, when no modifier but shift is held.
        const input::Modifiers& held = event.modifiers;
        if (key && !held.ctrl && !held.alt && !held.meta && edit.editable &&
            event.unicode >= 0x20U) {
            type(editor, event.unicode, false);
            return;
        }
        handled_ = false;
    }

    // Inserts a typed character over the selection: text_changed at the end of the tick when
    // deferred, as gui_input does, else now, as unhandled_key_input does.
    static void type(Editor& editor, char32_t c, bool now) {
        editor.selection_delete();
        const int before = editor.length();
        editor.insert_text_at_caret(std::u32string(1, c));
        if (editor.length() == before) {
            return;
        }
        if (now) {
            editor.text_changed();
        } else {
            editor.state().text_changed_dirty = true;
        }
    }

    // The mouse button part of LineEdit::gui_input. Middle-click paste (the primary selection)
    // and the context menu aren't here.
    void line_edit_click(entt::entity entity, Editor& editor, const input::Event& event) {
        const world::LineEdit& edit = editor.edit();
        EditState& s = editor.state();
        const Vec2 at = to_local(entity, event.position).value_or(Vec2{});
        const Field* field = editor.field();
        if (event.pressed && event.button == input::MouseButton::Right) {
            editor.apply_ime();
            if (edit.editable && !s.selection.enabled) {
                editor.set_caret_at_pixel_pos(at.x);
            }
            if (edit.editable && !editor.editing() && editor.start_editing(true)) {
                toggled(entity, true);
            }
            handled_ = true;
            return;
        }
        if (event.button != input::MouseButton::Left) {
            return;
        }
        if (editor.editing()) {
            editor.reset_blink();
        }
        if (!event.pressed) {
            if (edit.editable && !editor.text().empty() && edit.clear_button_enabled) {
                const bool attempt = s.clear_press_attempt;
                s.clear_press_attempt = false;
                if (attempt && s.clear_pressing_inside && field != nullptr &&
                    field->over_clear_button(edit, at)) {
                    editor.clear();
                    return;
                }
            }
            if (!s.selection.creating && !s.selection.double_click) {
                editor.deselect();
            }
            s.selection.creating = false;
            s.selection.double_click = false;
            if (s.pending_select_all) {
                editor.select_all();
                s.pending_select_all = false;
            }
            if (edit.editable) {
                editor.show_virtual_keyboard();
            }
            return;
        }
        editor.apply_ime();
        handled_ = true;
        if (edit.editable && !editor.text().empty() && field != nullptr &&
            field->over_clear_button(edit, at)) {
            s.clear_press_attempt = true;
            s.clear_pressing_inside = true;
            return;
        }
        const bool shift = event.modifiers.shift;
        if (shift) {
            editor.shift_selection_check_pre(true);
        }
        editor.set_caret_at_pixel_pos(at.x);
        if (shift) {
            editor.selection_fill_at_caret();
            s.selection.creating = true;
        } else {
            if (edit.selecting_enabled && !editor.text().empty()) {
                const Vec2 apart = at - s.last_double_click_position;
                const bool triple = !event.double_click && s.last_double_click >= 0.0 &&
                                    now_ - s.last_double_click < kTripleClickSeconds &&
                                    std::hypot(apart.x, apart.y) < kTripleClickTolerance;
                if (triple) {
                    s.selection.enabled = true;
                    s.selection.begin = 0;
                    s.selection.end = editor.length();
                    s.selection.double_click = true;
                    s.last_double_click = -1.0;
                    editor.set_caret_column(0);
                } else if (event.double_click) {
                    s.last_double_click = now_;
                    s.last_double_click_position = at;
                    const std::vector<std::uint32_t> words = editor.word_breaks();
                    for (std::size_t i = 0; i + 1 < words.size(); i += 2) {
                        const auto start = static_cast<int>(words[i]);
                        const auto end = static_cast<int>(words[i + 1]);
                        if (start <= editor.caret() && end >= editor.caret()) {
                            s.selection.enabled = true;
                            s.selection.begin = start;
                            s.selection.end = end;
                            s.selection.double_click = true;
                            s.selection.creating = true;
                            s.selection.start_column = editor.caret();
                            editor.set_caret_column(end);
                            break;
                        }
                    }
                }
            }
            if (!s.selection.double_click) {
                editor.deselect();
                s.selection.start_column = editor.caret();
                s.selection.creating = true;
            }
        }
        if (edit.editable && !editor.editing() && editor.start_editing(true)) {
            toggled(entity, true);
        }
    }

    // MainLoop's NOTIFICATION_OS_IME_UPDATE, for the LineEdit being edited.
    void ime_event(const input::Event& event) {
        const entt::entity focus = find(state_.key_focus);
        if (is_text_edit(focus)) {
            edit_text(focus, [&](const TextEditContext& context, world::TextEdit& edit,
                                 TextEditState& s, TextArea& area) {
                text_edit_ime_update(context, edit, s, area, to_utf32(event.text),
                                     event.selection_start, event.selection_length);
            });
            handled_ = true;
            return;
        }
        if (!is_line_edit(focus)) {
            return;
        }
        Editor editor(world_, state_, focus, &layout_);
        if (!editor.editing()) {
            return;
        }
        editor.ime_update(to_utf32(event.text), event.selection_start, event.selection_length);
        handled_ = true;
    }

    // LineEdit::unhandled_key_input: a character typed with modifiers no shortcut took.
    void unhandled_key(const input::Event& event) {
        const entt::entity focus = find(state_.key_focus);
        if (is_text_edit(focus)) {
            edit_text(focus, [&](const TextEditContext& context, world::TextEdit& edit,
                                 TextEditState& s, TextArea& area) {
                if (text_edit_unhandled_key(context, edit, s, area, event)) {
                    handled_ = true;
                }
            });
            return;
        }
        if (!event.pressed || event.unicode < 0x20U || !is_line_edit(focus)) {
            return;
        }
        Editor editor(world_, state_, focus, &layout_);
        if (!editor.editing() || !editor.edit().editable) {
            return;
        }
        type(editor, event.unicode, true);
        handled_ = true;
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

    // Slider::gui_input: a press moves the grabber to the pointer and starts a drag, motion drags
    // it, the wheel and the ui_* actions step it.
    void slider_input(entt::entity entity, const input::Event& event) {
        const auto* slider = world_.ecs().try_get<world::Slider>(entity);
        const auto view = layout_.sliders.find(entity);
        const Placed* at = placed(entity);
        if (slider == nullptr || !slider->editable || view == layout_.sliders.end() ||
            at == nullptr) {
            return;
        }
        const std::string slider_id = id(entity);
        const bool vertical = slider->vertical;
        const bool rtl = view->second.rtl;
        const Vec2 grabber = view->second.center_grabber ? Vec2{} : view->second.grabber;
        const auto ratio = [&] { return range_ratio(slider_range(world_, entity)); };
        if (event.type == input::EventType::MouseButton) {
            if (event.button == input::MouseButton::Left) {
                SliderStatus& status = state_.sliders[slider_id];
                if (event.pressed) {
                    const Vec2 local = to_local(entity, event.position).value_or(Vec2{});
                    status.pos = vertical ? local.y : local.x;
                    status.before = ratio();
                    emit(world_, state_, "drag_started", slider_id);
                    const float length = vertical ? at->size.y - grabber.y : at->size.x - grabber.x;
                    if (length > 0.0F) {
                        if (vertical) {
                            set_slider_ratio(world_, state_, entity,
                                             1.0 - ((status.pos - (grabber.y / 2.0)) / length),
                                             true);
                        } else {
                            const double v = (status.pos - (grabber.x / 2.0)) / length;
                            set_slider_ratio(world_, state_, entity, rtl ? 1.0 - v : v, true);
                        }
                    }
                    SliderStatus& now = state_.sliders[slider_id];
                    now.active = true;
                    now.uvalue = ratio();
                    // _notify_shared_value_changed: emitted even when the value stayed.
                    emit_value(world_, state_, "value_changed", slider_id,
                               slider_range(world_, entity).value);
                } else {
                    status.active = false;
                    // Math::is_equal_approx.
                    const double after = ratio();
                    const double tolerance = std::max(0.00001 * std::abs(status.before), 0.00001);
                    const bool changed =
                        status.before != after && std::abs(status.before - after) >= tolerance;
                    emit_value(world_, state_, "drag_ended", slider_id, 0.0, changed);
                }
            } else if (slider->scrollable && event.pressed &&
                       (event.button == input::MouseButton::WheelUp ||
                        event.button == input::MouseButton::WheelDown)) {
                if (focusable(entity)) {
                    grab(world_, state_, slider_id, state_.hide_focus);
                }
                const Range range = slider_range(world_, entity);
                const double by =
                    event.button == input::MouseButton::WheelUp ? range.step : -range.step;
                set_slider_value(world_, state_, entity, range.value + by);
            }
            return;
        }
        if (event.type == input::EventType::MouseMotion) {
            const SliderStatus& status = state_.sliders[slider_id];
            if (!status.active) {
                return;
            }
            const Vec2 local = to_local(entity, event.position).value_or(Vec2{});
            double motion = (vertical ? local.y : local.x) - status.pos;
            if (vertical || rtl) {
                motion = -motion;
            }
            const double size = vertical ? std::trunc(at->size.y) : std::trunc(at->size.x);
            const double area = size - (vertical ? grabber.y : grabber.x);
            if (area <= 0.0) {
                return;
            }
            set_slider_ratio(world_, state_, entity, status.uvalue + (motion / area));
            return;
        }
        const bool joypad = event.type == input::EventType::JoypadMotion ||
                            event.type == input::EventType::JoypadButton;
        // is_action_just_pressed_by_event: a stick or button that just went that way.
        const auto just = [&](std::string_view action) {
            return event.type == input::EventType::JoypadButton ? !event.echo
                                                                : just_pushed_.contains(action);
        };
        const Range range = slider_range(world_, entity);
        // One step one way or the other; false if the action isn't for this orientation.
        const auto step = [&](std::string_view action, bool horizontal, double by) {
            if (horizontal == vertical) {
                return false;
            }
            if (joypad) {
                if (!just(action)) {
                    return false;
                }
                SliderStatus& status = state_.sliders[slider_id];
                status.repeating = true;
                status.repeat_rtl = rtl;
            }
            set_slider_value(world_, state_, entity, range.value + by);
            handled_ = true;
            return true;
        };
        if (action_pressed(event, "ui_left", true, false)) {
            step("ui_left", true, rtl ? range.step : -range.step);
        } else if (action_pressed(event, "ui_right", true, false)) {
            step("ui_right", true, rtl ? -range.step : range.step);
        } else if (action_pressed(event, "ui_up", true, false)) {
            step("ui_up", false, range.step);
        } else if (action_pressed(event, "ui_down", true, false)) {
            step("ui_down", false, -range.step);
        } else if (is_action(event, "ui_home", true) && is_pressed(event)) {
            set_slider_value(world_, state_, entity, range.min);
            handled_ = true;
        } else if (is_action(event, "ui_end", true) && is_pressed(event)) {
            set_slider_value(world_, state_, entity, range.max);
            handled_ = true;
        }
    }

    // A touch drag's speed, measured every tenth of a second while the finger moves, and once
    // it lets go, the glide: the scroll moves on at that speed, slowing by 1000 pixels a second
    // each second, until it stops or reaches both ends.
    void glide(entt::entity entity, const world::ScrollContainer& scroll, const ScrollView& view,
               ScrollStatus& status, double tick_seconds) {
        const auto delta = static_cast<float>(tick_seconds);
        if (!status.deaccel) {
            if (status.time_since_motion == 0.0 || status.time_since_motion > kSpeedSample) {
                const Vec2 diff = status.accum - status.last_accum;
                status.last_accum = status.accum;
                status.speed = {diff.x / delta, diff.y / delta};
            }
            status.time_since_motion += tick_seconds;
            return;
        }
        const std::array<double, 2> now = scroll_now(world_, &state_, entity);
        Vec2 pos{static_cast<float>(now[0]), static_cast<float>(now[1])};
        pos = pos + (status.speed * delta);
        bool off_h = false;
        bool off_v = false;
        const auto keep = [](float& value, const ScrollBarView& bar, bool& off) {
            if (value < 0.0F) {
                value = 0.0F;
                off = true;
            }
            const auto end = static_cast<float>(bar.max_value - bar.page);
            if (value > end) {
                value = end;
                off = true;
            }
        };
        keep(pos.x, view.h, off_h);
        keep(pos.y, view.v, off_v);
        if (scroll.horizontal_scroll_mode != "disabled") {
            set_scroll(world_, state_, entity, 0, view.h, pos.x);
        }
        if (scroll.vertical_scroll_mode != "disabled") {
            set_scroll(world_, state_, entity, 1, view.v, pos.y);
        }
        const auto slow = [&](float speed, bool& off) {
            const float sign = speed < 0.0F ? -1.0F : 1.0F;
            const auto left =
                static_cast<float>(static_cast<double>(std::abs(speed)) -
                                   (static_cast<double>(kDeceleration) * tick_seconds));
            if (left < 0.0F) {
                off = true;
            }
            return sign * left;
        };
        status.speed = {slow(status.speed.x, off_h), slow(status.speed.y, off_v)};
        if (off_h && off_v) {
            cancel_drag(world_, state_, id(entity), status);
        }
    }

    // _update_scrollbars: the scroll kept in its bars' ranges, as set_max and set_page keep their
    // values, and written to the component in whole pixels. A bar hidden, or in a hidden
    // container, lets go of its arrows and drag, as NOTIFICATION_VISIBILITY_CHANGED.
    void settle(entt::entity entity, const ScrollView& view) {
        auto& scroll = world_.ecs().get<world::ScrollContainer>(entity);
        const std::string owner = id(entity);
        const std::array<double, 2> now = scroll_now(world_, &state_, entity);
        const std::array<double, 2> kept{range_value(view_range(view.h, now[0]), now[0]),
                                         range_value(view_range(view.v, now[1]), now[1])};
        const bool fraction = kept[0] != std::trunc(kept[0]) || kept[1] != std::trunc(kept[1]);
        auto found = state_.scrolls.find(owner);
        if (found == state_.scrolls.end() && fraction) {
            found = state_.scrolls.try_emplace(owner).first;
        }
        if (found != state_.scrolls.end()) {
            ScrollStatus& status = found->second;
            status.value = kept;
            const bool shown = visible(entity);
            const auto let_go = [](BarStatus& bar) {
                bar.decrement_pressed = false;
                bar.increment_pressed = false;
                bar.dragging = false;
            };
            if (!shown || !view.h.visible) {
                let_go(status.h);
            }
            if (!shown || !view.v.visible) {
                let_go(status.v);
            }
            if (idle(status.h) && idle(status.v) && !status.touching && !status.beyond_deadzone &&
                !fraction) {
                state_.scrolls.erase(found);
            }
        }
        const std::array<int, 2> whole{static_cast<int>(kept[0]), static_cast<int>(kept[1])};
        if (scroll.scroll_horizontal != whole[0]) {
            scroll.scroll_horizontal = whole[0];
        }
        if (scroll.scroll_vertical != whole[1]) {
            scroll.scroll_vertical = whole[1];
        }
    }

    // ScrollContainer::_gui_focus_changed: each ScrollContainer around the control that follows
    // focus scrolls it into view, the innermost first.
    void follow_focus(entt::entity focus) {
        std::map<entt::entity, Vec2> moved; // each one's scroll_diff, for the ones around it
        for (entt::entity at = parent(focus); at != entt::null; at = parent(at)) {
            const auto* scroll = world_.ecs().try_get<world::ScrollContainer>(at);
            if (scroll != nullptr && scroll->follow_focus && layout_.scrolls.contains(at) &&
                !world_.id_of(at).empty()) {
                moved[at] = ensure_visible(at, focus, moved);
            }
        }
    }

    // How far one axis must scroll to show a span: to its start when it fits, else as little
    // as shows the most of it, as ensure_control_visible's per-axis rule.
    static float scroll_needed(float begin, float end, bool fits) {
        if (fits) {
            if (begin <= 0.0F) {
                return begin;
            }
            return end <= 0.0F ? 0.0F : end;
        }
        if (begin >= 0.0F) {
            return begin;
        }
        return end >= 0.0F ? 0.0F : end;
    }

    // The bounding box of a rect moved by a transform.
    static Rect2 transformed(const Transform& t, const Rect2& r) {
        const std::array<Vec2, 4> corners{t.apply(r.position), t.apply({r.end().x, r.position.y}),
                                          t.apply({r.position.x, r.end().y}), t.apply(r.end())};
        Vec2 low = corners[0];
        Vec2 high = corners[0];
        for (const Vec2 corner : corners) {
            low = {std::min(low.x, corner.x), std::min(low.y, corner.y)};
            high = {std::max(high.x, corner.x), std::max(high.y, corner.y)};
        }
        return {low, high - low};
    }

    // ScrollContainer::ensure_control_visible: the least scroll that shows the control, or the
    // part of it the ScrollContainers inside this one show, from where the layout placed it.
    // Returns how far it scrolled (scroll_diff).
    Vec2 ensure_visible(entt::entity container, entt::entity control,
                        const std::map<entt::entity, Vec2>& moved) {
        const Placed* shown = placed(control);
        if (shown == nullptr || placed(container) == nullptr) {
            return {};
        }
        Rect2 local{{}, shown->size};
        Transform target_in_sc;
        entt::entity target = control;
        for (entt::entity at = parent(control); at != entt::null; at = parent(at)) {
            const auto inner = layout_.scrolls.find(at);
            if (inner == layout_.scrolls.end()) {
                continue;
            }
            const auto to_sc = inverse(placed(at)->transform);
            if (!to_sc) {
                return {};
            }
            target_in_sc = (*to_sc * placed(target)->transform) * target_in_sc;
            if (at == container) {
                break;
            }
            target = at;
            if (const auto diff = moved.find(at); diff != moved.end()) {
                // The inner one has scrolled since the layout: move by that too.
                target_in_sc.tx -= diff->second.x;
                target_in_sc.ty -= diff->second.y;
            }
            const auto back = inverse(target_in_sc);
            if (!back) {
                return {};
            }
            const Rect2 seen =
                transformed(*back, {inner->second.visible_position, inner->second.visible_size});
            const Vec2 low{std::max(local.position.x, seen.position.x),
                           std::max(local.position.y, seen.position.y)};
            const Vec2 high{std::min(local.end().x, seen.end().x),
                            std::min(local.end().y, seen.end().y)};
            if (high.x <= low.x || high.y <= low.y) {
                return {}; // hidden by the inner one
            }
            local = {low, high - low};
        }
        const ScrollView& view = layout_.scrolls.at(container);
        const Rect2 visible_rect{view.visible_position, view.visible_size};
        const Rect2 target_rect = transformed(target_in_sc, local);
        const Vec2 begin = target_rect.position - visible_rect.position;
        const Vec2 end = target_rect.end() - visible_rect.end();
        const Vec2 diff{scroll_needed(begin.x, end.x, visible_rect.size.x > target_rect.size.x),
                        scroll_needed(begin.y, end.y, visible_rect.size.y > target_rect.size.y)};
        // set_h_scroll and set_v_scroll take whole pixels, and stop a touch drag.
        set_scroll(world_, state_, container, 0, view.h,
                   static_cast<double>(static_cast<int>(view.scroll.x + diff.x)));
        set_scroll(world_, state_, container, 1, view.v,
                   static_cast<double>(static_cast<int>(view.scroll.y + diff.y)));
        if (const auto found = state_.scrolls.find(world_.id_of(container));
            found != state_.scrolls.end()) {
            cancel_drag(world_, state_, id(container), found->second);
        }
        return diff;
    }

    // A ScrollContainer's status, made with the scroll its component has.
    ScrollStatus& scroll_status(entt::entity entity) {
        const auto [found, made] = state_.scrolls.try_emplace(id(entity));
        if (made) {
            found->second.value = scroll_now(world_, nullptr, entity);
        }
        return found->second;
    }

    BarStatus& bar_state(entt::entity entity, Bar bar) {
        if (bar == Bar::Own) {
            return state_.scroll_bars[id(entity)].bar;
        }
        ScrollStatus& scroll = scroll_status(entity);
        return bar == Bar::Horizontal ? scroll.h : scroll.v;
    }

    // A bar as the layout measured it: a ScrollBar's, or one of a ScrollContainer's.
    [[nodiscard]] const ScrollBarView* bar_view(entt::entity entity, Bar bar) const {
        if (bar == Bar::Own) {
            const auto found = layout_.scroll_bars.find(entity);
            return found != layout_.scroll_bars.end() ? &found->second : nullptr;
        }
        const auto found = layout_.scrolls.find(entity);
        if (found == layout_.scrolls.end()) {
            return nullptr;
        }
        return bar == Bar::Horizontal ? &found->second.h : &found->second.v;
    }

    // A bar's range holding the value it has now: a ScrollBar's own, or its container's scroll
    // along it.
    [[nodiscard]] Range bar_range(entt::entity entity, Bar bar, const ScrollBarView& view) const {
        if (bar == Bar::Own) {
            const auto* own = world_.ecs().try_get<world::ScrollBar>(entity);
            return own != nullptr ? range_of(*own) : view_range(view, view.value);
        }
        const std::array<double, 2> now = scroll_now(world_, &state_, entity);
        return view_range(view, now.at(bar == Bar::Horizontal ? 0 : 1));
    }

    // ScrollBar::scroll_to: its value set, and scrolling emitted when it moved.
    void bar_scroll_to(entt::entity entity, Bar bar, const ScrollBarView& view, double value) {
        if (bar != Bar::Own) {
            set_scroll(world_, state_, entity, bar == Bar::Horizontal ? 0 : 1, view, value);
            return;
        }
        const double before = bar_range(entity, bar, view).value;
        set_bar_value(world_, state_, entity, value);
        if (!approx(before, bar_range(entity, bar, view).value)) {
            emit(world_, state_, "scrolling", id(entity));
        }
    }

    // ScrollBar::gui_input, for a ScrollBar or one of a ScrollContainer's bars: a click on an
    // arrow steps it, a click before or after the grabber pages it, a click on the grabber drags
    // it, the wheel moves it an eighth of a page, and with focus the ui_* actions step it. It
    // takes every mouse event.
    void bar_input(entt::entity entity, Bar bar, const input::Event& event) {
        const ScrollBarView* found = bar_view(entity, bar);
        if (found == nullptr || world_.id_of(entity).empty()) {
            return;
        }
        const ScrollBarView& view = *found;
        const bool vertical = view.vertical;
        const auto range = [&] { return bar_range(entity, bar, view); };
        const double step = view.custom_step >= 0.0 ? view.custom_step : range().step;
        const auto along = [vertical](Vec2 v) { return static_cast<double>(vertical ? v.y : v.x); };
        // Where along it the pointer is.
        const auto offset = [&] {
            Vec2 at = to_local(entity, event.position).value_or(Vec2{});
            if (bar != Bar::Own) {
                at = at - view.position;
            }
            return along(at);
        };
        const auto decrement = static_cast<double>(view.decrement);
        const auto increment = static_cast<double>(view.increment);
        const double total = along(view.size);
        if (event.type == input::EventType::MouseButton) {
            handled_ = true;
            const bool down = event.button == input::MouseButton::WheelDown;
            if (event.pressed && (down || event.button == input::MouseButton::WheelUp)) {
                const Range r = range();
                const double change =
                    (r.page != 0.0 ? r.page / kPageDivisor : (r.max - r.min) / 16.0) *
                    static_cast<double>(event.factor);
                const double by = std::max(change, r.step);
                bar_scroll_to(entity, bar, view, r.value + (down ? by : -by));
            }
            if (event.button != input::MouseButton::Left) {
                return;
            }
            if (!event.pressed) {
                BarStatus& status = bar_state(entity, bar);
                status.decrement_pressed = false;
                status.increment_pressed = false;
                status.dragging = false;
                return;
            }
            double ofs = offset();
            if (ofs < decrement) {
                bar_state(entity, bar).decrement_pressed = true;
                bar_scroll_to(entity, bar, view, range().value - step);
                return;
            }
            if (ofs > total - increment) {
                bar_state(entity, bar).increment_pressed = true;
                bar_scroll_to(entity, bar, view, range().value + step);
                return;
            }
            ofs -= decrement + static_cast<double>(view.begin);
            const Range r = range();
            const double grabber_offset = view.area * range_ratio(r);
            const double page = r.page != 0.0 ? r.page : (r.max - r.min) / 16.0;
            if (ofs < grabber_offset) {
                bar_scroll_to(entity, bar, view, std::clamp(r.value - page, r.min, r.max - r.page));
                return;
            }
            ofs -= grabber_offset;
            if (ofs < view.grabber_size) {
                BarStatus& status = bar_state(entity, bar);
                status.dragging = true;
                status.pos_at_click = grabber_offset + ofs;
                status.value_at_click = range_ratio(r);
            } else {
                bar_scroll_to(entity, bar, view, std::clamp(r.value + page, r.min, r.max - r.page));
            }
            return;
        }
        if (event.type == input::EventType::MouseMotion) {
            handled_ = true;
            const double ofs = offset();
            BarStatus& status = bar_state(entity, bar);
            if (status.dragging) {
                const double diff =
                    (ofs - decrement - static_cast<double>(view.begin) - status.pos_at_click) /
                    view.area;
                const double ratio = status.value_at_click + diff;
                bar_scroll_to(entity, bar, view, range_from_ratio(range(), ratio));
            } else if (ofs < decrement) {
                status.highlight = 1;
            } else if (ofs > total - increment) {
                status.highlight = 3;
            } else {
                status.highlight = 2;
            }
            return;
        }
        if (!is_pressed(event)) {
            return;
        }
        const Range r = range();
        if (is_action(event, "ui_left", true)) {
            if (!vertical) {
                bar_scroll_to(entity, bar, view, r.value - step);
            }
        } else if (is_action(event, "ui_right", true)) {
            if (!vertical) {
                bar_scroll_to(entity, bar, view, r.value + step);
            }
        } else if (is_action(event, "ui_up", true)) {
            if (vertical) {
                bar_scroll_to(entity, bar, view, r.value - step);
            }
        } else if (is_action(event, "ui_down", true)) {
            if (vertical) {
                bar_scroll_to(entity, bar, view, r.value + step);
            }
        } else if (is_action(event, "ui_home", true)) {
            bar_scroll_to(entity, bar, view, r.min);
        } else if (is_action(event, "ui_end", true)) {
            bar_scroll_to(entity, bar, view, r.max);
        }
    }

    // ScrollContainer::gui_input: the wheel scrolls it an eighth of a page, the vertical bar
    // first unless scroll_horizontal_by_default or Shift says otherwise, and a finger drags it
    // once past its deadzone, gliding on when it lets go. It takes what moved it.
    void scroll_input(entt::entity entity, const input::Event& event) {
        const auto found = layout_.scrolls.find(entity);
        const auto* scroll = world_.ecs().try_get<world::ScrollContainer>(entity);
        if (found == layout_.scrolls.end() || scroll == nullptr ||
            event.device == input::kEmulatedDevice) {
            return;
        }
        const ScrollView& view = found->second;
        const std::array<double, 2> before = scroll_now(world_, &state_, entity);
        const bool h_enabled = scroll->horizontal_scroll_mode != "disabled";
        const bool v_enabled = scroll->vertical_scroll_mode != "disabled";
        const auto moved = [&] { return scroll_now(world_, &state_, entity) != before; };
        const auto scroll_to = [&](std::size_t axis, double value) {
            set_scroll(world_, state_, entity, axis, axis == 0 ? view.h : view.v, value);
        };
        if (event.type == input::EventType::MouseButton) {
            if (!event.pressed) {
                return;
            }
            bool modified = false;
            const bool swap = scroll->scroll_horizontal_by_default != event.modifiers.shift;
            const bool v_hidden = !view.v.visible && scroll->vertical_scroll_mode != "never";
            const bool h_hidden = !view.h.visible && scroll->horizontal_scroll_mode != "never";
            // ScrollBar::scroll by an eighth of its page, one way or the other.
            const auto wheel = [&](std::size_t axis, double sign) {
                const double page = axis == 0 ? view.h.page : view.v.page;
                const double now = scroll_now(world_, &state_, entity).at(axis);
                scroll_to(axis,
                          now + (sign * page / kPageDivisor * static_cast<double>(event.factor)));
                modified = true;
            };
            using input::MouseButton;
            if (event.button == MouseButton::WheelUp || event.button == MouseButton::WheelDown) {
                const double sign = event.button == MouseButton::WheelUp ? -1.0 : 1.0;
                if ((h_enabled && swap) || v_hidden) {
                    wheel(0, sign);
                } else if (v_enabled) {
                    wheel(1, sign);
                }
            }
            if (event.button == MouseButton::WheelLeft || event.button == MouseButton::WheelRight) {
                const double sign = event.button == MouseButton::WheelLeft ? -1.0 : 1.0;
                if ((v_enabled && swap) || h_hidden) {
                    wheel(1, sign);
                } else if (h_enabled) {
                    wheel(0, sign);
                }
            }
            if (modified && moved()) {
                handled_ = true;
            }
            return;
        }
        const std::string owner = id(entity);
        if (event.type == input::EventType::ScreenTouch) {
            ScrollStatus& status = scroll_status(entity);
            if (event.pressed) {
                if (status.touching) {
                    cancel_drag(world_, state_, owner, status);
                }
                status.speed = {};
                status.accum = {};
                status.last_accum = {};
                status.from = {static_cast<float>(before[0]), static_cast<float>(before[1])};
                status.touching = true;
                status.deaccel = false;
                status.beyond_deadzone = false;
                status.time_since_motion = 0.0;
            } else if (status.touching) {
                if (status.speed == Vec2{}) {
                    cancel_drag(world_, state_, owner, status);
                } else {
                    status.deaccel = true;
                }
            }
            return;
        }
        if (event.type != input::EventType::ScreenDrag) {
            return;
        }
        const auto status_found = state_.scrolls.find(owner);
        if (status_found != state_.scrolls.end() && status_found->second.touching &&
            !status_found->second.deaccel) {
            ScrollStatus& status = status_found->second;
            const Vec2 motion = event.relative.value_or(Vec2{});
            status.accum = status.accum - motion;
            const auto deadzone = static_cast<float>(scroll->scroll_deadzone);
            if (status.beyond_deadzone || (h_enabled && std::abs(status.accum.x) > deadzone) ||
                (v_enabled && std::abs(status.accum.y) > deadzone)) {
                if (!status.beyond_deadzone) {
                    emit(world_, state_, "scroll_started", owner);
                    status.beyond_deadzone = true;
                    // Starting over here keeps the scroll from jumping by the deadzone.
                    status.accum = Vec2{} - motion;
                }
                const Vec2 diff = status.from + status.accum;
                if (h_enabled) {
                    scroll_to(0, diff.x);
                } else {
                    status.accum.x = 0.0F;
                }
                if (v_enabled) {
                    scroll_to(1, diff.y);
                } else {
                    status.accum.y = 0.0F;
                }
                status.time_since_motion = 0.0;
            }
        }
        if (moved()) {
            handled_ = true;
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
            fire(world_, state_, entity,
                 event.type == input::EventType::MouseButton ||
                     event.type == input::EventType::ScreenTouch);
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
                const Target over = find_control(event.position);
                if (over.entity == entt::null) {
                    // A click on nothing hides focus, though the focus stays.
                    if (!state_.key_focus.empty() && event.button == input::MouseButton::Left) {
                        state_.hide_focus = true;
                    }
                    return;
                }
                state_.mouse_focus = target_id(over);
                state_.mouse_focus_mask |= bit;
            }
            const Target focus = target(state_.mouse_focus);
            if (event.button == input::MouseButton::Left && focus.bar == Bar::Own) {
                // The first focusable control up from the one clicked takes focus, hidden. A
                // container's bar can't have focus, and stops the search.
                for (entt::entity at = focus.entity; at != entt::null; at = parent(at)) {
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
            if (focus.entity != entt::null) {
                call_input(focus, event);
            }
            return;
        }
        state_.mouse_focus_mask &= static_cast<std::uint16_t>(~bit);
        if (state_.mouse_focus.empty()) {
            return;
        }
        const Target focus = target(state_.mouse_focus);
        if (state_.mouse_focus_mask == 0) {
            state_.mouse_focus.clear();
        }
        if (focus.entity != entt::null) {
            call_input(focus, event);
        }
    }

    void mouse_motion(const input::Event& event) {
        const Target over =
            !state_.mouse_focus.empty() ? target(state_.mouse_focus) : find_control(event.position);
        if (over.entity != entt::null) {
            call_input(over, event);
        }
    }

    void screen_touch(const input::Event& event) {
        if (event.pressed) {
            const Target over = find_control(event.position);
            if (over.entity != entt::null) {
                state_.touch_focus[event.index] = target_id(over);
                call_input(over, event);
            }
            return;
        }
        const auto found = state_.touch_focus.find(event.index);
        if (found == state_.touch_focus.end()) {
            return;
        }
        const Target over = target(found->second);
        state_.touch_focus.erase(found);
        if (over.entity != entt::null) {
            call_input(over, event);
        }
    }

    void screen_drag(const input::Event& event) {
        const auto found = state_.touch_focus.find(event.index);
        const Target over = found != state_.touch_focus.end() ? target(found->second)
                                                              : find_control(event.position);
        if (over.entity != entt::null) {
            call_input(over, event);
        }
    }

    // Keys, gamepads and actions: the control with focus first, then the ui_* actions move
    // focus.
    void action_event(const input::Event& event) {
        static constexpr std::array<std::string_view, 6> kMoves{
            "ui_focus_next", "ui_focus_prev", "ui_up", "ui_left", "ui_right", "ui_down"};
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
        bool arrow = false;
        for (const std::string_view action : kMoves) {
            const bool moved =
                event.type == input::EventType::JoypadMotion
                    ? action_pressed(event, action, false, false) && just_pushed_.contains(action)
                    : action_pressed(event, action, true, true);
            if (!moved) {
                continue;
            }
            show_focus = true;
            arrow = action != "ui_focus_next" && action != "ui_focus_prev";
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
            state_.focus_by_arrow = arrow;
            grab(world_, state_, id(next), false);
            state_.focus_by_arrow = false;
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
    text::Fonts& fonts_;
    Layout layout_;
    const input::Map* map_;
    std::vector<entt::entity> roots_;
    std::unordered_map<entt::entity, std::vector<entt::entity>> kids_;
    double now_ = 0.0; // seconds of world time, for triple clicks
    Vec2 last_mouse_;  // where the mouse last was
    bool handled_ = false;
    std::set<std::string_view> just_pushed_; // actions a stick just pushed, for the event sent
    struct CachedArea {
        std::unique_ptr<TextArea> area;
        std::string text; // the text it was shaped for
    };
    std::unordered_map<entt::entity, CachedArea> areas_;
};

// Drops the state of LineEdits that went away, stops editing one that can no longer be edited,
// as set_editable does, and blinks carets, as NOTIFICATION_INTERNAL_PROCESS does.
void edit_upkeep(world::World& world, GuiState& state, double tick_seconds) {
    for (auto it = state.edits.begin(); it != state.edits.end();) {
        const entt::entity entity = world.find(it->first);
        auto* edit = entity != entt::null ? world.ecs().try_get<world::LineEdit>(entity) : nullptr;
        if (edit == nullptr) {
            it = state.edits.erase(it);
            continue;
        }
        const std::string id = it->first;
        EditState& s = it->second;
        if (s.editing && (!edit->editable || state.key_focus != id)) {
            Editor(world, state, entity, nullptr).unedit();
            emit(world, state, "editing_toggled", id, false);
        }
        const bool can_draw = edit->caret_force_displayed || (s.editing && state.key_focus == id);
        if (edit->caret_blink && can_draw && edit->caret_blink_interval > 0.0F) {
            s.blink_timer += tick_seconds;
            if (s.blink_timer >= static_cast<double>(edit->caret_blink_interval) - 1e-9) {
                s.blink_timer = 0.0;
                s.draw_caret = !s.draw_caret;
            }
        } else if (!edit->caret_blink) {
            s.draw_caret = true;
        }
        ++it;
    }
}

// Drops the state of TextEdits that went away and runs their timers. One whose text or caret
// changed from outside catches up, as set_text does, and a drag selection held still follows
// the pointer.
void text_edit_upkeep(world::World& world, GuiState& state, double tick_seconds) {
    std::vector<std::string> stale;
    std::vector<std::string> held;
    const bool left = input::is_mouse_button_pressed(world, input::MouseButton::Left);
    for (auto it = state.text_edits.begin(); it != state.text_edits.end();) {
        const entt::entity entity = world.find(it->first);
        const auto* edit =
            entity != entt::null ? world.ecs().try_get<world::TextEdit>(entity) : nullptr;
        if (edit == nullptr) {
            it = state.text_edits.erase(it);
            continue;
        }
        TextEditState& s = it->second;
        if (text_edit_tick(*edit, s, state.key_focus == it->first, tick_seconds, left)) {
            held.push_back(it->first);
        }
        if (s.known && (s.known_text != edit->text || s.caret.line != edit->caret_line ||
                        s.caret.column != edit->caret_column)) {
            stale.push_back(it->first);
        }
        ++it;
    }
    text::Fonts* found = fonts(world);
    if ((stale.empty() && held.empty()) || found == nullptr) {
        return;
    }
    const Layout placed = layout(world, *found, settings(world).viewport);
    const input::Map* map = input::map(std::as_const(world));
    const auto each = [&](const std::string& id, const auto& call) {
        const entt::entity entity = world.find(id);
        TextEditState& s = state.text_edits[id];
        std::optional<TextArea> area = text_area(world, placed, entity, s);
        if (!area) {
            return;
        }
        std::vector<TextEditSignal> signals;
        const TextEditContext context{.world = &world,
                                      .map = map,
                                      .now = 0.0,
                                      .focused = state.key_focus == id,
                                      .signals = &signals};
        call(context, world.ecs().get<world::TextEdit>(entity), s, *area, entity);
        emit_text_edit(world, state, id, signals);
    };
    for (const std::string& id : stale) {
        each(id, [](const TextEditContext& context, world::TextEdit& edit, TextEditState& s,
                    TextArea& area,
                    entt::entity /*entity*/) { sync_text_edit(context, edit, s, area); });
    }
    const Vec2 mouse = input::mouse_position(world);
    for (const std::string& id : held) {
        each(id, [&](const TextEditContext& context, world::TextEdit& edit, TextEditState& s,
                     TextArea& area, entt::entity entity) {
            const Placed* at = placed.find(entity);
            const auto local = at != nullptr ? inverse(at->transform) : std::nullopt;
            text_edit_click_held(context, edit, s, area, local ? local->apply(mouse) : Vec2{});
        });
    }
}

// Drops what refers to controls that went away, were hidden or can no longer have it, as
// Viewport::_gui_hide_control and _gui_remove_control, BaseButton's visibility and
// set_disabled do, and runs shortcut highlights down.
void upkeep(world::World& world, GuiState& state, double tick_seconds) {
    const bool tracking = !state.key_focus.empty() || !state.mouse_focus.empty() ||
                          !state.mouse_over_hierarchy.empty() || !state.touch_focus.empty() ||
                          !state.buttons.empty() || !state.edits.empty() ||
                          !state.text_edits.empty();
    if (!tracking) {
        return;
    }
    edit_upkeep(world, state, tick_seconds);
    text_edit_upkeep(world, state, tick_seconds);
    const Parents parents(world);
    const auto shown = [&](const std::string& id) {
        const entt::entity entity = world.find(owner_of(id));
        return entity != entt::null && is_control(world, entity) &&
               (bar_named(id) == Bar::Own || world.ecs().all_of<world::ScrollContainer>(entity)) &&
               parents.visible_in_tree(entity);
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

// Keeps the open popups in step with the world: one whose entity went away goes quietly; a
// PopupMenu set invisible closes, as does a button's list when the button is hidden
// (NOTIFICATION_VISIBILITY_CHANGED); a PopupMenu set visible opens.
void popup_upkeep(world::World& world, GuiState& state) {
    std::erase_if(state.popups, [&](const PopupStatus& status) {
        return popup_kind(world, world.find(status.owner)) == PopupKind::None;
    });
    std::vector<std::string> hidden;
    if (!state.popups.empty()) {
        const Parents parents(world);
        for (const PopupStatus& status : state.popups) {
            const entt::entity entity = world.find(status.owner);
            if (const auto* menu = world.ecs().try_get<world::PopupMenu>(entity); menu != nullptr) {
                if (!menu->visible) {
                    hidden.push_back(status.owner);
                }
            } else if (!parents.visible_in_tree(entity)) {
                hidden.push_back(status.owner);
            }
        }
    }
    for (const std::string& owner : hidden) {
        hide_popup(world, state, owner);
    }
    std::vector<entt::entity> shown;
    for (const auto [entity, menu] : world.ecs().view<world::PopupMenu>().each()) {
        if (menu.visible && !world.id_of(entity).empty() &&
            popup_status(state, world.id_of(entity)) == nullptr) {
            shown.push_back(entity);
        }
    }
    std::ranges::sort(shown, [](entt::entity a, entt::entity b) {
        return entt::to_entity(a) < entt::to_entity(b);
    });
    for (const entt::entity entity : shown) {
        show_popup(world, state, entity, false);
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
    state->now = static_cast<double>(world.tick()) * tick_seconds;
    upkeep(world, *state, tick_seconds);
    popup_upkeep(world, *state);
    track_sliders(world, *state);
    track_scroll_bars(world, *state);
    absorb_scrolls(world, *state);
    const std::span<const input::Event> events = input::events(std::as_const(world));
    const std::vector<std::string> presses = std::move(state->presses);
    state->presses.clear();
    const std::vector<std::pair<std::string, int>> selects = std::move(state->selects);
    state->selects.clear();
    // ScrollContainers need their layout every tick, to glide, follow focus and keep in range.
    const bool scrolls = !world.ecs().view<world::ScrollContainer>().empty();
    std::optional<Gui> gui;
    if (!events.empty() || scrolls || !presses.empty() || !selects.empty() ||
        state->drop_mouse_focus) {
        if (text::Fonts* found = fonts(world); found != nullptr) {
            gui.emplace(world, *state, *found, state->now);
            gui->after_event();
            for (const input::Event& event : events) {
                gui->send(event);
            }
            if (scrolls) {
                gui->process_scrolls(tick_seconds);
            }
        }
    }
    for (const std::string& id : presses) {
        if (const entt::entity entity = world.find(id); entity != entt::null) {
            fire(world, *state, entity);
            if (gui) {
                gui->after_event();
            }
        }
    }
    for (const auto& [id, index] : selects) {
        if (const entt::entity entity = world.find(id); entity != entt::null) {
            activate_item(world, *state, entity, index);
            if (gui) {
                gui->after_event();
            }
        }
    }
    if (gui) {
        gui->process_popups();
    }
    // Popup::_close_pressed's deferred hide.
    std::vector<std::string> closing;
    for (const PopupStatus& status : state->popups) {
        if (status.closing) {
            closing.push_back(status.owner);
        }
    }
    for (const std::string& owner : closing) {
        hide_popup(world, *state, owner);
    }
    if (!gui && state->drop_mouse_focus) {
        state->mouse_focus.clear();
        state->mouse_focus_mask = 0;
    }
    state->drop_mouse_focus = false;
    state->popups_changed = false;
    repeat_sliders(world, *state, tick_seconds);
    std::erase_if(state->buttons, [](const auto& each) { return idle(each.second); });
    // LineEdit's deferred _text_changed: one text_changed a tick, and one step to undo.
    for (auto& [id, edit] : state->edits) {
        if (!edit.text_changed_dirty) {
            continue;
        }
        const entt::entity entity = world.find(id);
        if (entity != entt::null && world.ecs().all_of<world::LineEdit>(entity)) {
            Editor(world, *state, entity, nullptr).text_changed();
        }
        edit.text_changed_dirty = false;
    }
    // TextEdit's deferred text_changed and caret_changed.
    for (auto& [id, edit] : state->text_edits) {
        const std::vector<std::string> deferred = std::move(edit.deferred);
        edit.deferred.clear();
        if (world.find(id) == entt::null) {
            continue;
        }
        for (const std::string& kind : deferred) {
            emit(world, *state, kind, id);
        }
    }
}

} // namespace

// ---- Public ----

void install_gui(world::World& world) {
    if (state_of(world) != nullptr) {
        return;
    }
    world.ecs().ctx().emplace<Handle>(std::make_shared<GuiState>());
    // Focus, hover, presses and edits start over; the clipboard stays, as the system's does.
    world.add_reset([](world::World& in_world) {
        if (GuiState* state = state_of(in_world); state != nullptr) {
            GuiState fresh;
            fresh.clipboard = std::move(state->clipboard);
            *state = std::move(fresh);
        }
    });
    world.add_system(
        "gui", run,
        {.doc = "Sends input events to controls: hover, focus, buttons, sliders and text fields.",
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

bool open_popup(world::World& world, entt::entity owner) {
    GuiState* state = state_of(world);
    const PopupKind kind = popup_kind(world, owner);
    if (state == nullptr || kind == PopupKind::None || world.id_of(owner).empty()) {
        return false;
    }
    if (kind != PopupKind::Menu &&
        (!is_control(world, owner) || !Parents(world).visible_in_tree(owner))) {
        return false;
    }
    const std::string id(world.id_of(owner));
    if (PopupStatus* status = popup_status(*state, id); status == nullptr) {
        show_popup(world, *state, owner, false);
    } else if (status->closing) {
        // Closing as another opened: it stays open, on top, and the others close instead.
        for (PopupStatus& other : state->popups) {
            other.closing = other.owner != id;
        }
        const auto found = std::ranges::find(state->popups, id, &PopupStatus::owner);
        std::rotate(found, found + 1, state->popups.end());
        state->popups_changed = true;
    }
    return true;
}

bool close_popup(world::World& world, entt::entity owner) {
    GuiState* state = state_of(world);
    if (state == nullptr || world.id_of(owner).empty()) {
        return false;
    }
    const std::string id(world.id_of(owner));
    if (popup_status(*state, id) != nullptr) {
        hide_popup(world, *state, id);
        return true;
    }
    // A PopupMenu made visible that the gui system hasn't opened yet.
    auto* menu = world.ecs().try_get<world::PopupMenu>(owner);
    if (menu == nullptr || !menu->visible) {
        return false;
    }
    menu->visible = false;
    return true;
}

std::vector<entt::entity> open_popups(const world::World& world) {
    std::vector<entt::entity> out;
    const GuiState* state = state_of(world);
    for (const PopupState& each : popup_states(world)) {
        // One closing at the end of the tick, as another opened, is as good as closed.
        const bool closing =
            state != nullptr && std::ranges::any_of(state->popups, [&](const PopupStatus& status) {
                return status.closing && status.owner == world.id_of(each.owner);
            });
        if (!closing) {
            out.push_back(each.owner);
        }
    }
    return out;
}

bool select_item(world::World& world, entt::entity owner, int index) {
    GuiState* state = state_of(world);
    if (state == nullptr || world.id_of(owner).empty() ||
        popup_kind(world, owner) == PopupKind::None) {
        return false;
    }
    const std::vector<world::PopupMenuItem> items = popup_items(world, owner);
    if (index < 0 || !std::cmp_less(index, items.size())) {
        return false;
    }
    const world::PopupMenuItem& item = items[static_cast<std::size_t>(index)];
    const std::optional<Base> base = base_of(world, owner);
    if (item.separator || item.disabled || (base && base->disabled)) {
        return false;
    }
    state->selects.emplace_back(world.id_of(owner), index);
    return true;
}

std::vector<PopupState> popup_states(const world::World& world) {
    std::vector<PopupState> out;
    if (const GuiState* state = state_of(world); state != nullptr) {
        for (const PopupStatus& status : state->popups) {
            if (const entt::entity owner = world.find(status.owner); owner != entt::null) {
                out.push_back({.owner = owner,
                               .focused = status.focused,
                               .scroll = static_cast<float>(status.scroll),
                               .scroll_to = status.scroll_to});
            }
        }
    }
    std::vector<entt::entity> untracked;
    for (const auto [entity, menu] : world.ecs().view<world::PopupMenu>().each()) {
        if (menu.visible && std::ranges::find(out, entity, &PopupState::owner) == out.end()) {
            untracked.push_back(entity);
        }
    }
    std::ranges::sort(untracked, [](entt::entity a, entt::entity b) {
        return entt::to_entity(a) < entt::to_entity(b);
    });
    for (const entt::entity entity : untracked) {
        out.push_back({.owner = entity, .focused = -1, .scroll = 0.0F, .scroll_to = -1});
    }
    return out;
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

bool is_pressed(const world::World& world, entt::entity button) {
    const std::optional<Base> base = base_of(world, button);
    if (!base) {
        return false;
    }
    if (base->toggle_mode) {
        return base->button_pressed;
    }
    const GuiState* state = state_of(world);
    if (state == nullptr) {
        return false;
    }
    const auto found = state->buttons.find(world.id_of(button));
    return found != state->buttons.end() && found->second.press_attempt;
}

bool is_hovered(const world::World& world, entt::entity control) {
    const GuiState* state = state_of(world);
    const std::string_view id = world.id_of(control);
    return state != nullptr && !id.empty() &&
           std::ranges::find(state->mouse_over_hierarchy, id) != state->mouse_over_hierarchy.end();
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
    return state != nullptr && !state->mouse_over.empty() ? world.find(owner_of(state->mouse_over))
                                                          : entt::null;
}

bool shows_focus(const world::World& world, entt::entity control) {
    const GuiState* state = state_of(world);
    return state != nullptr && !state->hide_focus && !state->key_focus.empty() &&
           state->key_focus == world.id_of(control);
}

BarLook bar_look(const world::World& world, entt::entity control, Bar bar) {
    const GuiState* state = state_of(world);
    const std::string_view id = world.id_of(control);
    if (state == nullptr || id.empty()) {
        return {};
    }
    const BarStatus* status = bar_status(*state, bar_id(id, bar));
    if (status == nullptr) {
        return {};
    }
    return {.highlight = status->highlight,
            .decrement_pressed = status->decrement_pressed,
            .increment_pressed = status->increment_pressed,
            .dragging = status->dragging};
}

std::array<double, 2> wanted_scroll(const world::World& world, entt::entity container) {
    return scroll_now(world, state_of(world), container);
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

// ---- Text fields ----

const EditState* edit_state(const world::World& world, entt::entity entity) {
    const GuiState* state = state_of(world);
    if (state == nullptr || world.id_of(entity).empty()) {
        return nullptr;
    }
    const auto found = state->edits.find(world.id_of(entity));
    return found != state->edits.end() ? &found->second : nullptr;
}

bool has_focus(const world::World& world, entt::entity entity) {
    const GuiState* state = state_of(world);
    return state != nullptr && !state->key_focus.empty() && state->key_focus == world.id_of(entity);
}

bool is_editing(const world::World& world, entt::entity line_edit) {
    const EditState* state = edit_state(world, line_edit);
    return state != nullptr && state->editing;
}

std::optional<std::pair<int, int>> selection(const world::World& world, entt::entity line_edit) {
    const EditState* state = edit_state(world, line_edit);
    const auto* edit = world.ecs().try_get<world::LineEdit>(line_edit);
    // Text changed from outside drops the selection.
    if (state == nullptr || edit == nullptr || !state->selection.enabled ||
        state->known_text != edit->text) {
        return std::nullopt;
    }
    return std::pair{state->selection.begin, state->selection.end};
}

std::string ime_text(const world::World& world, entt::entity line_edit) {
    const EditState* state = edit_state(world, line_edit);
    return state != nullptr ? to_utf8(state->ime_text) : std::string();
}

const TextEditState* text_edit_state(const world::World& world, entt::entity entity) {
    const GuiState* state = state_of(world);
    if (state == nullptr || world.id_of(entity).empty()) {
        return nullptr;
    }
    const auto found = state->text_edits.find(world.id_of(entity));
    return found != state->text_edits.end() ? &found->second : nullptr;
}

namespace {

// A TextEdit with focus takes text while it's editable, with the input method's area on the
// caret's line, as _update_ime_window_position.
input::TextInput text_edit_text_input(const world::World& world, entt::entity focus,
                                      const world::TextEdit& edit) {
    input::TextInput out;
    if (!edit.editable) {
        return out;
    }
    const TextEditState* state = text_edit_state(world, focus);
    out.active = true;
    out.show_keyboard = state != nullptr && state->show_keyboard && edit.virtual_keyboard_enabled;
    out.keyboard = "multiline";
    if (text::Fonts* found = fonts(world); found != nullptr) {
        const Layout placed = layout(world, *found, settings(world).viewport);
        const Placed* at = placed.find(focus);
        const auto view = placed.text_edits.find(focus);
        if (at != nullptr && view != placed.text_edits.end()) {
            const Rect2 rect = global_rect(*at);
            const float height = view->second.font_height;
            out.position = {rect.position.x,
                            rect.position.y + view->second.caret_position.y - height};
            out.size = {rect.size.x, height};
            out.caret = static_cast<int>(view->second.caret_position.x);
        }
    }
    return out;
}

} // namespace

input::TextInput text_input(const world::World& world) {
    input::TextInput out;
    const entt::entity focus = focus_owner(world);
    if (const auto* text_edit =
            focus != entt::null ? world.ecs().try_get<world::TextEdit>(focus) : nullptr;
        text_edit != nullptr) {
        return text_edit_text_input(world, focus, *text_edit);
    }
    const auto* edit = focus != entt::null ? world.ecs().try_get<world::LineEdit>(focus) : nullptr;
    const EditState* state = edit != nullptr ? edit_state(world, focus) : nullptr;
    if (state == nullptr || !state->editing) {
        return out;
    }
    out.active = true;
    out.show_keyboard = state->show_keyboard && edit->virtual_keyboard_enabled;
    out.keyboard = edit->virtual_keyboard_type;
    if (text::Fonts* found = fonts(world); found != nullptr) {
        const Layout placed = layout(world, *found, settings(world).viewport);
        if (const Placed* at = placed.find(focus); at != nullptr) {
            const Rect2 rect = global_rect(*at);
            out.position = rect.position;
            out.size = rect.size;
            if (const auto view = placed.line_edits.find(focus); view != placed.line_edits.end()) {
                out.caret = static_cast<int>(view->second.caret_x);
            }
        }
    }
    return out;
}

void set_clipboard_backend(world::World& world, ClipboardBackend backend) {
    world.ecs().ctx().insert_or_assign(ClipboardHolder{std::move(backend)});
}

std::string clipboard(const world::World& world) {
    if (const auto* holder = world.ecs().ctx().find<ClipboardHolder>();
        holder != nullptr && holder->backend.get) {
        return holder->backend.get();
    }
    const GuiState* state = state_of(world);
    return state != nullptr ? state->clipboard : std::string();
}

void set_clipboard(world::World& world, const std::string& text) {
    if (auto* holder = world.ecs().ctx().find<ClipboardHolder>();
        holder != nullptr && holder->backend.set) {
        holder->backend.set(text);
    }
    if (GuiState* state = state_of(world); state != nullptr) {
        state->clipboard = text;
    }
}

void check_buttons(const world::World& world, std::vector<Problem>& problems) {
    const auto& ecs = world.ecs();
    const input::Map* map = input::map(world);
    const input::Map defaults = input::Map::with_defaults();
    const input::Map& actions = map != nullptr ? *map : defaults;
    std::vector<std::pair<std::string, entt::entity>> entities;
    for (const auto [entity] : ecs.storage<entt::entity>()->each()) {
        if ((is_control(world, entity) || ecs.all_of<world::PopupMenu>(entity)) &&
            !world.id_of(entity).empty()) {
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
    // Items' checkable values, which the schema takes as any string.
    const auto check_items = [&](const std::string& id, std::string_view type,
                                 const std::vector<world::PopupMenuItem>& items) {
        static const std::vector<std::string_view> kCheckable{"none", "check", "radio"};
        for (std::size_t i = 0; i < items.size(); ++i) {
            const std::string& checkable = items[i].checkable;
            if (std::ranges::find(kCheckable, checkable) != kCheckable.end()) {
                continue;
            }
            std::string message = "\"" + checkable + R"(" isn't "none", "check" or "radio")";
            if (const std::string_view hint = closest_match(checkable, kCheckable); !hint.empty()) {
                message += "; did you mean \"" + std::string(hint) + "\"?";
            }
            problems.push_back({.id = id,
                                .path = "/components/" + std::string(type) + "/items/" +
                                        std::to_string(i) + "/checkable",
                                .message = std::move(message)});
        }
    };
    for (const auto& entry : entities) {
        // Named, not bound, so the lambda below can capture them on every compiler.
        const std::string& id = entry.first;
        const entt::entity entity = entry.second;
        if (const auto* menu = ecs.try_get<world::PopupMenu>(entity); menu != nullptr) {
            check_items(id, "PopupMenu", menu->items);
            if (is_control(world, entity)) {
                problems.push_back({.id = id,
                                    .path = "/components/PopupMenu",
                                    .message = "a PopupMenu is a window of its own, not a "
                                               "control; give it an entity of its own"});
            }
        }
        if (const auto* menu = ecs.try_get<world::MenuButton>(entity); menu != nullptr) {
            check_items(id, "MenuButton", menu->items);
        }
        if (const auto* option = ecs.try_get<world::OptionButton>(entity);
            option != nullptr && option->selected >= 0 &&
            !std::cmp_less(option->selected, option->items.size())) {
            problems.push_back({.id = id,
                                .path = "/components/OptionButton/selected",
                                .message = "it has " + std::to_string(option->items.size()) +
                                           (option->items.size() == 1 ? " option" : " options") +
                                           "; -1 selects none"});
        }
        std::vector<std::string_view> types; // the buttons it has
        const auto check = [&](std::string_view type, const auto* button) {
            if (button != nullptr) {
                types.push_back(type);
                check_shortcut(id, type, button->shortcut);
                check_group(id, type, *button);
            }
        };
        check("Button", ecs.try_get<world::Button>(entity));
        check("CheckBox", ecs.try_get<world::CheckBox>(entity));
        check("CheckButton", ecs.try_get<world::CheckButton>(entity));
        check("OptionButton", ecs.try_get<world::OptionButton>(entity));
        check("MenuButton", ecs.try_get<world::MenuButton>(entity));
        check("TextureButton", ecs.try_get<world::TextureButton>(entity));
        if (types.size() > 1) {
            problems.push_back({.id = id,
                                .path = "/components/" + std::string(types[1]),
                                .message = "an entity is one button; it has " +
                                           std::string(types[0] == "OptionButton" ? "an " : "a ") +
                                           std::string(types[0]) + " too"});
        }
        if (!types.empty() && ecs.all_of<world::Label>(entity)) {
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
