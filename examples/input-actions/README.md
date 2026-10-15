# Input actions

Keys, mouse buttons, touches and gamepads, read through named actions as Godot's input map.
[project.json](project.json) binds `move_left`, `move_right`, `move_up` and `move_down` to WASD
(by physical key, so they sit in the same place on any keyboard layout), the arrow keys and the
left stick, `jump` to Space and the A button, and `move_to` to the left mouse button. Godot's
`ui_*` actions (`ui_accept`, `ui_cancel`, `ui_left` and the rest) are built in.
[main.cpp](main.cpp) adds a `player` system that moves by `get_vector`, counts jumps with
`is_action_just_pressed` and remembers where `move_to` clicked, then plays it the way an agent
does over MCP (tools `input_inject`, `input_state`, `input_actions`) or JSON-RPC. `expected.txt`
holds each call and its reply.

- `input.inject` queues events for the next tick and replies with the tick they apply at. A
  press and a release in one call is a tap: that tick the action is both `just_pressed` and
  `just_released`, so gameplay code never misses it.
- Half a stick past the action's `deadzone` (0.25 here) reads as `strength` 0.5, and
  `get_vector` moves the player at half speed.
- A touch clicks the left mouse button too (`input_devices.pointing.emulate_mouse_from_touch`,
  on by default), so mouse code works on phones.
- `{"type": "action", "action": "move_right"}` presses an action by name, with an optional
  `strength`, as Godot's `Input.action_press`. A misspelled action is an error with the closest
  name, and nothing is queued.
- Gameplay code rebinds actions with `input::map(world)`, e.g. from an options menu.
- `input.state` with no params lists only actions that are pressed or just changed; name
  actions to see them either way.
- In a game, the same events come from SDL: keys, the mouse, fingers and gamepads, which
  connect and disconnect as they are plugged in. `input::events(world)` lists a tick's events
  for code that reacts to events rather than actions.
