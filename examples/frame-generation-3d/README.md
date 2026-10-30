# Frame generation 3D

A frame made between two drawn ones by the frame interpolation of AMD's FidelityFX Super
Resolution 3.1 (FidelityFX SDK 1.1.4), which Godot doesn't have. [main.cpp](main.cpp) draws six
Forward+ frames into a 48x16 render target on the software device: a white card six pixels wide
slides right six pixels a frame, in front of a blue post. Each frame sets
`Scene3D::frame_generation`, so its prepass draws the meshes' motion vectors and keeps them with
its depth, and after each frame `RenderDevice::generate_frame` writes the frame halfway between it
and the frame before into a second target. It prints the middle row of each: `#` is the card,
`|` the post, `.` the background.

```text
first frame generated as drawn: yes
frame 0    ...######......................||...............
generated  ......######...................||...............
frame 1    .........######................||...............
...
generated  ........................######..................
frame 4    ...........................######...............
generated  .............................|######............
frame 5    ...............................||######.........
```

- A first frame has no frame before it, so the generated frame is the frame as drawn.
- Each generated frame has the card three pixels on from the frame before: the game's motion
  vectors, laid halfway back over the frame between, say where each of its pixels goes, and the
  two frames are blended there.
- Where the card covers or uncovers the post, the frame between gets the post's edges wrong: as
  the card covers it, nothing in the newer frame shows the post, so its pixels take the card's
  trail; as it uncovers it, AMD widens each moving thing's vectors by a pixel (the prepare pass
  takes the nearest depth around each pixel), so a column of the post moves with the card. AMD's
  frame generation has the same edges; at a game's frame rates they show for a few milliseconds.

The optical flow, which finds how the picture moved by blocks of 8x8 pixels, runs on every frame
too: it carries what has no motion vectors, such as 2D drawing, once ten frames have passed since
its first frames, which count as scene changes. `tests/test_frame_generation.cpp` moves a picture
that way. The software device generates frames on the CPU, the same pixels on every machine, and
the GPU and WebGPU devices make the same frames in compute shaders. A game shows them between its
drawn frames with project.json's `rendering.frame_generation`
([project-format.md](../../docs/reference/project-format.md#frame-generation)).
