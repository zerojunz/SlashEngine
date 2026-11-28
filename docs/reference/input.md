# Input

Input events, action maps and input state.

Status: current · Updated: 2026-11-26

Parts of this area are ported from [Godot Engine](https://godotengine.org) (MIT); see [THIRD_PARTY.md](../../THIRD_PARTY.md).

`engine/input` is input. Events (`input::Event`: key, mouse button and motion, screen touch and
drag, gamepad button and motion, gamepad connection and sensors, action) queue with `input::push` and apply when the `input` system
runs, first in each tick (priority -1000), so runs repeat. Gameplay code asks about actions:
`is_action_pressed`, `is_action_just_pressed` (the tick it changed; a press and release in one tick
count as both), `get_action_strength`, `get_axis` and `get_vector`, and `input::map(world)` rebinds
them. `project.json`'s `input` adds actions over the built-in `ui_*` ones (`Map::with_defaults`) and
`pointer` sets touch and mouse emulation. `src/event.cpp` matches events to actions; `src/state.cpp`
holds the state. `input::SdlReader` (`src/sdl.cpp`) turns SDL's events into input events: games call
it from `Game::handle_event` and the platform's event handler, and so does `slash-cli run`. Agents
send events with `input.inject` and read `input.state` and `input.actions`; sessions put input back
with the world when a command fails or is a dry run. `examples/input-actions` moves a player with
keys, a stick and taps.

## Gamepads

`input::SdlReader` opens each gamepad as it connects, numbered from 0, and sends a
`joypad_connection` event with its name, kind (`input::GamepadType`: `standard`, `xbox_360`,
`xbox_one`, `ps3`, `ps4`, `ps5`, `switch_pro`, `joycon_left`, `joycon_right`, `joycon_pair`) and
whether it rumbles and has motion sensors, which it turns on. Its gyroscope and accelerometer
come as `joypad_sensor` events, merged per tick; a disconnection lets go of the gamepad's
buttons, axes and the actions they held. `input::gamepads(world)` and `gamepad(world, device)`
list them, and `gamepad_gyro` and `gamepad_accelerometer` read the sensors (radians a second and
m/s^2; x right, y up, z toward the player). Agents inject the same events to test with
controllers they don't have.

Game code starts a rumble with `input::rumble(world, device, {low, high, left_trigger,
right_trigger, seconds})` (`stop_rumble` stops it). It runs out by itself, `rumble_of` and
`input.state` show it with the seconds left, and `SdlReader::update`, called after each tick by
`runtime::Game` and `slash-cli run`, plays each change on the controller (`SDL_RumbleGamepad`,
and the trigger motors where there are some), so headless runs record what a player would feel.

Button prompts follow what the player last used (`input::last_input`: a key, mouse or touch
press, or a gamepad button or a stick or trigger past halfway). `input::action_prompt(world,
action)` picks the action's binding for it (a key, then a mouse button; or a gamepad button,
then an axis) and labels it for that controller (`ButtonLabel`: `label` and `glyph`). The bottom
face button is A on Xbox gamepads, Cross on PlayStation ones and B on Nintendo ones, where the
right one is A; `joy_button_label`, `joy_axis_label` and `binding_label` give any binding's
label. Glyphs name the picture to draw, `<family>_<label>` (`playstation_cross`, `xbox_lb`,
`nintendo_zr`, `key_space`, `mouse_left`), and the game maps them to its own images.
`input.actions` with `prompt` returns them for an agent. `src/gamepad.cpp` holds the labels;
`tests/test_gamepad.cpp` drives a virtual SDL gamepad; `examples/gamepad-feedback` rumbles, aims
by gyro and switches its prompts.
