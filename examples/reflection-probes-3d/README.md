# Reflection probes 3D

A chrome ball on a polished floor reflecting the room around it through a `ReflectionProbe`, as
Godot 4.6's Forward+ renderer, the default `rendering.rendering_method`.
[main.scene.json](main.scene.json) is a room seen through a `Camera3D` in a 320x180 window: green,
blue, cream and orange walls, a metallic floor with a roughness of 0.1 and a ball with a roughness
of 0.05, lit by grey ambient light and a lamp, over a nearly black background.

The `probe` entity's box is the room's size plus a little, so the floor and walls are inside it,
and `blend_distance` 0.25 fades it in over the last quarter meter rather than the default meter.
`interior` draws its cube with no sky, fog or ambient light, as for a room.
[project.json](project.json) sets `rendering.reflections.reflection_atlas.reflection_size` to 128
texels across each face of the cube, down from 256, which is enough for a small window.

[calls.jsonl](calls.jsonl) drives it the way an agent does over MCP or JSON-RPC, and
[results.jsonl](results.jsonl) holds the replies:

```text
frame.capture {"points":[[160,88],[160,150],[100,100],[250,110]],"path":"shots/room.png"}
{"colors":[[0.608,0.275,0.176,1],[0.392,0.392,0.4,1],[0.259,0.286,0.42,1],[0.271,0.282,0.353,1]],
 "drawn":{"meshes":7},"height":180,"path":"shots/room.png","width":320}
```

- `frame.capture` draws the frame on the CPU, the probe's cube included, so it needs no GPU and
  gives the same pixels on every machine. `points` reads the middle of the ball, the open floor
  near the camera, the floor left of the ball and the floor by the right wall. The ball shows the
  orange wall behind the camera, which is nowhere on screen, and the floor left of the ball the
  blue back wall. `path` saves the frame as `shots/room.png`.
- `component.set` turns on `box_projection`, which aims each reflection at the box's walls instead
  of at a room infinitely far away. The floor by the right wall now reflects that wall's cream
  color where before it showed the blue of the wall across the room.
- A rougher floor, at 0.4, reads a blurrier level of the cube, and the lamp's highlight spreads
  across the open floor.
- `batch` makes the floor polished again and hides the probe, in one call. Without it the ball and
  the floor reflect only the dark background, and the ball's middle is black.

The cube is drawn once, when the probe first shows, and again only when it moves or its box,
offset, distance, layers or shadows change, as Godot's `"once"` update mode; a scene whose lights
or meshes move would set `update_mode` to `"always"`. Godot's Compatibility renderer
(`"rendering_method": "gl_compatibility"`, and always with the OpenGL drivers) draws no probes
here yet.
