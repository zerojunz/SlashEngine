#include "slash/input/sdl.hpp"

#include "slash/core/log.hpp"
#include "slash/core/text.hpp"
#include "slash/world/world.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

namespace slash::input {
namespace {

Key key_from_keycode(SDL_Keycode code) {
    if (code >= SDLK_A && code <= SDLK_Z) {
        return static_cast<Key>(static_cast<unsigned>(Key::A) + (code - SDLK_A));
    }
    if (code >= SDLK_0 && code <= SDLK_9) {
        return static_cast<Key>(static_cast<unsigned>(Key::Key0) + (code - SDLK_0));
    }
    if (code >= SDLK_F1 && code <= SDLK_F12) {
        return static_cast<Key>(static_cast<unsigned>(Key::F1) + (code - SDLK_F1));
    }
    if (code >= SDLK_F13 && code <= SDLK_F24) {
        return static_cast<Key>(static_cast<unsigned>(Key::F13) + (code - SDLK_F13));
    }
    if (code >= SDLK_KP_1 && code <= SDLK_KP_9) {
        return static_cast<Key>(static_cast<unsigned>(Key::Kp1) + (code - SDLK_KP_1));
    }
    switch (code) {
    case SDLK_RETURN:
    case SDLK_RETURN2:
        return Key::Enter;
    case SDLK_ESCAPE:
        return Key::Escape;
    case SDLK_BACKSPACE:
        return Key::Backspace;
    case SDLK_TAB:
        return Key::Tab;
    case SDLK_SPACE:
        return Key::Space;
    case SDLK_EXCLAIM:
        return Key::Exclam;
    case SDLK_DBLAPOSTROPHE:
        return Key::QuoteDbl;
    case SDLK_HASH:
        return Key::NumberSign;
    case SDLK_DOLLAR:
        return Key::Dollar;
    case SDLK_PERCENT:
        return Key::Percent;
    case SDLK_AMPERSAND:
        return Key::Ampersand;
    case SDLK_APOSTROPHE:
        return Key::Apostrophe;
    case SDLK_LEFTPAREN:
        return Key::ParenLeft;
    case SDLK_RIGHTPAREN:
        return Key::ParenRight;
    case SDLK_ASTERISK:
        return Key::Asterisk;
    case SDLK_PLUS:
        return Key::Plus;
    case SDLK_COMMA:
        return Key::Comma;
    case SDLK_MINUS:
        return Key::Minus;
    case SDLK_PERIOD:
        return Key::Period;
    case SDLK_SLASH:
        return Key::Slash;
    case SDLK_COLON:
        return Key::Colon;
    case SDLK_SEMICOLON:
        return Key::Semicolon;
    case SDLK_LESS:
        return Key::Less;
    case SDLK_EQUALS:
        return Key::Equal;
    case SDLK_GREATER:
        return Key::Greater;
    case SDLK_QUESTION:
        return Key::Question;
    case SDLK_AT:
        return Key::At;
    case SDLK_LEFTBRACKET:
        return Key::BracketLeft;
    case SDLK_BACKSLASH:
        return Key::BackSlash;
    case SDLK_RIGHTBRACKET:
        return Key::BracketRight;
    case SDLK_CARET:
        return Key::AsciiCircum;
    case SDLK_UNDERSCORE:
        return Key::UnderScore;
    case SDLK_GRAVE:
        return Key::QuoteLeft;
    case SDLK_LEFTBRACE:
        return Key::BraceLeft;
    case SDLK_PIPE:
        return Key::Bar;
    case SDLK_RIGHTBRACE:
        return Key::BraceRight;
    case SDLK_TILDE:
        return Key::AsciiTilde;
    case SDLK_DELETE:
        return Key::Delete;
    case SDLK_CAPSLOCK:
        return Key::CapsLock;
    case SDLK_PRINTSCREEN:
        return Key::Print;
    case SDLK_SCROLLLOCK:
        return Key::ScrollLock;
    case SDLK_PAUSE:
        return Key::Pause;
    case SDLK_INSERT:
        return Key::Insert;
    case SDLK_HOME:
        return Key::Home;
    case SDLK_PAGEUP:
        return Key::PageUp;
    case SDLK_END:
        return Key::End;
    case SDLK_PAGEDOWN:
        return Key::PageDown;
    case SDLK_RIGHT:
        return Key::Right;
    case SDLK_LEFT:
        return Key::Left;
    case SDLK_DOWN:
        return Key::Down;
    case SDLK_UP:
        return Key::Up;
    case SDLK_NUMLOCKCLEAR:
        return Key::NumLock;
    case SDLK_KP_DIVIDE:
        return Key::KpDivide;
    case SDLK_KP_MULTIPLY:
        return Key::KpMultiply;
    case SDLK_KP_MINUS:
        return Key::KpSubtract;
    case SDLK_KP_PLUS:
        return Key::KpAdd;
    case SDLK_KP_ENTER:
        return Key::KpEnter;
    case SDLK_KP_0:
        return Key::Kp0;
    case SDLK_KP_PERIOD:
        return Key::KpPeriod;
    case SDLK_APPLICATION:
    case SDLK_MENU:
        return Key::Menu;
    case SDLK_HELP:
        return Key::Help;
    case SDLK_STOP:
    case SDLK_AC_STOP:
        return Key::Stop;
    case SDLK_SYSREQ:
        return Key::SysReq;
    case SDLK_CLEAR:
        return Key::Clear;
    case SDLK_LCTRL:
    case SDLK_RCTRL:
        return Key::Ctrl;
    case SDLK_LSHIFT:
    case SDLK_RSHIFT:
        return Key::Shift;
    case SDLK_LALT:
    case SDLK_RALT:
        return Key::Alt;
    case SDLK_LGUI:
    case SDLK_RGUI:
        return Key::Meta;
    case SDLK_MUTE:
        return Key::VolumeMute;
    case SDLK_VOLUMEUP:
        return Key::VolumeUp;
    case SDLK_VOLUMEDOWN:
        return Key::VolumeDown;
    case SDLK_MEDIA_PLAY:
    case SDLK_MEDIA_PLAY_PAUSE:
        return Key::MediaPlay;
    case SDLK_MEDIA_STOP:
        return Key::MediaStop;
    case SDLK_MEDIA_PREVIOUS_TRACK:
        return Key::MediaPrevious;
    case SDLK_MEDIA_NEXT_TRACK:
        return Key::MediaNext;
    case SDLK_MEDIA_RECORD:
        return Key::MediaRecord;
    case SDLK_AC_SEARCH:
        return Key::Search;
    case SDLK_AC_BACK:
        return Key::Back;
    case SDLK_AC_FORWARD:
        return Key::Forward;
    case SDLK_AC_REFRESH:
        return Key::Refresh;
    default:
        return Key::None;
    }
}

// Where a key sits on a US QWERTY keyboard, whatever the layout says.
Key key_from_scancode(SDL_Scancode code) {
    if (code >= SDL_SCANCODE_A && code <= SDL_SCANCODE_Z) {
        return static_cast<Key>(static_cast<unsigned>(Key::A) +
                                static_cast<unsigned>(code - SDL_SCANCODE_A));
    }
    if (code >= SDL_SCANCODE_1 && code <= SDL_SCANCODE_9) {
        return static_cast<Key>(static_cast<unsigned>(Key::Key1) +
                                static_cast<unsigned>(code - SDL_SCANCODE_1));
    }
    switch (code) {
    case SDL_SCANCODE_0:
        return Key::Key0;
    case SDL_SCANCODE_MINUS:
        return Key::Minus;
    case SDL_SCANCODE_EQUALS:
        return Key::Equal;
    case SDL_SCANCODE_LEFTBRACKET:
        return Key::BracketLeft;
    case SDL_SCANCODE_RIGHTBRACKET:
        return Key::BracketRight;
    case SDL_SCANCODE_BACKSLASH:
    case SDL_SCANCODE_NONUSHASH:
        return Key::BackSlash;
    case SDL_SCANCODE_SEMICOLON:
        return Key::Semicolon;
    case SDL_SCANCODE_APOSTROPHE:
        return Key::Apostrophe;
    case SDL_SCANCODE_GRAVE:
        return Key::QuoteLeft;
    case SDL_SCANCODE_COMMA:
        return Key::Comma;
    case SDL_SCANCODE_PERIOD:
        return Key::Period;
    case SDL_SCANCODE_SLASH:
        return Key::Slash;
    case SDL_SCANCODE_KP_0:
        return Key::Kp0;
    default:
        break;
    }
    if (code >= SDL_SCANCODE_KP_1 && code <= SDL_SCANCODE_KP_9) {
        return static_cast<Key>(static_cast<unsigned>(Key::Kp1) +
                                static_cast<unsigned>(code - SDL_SCANCODE_KP_1));
    }
    // The rest are the same key whatever the layout.
    return key_from_keycode(SDL_GetKeyFromScancode(code, SDL_KMOD_NONE, false));
}

Modifiers modifiers_of(SDL_Keymod mod) {
    return {.shift = (mod & SDL_KMOD_SHIFT) != 0,
            .ctrl = (mod & SDL_KMOD_CTRL) != 0,
            .alt = (mod & SDL_KMOD_ALT) != 0,
            .meta = (mod & SDL_KMOD_GUI) != 0};
}

std::optional<MouseButton> mouse_button_of(Uint8 button) {
    switch (button) {
    case SDL_BUTTON_LEFT:
        return MouseButton::Left;
    case SDL_BUTTON_RIGHT:
        return MouseButton::Right;
    case SDL_BUTTON_MIDDLE:
        return MouseButton::Middle;
    case SDL_BUTTON_X1:
        return MouseButton::Xbutton1;
    case SDL_BUTTON_X2:
        return MouseButton::Xbutton2;
    default:
        return std::nullopt;
    }
}

// The lowest number not in use.
template <typename Map>
int lowest_free(const Map& used) {
    int index = 0;
    while (used.contains(index)) {
        ++index;
    }
    return index;
}

Vec2 window_size(SDL_WindowID id) {
    int width = 0;
    int height = 0;
    if (SDL_Window* window = SDL_GetWindowFromID(id); window != nullptr) {
        SDL_GetWindowSize(window, &width, &height);
    }
    return {static_cast<float>(width), static_cast<float>(height)};
}

void wheel(world::World& world, MouseButton button, float amount, Vec2 position) {
    Event event;
    event.type = EventType::MouseButton;
    event.button = button;
    event.factor = amount;
    event.position = position;
    event.modifiers = modifiers_of(SDL_GetModState());
    event.pressed = true;
    push(world, event);
    event.pressed = false;
    push(world, event);
}

} // namespace

SdlReader::~SdlReader() {
    for (const auto& [device, gamepad] : gamepads_) {
        SDL_CloseGamepad(gamepad);
    }
}

int SdlReader::finger_index(std::uint64_t finger, bool add) {
    if (const auto found = fingers_.find(finger); found != fingers_.end()) {
        return found->second;
    }
    if (!add) {
        return -1;
    }
    std::map<int, bool> used;
    for (const auto& [id, index] : fingers_) {
        used[index] = true;
    }
    const int index = lowest_free(used);
    fingers_[finger] = index;
    return index;
}

void SdlReader::read(const SDL_Event& event, world::World& world) {
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        Event key;
        key.type = EventType::Key;
        key.key = key_from_keycode(event.key.key);
        key.physical_key = key_from_scancode(event.key.scancode);
        if (key.key == Key::None && key.physical_key == Key::None) {
            return;
        }
        key.pressed = event.key.down;
        key.echo = event.key.repeat;
        key.modifiers = modifiers_of(event.key.mod);
        push(world, key);
        return;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        const auto button = mouse_button_of(event.button.button);
        if (!button || event.button.which == SDL_TOUCH_MOUSEID) {
            return;
        }
        Event click;
        click.type = EventType::MouseButton;
        click.button = *button;
        click.pressed = event.button.down;
        click.double_click = event.button.down && event.button.clicks == 2;
        click.position = {event.button.x, event.button.y};
        click.modifiers = modifiers_of(SDL_GetModState());
        push(world, click);
        return;
    }
    case SDL_EVENT_MOUSE_MOTION: {
        if (event.motion.which == SDL_TOUCH_MOUSEID) {
            return;
        }
        Event motion;
        motion.type = EventType::MouseMotion;
        motion.position = {event.motion.x, event.motion.y};
        motion.relative = Vec2{event.motion.xrel, event.motion.yrel};
        push(world, motion);
        return;
    }
    case SDL_EVENT_MOUSE_WHEEL: {
        if (event.wheel.which == SDL_TOUCH_MOUSEID) {
            return;
        }
        const float flip = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1.0F : 1.0F;
        const float x = event.wheel.x * flip;
        const float y = event.wheel.y * flip;
        const Vec2 position{event.wheel.mouse_x, event.wheel.mouse_y};
        if (y != 0.0F) {
            wheel(world, y > 0.0F ? MouseButton::WheelUp : MouseButton::WheelDown, std::abs(y),
                  position);
        }
        if (x != 0.0F) {
            wheel(world, x > 0.0F ? MouseButton::WheelRight : MouseButton::WheelLeft, std::abs(x),
                  position);
        }
        return;
    }
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_CANCELED:
    case SDL_EVENT_FINGER_MOTION: {
        if (event.tfinger.touchID == SDL_MOUSE_TOUCHID) {
            return;
        }
        const bool down = event.type == SDL_EVENT_FINGER_DOWN;
        const int index = finger_index(event.tfinger.fingerID, down);
        if (index < 0) {
            return;
        }
        const Vec2 size = window_size(event.tfinger.windowID);
        Event touch;
        touch.index = index;
        touch.position = {event.tfinger.x * size.x, event.tfinger.y * size.y};
        if (event.type == SDL_EVENT_FINGER_MOTION) {
            touch.type = EventType::ScreenDrag;
            touch.relative = Vec2{event.tfinger.dx * size.x, event.tfinger.dy * size.y};
        } else {
            touch.type = EventType::ScreenTouch;
            touch.pressed = down;
            if (!down) {
                fingers_.erase(event.tfinger.fingerID);
            }
        }
        push(world, touch);
        return;
    }
    case SDL_EVENT_GAMEPAD_ADDED: {
        if (devices_.contains(event.gdevice.which)) {
            return;
        }
        SDL_Gamepad* gamepad = SDL_OpenGamepad(event.gdevice.which);
        if (gamepad == nullptr) {
            log::warn("a gamepad did not open", {{"error", SDL_GetError()}});
            return;
        }
        const int device = lowest_free(gamepads_);
        devices_[event.gdevice.which] = device;
        gamepads_[device] = gamepad;
        const char* name = SDL_GetGamepadName(gamepad);
        log::info("gamepad connected",
                  {{"device", device}, {"name", name != nullptr ? name : "unknown"}});
        return;
    }
    case SDL_EVENT_GAMEPAD_REMOVED: {
        const auto found = devices_.find(event.gdevice.which);
        if (found == devices_.end()) {
            return;
        }
        const int device = found->second;
        SDL_CloseGamepad(gamepads_[device]);
        gamepads_.erase(device);
        devices_.erase(found);
        // Unplugged mid-press, it lets go of its buttons and sticks.
        Event release;
        release.device = device;
        release.type = EventType::JoypadButton;
        for (unsigned button = 0; button <= static_cast<unsigned>(JoyButton::Touchpad); ++button) {
            release.joy_button = static_cast<JoyButton>(button);
            push(world, release);
        }
        release.type = EventType::JoypadMotion;
        for (unsigned axis = 0; axis <= static_cast<unsigned>(JoyAxis::TriggerRight); ++axis) {
            release.axis = static_cast<JoyAxis>(axis);
            push(world, release);
        }
        log::info("gamepad disconnected", {{"device", device}});
        return;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        const auto found = devices_.find(event.gbutton.which);
        if (found == devices_.end() ||
            event.gbutton.button > static_cast<Uint8>(JoyButton::Touchpad)) {
            return;
        }
        Event button;
        button.type = EventType::JoypadButton;
        button.device = found->second;
        button.joy_button = static_cast<JoyButton>(event.gbutton.button);
        button.pressed = event.gbutton.down;
        push(world, button);
        return;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        const auto found = devices_.find(event.gaxis.which);
        if (found == devices_.end() ||
            event.gaxis.axis > static_cast<Uint8>(JoyAxis::TriggerRight)) {
            return;
        }
        Event motion;
        motion.type = EventType::JoypadMotion;
        motion.device = found->second;
        motion.axis = static_cast<JoyAxis>(event.gaxis.axis);
        motion.axis_value =
            std::clamp(static_cast<float>(event.gaxis.value) / 32767.0F, -1.0F, 1.0F);
        push(world, motion);
        return;
    }
    case SDL_EVENT_TEXT_INPUT:
        if (event.text.text != nullptr) {
            for (Event& typed : text_events(event.text.text)) {
                push(world, std::move(typed));
            }
        }
        return;
    case SDL_EVENT_TEXT_EDITING: {
        Event ime;
        ime.type = EventType::Ime;
        ime.text = event.edit.text != nullptr ? event.edit.text : "";
        const int length = static_cast<int>(to_utf32(ime.text).size());
        // SDL says -1 when the input method names no selection: the caret is at the end.
        ime.selection_start = event.edit.start >= 0 ? std::min(event.edit.start, length) : length;
        ime.selection_length =
            event.edit.length >= 0 ? std::min(event.edit.length, length - ime.selection_start) : 0;
        push(world, std::move(ime));
        return;
    }
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        release_pressed(world);
        return;
    default:
        return;
    }
}

