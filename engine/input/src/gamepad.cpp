#include "slash/input/input.hpp"

#include <array>
#include <cctype>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>

// Kinds of gamepad, and the labels and glyph names prompts show for bindings.
namespace slash::input {
namespace {

constexpr std::array<std::string_view, 10> kGamepadTypes{
    "standard", "xbox_360",   "xbox_one",    "ps3",          "ps4",
    "ps5",      "switch_pro", "joycon_left", "joycon_right", "joycon_pair",
};

enum class Family : std::uint8_t { Gamepad, Xbox, PlayStation, Nintendo };

Family family_of(GamepadType type) {
    switch (type) {
    case GamepadType::Xbox360:
    case GamepadType::XboxOne:
        return Family::Xbox;
    case GamepadType::Ps3:
    case GamepadType::Ps4:
    case GamepadType::Ps5:
        return Family::PlayStation;
    case GamepadType::SwitchPro:
    case GamepadType::JoyconLeft:
    case GamepadType::JoyconRight:
    case GamepadType::JoyconPair:
        return Family::Nintendo;
    case GamepadType::Standard:
        break;
    }
    return Family::Gamepad;
}

std::string_view family_name(Family family) {
    switch (family) {
    case Family::Xbox:
        return "xbox";
    case Family::PlayStation:
        return "playstation";
    case Family::Nintendo:
        return "nintendo";
    case Family::Gamepad:
        break;
    }
    return "gamepad";
}

// "Left Stick Up" -> "left_stick_up", "D-pad Up" -> "dpad_up".
std::string snake(std::string_view label) {
    std::string out;
    for (const char c : label) {
        if (c == '-') {
            continue;
        }
        if (std::isalnum(static_cast<unsigned char>(c)) != 0) {
            out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        } else if (!out.empty() && out.back() != '_') {
            out += '_';
        }
    }
    while (!out.empty() && out.back() == '_') {
        out.pop_back();
    }
    return out;
}

ButtonLabel labelled(std::string_view family, std::string_view label, std::string_view glyph = {}) {
    return {.label = std::string(label),
            .glyph =
                std::string(family) + "_" + (glyph.empty() ? snake(label) : std::string(glyph))};
}

// One label per family: gamepad, xbox, playstation, nintendo.
using Labels = std::array<std::string_view, 4>;

std::string_view pick(const Labels& labels, Family family) {
    return labels.at(static_cast<std::size_t>(family));
}

} // namespace

std::string_view gamepad_type_name(GamepadType type) {
    return kGamepadTypes.at(static_cast<std::size_t>(type));
}

std::optional<GamepadType> parse_gamepad_type(std::string_view name) {
    for (std::size_t i = 0; i < kGamepadTypes.size(); ++i) {
        if (kGamepadTypes.at(i) == name) {
            return static_cast<GamepadType>(i);
        }
    }
    return std::nullopt;
}

std::span<const std::string_view> gamepad_type_names() {
    return kGamepadTypes;
}

std::string_view input_kind_name(InputKind kind) {
    switch (kind) {
    case InputKind::Mouse:
        return "mouse";
    case InputKind::Touch:
        return "touch";
    case InputKind::Gamepad:
        return "gamepad";
    case InputKind::Keyboard:
        break;
    }
    return "keyboard";
}

ButtonLabel joy_button_label(GamepadType type, JoyButton button) {
    const Family family = family_of(type);
    const std::string_view prefix = family_name(family);
    // The face buttons by where they sit: Nintendo's swap A and B, and X and Y, against Xbox's.
    switch (button) {
    case JoyButton::A:
        return labelled(prefix, pick({"South", "A", "Cross", "B"}, family));
    case JoyButton::B:
        return labelled(prefix, pick({"East", "B", "Circle", "A"}, family));
    case JoyButton::X:
        return labelled(prefix, pick({"West", "X", "Square", "Y"}, family));
    case JoyButton::Y:
        return labelled(prefix, pick({"North", "Y", "Triangle", "X"}, family));
    case JoyButton::Back: {
        std::string_view label = pick({"Back", "View", "Share", "Minus"}, family);
        if (type == GamepadType::Xbox360) {
            label = "Back";
        } else if (type == GamepadType::Ps3) {
            label = "Select";
        } else if (type == GamepadType::Ps5) {
            label = "Create";
        }
        return labelled(prefix, label);
    }
    case JoyButton::Guide:
        return labelled(prefix, pick({"Guide", "Xbox", "PS", "Home"}, family), "guide");
    case JoyButton::Start: {
        std::string_view label = pick({"Start", "Menu", "Options", "Plus"}, family);
        if (type == GamepadType::Xbox360 || type == GamepadType::Ps3) {
            label = "Start";
        }
        return labelled(prefix, label);
    }
    case JoyButton::LeftStick:
        return labelled(prefix, pick({"Left Stick Press", "LS", "L3", "Left Stick Press"}, family));
    case JoyButton::RightStick:
        return labelled(prefix,
                        pick({"Right Stick Press", "RS", "R3", "Right Stick Press"}, family));
    case JoyButton::LeftShoulder:
        return labelled(prefix, pick({"Left Shoulder", "LB", "L1", "L"}, family));
    case JoyButton::RightShoulder:
        return labelled(prefix, pick({"Right Shoulder", "RB", "R1", "R"}, family));
    case JoyButton::DpadUp:
        return labelled(prefix, "D-pad Up");
    case JoyButton::DpadDown:
        return labelled(prefix, "D-pad Down");
    case JoyButton::DpadLeft:
        return labelled(prefix, "D-pad Left");
    case JoyButton::DpadRight:
        return labelled(prefix, "D-pad Right");
    case JoyButton::Misc1: {
        std::string_view label = pick({"Misc", "Share", "Misc", "Capture"}, family);
        if (type == GamepadType::Ps5) {
            label = "Mic";
        }
        return labelled(prefix, label);
    }
    case JoyButton::Paddle1:
        return labelled(prefix, "P1");
    case JoyButton::Paddle2:
        return labelled(prefix, "P2");
    case JoyButton::Paddle3:
        return labelled(prefix, "P3");
    case JoyButton::Paddle4:
        return labelled(prefix, "P4");
    case JoyButton::Touchpad:
        return labelled(prefix, "Touchpad");
    }
    return labelled(prefix, joy_button_name(button));
}

ButtonLabel joy_axis_label(GamepadType type, JoyAxis axis, float direction) {
    const Family family = family_of(type);
    const std::string_view prefix = family_name(family);
    const bool negative = direction < 0.0F;
    switch (axis) {
    case JoyAxis::LeftX:
        return labelled(prefix, negative ? "Left Stick Left" : "Left Stick Right");
    case JoyAxis::LeftY:
        return labelled(prefix, negative ? "Left Stick Up" : "Left Stick Down");
    case JoyAxis::RightX:
        return labelled(prefix, negative ? "Right Stick Left" : "Right Stick Right");
    case JoyAxis::RightY:
        return labelled(prefix, negative ? "Right Stick Up" : "Right Stick Down");
    case JoyAxis::TriggerLeft:
        return labelled(prefix, pick({"Left Trigger", "LT", "L2", "ZL"}, family));
    case JoyAxis::TriggerRight:
        return labelled(prefix, pick({"Right Trigger", "RT", "R2", "ZR"}, family));
    }
    return labelled(prefix, joy_axis_name(axis));
}

ButtonLabel binding_label(const Event& binding, GamepadType type) {
    switch (binding.type) {
    case EventType::Key: {
        const Key key = binding.key != Key::None ? binding.key : binding.physical_key;
        const std::string_view name = key_name(key);
        std::string label;
        if (binding.modifiers.ctrl) {
            label += "Ctrl+";
        }
        if (binding.modifiers.alt) {
            label += "Alt+";
        }
        if (binding.modifiers.shift) {
            label += "Shift+";
        }
        if (binding.modifiers.meta) {
            label += "Meta+";
        }
        label += name;
        return {.label = std::move(label), .glyph = "key_" + snake(name)};
    }
    case EventType::MouseButton: {
        static constexpr std::array<std::string_view, 10> kLabels{
            "",           "Left Click", "Right Click", "Middle Click", "Wheel Up",
            "Wheel Down", "Wheel Left", "Wheel Right", "Mouse 4",      "Mouse 5",
        };
        return {.label = std::string(kLabels.at(static_cast<std::size_t>(binding.button))),
                .glyph = "mouse_" + std::string(mouse_button_name(binding.button))};
    }
    case EventType::JoypadButton:
        return joy_button_label(type, binding.joy_button);
    case EventType::JoypadMotion:
        return joy_axis_label(type, binding.axis, binding.axis_value);
    case EventType::Action:
        return {.label = binding.action, .glyph = {}};
    case EventType::MouseMotion:
    case EventType::ScreenTouch:
    case EventType::ScreenDrag:
    case EventType::Ime:
    case EventType::JoypadConnection:
    case EventType::JoypadSensor:
        break;
    }
    return {};
}

std::optional<Prompt> action_prompt(const Map& map, std::string_view action, InputKind kind,
                                    GamepadType type) {
    const Action* found = map.find(action);
    if (found == nullptr) {
        return std::nullopt;
    }
    const auto first = [&](EventType a, EventType b) -> const Event* {
        for (const EventType wanted : {a, b}) {
            for (const Event& binding : found->events) {
                if (binding.type == wanted) {
                    return &binding;
                }
            }
        }
        return nullptr;
    };
    const Event* binding = kind == InputKind::Gamepad
                               ? first(EventType::JoypadButton, EventType::JoypadMotion)
                               : first(EventType::Key, EventType::MouseButton);
    if (binding == nullptr) {
        return std::nullopt;
    }
    return Prompt{.binding = *binding, .label = binding_label(*binding, type)};
}

} // namespace slash::input
