#include "slash/core/text.hpp"
#include "slash/input/input.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Input events as Godot's InputEvent subclasses (core/input/input_event.cpp), read from and
// written to JSON with Godot's property names.
namespace slash::input {
namespace {

// Godot's key names (core/os/keyboard.cpp), in the order of the Key enum.
constexpr std::array<std::string_view, 150> kKeyNames{{
    "",
    "Escape",
    "Tab",
    "Backspace",
    "Enter",
    "Kp Enter",
    "Insert",
    "Delete",
    "Pause",
    "Print",
    "SysReq",
    "Clear",
    "Home",
    "End",
    "Left",
    "Up",
    "Right",
    "Down",
    "PageUp",
    "PageDown",
    "Shift",
    "Ctrl",
    "Meta",
    "Alt",
    "CapsLock",
    "NumLock",
    "ScrollLock",
    "F1",
    "F2",
    "F3",
    "F4",
    "F5",
    "F6",
    "F7",
    "F8",
    "F9",
    "F10",
    "F11",
    "F12",
    "F13",
    "F14",
    "F15",
    "F16",
    "F17",
    "F18",
    "F19",
    "F20",
    "F21",
    "F22",
    "F23",
    "F24",
    "Kp Multiply",
    "Kp Divide",
    "Kp Subtract",
    "Kp Period",
    "Kp Add",
    "Kp 0",
    "Kp 1",
    "Kp 2",
    "Kp 3",
    "Kp 4",
    "Kp 5",
    "Kp 6",
    "Kp 7",
    "Kp 8",
    "Kp 9",
    "Menu",
    "Help",
    "Back",
    "Forward",
    "Stop",
    "Refresh",
    "VolumeDown",
    "VolumeMute",
    "VolumeUp",
    "MediaPlay",
    "MediaStop",
    "MediaPrevious",
    "MediaNext",
    "MediaRecord",
    "Search",
    "Space",
    "Exclam",
    "QuoteDbl",
    "NumberSign",
    "Dollar",
    "Percent",
    "Ampersand",
    "Apostrophe",
    "ParenLeft",
    "ParenRight",
    "Asterisk",
    "Plus",
    "Comma",
    "Minus",
    "Period",
    "Slash",
    "0",
    "1",
    "2",
    "3",
    "4",
    "5",
    "6",
    "7",
    "8",
    "9",
    "Colon",
    "Semicolon",
    "Less",
    "Equal",
    "Greater",
    "Question",
    "At",
    "A",
    "B",
    "C",
    "D",
    "E",
    "F",
    "G",
    "H",
    "I",
    "J",
    "K",
    "L",
    "M",
    "N",
    "O",
    "P",
    "Q",
    "R",
    "S",
    "T",
    "U",
    "V",
    "W",
    "X",
    "Y",
    "Z",
    "BracketLeft",
    "BackSlash",
    "BracketRight",
    "AsciiCircum",
    "UnderScore",
    "QuoteLeft",
    "BraceLeft",
    "Bar",
    "BraceRight",
    "AsciiTilde",
}};

constexpr std::array<std::string_view, 10> kMouseButtons{
    "",           "left",       "right",       "middle",   "wheel_up",
    "wheel_down", "wheel_left", "wheel_right", "xbutton1", "xbutton2",
};

constexpr std::array<std::string_view, 21> kJoyButtons{
    "a",
    "b",
    "x",
    "y",
    "back",
    "guide",
    "start",
    "left_stick",
    "right_stick",
    "left_shoulder",
    "right_shoulder",
    "dpad_up",
    "dpad_down",
    "dpad_left",
    "dpad_right",
    "misc1",
    "paddle1",
    "paddle2",
    "paddle3",
    "paddle4",
    "touchpad",
};

constexpr std::array<std::string_view, 6> kJoyAxes{
    "left_x", "left_y", "right_x", "right_y", "trigger_left", "trigger_right",
};

constexpr std::array<std::string_view, 8> kTypes{
    "key",         "mouse_button",  "mouse_motion",  "screen_touch",
    "screen_drag", "joypad_button", "joypad_motion", "action",
};

constexpr std::size_t kMaxDevice = 63;
constexpr std::size_t kMaxFingers = 32;

// Bits of EventType, for which types a field applies to.
constexpr std::uint16_t bit(EventType type) {
    return static_cast<std::uint16_t>(1U << static_cast<unsigned>(type));
}
constexpr std::uint16_t kAnyType = 0xFF;
constexpr std::uint16_t kBindingTypes = bit(EventType::Key) | bit(EventType::MouseButton) |
                                        bit(EventType::JoypadButton) | bit(EventType::JoypadMotion);
constexpr std::uint16_t kPressTypes = bit(EventType::Key) | bit(EventType::MouseButton) |
                                      bit(EventType::ScreenTouch) | bit(EventType::JoypadButton) |
                                      bit(EventType::Action);
constexpr std::uint16_t kPointTypes = bit(EventType::MouseButton) | bit(EventType::MouseMotion) |
                                      bit(EventType::ScreenTouch) | bit(EventType::ScreenDrag);
constexpr std::uint16_t kModifierTypes = bit(EventType::Key) | bit(EventType::MouseButton);

struct Field {
    std::string_view name;
    std::uint16_t event_types = 0;   // types whose events have it
    std::uint16_t binding_types = 0; // types whose bindings have it
};

constexpr std::array<Field, 21> kFields{{
    {"type", kAnyType, kBindingTypes},
    {"device", kAnyType, kBindingTypes},
    {"pressed", kPressTypes, 0},
    {"key", bit(EventType::Key), bit(EventType::Key)},
    {"physical_key", bit(EventType::Key), bit(EventType::Key)},
    {"echo", bit(EventType::Key), 0},
    {"shift", kModifierTypes, kModifierTypes},
    {"ctrl", kModifierTypes, kModifierTypes},
    {"alt", kModifierTypes, kModifierTypes},
    {"meta", kModifierTypes, kModifierTypes},
    {"button", bit(EventType::MouseButton) | bit(EventType::JoypadButton),
     bit(EventType::MouseButton) | bit(EventType::JoypadButton)},
    {"double_click", bit(EventType::MouseButton), 0},
    {"double_tap", bit(EventType::ScreenTouch), 0},
    {"factor", bit(EventType::MouseButton), 0},
    {"position", kPointTypes, 0},
    {"relative", bit(EventType::MouseMotion) | bit(EventType::ScreenDrag), 0},
    {"index", bit(EventType::ScreenTouch) | bit(EventType::ScreenDrag), 0},
    {"axis", bit(EventType::JoypadMotion), bit(EventType::JoypadMotion)},
    {"axis_value", bit(EventType::JoypadMotion), bit(EventType::JoypadMotion)},
    {"action", bit(EventType::Action), 0},
    {"strength", bit(EventType::Action), 0},
}};

template <std::size_t N>
std::optional<std::size_t> index_of(const std::array<std::string_view, N>& names,
                                    std::string_view name) {
    const auto found = std::ranges::find(names, name);
    if (found == names.end() || name.empty()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(found - names.begin());
}

std::string quoted(std::string_view text) {
    return "\"" + std::string(text) + "\"";
}

// Names a short list of choices when none is close.
constexpr std::size_t kMaxListedNames = 24;

// "no key \"Spce\"; did you mean \"Space\"?"
template <typename Range>
std::string unknown(std::string_view what, std::string_view name, const Range& candidates) {
    std::string message = "no " + std::string(what) + " " + quoted(name);
    if (const std::string_view hint = closest_match(name, candidates); !hint.empty()) {
        message += "; did you mean " + quoted(hint) + "?";
    } else if (std::size(candidates) <= kMaxListedNames) {
        // Short lists are cheaper to read than to guess from.
        message += "; expected one of";
        const char* separator = " ";
        for (const std::string_view candidate : candidates) {
            message += separator + std::string(candidate);
            separator = ", ";
        }
    }
    return message;
}

json::Value string_property(std::string_view description) {
    json::Value schema;
    schema["type"] = "string";
    schema["description"] = description;
    return schema;
}

json::Value boolean_property(std::string_view description) {
    json::Value schema;
    schema["type"] = "boolean";
    schema["description"] = description;
    return schema;
}

json::Value number_property(std::string_view description, double minimum, double maximum) {
    json::Value schema;
    schema["type"] = "number";
    schema["minimum"] = minimum;
    schema["maximum"] = maximum;
    schema["description"] = description;
    return schema;
}

json::Value point_property(std::string_view description) {
    json::Value schema;
    schema["type"] = "array";
    schema["items"]["type"] = "number";
    schema["minItems"] = 2;
    schema["maxItems"] = 2;
    schema["description"] = description;
    return schema;
}

EventType type_of(const json::Value& doc) {
    const json::Value* type = doc.find("type");
    const auto found =
        type != nullptr && type->is_string() ? index_of(kTypes, type->as_string()) : std::nullopt;
    return static_cast<EventType>(found.value_or(0));
}

Vec2 point(const json::Value& value) {
    const json::Array& items = value.as_array();
    return {static_cast<float>(items[0].as_double()), static_cast<float>(items[1].as_double())};
}

json::Value point_json(Vec2 value) {
    return json::Array{json::Value(value.x), json::Value(value.y)};
}

bool flag(const json::Value& doc, std::string_view name) {
    const json::Value* value = doc.find(name);
    return value != nullptr && value->as_bool();
}

std::uint8_t modifier_bits(const Modifiers& modifiers) {
    return static_cast<std::uint8_t>((modifiers.shift ? 1U : 0U) | (modifiers.ctrl ? 2U : 0U) |
                                     (modifiers.alt ? 4U : 0U) | (modifiers.meta ? 8U : 0U));
}

// Godot's modifier rule: a press must hold at least the binding's modifiers, and an exact match
// exactly them.
bool modifiers_match(const Event& binding, const Event& event, bool exact) {
    const std::uint8_t wanted = modifier_bits(binding.modifiers);
    const std::uint8_t held = modifier_bits(event.modifiers);
    if (event.pressed && (wanted & held) != wanted) {
        return false;
    }
    return !exact || wanted == held;
}

Match button_match(bool pressed) {
    const float strength = pressed ? 1.0F : 0.0F;
    return {.pressed = pressed, .strength = strength, .raw_strength = strength};
}

} // namespace

std::string_view key_name(Key key) {
    const auto index = static_cast<std::size_t>(key);
    return index < kKeyNames.size() ? kKeyNames.at(index) : std::string_view();
}

std::optional<Key> parse_key(std::string_view name) {
    const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c + 32) : c; };
    for (std::size_t i = 1; i < kKeyNames.size(); ++i) {
        const std::string_view candidate = kKeyNames.at(i);
        if (candidate.size() == name.size() &&
            std::ranges::equal(candidate, name, {}, lower, lower)) {
            return static_cast<Key>(i);
        }
    }
    return std::nullopt;
}

