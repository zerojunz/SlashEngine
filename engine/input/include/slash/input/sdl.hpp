#pragma once

#include "slash/input/input.hpp"

#include <cstdint>
#include <map>
#include <vector>

union SDL_Event;
struct SDL_Gamepad;
struct SDL_Window;

namespace slash::input {

// Turns SDL's events into input events and queues them in a world, as Godot's display servers
// do with the OS's: keys by SDL keycode and scancode, the mouse and fingers in window
// coordinates (the space sprites and controls are drawn in), and gamepads, opened as they
// connect and numbered from 0. SDL's own mouse events made from touches are skipped, since the
// input state emulates the mouse from touches itself. A window losing focus releases everything.
// Typed text, from the keyboard, an input method or an on-screen keyboard, comes as key presses
// with a character and no key, and an input method's composition as Ime events.
class SdlReader {
public:
    SdlReader() = default;
    SdlReader(const SdlReader&) = delete;
    SdlReader& operator=(const SdlReader&) = delete;
    SdlReader(SdlReader&&) = delete;
    SdlReader& operator=(SdlReader&&) = delete;
    // Closes the gamepads it opened; destroy it before SDL quits.
    ~SdlReader();

    void read(const SDL_Event& event, world::World& world);

    // Turns SDL's text input on or off for a window, with the on-screen keyboard and the input
    // method's position a text field asks for. Changes only what differs from the last call.
    void set_text_input(SDL_Window* window, const TextInput& request);

    // The gamepads connected, by device number.
    [[nodiscard]] std::size_t gamepads() const noexcept { return gamepads_.size(); }

private:
    int finger_index(std::uint64_t finger, bool add);

    std::map<std::uint32_t, int> devices_; // SDL joystick ID to device number
    std::map<int, SDL_Gamepad*> gamepads_; // device number to the open gamepad
    std::map<std::uint64_t, int> fingers_; // SDL finger ID to finger index
    TextInput text_input_;                 // the last request applied
};

} // namespace slash::input
