#pragma once

#include "slash/input/input.hpp"

#include <cstdint>
#include <map>
#include <vector>

union SDL_Event;
struct SDL_Gamepad;

namespace slash::input {

// Turns SDL's events into input events and queues them in a world, as Godot's display servers
// do with the OS's: keys by SDL keycode and scancode, the mouse and fingers in window
// coordinates (the space sprites and controls are drawn in), and gamepads, opened as they
// connect and numbered from 0. SDL's own mouse events made from touches are skipped, since the
// input state emulates the mouse from touches itself. A window losing focus releases everything.
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

    // The gamepads connected, by device number.
    [[nodiscard]] std::size_t gamepads() const noexcept { return gamepads_.size(); }

private:
    int finger_index(std::uint64_t finger, bool add);

    std::map<std::uint32_t, int> devices_; // SDL joystick ID to device number
    std::map<int, SDL_Gamepad*> gamepads_; // device number to the open gamepad
    std::map<std::uint64_t, int> fingers_; // SDL finger ID to finger index
};

} // namespace slash::input