std::span<const std::string_view> key_names() {
    return std::span(kKeyNames).subspan(1);
}

std::string_view mouse_button_name(MouseButton button) {
    return kMouseButtons.at(static_cast<std::size_t>(button));
}

std::string_view joy_button_name(JoyButton button) {
    return kJoyButtons.at(static_cast<std::size_t>(button));
}

std::string_view joy_axis_name(JoyAxis axis) {
    return kJoyAxes.at(static_cast<std::size_t>(axis));
}

std::string_view type_name(EventType type) {
    return kTypes.at(static_cast<std::size_t>(type));
}

json::Value event_schema(Use use) {
    const bool binding = use == Use::Binding;
    json::Value schema;
    schema["type"] = "object";
    schema["description"] =
        binding ? "An event bound to an action: a key, mouse button, gamepad button or gamepad "
                  "axis, e.g. {\"type\": \"key\", \"key\": \"Space\"}."
                : "An input event, e.g. {\"type\": \"key\", \"key\": \"Space\", \"pressed\": "
                  "true} or {\"type\": \"action\", \"action\": \"jump\", \"pressed\": true}.";
    json::Value& properties = schema["properties"];
    json::Array types;
    for (std::size_t i = 0; i < kTypes.size(); ++i) {
        if (!binding || (kBindingTypes & bit(static_cast<EventType>(i))) != 0) {
            types.emplace_back(kTypes.at(i));
        }
    }
    properties["type"]["type"] = "string";
    properties["type"]["enum"] = std::move(types);
    properties["type"]["description"] = "What kind of event.";
    json::Value device;
    device["type"] = "integer";
    device["minimum"] = binding ? -1 : 0;
    device["maximum"] = kMaxDevice;
    device["description"] = binding ? "Only events from this device; -1 (the default) for any."
                                    : "Which keyboard, mouse or gamepad, from 0 (the default).";
    properties["device"] = std::move(device);
    properties["key"] =
        string_property("key: the key as labelled, by Godot's name, e.g. \"A\", \"Space\", "
                        "\"Left\", \"Enter\", \"Kp 1\", \"F5\".");
    properties["physical_key"] = string_property(
        "key: the key at this place on a US QWERTY keyboard, whatever the layout says.");
    properties["shift"] = boolean_property(binding ? "key and mouse_button: Shift must be held."
                                                   : "key and mouse_button: Shift is held.");
    properties["ctrl"] = boolean_property("key and mouse_button: Ctrl.");
    properties["alt"] = boolean_property("key and mouse_button: Alt (Option on a Mac).");
    properties["meta"] =
        boolean_property("key and mouse_button: Meta (Command on a Mac, Windows elsewhere).");
    properties["button"] = string_property(
        "mouse_button: left, right, middle, wheel_up, wheel_down, wheel_left, wheel_right, "
        "xbutton1 or xbutton2. joypad_button: a, b, x, y (bottom, right, left and top face "
        "buttons), back, guide, start, left_stick, right_stick, left_shoulder, right_shoulder, "
        "dpad_up, dpad_down, dpad_left, dpad_right, misc1, paddle1 to paddle4 or touchpad.");
    properties["axis"] = string_property(
        "joypad_motion: left_x, left_y, right_x, right_y, trigger_left or trigger_right.");
    properties["axis_value"] =
        number_property(binding ? "joypad_motion: which way, -1 or 1."
                                : "joypad_motion: where the axis is, from -1 to 1 (y down); "
                                  "triggers from 0 to 1.",
                        -1.0, 1.0);
    if (!binding) {
        properties["pressed"] = boolean_property(
            "key, mouse_button, screen_touch, joypad_button and action: pressed, or released.");
        properties["echo"] = boolean_property("key: a repeat while the key is held.");
        properties["double_click"] = boolean_property("mouse_button: a double click.");
        properties["double_tap"] = boolean_property("screen_touch: a double tap.");
        properties["factor"] = number_property(
            "mouse_button: how far a wheel turned, in steps (default 1).", 0.0, 1000.0);
        properties["position"] = point_property(
            "mouse_button, mouse_motion, screen_touch and screen_drag: [x, y] in window "
            "pixels from the top left.");
        properties["relative"] =
            point_property("mouse_motion and screen_drag: [x, y] moved since the last event; "
                           "left out, worked out from the last position.");
        json::Value index;
        index["type"] = "integer";
        index["minimum"] = 0;
        index["maximum"] = kMaxFingers - 1;
        index["description"] = "screen_touch and screen_drag: which finger, from 0.";
        properties["index"] = std::move(index);
        properties["action"] = string_property("action: the action's name.");
        properties["strength"] =
            number_property("action: how hard, from 0 to 1 (default 1).", 0.0, 1.0);
    }
    schema["required"] = json::Array{"type"};
    schema["additionalProperties"] = false;
    return schema;
}

