# Decals 3D

A street painted with decals, as Godot 4.6's `Decal` in its Forward+ renderer, the default
`rendering.rendering_method`. [main.scene.json](main.scene.json) is a street and a brick wall seen
through a `Camera3D` in a 320x180 window, lit by the sun and grey ambient light, with three
`Decal`s. Each is a box that projects its textures down its -Y onto the meshes inside it:

- `paint` is a dashed yellow line down the street, from an albedo texture whose gaps are clear.
- `puddle` darkens a round patch of the street with an albedo texture (its `albedo_mix` 0.7) and
  smooths it with an ORM texture, whose green, the roughness, is low.
- `sign` glows on the wall from an emission texture alone, turned a quarter turn about x so that
  its -Y points into the wall.

Games ship PNG files made in an image editor; [main.cpp](main.cpp) draws the textures in code, so
the repository holds no images, then drives the scene the way an agent does over MCP or JSON-RPC.
[expected.txt](expected.txt) holds what it prints:

```text
frame.capture {"path":"shots/street.png","points":[[96,120],[130,120],[214,128],[137,18],[60,40]]}
{"colors":[[1,0.859,0.196,1],[0.392,0.392,0.42,1],[0.227,0.235,0.267,1],[0.612,1,1,1],
 [0.58,0.345,0.282,1]],"drawn":{"meshes":2},"height":180,"path":"shots/street.png","width":320}
```

- `frame.capture` draws the frame on the CPU, decals included, so it needs no GPU and gives the
  same pixels on every machine. `points` reads a dash of the line, the bare street beside it, the
  puddle, a bar of the sign and the bare wall. `path` saves the frame as `shots/street.png`.
- One `batch` fades the line to 40% with its `modulate`'s alpha, sets the puddle's `cull_mask` to
  layer 2, which the street (on layer 1) isn't on, and triples the sign's `emission_energy`. The
  next frame shows the line between yellow and grey, the street bare where the puddle was, and the
  sign brighter.

The textures of a frame's decals are packed into one atlas with mipmaps, as Godot's, sampled by
project.json's `rendering.textures.decals.filter` (`linear_mipmaps` by default). Godot's
Compatibility renderer (`"rendering_method": "gl_compatibility"`, and always with the OpenGL
drivers) draws no decals.