void SdlReader::set_text_input(SDL_Window* window, const TextInput& request) {
    if (window == nullptr || request == text_input_) {
        return;
    }
    if (!request.active) {
        if (text_input_.active) {
            SDL_StopTextInput(window);
        }
        text_input_ = request;
        return;
    }
    const bool restart = !text_input_.active || request.keyboard != text_input_.keyboard ||
                         request.show_keyboard != text_input_.show_keyboard;
    if (restart) {
        if (text_input_.active) {
            SDL_StopTextInput(window);
        }
        // SDL shows the on-screen keyboard when text input starts, where there is no hardware
        // keyboard, unless this hint says not to.
        SDL_SetHint(SDL_HINT_ENABLE_SCREEN_KEYBOARD, request.show_keyboard ? "auto" : "0");
        const SDL_PropertiesID props = SDL_CreateProperties();
        SDL_TextInputType type = SDL_TEXTINPUT_TYPE_TEXT;
        // Android's InputType for the kinds SDL has no type for.
        constexpr std::int64_t kClassText = 0x1;
        constexpr std::int64_t kClassNumber = 0x2;
        constexpr std::int64_t kClassPhone = 0x3;
        constexpr std::int64_t kNumberDecimal = 0x2000;
        constexpr std::int64_t kTextUri = 0x10;
        std::int64_t android = 0;
        if (request.keyboard == "email_address") {
            type = SDL_TEXTINPUT_TYPE_TEXT_EMAIL;
        } else if (request.keyboard == "password") {
            type = SDL_TEXTINPUT_TYPE_TEXT_PASSWORD_HIDDEN;
        } else if (request.keyboard == "number") {
            type = SDL_TEXTINPUT_TYPE_NUMBER;
        } else if (request.keyboard == "number_decimal") {
            type = SDL_TEXTINPUT_TYPE_NUMBER;
            android = kClassNumber | kNumberDecimal;
        } else if (request.keyboard == "phone") {
            type = SDL_TEXTINPUT_TYPE_NUMBER;
            android = kClassPhone;
        } else if (request.keyboard == "url") {
            android = kClassText | kTextUri;
        }
        SDL_SetNumberProperty(props, SDL_PROP_TEXTINPUT_TYPE_NUMBER, type);
        SDL_SetBooleanProperty(props, SDL_PROP_TEXTINPUT_MULTILINE_BOOLEAN,
                               request.keyboard == "multiline");
        if (android != 0) {
            SDL_SetNumberProperty(props, SDL_PROP_TEXTINPUT_ANDROID_INPUTTYPE_NUMBER, android);
        }
        if (!SDL_StartTextInputWithProperties(window, props)) {
            log::warn("text input did not start", {{"error", SDL_GetError()}});
        }
        SDL_DestroyProperties(props);
    }
    const SDL_Rect area{static_cast<int>(std::lround(request.position.x)),
                        static_cast<int>(std::lround(request.position.y)),
                        static_cast<int>(std::lround(request.size.x)),
                        static_cast<int>(std::lround(request.size.y))};
    SDL_SetTextInputArea(window, &area, request.caret);
    text_input_ = request;
}

} // namespace slash::input