std::vector<json::ValidationError> validate_event(const json::Value& doc, Use use,
                                                  const std::string& path) {
    std::vector<json::ValidationError> errors = json::validate(doc, event_schema(use));
    for (json::ValidationError& error : errors) {
        error.path = path + error.path;
    }
    if (!errors.empty()) {
        return errors;
    }
    const bool binding = use == Use::Binding;
    const EventType type = type_of(doc);
    const std::string name(type_name(type));
    const std::string kind = binding ? name + " bindings" : name + " events";
    for (const auto& [member, value] : doc.as_object()) {
        const Field* field = nullptr;
        for (const Field& candidate : kFields) {
            if (candidate.name == member) {
                field = &candidate;
                break;
            }
        }
        if (field == nullptr) {
            continue;
        }
        const std::uint16_t types = binding ? field->binding_types : field->event_types;
        if ((types & bit(type)) == 0) {
            errors.push_back(
                {path + "/" + json::pointer_escape(member), kind + " have no " + quoted(member)});
        }
    }
    const auto require = [&](std::string_view member) {
        if (doc.find(member) == nullptr) {
            errors.push_back({path, kind + " need " + quoted(member)});
        }
    };
    const auto check_name = [&](std::string_view member, auto&& is_known, std::string_view what,
                                const auto& candidates) {
        const json::Value* value = doc.find(member);
        if (value != nullptr && !is_known(value->as_string())) {
            errors.push_back(
                {path + "/" + std::string(member), unknown(what, value->as_string(), candidates)});
        }
    };
    const auto is_key = [](std::string_view text) { return parse_key(text).has_value(); };
    switch (type) {
    case EventType::Key:
        if (doc.find("key") == nullptr && doc.find("physical_key") == nullptr) {
            errors.push_back({path, kind + R"( need "key" or "physical_key")"});
        }
        check_name("key", is_key, "key", key_names());
        check_name("physical_key", is_key, "key", key_names());
        break;
    case EventType::MouseButton:
        require("button");
        check_name(
            "button",
            [](std::string_view text) { return index_of(kMouseButtons, text).has_value(); },
            "mouse button", std::span(kMouseButtons).subspan(1));
        if (!binding) {
            require("position");
        }
        break;
    case EventType::MouseMotion:
    case EventType::ScreenTouch:
    case EventType::ScreenDrag:
        require("position");
        break;
    case EventType::JoypadButton:
        require("button");
        check_name(
            "button", [](std::string_view text) { return index_of(kJoyButtons, text).has_value(); },
            "gamepad button", kJoyButtons);
        break;
    case EventType::JoypadMotion:
        require("axis");
        require("axis_value");
        check_name(
            "axis", [](std::string_view text) { return index_of(kJoyAxes, text).has_value(); },
            "gamepad axis", kJoyAxes);
        if (const json::Value* value = doc.find("axis_value");
            binding && value != nullptr && value->as_double() == 0.0) {
            errors.push_back({path + "/axis_value", "a binding's axis_value is -1 or 1"});
        }
        break;
    case EventType::Action:
        require("action");
        break;
    }
    return errors;
}

