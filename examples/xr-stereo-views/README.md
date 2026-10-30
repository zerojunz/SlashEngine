# XR stereo views

What each eye of a headset sees, as Godot draws an `XRCamera3D`'s views.
[main.scene.json](main.scene.json) is an `XROrigin3D` with an `XRCamera3D` head, a thin red post
40 cm in front of it and a wide green post 8 m away. [project.json](project.json) enables XR, so
a session starts the test headset, whose eyes are 0.064 m apart and see 50 degrees out, 40 in
and 45 up and down, as headsets' canted views do. [calls.jsonl](calls.jsonl) captures each eye
over JSON-RPC (MCP tools `xr_set`, `frame_capture`), and [results.jsonl](results.jsonl) holds the
replies.

```text
xr.set {"render_target_size": [64, 32]}
frame.capture {"xr_view": 0, "points": [[24.5, 16.5], [26.5, 16.5], [37.5, 16.5], [39.5, 16.5]]}
{"colors":[[0.102,0.102,0.2,1],[0.102,0.102,0.2,1],[0.196,0.8,0.196,1],[0.898,0.196,0.09,1]],...}
```

- `xr.state` gives the headset's `view_count` (2) and `render_target_size`.
- `frame.capture`'s `xr_view` draws one eye at the render target size. Each eye sees straight
  ahead right of its image's center for the left eye and left of it for the right, since each
  sees further out than in.
- The near post stands right of the far one for the left eye and left of it for the right: the
  eyes see it from places 0.064 m apart.
- With `ipd` 0 both eyes stand at the head, and the near post hides the middle of the far one.
- A view the headset doesn't have is an error at `/xr_view`.

[XR](../../docs/reference/scene-format.md#xr) in the scene format reference explains the views,
and [commands](../../docs/reference/commands.md#framecapture) `frame.capture`.
