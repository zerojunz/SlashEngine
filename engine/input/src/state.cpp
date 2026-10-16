#include "slash/core/text.hpp"
#include "slash/input/input.hpp"
#include "slash/world/world.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The action map as Godot's InputMap (core/input/input_map.cpp) and the input state as its Input
// singleton (core/input/input.cpp), kept per world and stepped once per tick.
namespace slash::input {
namespace {

constexpr std::uint64_t kNever = std::numeric_limits<std::uint64_t>::max();
constexpr std::size_t kMaxBindings = 32;
constexpr std::size_t kMaxActionName = 64;

struct ActionState {
    // What each device's matching binding last said, by (device, binding index). Action events
    // use their own index, so rebinding never strands a press made by name.
    struct Slot {
        bool pressed = false;
        float strength = 0.0F;
        float raw_strength = 0.0F;
    };
    std::map<std::pair<int, std::size_t>, Slot> slots;
    bool pressed = false;
    float strength = 0.0F;
    float raw_strength = 0.0F;
    bool exact = true; // the last event matched with exactly its modifiers
    std::uint64_t pressed_tick = kNever;
    std::uint64_t released_tick = kNever;
};

struct Pending {
    Event event;
    bool release_all = false;
};

struct InputState {
    Map map;
    Settings settings;
    std::vector<Pending> queue;
    std::vector<Event> applied;  // this tick's events
    std::uint64_t tick = kNever; // the tick the input system last ran
    std::map<std::string, ActionState, std::less<>> actions;
    std::set<Key> keys;
    std::set<Key> physical_keys;
    std::uint16_t mouse_buttons = 0; // bit (button - 1)
    Vec2 mouse;
    std::map<int, Vec2> touches;
    std::set<std::pair<int, JoyButton>> joy_buttons;
    std::map<std::pair<int, JoyAxis>, float> joy_axes;
    int mouse_from_touch = -1; // the finger the mouse follows, or -1
};

using Handle = std::shared_ptr<InputState>;

// Kept apart from InputState so snapshots leave it alone.
struct RecorderHolder {
    Recorder recorder;
};

// Hands input pushed from outside the world's systems to the recorder, if there is one.
void record(const world::World& world, const Recorded& input) {
    if (world.stepping()) {
        return;
    }
    if (const auto* holder = world.ecs().ctx().find<RecorderHolder>();
        holder != nullptr && holder->recorder) {
        holder->recorder(input);
    }
}

InputState* state_of(world::World& world) {
    auto* found = world.ecs().ctx().find<Handle>();
    return found != nullptr ? found->get() : nullptr;
}

const InputState* state_of(const world::World& world) {
    const auto* found = world.ecs().ctx().find<Handle>();
    return found != nullptr ? found->get() : nullptr;
}

bool is_motion(EventType type) {
    return type == EventType::MouseMotion || type == EventType::ScreenDrag ||
           type == EventType::JoypadMotion;
}

// Whether two motions come from the same mouse, finger or gamepad axis.
bool same_source(const Event& a, const Event& b) {
    return a.type == b.type && a.device == b.device &&
           (a.type != EventType::ScreenDrag || a.index == b.index) &&
           (a.type != EventType::JoypadMotion || a.axis == b.axis);
}

// Merges a motion into the queued one from the same source, as Godot's accumulated input does:
// the newer position or axis value, with the distances moved added up.
bool accumulate(Event& earlier, const Event& next) {
    if (next.type == EventType::JoypadMotion) {
        earlier.axis_value = next.axis_value;
        return true;
    }
    if (earlier.relative.has_value() != next.relative.has_value() ||
        earlier.button_mask != next.button_mask || earlier.modifiers != next.modifiers) {
        return false;
    }
    if (earlier.relative && next.relative) {
        earlier.relative = *earlier.relative + *next.relative;
    }
    earlier.position = next.position;
    return true;
}

std::uint16_t mouse_bit(MouseButton button) {
    return button == MouseButton::None
               ? 0
               : static_cast<std::uint16_t>(1U << (static_cast<unsigned>(button) - 1U));
}

Event key_binding(Key key, bool shift = false) {
    Event event;
    event.type = EventType::Key;
    event.device = kAllDevices;
    event.key = key;
    event.modifiers.shift = shift;
    return event;
}

// A key with modifiers, as Godot's InputEventKey::create_reference(key | mask).
Event key_binding(Key key, Modifiers modifiers) {
    Event event = key_binding(key);
    event.modifiers = modifiers;
    return event;
}

Event joy_button_binding(JoyButton button) {
    Event event;
    event.type = EventType::JoypadButton;
    event.device = kAllDevices;
    event.joy_button = button;
    return event;
}

Event joy_axis_binding(JoyAxis axis, float value) {
    Event event;
    event.type = EventType::JoypadMotion;
    event.device = kAllDevices;
    event.axis = axis;
    event.axis_value = value;
    return event;
}

void update_cache(ActionState& action) {
    action.pressed = false;
    action.strength = 0.0F;
    action.raw_strength = 0.0F;
    for (const auto& [key, slot] : action.slots) {
        action.pressed = action.pressed || slot.pressed;
        action.strength = std::max(action.strength, slot.strength);
        action.raw_strength = std::max(action.raw_strength, slot.raw_strength);
    }
}

void release_action(InputState& state, ActionState& action) {
    if (action.pressed) {
        action.released_tick = state.tick;
    }
    action.slots.clear();
    action.pressed = false;
    action.strength = 0.0F;
    action.raw_strength = 0.0F;
    action.exact = true;
}

// Godot's Input::_parse_input_event_impl: updates what is held, emulates mouse from touch and
// touch from mouse, then each action the event is bound to.
void apply(InputState& state, Event event, bool emulated) {
    switch (event.type) {
    case EventType::Key:
        if (!event.echo) {
            if (event.key != Key::None) {
                if (event.pressed) {
                    state.keys.insert(event.key);
                } else {
                    state.keys.erase(event.key);
                }
            }
            if (event.physical_key != Key::None) {
                if (event.pressed) {
                    state.physical_keys.insert(event.physical_key);
                } else {
                    state.physical_keys.erase(event.physical_key);
                }
            }
        }
        break;
    case EventType::MouseButton:
        if (event.pressed) {
            state.mouse_buttons |= mouse_bit(event.button);
        } else {
            state.mouse_buttons &= static_cast<std::uint16_t>(~mouse_bit(event.button));
        }
        state.mouse = event.position;
        if (state.settings.emulate_touch_from_mouse && !emulated &&
            event.button == MouseButton::Left) {
            Event touch;
            touch.type = EventType::ScreenTouch;
            touch.device = kEmulatedDevice;
            touch.pressed = event.pressed;
            touch.position = event.position;
            touch.double_click = event.double_click;
            state.applied.push_back(touch);
        }
        break;
    case EventType::MouseMotion:
        if (!event.relative) {
            event.relative = event.position - state.mouse;
        }
        event.button_mask = static_cast<std::uint8_t>(state.mouse_buttons & 0xFFU);
        state.mouse = event.position;
        if (state.settings.emulate_touch_from_mouse && !emulated &&
            (state.mouse_buttons & mouse_bit(MouseButton::Left)) != 0) {
            Event drag;
            drag.type = EventType::ScreenDrag;
            drag.device = kEmulatedDevice;
            drag.position = event.position;
            drag.relative = event.relative;
            state.applied.push_back(drag);
        }
        break;
    case EventType::ScreenTouch: {
        if (event.pressed) {
            state.touches[event.index] = event.position;
        } else {
            state.touches.erase(event.index);
        }
        bool translate = false;
        if (state.settings.emulate_mouse_from_touch) {
            if (event.pressed && state.mouse_from_touch == -1) {
                translate = true;
                state.mouse_from_touch = event.index;
            } else if (!event.pressed && event.index == state.mouse_from_touch) {
                translate = true;
                state.mouse_from_touch = -1;
            }
        }
        if (translate) {
            Event button;
            button.type = EventType::MouseButton;
            button.device = kEmulatedDevice;
            button.button = MouseButton::Left;
            button.pressed = event.pressed;
            button.position = event.position;
            button.double_click = event.double_click;
            apply(state, button, true);
        }
        break;
    }
    case EventType::ScreenDrag: {
        const auto touch = state.touches.find(event.index);
        if (!event.relative) {
            event.relative = touch != state.touches.end() ? event.position - touch->second : Vec2{};
        }
        state.touches[event.index] = event.position;
        if (state.settings.emulate_mouse_from_touch && event.index == state.mouse_from_touch) {
            Event motion;
            motion.type = EventType::MouseMotion;
            motion.device = kEmulatedDevice;
            motion.position = event.position;
            motion.relative = event.relative;
            apply(state, motion, true);
        }
        break;
    }
    case EventType::JoypadButton:
        if (event.pressed) {
            state.joy_buttons.insert({event.device, event.joy_button});
        } else {
            state.joy_buttons.erase({event.device, event.joy_button});
        }
        break;
    case EventType::JoypadMotion:
        state.joy_axes[{event.device, event.axis}] = event.axis_value;
        break;
    case EventType::Action:
    case EventType::Ime:
        break;
    }

    for (const auto& [name, binding] : state.map.actions()) {
        const auto found = state.map.find_event(name, event);
        if (!found) {
            continue;
        }
        ActionState& action = state.actions[name];
        action.slots[{event.device, found->index}] = {.pressed = found->match.pressed,
                                                      .strength = found->match.strength,
                                                      .raw_strength = found->match.raw_strength};
        action.exact = state.map.find_event(name, event, true).has_value();
        const bool was_pressed = action.pressed;
        update_cache(action);
        if (action.pressed && !was_pressed) {
            action.pressed_tick = state.tick;
        }
        if (!action.pressed && was_pressed) {
            action.released_tick = state.tick;
        }
    }
    state.applied.push_back(std::move(event));
}

// Godot's Input::release_pressed_events, for when the window loses focus.
void release_all(InputState& state) {
    state.keys.clear();
    state.physical_keys.clear();
    state.joy_buttons.clear();
    state.joy_axes.clear();
    for (auto& [name, action] : state.actions) {
        release_action(state, action);
    }
}

const ActionState* action_of(const world::World& world, std::string_view name, bool exact) {
    const InputState* state = state_of(world);
    if (state == nullptr) {
        return nullptr;
    }
    const auto found = state->actions.find(name);
    if (found == state->actions.end() || (exact && !found->second.exact)) {
        return nullptr;
    }
    return &found->second;
}

json::Value point_json(Vec2 value) {
    return json::Array{json::Value(value.x), json::Value(value.y)};
}

} // namespace

Keyboard host_keyboard() {
#if defined(__APPLE__)
    return Keyboard::Mac;
#else
    return Keyboard::Pc;
#endif
}

bool is_text_action(std::string_view name) {
    static constexpr std::array<std::string_view, 8> kEditing = {
        "ui_cut",  "ui_copy",      "ui_paste", "ui_undo", "ui_redo", "ui_swap_input_direction",
        "ui_menu", "ui_focus_mode"};
    return name.starts_with("ui_text_") ||
           std::find(kEditing.begin(), kEditing.end(), name) != kEditing.end();
}

Map Map::with_defaults(Keyboard keyboard) {
    Map map;
    const auto add = [&map](const std::string& name, const std::vector<Event>& events) {
        map.add_action(name, kToggleDeadzone);
        for (const Event& event : events) {
            map.action_add_event(name, event);
        }
    };
    add("ui_accept", {key_binding(Key::Enter), key_binding(Key::KpEnter), key_binding(Key::Space)});
    add("ui_select", {joy_button_binding(JoyButton::Y), key_binding(Key::Space)});
    add("ui_cancel", {key_binding(Key::Escape)});
    add("ui_focus_next", {key_binding(Key::Tab)});
    add("ui_focus_prev", {key_binding(Key::Tab, true)});
    add("ui_left", {key_binding(Key::Left), joy_button_binding(JoyButton::DpadLeft),
                    joy_axis_binding(JoyAxis::LeftX, -1.0F)});
    add("ui_right", {key_binding(Key::Right), joy_button_binding(JoyButton::DpadRight),
                     joy_axis_binding(JoyAxis::LeftX, 1.0F)});
    add("ui_up", {key_binding(Key::Up), joy_button_binding(JoyButton::DpadUp),
                  joy_axis_binding(JoyAxis::LeftY, -1.0F)});
    add("ui_down", {key_binding(Key::Down), joy_button_binding(JoyButton::DpadDown),
                    joy_axis_binding(JoyAxis::LeftY, 1.0F)});
    add("ui_page_up", {key_binding(Key::PageUp)});
    add("ui_page_down", {key_binding(Key::PageDown)});
    add("ui_home", {key_binding(Key::Home)});
    add("ui_end", {key_binding(Key::End)});

    // Text editing, from Godot's InputMap::get_builtins: Command on a Mac where Godot says
    // CMD_OR_CTRL, Ctrl elsewhere, and the ".macos" overrides on a Mac.
    const bool mac = keyboard == Keyboard::Mac;
    const Modifiers cmd = mac ? Modifiers{.meta = true} : Modifiers{.ctrl = true};
    const Modifiers shift_cmd =
        mac ? Modifiers{.shift = true, .meta = true} : Modifiers{.shift = true, .ctrl = true};
    const Modifiers shift{.shift = true};
    const Modifiers alt{.alt = true};
    const Modifiers ctrl{.ctrl = true};
    add("ui_cut", {key_binding(Key::X, cmd), key_binding(Key::Delete, shift)});
    add("ui_copy", {key_binding(Key::C, cmd), key_binding(Key::Insert, cmd)});
    add("ui_paste", {key_binding(Key::V, cmd), key_binding(Key::Insert, shift)});
    add("ui_undo", {key_binding(Key::Z, cmd)});
    add("ui_redo", {key_binding(Key::Z, shift_cmd), key_binding(Key::Y, cmd)});
    add("ui_text_submit", {key_binding(Key::Enter), key_binding(Key::KpEnter)});
    add("ui_text_backspace", {key_binding(Key::Backspace), key_binding(Key::Backspace, shift)});
    add("ui_text_backspace_word", {key_binding(Key::Backspace, mac ? alt : cmd)});
    add("ui_text_backspace_all_to_left",
        mac ? std::vector<Event>{key_binding(Key::Backspace, cmd)} : std::vector<Event>{});
    add("ui_text_delete", {key_binding(Key::Delete)});
    add("ui_text_delete_word", {key_binding(Key::Delete, mac ? alt : cmd)});
    add("ui_text_delete_all_to_right",
        mac ? std::vector<Event>{key_binding(Key::Delete, cmd)} : std::vector<Event>{});
    add("ui_text_caret_left", {key_binding(Key::Left)});
    add("ui_text_caret_word_left", {key_binding(Key::Left, mac ? alt : cmd)});
    add("ui_text_caret_right", {key_binding(Key::Right)});
    add("ui_text_caret_word_right", {key_binding(Key::Right, mac ? alt : cmd)});
    add("ui_text_caret_up", {key_binding(Key::Up)});
    add("ui_text_caret_down", {key_binding(Key::Down)});
    add("ui_text_caret_line_start",
        mac ? std::vector<Event>{key_binding(Key::A, ctrl), key_binding(Key::Left, cmd),
                                 key_binding(Key::Home)}
            : std::vector<Event>{key_binding(Key::Home)});
    add("ui_text_caret_line_end",
        mac ? std::vector<Event>{key_binding(Key::E, ctrl), key_binding(Key::Right, cmd),
                                 key_binding(Key::End)}
            : std::vector<Event>{key_binding(Key::End)});
    add("ui_text_caret_page_up", {key_binding(Key::PageUp)});
    add("ui_text_caret_page_down", {key_binding(Key::PageDown)});
    add("ui_text_caret_document_start",
        mac ? std::vector<Event>{key_binding(Key::Up, cmd), key_binding(Key::Home, cmd)}
            : std::vector<Event>{key_binding(Key::Home, cmd)});
    add("ui_text_caret_document_end",
        mac ? std::vector<Event>{key_binding(Key::Down, cmd), key_binding(Key::End, cmd)}
            : std::vector<Event>{key_binding(Key::End, cmd)});
    add("ui_text_select_all", {key_binding(Key::A, cmd)});
    add("ui_text_clear_carets_and_selection", {key_binding(Key::Escape)});
    add("ui_swap_input_direction", {key_binding(Key::QuoteLeft, cmd)});
    // TextEdit's.
    const Modifiers cmd_alt =
        mac ? Modifiers{.alt = true, .meta = true} : Modifiers{.ctrl = true, .alt = true};
    add("ui_text_newline", {key_binding(Key::Enter), key_binding(Key::KpEnter),
                            key_binding(Key::Enter, shift), key_binding(Key::KpEnter, shift)});
    add("ui_text_newline_blank", {key_binding(Key::Enter, cmd), key_binding(Key::KpEnter, cmd)});
    add("ui_text_newline_above",
        {key_binding(Key::Enter, shift_cmd), key_binding(Key::KpEnter, shift_cmd)});
    add("ui_text_indent", {key_binding(Key::Tab)});
    add("ui_text_dedent", {key_binding(Key::Tab, shift)});
    add("ui_text_scroll_up", {key_binding(Key::Up, mac ? cmd_alt : cmd)});
    add("ui_text_scroll_down", {key_binding(Key::Down, mac ? cmd_alt : cmd)});
    add("ui_text_select_word_under_caret",
        {key_binding(Key::G, mac ? Modifiers{.ctrl = true, .meta = true} : alt)});
    add("ui_text_toggle_insert_mode", {key_binding(Key::Insert)});
    add("ui_focus_mode", {key_binding(Key::M, ctrl)});
    add("ui_menu", {key_binding(Key::Menu)});
    return map;
}

bool Map::has_action(std::string_view name) const {
    return actions_.contains(name);
}

void Map::add_action(const std::string& name, float deadzone) {
    actions_[name].deadzone = deadzone;
}

void Map::erase_action(std::string_view name) {
    if (const auto found = actions_.find(name); found != actions_.end()) {
        actions_.erase(found);
    }
}

void Map::action_add_event(std::string_view name, const Event& binding) {
    const auto found = actions_.find(name);
    if (found == actions_.end() ||
        std::ranges::find(found->second.events, binding) != found->second.events.end()) {
        return;
    }
    found->second.events.push_back(binding);
}

void Map::action_erase_event(std::string_view name, const Event& binding) {
    if (const auto found = actions_.find(name); found != actions_.end()) {
        std::erase(found->second.events, binding);
    }
}

void Map::action_erase_events(std::string_view name) {
    if (const auto found = actions_.find(name); found != actions_.end()) {
        found->second.events.clear();
    }
}

const Action* Map::find(std::string_view name) const {
    const auto found = actions_.find(name);
    return found != actions_.end() ? &found->second : nullptr;
}

std::vector<std::string_view> Map::names() const {
    std::vector<std::string_view> out;
    out.reserve(actions_.size());
    for (const auto& [name, action] : actions_) {
        out.emplace_back(name);
    }
    return out;
}

std::optional<Map::Found> Map::find_event(std::string_view name, const Event& event,
                                          bool exact) const {
    const Action* action = find(name);
    if (action == nullptr) {
        return std::nullopt;
    }
    if (event.type == EventType::Action) {
        if (event.action != name) {
            return std::nullopt;
        }
        const float strength = event.pressed ? event.strength : 0.0F;
        return Found{
            .index = std::numeric_limits<std::size_t>::max(),
            .match = {.pressed = event.pressed, .strength = strength, .raw_strength = strength}};
    }
    for (std::size_t i = 0; i < action->events.size(); ++i) {
        const Event& binding = action->events[i];
        if (binding.device != kAllDevices && binding.device != event.device) {
            continue;
        }
        if (const auto matched = match(binding, event, action->deadzone, exact)) {
            return Found{.index = i, .match = *matched};
        }
    }
    return std::nullopt;
}

json::Value map_schema() {
    json::Value action;
    action["type"] = "object";
    action["description"] = "An action and the events that press it.";
    action["properties"]["deadzone"]["type"] = "number";
    action["properties"]["deadzone"]["minimum"] = 0.0;
    action["properties"]["deadzone"]["maximum"] = 1.0;
    action["properties"]["deadzone"]["default"] = kDefaultDeadzone;
    action["properties"]["deadzone"]["description"] =
        "How far a gamepad axis must move to press it, from 0 to 1.";
    action["properties"]["events"]["type"] = "array";
    action["properties"]["events"]["maxItems"] = kMaxBindings;
    action["properties"]["events"]["items"] = event_schema(Use::Binding);
    action["properties"]["events"]["description"] = "Keys, mouse buttons and gamepad buttons and "
                                                    "axes that press it.";
    action["additionalProperties"] = false;

    json::Value schema;
    schema["type"] = "object";
    schema["description"] =
        "Input actions by name, as Godot's input map, e.g. {\"jump\": {\"events\": [{\"type\": "
        "\"key\", \"key\": \"Space\"}]}}. Godot's ui_* actions are built in; an entry with the "
        "same name replaces one.";
    schema["propertyNames"]["minLength"] = 1;
    schema["propertyNames"]["maxLength"] = kMaxActionName;
    schema["additionalProperties"] = std::move(action);
    return schema;
}

std::vector<json::ValidationError> validate_map(const json::Value& doc, const std::string& path) {
    std::vector<json::ValidationError> errors = json::validate(doc, map_schema());
    for (json::ValidationError& error : errors) {
        error.path = path + error.path;
    }
    if (!errors.empty()) {
        return errors;
    }
    for (const auto& [name, action] : doc.as_object()) {
        const json::Value* events = action.find("events");
        if (events == nullptr) {
            continue;
        }
        const json::Array& items = events->as_array();
        for (std::size_t i = 0; i < items.size(); ++i) {
            auto found = validate_event(items[i], Use::Binding,
                                        path + "/" + json::pointer_escape(name) + "/events/" +
                                            std::to_string(i));
            errors.insert(errors.end(), std::make_move_iterator(found.begin()),
                          std::make_move_iterator(found.end()));
        }
    }
    return errors;
}

Result<Map> parse_map(const json::Value& doc, Keyboard keyboard) {
    Map map = Map::with_defaults(keyboard);
    if (doc.is_null()) {
        return map;
    }
    if (const auto errors = validate_map(doc); !errors.empty()) {
        return make_error(ErrorCode::InvalidArgument,
                          errors.front().path + ": " + errors.front().message);
    }
    for (const auto& [name, doc_action] : doc.as_object()) {
        map.erase_action(name);
        const json::Value* deadzone = doc_action.find("deadzone");
        map.add_action(name, deadzone != nullptr ? static_cast<float>(deadzone->as_double())
                                                 : kDefaultDeadzone);
        if (const json::Value* events = doc_action.find("events"); events != nullptr) {
            for (const json::Value& item : events->as_array()) {
                auto event = parse_event(item, Use::Binding);
                if (!event) {
                    return event.error();
                }
                map.action_add_event(name, event.value());
            }
        }
    }
    return map;
}

json::Value action_json(const Action& action) {
    json::Value out;
    out["deadzone"] = action.deadzone;
    json::Array events;
    for (const Event& event : action.events) {
        events.push_back(event_json(event, Use::Binding));
    }
    out["events"] = std::move(events);
    return out;
}

json::Value settings_schema() {
    json::Value pointing;
    pointing["type"] = "object";
    pointing["description"] = "How touches and the mouse stand in for each other.";
    pointing["properties"]["emulate_mouse_from_touch"]["type"] = "boolean";
    pointing["properties"]["emulate_mouse_from_touch"]["default"] = true;
    pointing["properties"]["emulate_mouse_from_touch"]["description"] =
        "A touch also clicks the left mouse button, and dragging moves the mouse.";
    pointing["properties"]["emulate_touch_from_mouse"]["type"] = "boolean";
    pointing["properties"]["emulate_touch_from_mouse"]["default"] = false;
    pointing["properties"]["emulate_touch_from_mouse"]["description"] =
        "A left click also touches the screen, and dragging with it drags a finger.";
    pointing["additionalProperties"] = false;

    json::Value schema;
    schema["type"] = "object";
    schema["description"] = "Input device settings, as Godot's input_devices.";
    schema["properties"]["pointing"] = std::move(pointing);
    schema["additionalProperties"] = false;
    return schema;
}

Settings parse_settings(const json::Value& doc) {
    Settings settings;
    const json::Value* pointing = doc.is_object() ? doc.find("pointing") : nullptr;
    if (pointing == nullptr || !pointing->is_object()) {
        return settings;
    }
    if (const json::Value* value = pointing->find("emulate_mouse_from_touch");
        value != nullptr && value->is_bool()) {
        settings.emulate_mouse_from_touch = value->as_bool();
    }
    if (const json::Value* value = pointing->find("emulate_touch_from_mouse");
        value != nullptr && value->is_bool()) {
        settings.emulate_touch_from_mouse = value->as_bool();
    }
    return settings;
}

void install(world::World& world, Map map, Settings settings) {
    if (InputState* state = state_of(world); state != nullptr) {
        state->map = std::move(map);
        state->settings = settings;
        state->actions.clear();
        return;
    }
    auto state = std::make_shared<InputState>();
    state->map = std::move(map);
    state->settings = settings;
    world.ecs().ctx().emplace<Handle>(std::move(state));
    world.ecs().ctx().emplace<RecorderHolder>();
    world.add_reset([](world::World& in_world) {
        if (InputState* current = state_of(in_world); current != nullptr) {
            InputState fresh;
            fresh.map = std::move(current->map);
            fresh.settings = current->settings;
            *current = std::move(fresh);
        }
    });
    world.add_system(
        "input",
        [](world::World& in_world, double /*tick_seconds*/) {
            InputState* current = state_of(in_world);
            if (current == nullptr) {
                return;
            }
            current->tick = in_world.tick();
            current->applied.clear();
            std::vector<Pending> queue = std::move(current->queue);
            current->queue.clear();
            for (Pending& pending : queue) {
                if (pending.release_all) {
                    release_all(*current);
                } else {
                    apply(*current, std::move(pending.event), false);
                }
            }
        },
        {.doc = "Applies the input events queued since the last tick.", .priority = -1000});
}

Map* map(world::World& world) {
    InputState* state = state_of(world);
    return state != nullptr ? &state->map : nullptr;
}

const Map* map(const world::World& world) {
    const InputState* state = state_of(world);
    return state != nullptr ? &state->map : nullptr;
}

void push(world::World& world, Event event) {
    InputState* state = state_of(world);
    if (state == nullptr) {
        return;
    }
    record(world, {event, false});
    // Only motions since the last press or release merge, so the queue holds at most one per
    // mouse, finger and gamepad axis between presses, however fast they report or however long
    // a paused game leaves them queued.
    if (is_motion(event.type)) {
        for (auto it = state->queue.rbegin();
             it != state->queue.rend() && !it->release_all && is_motion(it->event.type); ++it) {
            if (same_source(it->event, event)) {
                if (accumulate(it->event, event)) {
                    return;
                }
                break;
            }
        }
    }
    state->queue.push_back({std::move(event), false});
}

void action_press(world::World& world, std::string_view action, float strength) {
    Event event;
    event.type = EventType::Action;
    event.action = std::string(action);
    event.pressed = true;
    event.strength = std::clamp(strength, 0.0F, 1.0F);
    push(world, std::move(event));
}

void action_release(world::World& world, std::string_view action) {
    Event event;
    event.type = EventType::Action;
    event.action = std::string(action);
    event.pressed = false;
    push(world, std::move(event));
}

void release_pressed(world::World& world) {
    if (InputState* state = state_of(world); state != nullptr) {
        record(world, {{}, true});
        state->queue.push_back({{}, true});
    }
}

bool is_action_pressed(const world::World& world, std::string_view action, bool exact) {
    const ActionState* state = action_of(world, action, exact);
    return state != nullptr && state->pressed;
}

bool is_action_just_pressed(const world::World& world, std::string_view action, bool exact) {
    const ActionState* state = action_of(world, action, exact);
    return state != nullptr && state->pressed_tick != kNever &&
           state->pressed_tick == state_of(world)->tick;
}

bool is_action_just_released(const world::World& world, std::string_view action, bool exact) {
    const ActionState* state = action_of(world, action, exact);
    return state != nullptr && state->released_tick != kNever &&
           state->released_tick == state_of(world)->tick;
}

float get_action_strength(const world::World& world, std::string_view action, bool exact) {
    const ActionState* state = action_of(world, action, exact);
    return state != nullptr ? state->strength : 0.0F;
}

float get_action_raw_strength(const world::World& world, std::string_view action, bool exact) {
    const ActionState* state = action_of(world, action, exact);
    return state != nullptr ? state->raw_strength : 0.0F;
}

float get_axis(const world::World& world, std::string_view negative, std::string_view positive) {
    return get_action_strength(world, positive) - get_action_strength(world, negative);
}

Vec2 get_vector(const world::World& world, std::string_view negative_x, std::string_view positive_x,
                std::string_view negative_y, std::string_view positive_y, float deadzone) {
    const Vec2 vector{
        get_action_raw_strength(world, positive_x) - get_action_raw_strength(world, negative_x),
        get_action_raw_strength(world, positive_y) - get_action_raw_strength(world, negative_y)};
    if (deadzone < 0.0F) {
        const Map* actions = map(world);
        const auto deadzone_of = [actions](std::string_view name) {
            const Action* action = actions != nullptr ? actions->find(name) : nullptr;
            return action != nullptr ? action->deadzone : 0.0F;
        };
        deadzone = 0.25F * (deadzone_of(positive_x) + deadzone_of(negative_x) +
                            deadzone_of(positive_y) + deadzone_of(negative_y));
    }
    const float length = std::sqrt((vector.x * vector.x) + (vector.y * vector.y));
    if (length <= deadzone) {
        return {};
    }
    if (length > 1.0F) {
        return vector * (1.0F / length);
    }
    return vector * (((length - deadzone) / (1.0F - deadzone)) / length);
}

bool is_key_pressed(const world::World& world, Key key) {
    const InputState* state = state_of(world);
    return state != nullptr && state->keys.contains(key);
}

bool is_physical_key_pressed(const world::World& world, Key key) {
    const InputState* state = state_of(world);
    return state != nullptr && state->physical_keys.contains(key);
}

bool is_mouse_button_pressed(const world::World& world, MouseButton button) {
    const InputState* state = state_of(world);
    return state != nullptr && (state->mouse_buttons & mouse_bit(button)) != 0;
}

Vec2 mouse_position(const world::World& world) {
    const InputState* state = state_of(world);
    return state != nullptr ? state->mouse : Vec2{};
}

bool is_joy_button_pressed(const world::World& world, int device, JoyButton button) {
    const InputState* state = state_of(world);
    return state != nullptr && state->joy_buttons.contains({device, button});
}

float get_joy_axis(const world::World& world, int device, JoyAxis axis) {
    const InputState* state = state_of(world);
    if (state == nullptr) {
        return 0.0F;
    }
    const auto found = state->joy_axes.find({device, axis});
    return found != state->joy_axes.end() ? found->second : 0.0F;
}

std::span<const Event> events(const world::World& world) {
    const InputState* state = state_of(world);
    return state != nullptr ? std::span<const Event>(state->applied) : std::span<const Event>();
}

std::size_t queued(const world::World& world) {
    const InputState* state = state_of(world);
    return state != nullptr ? state->queue.size() : 0;
}

json::Value state_json(const world::World& world, std::span<const std::string> actions) {
    json::Value out = json::Object{};
    const InputState* state = state_of(world);
    if (state == nullptr) {
        return out;
    }
    if (state->tick != kNever) {
        out["tick"] = state->tick;
    }
    json::Value pressed = json::Object{};
    const auto describe = [&](std::string_view name, const ActionState* action) {
        json::Value entry;
        entry["pressed"] = action != nullptr && action->pressed;
        if (action != nullptr && action->strength != (action->pressed ? 1.0F : 0.0F)) {
            entry["strength"] = action->strength;
        }
        if (action != nullptr && action->pressed_tick != kNever &&
            action->pressed_tick == state->tick) {
            entry["just_pressed"] = true;
        }
        if (action != nullptr && action->released_tick != kNever &&
            action->released_tick == state->tick) {
            entry["just_released"] = true;
        }
        pressed[std::string(name)] = std::move(entry);
    };
    if (actions.empty()) {
        for (const auto& [name, action] : state->actions) {
            if (is_text_action(name)) {
                continue;
            }
            if (action.pressed ||
                (state->tick != kNever &&
                 (action.pressed_tick == state->tick || action.released_tick == state->tick))) {
                describe(name, &action);
            }
        }
    } else {
        for (const std::string& name : actions) {
            const auto found = state->actions.find(name);
            describe(name, found != state->actions.end() ? &found->second : nullptr);
        }
    }
    out["actions"] = std::move(pressed);
    if (!state->keys.empty()) {
        json::Array keys;
        for (const Key key : state->keys) {
            keys.emplace_back(key_name(key));
        }
        out["keys"] = std::move(keys);
    }
    json::Value mouse;
    mouse["position"] = point_json(state->mouse);
    if (state->mouse_buttons != 0) {
        json::Array buttons;
        for (unsigned i = 1; i <= static_cast<unsigned>(MouseButton::Xbutton2); ++i) {
            const auto button = static_cast<MouseButton>(i);
            if ((state->mouse_buttons & mouse_bit(button)) != 0) {
                buttons.emplace_back(mouse_button_name(button));
            }
        }
        mouse["buttons"] = std::move(buttons);
    }
    out["mouse"] = std::move(mouse);
    if (!state->touches.empty()) {
        json::Array touches;
        for (const auto& [index, position] : state->touches) {
            json::Value touch;
            touch["index"] = index;
            touch["position"] = point_json(position);
            touches.push_back(std::move(touch));
        }
        out["touches"] = std::move(touches);
    }
    std::map<int, json::Value> joypads;
    for (const auto& [device, button] : state->joy_buttons) {
        json::Value& pad = joypads[device];
        if (pad.find("buttons") == nullptr) {
            pad["buttons"] = json::Array{};
        }
        pad["buttons"].as_array().emplace_back(joy_button_name(button));
    }
    for (const auto& [key, value] : state->joy_axes) {
        if (value != 0.0F) {
            joypads[key.first]["axes"][std::string(joy_axis_name(key.second))] = value;
        }
    }
    if (!joypads.empty()) {
        json::Array pads;
        for (auto& [device, pad] : joypads) {
            pad["device"] = device;
            pads.push_back(std::move(pad));
        }
        out["joypads"] = std::move(pads);
    }
    if (!state->queue.empty()) {
        out["queued"] = state->queue.size();
    }
    return out;
}

Snapshot save(const world::World& world) {
    const InputState* state = state_of(world);
    if (state == nullptr) {
        return {};
    }
    return {std::make_shared<const InputState>(*state)};
}

void restore(world::World& world, const Snapshot& snapshot) {
    InputState* state = state_of(world);
    if (state == nullptr || snapshot.data == nullptr) {
        return;
    }
    *state = *static_cast<const InputState*>(snapshot.data.get());
}

void set_recorder(world::World& world, Recorder recorder) {
    if (auto* holder = world.ecs().ctx().find<RecorderHolder>(); holder != nullptr) {
        holder->recorder = std::move(recorder);
    }
}

} // namespace slash::input
