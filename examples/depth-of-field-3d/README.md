# Depth of field 3D

A camera focusing, as Godot 4.6's depth of field in its Forward+ renderer, the default
`rendering.rendering_method`. [main.scene.json](main.scene.json) is a garden seen through a
`Camera3D` in a 320x180 window: a yellow lantern 1.4 meters away, a red statue at 4 and a white
fence with dark posts at 15.

The camera's `CameraAttributesPractical` focuses on the statue, as Godot's camera `attributes` do:

- `dof_blur_far_enabled` blurs what is past `dof_blur_far_distance` 8, growing over
  `dof_blur_far_transition` 4 meters to the full blur, so the fence is fully blurred.
- `dof_blur_near_enabled` blurs what is nearer than `dof_blur_near_distance` 3, growing over
  `dof_blur_near_transition` 1.5 meters, so the lantern is fully blurred too.
- `dof_blur_amount` 0.15 makes the full blur 0.15 times 64 pixels, about 10.

[project.json](project.json) sets `rendering.camera.depth_of_field`, as Godot's project settings:
the hexagon bokeh shape (the default) at `medium` quality, which blurs at full size.

[calls.jsonl](calls.jsonl) drives it the way an agent does over MCP or JSON-RPC, and
[results.jsonl](results.jsonl) holds the replies:

```text
frame.capture {"points":[[118,85],[50,123],[157,102]],"path":"shots/statue.png"}
{"colors":[[0.647,0.667,0.678,1],[0.537,0.553,0.235,1],[0.675,0.125,0.09,1]],
 "drawn":{"meshes":11},"height":180,"path":"shots/statue.png","width":320}
```

- `frame.capture` draws the frame on the CPU, depth of field included, so it needs no GPU and gives
  the same pixels on every machine. `points` reads a fence post, the grass just left of the
  lantern and the statue's left edge. The post is light gray, blurred into the fence; the
  lantern's blur spreads yellow over the grass beside it; the statue's edge is sharp red.
  `path` saves the frame as `shots/statue.png`.
- `component.set` racks focus to the fence: far blur off, and near blur for everything nearer than
  12 meters, full at 6. Now the post is dark and sharp, and the statue's edge is mixed with the
  grass and fence around it. The lantern stays as blurred as it was.
- `dof_blur_amount` 0.4 blurs 26 pixels: the lantern spreads further over the grass and the
  statue's edge fades more.
- Turning near blur off too leaves nothing to blur, so the frame is as sharp as one without
  depth of field: grass beside the lantern and red at the statue's edge.

As in Godot, what is blurred far behind something sharp takes a trace of its light at its edges,
and near blur spreads over what is in focus behind it. Godot's Compatibility renderer
(`"rendering_method": "gl_compatibility"`, and always with the OpenGL drivers) has no depth of
field.