Result<Event> parse_event(const json::Value& doc, Use use) {
    if (const auto errors = validate_event(doc, use); !errors.empty()) {
        return make_error(ErrorCode::InvalidArgument,
                          (errors.front().path.empty() ? "" : errors.front().path + ": ") +
                              errors.front().message);
    }
    Event event;
    event.type = type_of(doc);
    event.device = use == Use::Binding ? kAllDevices : 0;
    if (const json::Value* device = doc.find("device"); device != nullptr) {
        event.device = static_cast<int>(device->as_int());
    }
    event.pressed = flag(doc, "pressed");
    if (const json::Value* key = doc.find("key"); key != nullptr) {
        event.key = parse_key(key->as_string()).value_or(Key::None);
    }
    if (const json::Value* key = doc.find("physical_key"); key != nullptr) {
        event.physical_key = parse_key(key->as_string()).value_or(Key::None);
    }
    // An event names its key both ways, as one from a keyboard does.
    if (use == Use::Event && event.type == EventType::Key) {
        if (event.key == Key::None) {
            event.key = event.physical_key;
        } else if (event.physical_key == Key::None) {
            event.physical_key = event.key;
        }
    }
    event.echo = flag(doc, "echo");
    event.modifiers = {.shift = flag(doc, "shift"),
                       .ctrl = flag(doc, "ctrl"),
                       .alt = flag(doc, "alt"),
                       .meta = flag(doc, "meta")};
    if (const json::Value* button = doc.find("button"); button != nullptr) {
        if (event.type == EventType::MouseButton) {
            event.button =
                static_cast<MouseButton>(index_of(kMouseButtons, button->as_string()).value_or(0));
        } else {
            event.joy_button =
                static_cast<JoyButton>(index_of(kJoyButtons, button->as_string()).value_or(0));
        }
    }
    event.double_click = flag(doc, "double_click") || flag(doc, "double_tap");
    if (const json::Value* factor = doc.find("factor"); factor != nullptr) {
        event.factor = static_cast<float>(factor->as_double());
    }
    if (const json::Value* position = doc.find("position"); position != nullptr) {
        event.position = point(*position);
    }
    if (const json::Value* relative = doc.find("relative"); relative != nullptr) {
        event.relative = point(*relative);
    }
    if (const json::Value* index = doc.find("index"); index != nullptr) {
        event.index = static_cast<int>(index->as_int());
    }
    if (const json::Value* axis = doc.find("axis"); axis != nullptr) {
        event.axis = static_cast<JoyAxis>(index_of(kJoyAxes, axis->as_string()).value_or(0));
    }
    if (const json::Value* value = doc.find("axis_value"); value != nullptr) {
        event.axis_value = static_cast<float>(value->as_double());
        if (use == Use::Binding) {
            event.axis_value = event.axis_value < 0.0F ? -1.0F : 1.0F;
        }
    }
    if (const json::Value* action = doc.find("action"); action != nullptr) {
        event.action = action->as_string();
    }
    if (const json::Value* strength = doc.find("strength"); strength != nullptr) {
        event.strength = static_cast<float>(strength->as_double());
    }
    return event;
}

