#pragma once

#include "slash/core/json.hpp"
#include "slash/core/json_schema.hpp"
#include "slash/core/math.hpp"
#include "slash/core/result.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace slash::world {
class World;
} // namespace slash::world

// Input as Godot's: events from keys, mouse buttons, touches and gamepads (InputEvent), actions
// that name what a game does with them (InputMap), and the state gameplay code asks about
// (Input). Events wait in a queue and apply at the start of the next tick, so a run with the same
// events at the same ticks plays out the same way, and agents send events with input.inject.
namespace slash::input {

enum class EventType : std::uint8_t {
    Key,          // a keyboard key pressed or released
    MouseButton,  // a mouse button pressed or released, or the wheel turned
    MouseMotion,  // the mouse moved
    ScreenTouch,  // a finger touched or left the screen
    ScreenDrag,   // a finger moved on the screen
    JoypadButton, // a gamepad button pressed or released
    JoypadMotion, // a gamepad stick or trigger moved
    Action,       // an action pressed or released directly, by name
    // The text an input method (IME) is composing, as for Japanese, Chinese or Korean, before
    // it commits it as key events with unicode; empty text ends the composition. Godot reads it
    // from DisplayServer::ime_get_text rather than as an event.
    Ime,
};

// Keyboard keys, by Godot's names ("A", "Space", "Left", "Kp Enter"). A physical key is where
// the key sits on a US QWERTY keyboard, whatever the layout says.
enum class Key : std::uint8_t {
    None,
    Escape,
    Tab,
    Backspace,
    Enter,
    KpEnter,
    Insert,
    Delete,
    Pause,
    Print,
    SysReq,
    Clear,
    Home,
    End,
    Left,
    Up,
    Right,
    Down,
    PageUp,
    PageDown,
    Shift,
    Ctrl,
    Meta,
    Alt,
    CapsLock,
    NumLock,
    ScrollLock,
    F1,
    F2,
    F3,
    F4,
    F5,
    F6,
    F7,
    F8,
    F9,
    F10,
    F11,
    F12,
    F13,
    F14,
    F15,
    F16,
    F17,
    F18,
    F19,
    F20,
    F21,
    F22,
    F23,
    F24,
    KpMultiply,
    KpDivide,
    KpSubtract,
    KpPeriod,
    KpAdd,
    Kp0,
    Kp1,
    Kp2,
    Kp3,
    Kp4,
    Kp5,
    Kp6,
    Kp7,
    Kp8,
    Kp9,
    Menu,
    Help,
    Back,
    Forward,
    Stop,
    Refresh,
    VolumeDown,
    VolumeMute,
    VolumeUp,
    MediaPlay,
    MediaStop,
    MediaPrevious,
    MediaNext,
    MediaRecord,
    Search,
    Space,
    Exclam,
    QuoteDbl,
    NumberSign,
    Dollar,
    Percent,
    Ampersand,
    Apostrophe,
    ParenLeft,
    ParenRight,
    Asterisk,
    Plus,
    Comma,
    Minus,
    Period,
    Slash,
    Key0,
    Key1,
    Key2,
    Key3,
    Key4,
    Key5,
    Key6,
    Key7,
    Key8,
    Key9,
    Colon,
    Semicolon,
    Less,
    Equal,
    Greater,
    Question,
    At,
    A,
    B,
    C,
    D,
    E,
    F,
    G,
    H,
    I,
    J,
    K,
    L,
    M,
    N,
    O,
    P,
    Q,
    R,
    S,
    T,
    U,
    V,
    W,
    X,
    Y,
    Z,
    BracketLeft,
    BackSlash,
    BracketRight,
    AsciiCircum,
    UnderScore,
    QuoteLeft,
    BraceLeft,
    Bar,
    BraceRight,
    AsciiTilde,
};

// Godot's name for a key, e.g. "Kp Enter"; "" for Key::None.
[[nodiscard]] std::string_view key_name(Key key);
// The key with this name, ignoring case as Godot does, or nullopt.
[[nodiscard]] std::optional<Key> parse_key(std::string_view name);
// Every key name but Key::None's, in the order of the Key enum.
[[nodiscard]] std::span<const std::string_view> key_names();

// Mouse buttons, numbered as Godot's MouseButton. The wheel sends a press and a release.
enum class MouseButton : std::uint8_t {
    None,
    Left,
    Right,
    Middle,
    WheelUp,
    WheelDown,
    WheelLeft,
    WheelRight,
    Xbutton1,
    Xbutton2,
};

// Gamepad buttons, numbered as Godot's JoyButton and SDL's gamepad buttons: A, B, X and Y are
// the bottom, right, left and top face buttons, as on an Xbox controller.
enum class JoyButton : std::uint8_t {
    A,
    B,
    X,
    Y,
    Back,
    Guide,
    Start,
    LeftStick,
    RightStick,
    LeftShoulder,
    RightShoulder,
    DpadUp,
    DpadDown,
    DpadLeft,
    DpadRight,
    Misc1,
    Paddle1,
    Paddle2,
    Paddle3,
    Paddle4,
    Touchpad,
};

// Gamepad axes, numbered as Godot's JoyAxis. Sticks go from -1 to 1, y down; triggers from 0 to
// 1.
enum class JoyAxis : std::uint8_t {
    LeftX,
    LeftY,
    RightX,
    RightY,
    TriggerLeft,
    TriggerRight,
};

[[nodiscard]] std::string_view mouse_button_name(MouseButton button);
[[nodiscard]] std::string_view joy_button_name(JoyButton button);
[[nodiscard]] std::string_view joy_axis_name(JoyAxis axis);

// A binding's device that matches events from every device, as Godot's InputMap::ALL_DEVICES.
inline constexpr int kAllDevices = -1;
// The device of events made from other events (a mouse click from a touch), as Godot's
// InputEvent::DEVICE_ID_EMULATION.
inline constexpr int kEmulatedDevice = -1;

struct Modifiers {
    bool shift = false;
    bool ctrl = false;
    bool alt = false;
    bool meta = false; // Command on Apple keyboards, the Windows key elsewhere

