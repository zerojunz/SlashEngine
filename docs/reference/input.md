# Input

Input events, action maps and input state.

Status: current · Updated: 2026-11-20

`engine/input` is input, as Godot's InputEvent, InputMap and Input. Events (`input::Event`: key,
mouse button and motion, screen touch and drag, gamepad button and motion, action) queue with
`input::push` and apply when the `input` system runs, first in each tick (priority -1000), so
runs repeat. Gameplay code asks about actions: `is_action_pressed`, `is_action_just_pressed`
(the tick it changed; a press and release in one tick count as both), `get_action_strength`,
`get_axis` and `get_vector`, as Godot's, and `input::map(world)` rebinds them. `project.json`'s
`input` adds actions over Godot's `ui_*` ones (`Map::with_defaults`) and `pointer` sets
touch and mouse emulation. Matching follows Godot's `action_match` (`src/event.cpp`); the state
follows `Input::_parse_input_event_impl` (`src/state.cpp`). `input::SdlReader` (`src/sdl.cpp`)
turns SDL's events into input events: games call it from `Game::handle_event` and the platform's
event handler, and so does `slash-cli run`. Agents send events with `input.inject` and read
`input.state` and `input.actions`; sessions put input back with the world when a command fails or
is a dry run. `examples/input-actions` moves a player with keys, a stick and taps.