json::Value event_json(const Event& event, Use use) {
    const bool binding = use == Use::Binding;
    json::Value out;
    out["type"] = type_name(event.type);
    if (event.device != (binding ? kAllDevices : 0)) {
        out["device"] = event.device;
    }
    const auto modifiers = [&] {
        if (event.modifiers.shift) {
            out["shift"] = true;
        }
        if (event.modifiers.ctrl) {
            out["ctrl"] = true;
        }
        if (event.modifiers.alt) {
            out["alt"] = true;
        }
        if (event.modifiers.meta) {
            out["meta"] = true;
        }
    };
    const auto pressed = [&] {
        if (!binding) {
            out["pressed"] = event.pressed;
        }
    };
    switch (event.type) {
    case EventType::Key:
        if (event.key != Key::None) {
            out["key"] = key_name(event.key);
        }
        if (event.physical_key != Key::None && (binding || event.physical_key != event.key)) {
            out["physical_key"] = key_name(event.physical_key);
        }
        modifiers();
        pressed();
        if (!binding && event.echo) {
            out["echo"] = true;
        }
        break;
    case EventType::MouseButton:
        out["button"] = mouse_button_name(event.button);
        modifiers();
        if (!binding) {
            out["position"] = point_json(event.position);
            pressed();
            if (event.double_click) {
                out["double_click"] = true;
            }
            if (event.factor != 1.0F) {
                out["factor"] = event.factor;
            }
        }
        break;
    case EventType::MouseMotion:
    case EventType::ScreenDrag:
        if (event.type == EventType::ScreenDrag && event.index != 0) {
            out["index"] = event.index;
        }
        out["position"] = point_json(event.position);
        if (event.relative) {
            out["relative"] = point_json(*event.relative);
        }
        break;
    case EventType::ScreenTouch:
        if (event.index != 0) {
            out["index"] = event.index;
        }
        out["position"] = point_json(event.position);
        pressed();
        if (event.double_click) {
            out["double_tap"] = true;
        }
        break;
    case EventType::JoypadButton:
        out["button"] = joy_button_name(event.joy_button);
        pressed();
        break;
    case EventType::JoypadMotion:
        out["axis"] = joy_axis_name(event.axis);
        out["axis_value"] = event.axis_value;
        break;
    case EventType::Action:
        out["action"] = event.action;
        pressed();
        if (event.strength != 1.0F) {
            out["strength"] = event.strength;
        }
        break;
    }
    return out;
}