    friend bool operator==(const Modifiers&, const Modifiers&) = default;
};

// One input event, as Godot's InputEvent subclasses: type says which fields mean something. An
// action binding is an event too, with only the fields that identify it.
struct Event {
    EventType type = EventType::Key;
    // Which keyboard, mouse, finger set or gamepad: 0 for the first. A binding's device is
    // kAllDevices unless it names one.
    int device = 0;
    // Key, MouseButton, ScreenTouch, JoypadButton and Action: pressed, or released.
    bool pressed = false;

    // Key: the key as the layout labels it, and where it is on a US QWERTY keyboard. A binding
    // names one of them.
    Key key = Key::None;
    Key physical_key = Key::None;
    bool echo = false; // the key is held and repeating
    // Key: the character it types, as Godot's InputEventKey.unicode, or 0. Text typed with an
    // input method or an on-screen keyboard comes as presses with a character and no key.
    char32_t unicode = 0;
    // Key and MouseButton: the modifier keys held. A binding's modifiers must all be held.
    Modifiers modifiers;

    MouseButton button = MouseButton::None;
    bool double_click = false; // MouseButton: a double click; ScreenTouch: a double tap
    float factor = 1.0F;       // MouseButton: how far the wheel turned, in steps
    // MouseButton, MouseMotion, ScreenTouch and ScreenDrag: where, in window pixels (the space
    // sprites and controls are drawn in), from the top left.
    Vec2 position;
    // MouseMotion and ScreenDrag: how far it moved since the last event. Left out, the input
    // state works it out from the last position.
    std::optional<Vec2> relative;
    std::uint8_t button_mask = 0; // MouseMotion: the buttons held, bit (button - 1) for each
    int index = 0;                // ScreenTouch and ScreenDrag: which finger, from 0

