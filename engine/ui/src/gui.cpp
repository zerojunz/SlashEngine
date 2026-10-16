#include "internal.hpp"
#include "line_edit.hpp"
#include "text_edit.hpp"

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
// (scene/gui/base_button.cpp) and edits text as LineEdit and TextEdit do (scene/gui/line_edit.cpp,
// and text_editor.cpp for TextEdit). Its
// state is kept by entity ID, since sessions make entities anew when they roll a command back.
// Drag and drop, tooltips, context menus and cursor shapes aren't here.
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
    std::vector<std::string> presses;                             // press() calls for the next tick
    std::map<std::string, EditState, std::less<>> edits;          // each LineEdit's
    std::map<std::string, TextEditState, std::less<>> text_edits; // each TextEdit's
    std::map<std::string, SliderStatus, std::less<>> sliders;     // each Slider's
    std::string clipboard;       // the world's clipboard, which a backend shares with the system's
    bool focus_by_arrow = false; // an arrow key is moving focus, so a LineEdit won't edit
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
                        .value_changed = false};
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
                        .value_changed = ended && changed};
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
                        .value_changed = false};
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
                            .value_changed = false};
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
    Gui(world::World& world, GuiState& state, const Layout& layout, double now)
        : world_(world), state_(state), layout_(layout), map_(input::map(std::as_const(world))),
          now_(now) {
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
        just_pushed_ = just_pushed;
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
                    ? action_pressed(event, action, false, false) && just_pushed.contains(action)
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
    const Layout& layout_;
    const input::Map* map_;
    std::vector<entt::entity> roots_;
    std::unordered_map<entt::entity, std::vector<entt::entity>> kids_;
    double now_ = 0.0; // seconds of world time, for triple clicks
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
    track_sliders(world, *state);
    const std::span<const input::Event> events = input::events(std::as_const(world));
    const std::vector<std::string> presses = std::move(state->presses);
    state->presses.clear();
    if (!events.empty()) {
        if (text::Fonts* found = fonts(world); found != nullptr) {
            const Layout placed = layout(world, *found, settings(world).viewport);
            Gui gui(world, *state, placed, static_cast<double>(world.tick()) * tick_seconds);
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
    for (const auto& entry : entities) {
        // Named, not bound, so the lambda below can capture them on every compiler.
        const std::string& id = entry.first;
        const entt::entity entity = entry.second;
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
        check("TextureButton", ecs.try_get<world::TextureButton>(entity));
        if (types.size() > 1) {
            problems.push_back(
                {.id = id,
                 .path = "/components/" + std::string(types[1]),
                 .message = "an entity is one button; it has a " + std::string(types[0]) + " too"});
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