std::optional<Match> match(const Event& binding, const Event& event, float deadzone, bool exact) {
    if (binding.type != event.type) {
        return std::nullopt;
    }
    switch (event.type) {
    case EventType::Key: {
        bool matched = false;
        if (binding.key != Key::None) {
            matched = binding.key == event.key;
        } else if (binding.physical_key != Key::None) {
            matched = binding.physical_key == event.physical_key;
        }
        if (!matched || !modifiers_match(binding, event, exact)) {
            return std::nullopt;
        }
        return button_match(event.pressed);
    }
    case EventType::MouseButton:
        if (binding.button != event.button || !modifiers_match(binding, event, exact)) {
            return std::nullopt;
        }
        return button_match(event.pressed);
    case EventType::JoypadButton:
        if (binding.joy_button != event.joy_button) {
            return std::nullopt;
        }
        return button_match(event.pressed);
    case EventType::JoypadMotion: {
        // Matches either way, but the other way reads as released.
        if (binding.axis != event.axis ||
            (exact && (binding.axis_value < 0.0F) != (event.axis_value < 0.0F))) {
            return std::nullopt;
        }
        const float amount = std::abs(event.axis_value);
        const bool same_direction =
            (binding.axis_value < 0.0F) == (event.axis_value < 0.0F) || event.axis_value == 0.0F;
        Match result;
        result.pressed = same_direction && amount >= deadzone;
        if (result.pressed) {
            result.strength = deadzone >= 1.0F
                                  ? 1.0F
                                  : std::clamp((amount - deadzone) / (1.0F - deadzone), 0.0F, 1.0F);
        }
        result.raw_strength = same_direction ? amount : 0.0F;
        return result;
    }
    case EventType::MouseMotion:
    case EventType::ScreenTouch:
    case EventType::ScreenDrag:
    case EventType::Action:
        break;
    }
    return std::nullopt;
}

} // namespace slash::input