    JoyButton joy_button = JoyButton::A;
    JoyAxis axis = JoyAxis::LeftX;
    // JoypadMotion: where the axis is. A binding's axis_value says which way, -1 or 1.
    float axis_value = 0.0F;

    std::string action;    // Action: the action's name
    float strength = 1.0F; // Action: how hard, from 0 to 1

    // Ime: the text being composed, and the part of it the input method has selected, in
    // characters from its start, as Godot's ime_get_selection.
    std::string text;
    int selection_start = 0;
    int selection_length = 0;

    friend bool operator==(const Event&, const Event&) = default;
};

// The JSON names of event types, as input.inject and project.json write them: "key",
// "mouse_button", "mouse_motion", "screen_touch", "screen_drag", "joypad_button",
// "joypad_motion", "action" and "ime". input.inject also takes "text", which it turns into key
// presses (text_events).
[[nodiscard]] std::string_view type_name(EventType type);

// Whether JSON describes an event that happened (input.inject) or a binding (project.json's
// actions). Bindings are keys, mouse buttons, gamepad buttons and gamepad axes, with no pressed
// state or position.
enum class Use : std::uint8_t { Event, Binding };

// The JSON Schema of one event or binding, e.g. {"type": "key", "key": "Space", "pressed": true}.
[[nodiscard]] json::Value event_schema(Use use);
// Every error in an event or binding, with "did you mean" for misspelled names, as JSON
// Pointers under path.
[[nodiscard]] std::vector<json::ValidationError> validate_event(const json::Value& doc, Use use,
                                                                const std::string& path = "");
// Reads an event or binding; validate it first.
[[nodiscard]] Result<Event> parse_event(const json::Value& doc, Use use);
// Reads an event, or the key presses a {"type": "text"} event types, as input.inject does.
[[nodiscard]] Result<std::vector<Event>> parse_events(const json::Value& doc);
// The event or binding as JSON, leaving out fields equal to their defaults.
[[nodiscard]] json::Value event_json(const Event& event, Use use);
// Key presses that type text, one per character, as an input method commits it: no key, the
// character in unicode.
[[nodiscard]] std::vector<Event> text_events(std::string_view text);

// How a binding matches an event, as Godot's InputEvent::action_match.
struct Match {
    bool pressed = false;
    float strength = 0.0F;     // 0 to 1, past the deadzone
    float raw_strength = 0.0F; // 0 to 1, ignoring the deadzone
};
// Whether binding matches event and, if so, how. Exact also requires the same modifiers and, for
// an axis, the same direction.
[[nodiscard]] std::optional<Match> match(const Event& binding, const Event& event, float deadzone,
                                         bool exact = false);

// Godot's deadzone for new actions, and for its built-in ui_* actions.
inline constexpr float kDefaultDeadzone = 0.2F;
inline constexpr float kToggleDeadzone = 0.5F;

struct Action {
    float deadzone = kDefaultDeadzone;
    std::vector<Event> events; // bindings
};

// Which keyboard shortcuts Godot's built-in text actions use: Ctrl on most systems; Command
// (meta) on Apple's, with macOS's own (Option+Left for a word, Command+Left for the line's
// start), as Godot's ".macos" overrides.
enum class Keyboard : std::uint8_t { Pc, Mac };
// The machine's: Mac on macOS and iOS, else Pc.
[[nodiscard]] Keyboard host_keyboard();

// Actions by name and the events bound to each, as Godot's InputMap. A game can rebind them
// while it runs, e.g. from an options menu.
class Map {
public:
    // Godot's built-in actions: for moving through and pressing UI (ui_accept, ui_select,
    // ui_cancel, ui_focus_next, ui_focus_prev, ui_left, ui_right, ui_up, ui_down, ui_page_up,
    // ui_page_down, ui_home and ui_end), and for editing text (ui_cut, ui_copy, ui_paste,
    // ui_undo, ui_redo, ui_text_submit, ui_text_backspace, ui_text_caret_word_left and the rest)
    // with the keyboard's shortcuts.
    [[nodiscard]] static Map with_defaults(Keyboard keyboard = host_keyboard());

