# XR tracking

A player in virtual reality, as Godot's XR nodes place one. [main.scene.json](main.scene.json)
is an `XROrigin3D` (the play space) with an `XRCamera3D` head and two `XRController3D` hands,
the right one holding a sword. [project.json](project.json) enables XR on a `local_floor`
reference space, so a session starts the test headset: a head and two hand controllers that
agents move and press with `xr.set`. [calls.jsonl](calls.jsonl) plays it over JSON-RPC (MCP
tools `xr_state`, `xr_set`, `events_read`, `step`), and [results.jsonl](results.jsonl) holds the
replies.

```text
xr.set {"trackers": {"right_hand": {"position": [0.3, 1.9, -0.4], "rotation": [1.2, 0, 0],
        "inputs": {"trigger": 1.0, "trigger_click": true}}}}
step
events.read
{"events":[{"data":{"name":"trigger","value":1},"id":"right","kind":"input_float_changed",...},
           {"data":{"name":"trigger_click"},"id":"right","kind":"button_pressed",...}],...}
```

- `xr.state` lists the test headset (the primary interface), its trackers with each pose and
  input, and the XR nodes following them.
- Raising the right hand moves the `right` entity (and the sword under it) under the origin at
  the next tick, and its trigger arrives as `input_float_changed` and `button_pressed` events.
- Losing the hand's tracking leaves it where it was and hides it, since it is
  `show_when_tracked`.
- `center_on_hmd` makes where the head stands the play space's center, so the head is back over
  the origin facing forward, at its height.

[XR](../../docs/reference/scene-format.md#xr) in the scene format reference lists the
components, and [commands](../../docs/reference/commands.md#xrstate) the commands.
