# Gamepad feedback

Rumble, motion aiming and button prompts that match the controller: [main.cpp](main.cpp) runs a
game system that rumbles the gamepad that fired, turns the camera by its gyroscope while aiming,
and shows the jump prompt for whatever the player last used, then plays it as an agent does with
`input.inject`, `input.state` and `input.actions`.

```text
hint: Press Space to jump (key_space); yaw 0.000
...
hint: Press Cross to jump (playstation_cross); yaw 0.000
...
input.state {"actions":["fire"]}
{"actions":{"fire":{"just_pressed":true,"pressed":true}},"joypads":[{..."gamepad":"ps5",...,"rumble":{"high":0.5,"left_trigger":0.0,"low":0.75,"right_trigger":1.0,"seconds":0.2}}],...}
...
hint: Press Cross to jump (playstation_cross); yaw 0.500
```

- A `joypad_connection` event says a gamepad connected, with its name, kind (`ps5`, `xbox_one`,
  `switch_pro`, ...) and whether it rumbles and has motion sensors; `SdlReader` sends one for
  each real controller, and an agent injects one to test with a controller it doesn't have.
  Disconnecting lets go of what the gamepad held.
- `input::rumble(world, device, {low, high, left_trigger, right_trigger, seconds})` starts a
  rumble; it runs out by itself, `input.state` shows it with the seconds left, and in a window
  `SdlReader::update` plays it on the controller.
- `input::gamepad_gyro` and `gamepad_accelerometer` read the motion sensors (`joypad_sensor`
  events), in radians a second and m/s^2.
- `input::action_prompt(world, action)` picks the binding to show for what the player last used
  (`input::last_input`) and labels it for that controller: Cross on a PlayStation gamepad, A on
  an Xbox one, B on a Nintendo one, with a glyph name the game maps to its own images.
  `input.actions` with `prompt` gives the same to an agent.

See [input.md](../../docs/reference/input.md#gamepads) and
[commands.md](../../docs/reference/commands.md).