    [[nodiscard]] bool has_action(std::string_view name) const;
    // Adds an action with no bindings, or replaces one's deadzone.
    void add_action(const std::string& name, float deadzone = kDefaultDeadzone);
    void erase_action(std::string_view name);
    // Binds an event to an action, unless the same binding is there. Does nothing for an
    // unknown action.
    void action_add_event(std::string_view name, const Event& binding);
    void action_erase_event(std::string_view name, const Event& binding);
    void action_erase_events(std::string_view name);
    [[nodiscard]] const Action* find(std::string_view name) const;
    [[nodiscard]] const std::map<std::string, Action, std::less<>>& actions() const {
        return actions_;
    }
    // Action names, sorted, for "did you mean" hints.
    [[nodiscard]] std::vector<std::string_view> names() const;

    // The first of an action's bindings that matches event, with its index, as Godot's
    // InputMap::event_get_index. An Action event matches its own action, with the largest index.
    struct Found {
        std::size_t index = 0;
        Match match;
    };
    [[nodiscard]] std::optional<Found> find_event(std::string_view name, const Event& event,
                                                  bool exact = false) const;

    friend bool operator==(const Map&, const Map&) = default;

private:
    std::map<std::string, Action, std::less<>> actions_;
};

// Whether an action is one of Godot's built-in ones for editing text (ui_cut, ui_text_backspace
// and the like), which input.actions lists only when asked by name.
[[nodiscard]] bool is_text_action(std::string_view name);

// project.json's "input": actions by name, each {"deadzone", "events"}, over Godot's built-in
// ui_* actions, which an entry with the same name replaces.
[[nodiscard]] json::Value map_schema();
[[nodiscard]] std::vector<json::ValidationError> validate_map(const json::Value& doc,
                                                              const std::string& path = "");
// Reads project.json's "input" over the keyboard's defaults; null gives the defaults. Validate
// it first.
[[nodiscard]] Result<Map> parse_map(const json::Value& doc, Keyboard keyboard = host_keyboard());
[[nodiscard]] json::Value action_json(const Action& action);

// How touches and the mouse stand in for each other, as Godot's input_devices/pointing settings
// (project.json's "input_devices").
struct Settings {
    // A touch also clicks the left mouse button, and dragging it moves the mouse, so mouse-only
    // code works on phones.
    bool emulate_mouse_from_touch = true;
    // A left click also touches the screen, so touch-only code can be tried with a mouse.
    bool emulate_touch_from_mouse = false;

    friend bool operator==(const Settings&, const Settings&) = default;
};
[[nodiscard]] json::Value settings_schema();
// Reads project.json's "input_devices"; null gives the defaults. Validate it first.
[[nodiscard]] Settings parse_settings(const json::Value& doc);

// Adds the input state and the "input" system, which applies queued events at the start of each
// tick (priority -1000, before gameplay systems). Installing again replaces the map and settings
// and keeps what is held.
void install(world::World& world, Map map = Map::with_defaults(), Settings settings = {});
// The world's action map, to rebind actions while the game runs; nullptr if input isn't
// installed.
[[nodiscard]] Map* map(world::World& world);
[[nodiscard]] const Map* map(const world::World& world);

// Queues an event for the start of the next tick. A mouse motion, finger drag or gamepad axis
// motion merges into the queued one from the same source if no press or release came between,
// as Godot's accumulated input does. Does nothing if input isn't installed.
void push(world::World& world, Event event);
// Queues an Action event: the action is pressed from the next tick until action_release, as
// Godot's Input.action_press.
void action_press(world::World& world, std::string_view action, float strength = 1.0F);
void action_release(world::World& world, std::string_view action);
// Releases every key, button and action at the next tick, as Godot does when the window loses
// focus.
void release_pressed(world::World& world);

// What gameplay code asks, as Godot's Input. "Just" means during the tick the input system last
// ran: inside a system, this tick. A press and release in one tick count as both, so a quick tap
// is never missed. Unknown actions read as released. Exact also requires the same modifiers.
[[nodiscard]] bool is_action_pressed(const world::World& world, std::string_view action,
                                     bool exact = false);
[[nodiscard]] bool is_action_just_pressed(const world::World& world, std::string_view action,
                                          bool exact = false);
[[nodiscard]] bool is_action_just_released(const world::World& world, std::string_view action,
                                           bool exact = false);
[[nodiscard]] float get_action_strength(const world::World& world, std::string_view action,
                                        bool exact = false);
[[nodiscard]] float get_action_raw_strength(const world::World& world, std::string_view action,
                                            bool exact = false);
// Positive's strength minus negative's, from -1 to 1.
[[nodiscard]] float get_axis(const world::World& world, std::string_view negative,
                             std::string_view positive);
// A direction from four actions, at most 1 long, with a round deadzone: below it the vector is
// zero, and lengths past it are scaled to start at 0. A negative deadzone takes the average of
// the four actions' deadzones.
[[nodiscard]] Vec2 get_vector(const world::World& world, std::string_view negative_x,
                              std::string_view positive_x, std::string_view negative_y,
                              std::string_view positive_y, float deadzone = -1.0F);

[[nodiscard]] bool is_key_pressed(const world::World& world, Key key);
[[nodiscard]] bool is_physical_key_pressed(const world::World& world, Key key);
[[nodiscard]] bool is_mouse_button_pressed(const world::World& world, MouseButton button);
[[nodiscard]] Vec2 mouse_position(const world::World& world);
[[nodiscard]] bool is_joy_button_pressed(const world::World& world, int device, JoyButton button);
[[nodiscard]] float get_joy_axis(const world::World& world, int device, JoyAxis axis);

// The events the input system applied this tick, in order, with the ones emulated from them (a
// click from a touch), for code that reacts to events rather than state.
[[nodiscard]] std::span<const Event> events(const world::World& world);
// Events waiting for the next tick.
[[nodiscard]] std::size_t queued(const world::World& world);

// What input.state returns: the tick the input system last ran, the actions pressed or just
// pressed or released (text editing actions only when named), the keys and mouse buttons held, the
// mouse position, fingers down and gamepad buttons and axes away from rest. Only the given actions,
// if any are named.
[[nodiscard]] json::Value state_json(const world::World& world,
                                     std::span<const std::string> actions = {});

// What a text field with focus asks of the platform, as Godot's LineEdit asks DisplayServer to
// turn on the input method and show the on-screen keyboard: ui::text_input works it out each
// tick and SdlReader::set_text_input applies it.
struct TextInput {
    bool active = false; // a text field is being edited
    // The field in window pixels, and the caret's x from its left, so the input method's
    // candidate window opens next to it.
    Vec2 position;
    Vec2 size;
    int caret = 0;
    // The on-screen keyboard: shown, and which kind, as LineEdit.virtual_keyboard_type
    // ("default", "multiline", "number", "number_decimal", "phone", "email_address",
    // "password" or "url").
    bool show_keyboard = false;
    std::string keyboard = "default";

    friend bool operator==(const TextInput&, const TextInput&) = default;
};

// A copy of the input state, so a session can put it back when a command fails or was a dry
// run.
struct Snapshot {
    std::shared_ptr<const void> data;
};
[[nodiscard]] Snapshot save(const world::World& world);
void restore(world::World& world, const Snapshot& snapshot);

// Input queued from outside the world's systems (the platform's events, input.inject), for
// recording replays: an event as push got it, or a release_pressed when release_all is set.
struct Recorded {
    Event event;
    bool release_all = false;
};
using Recorder = std::function<void(const Recorded& input)>;
// Calls recorder for each push and release_pressed made while the world isn't stepping; an empty
// one stops.
void set_recorder(world::World& world, Recorder recorder);

} // namespace slash::input
