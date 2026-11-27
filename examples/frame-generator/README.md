# Frame generator

A game's own frame generator, run in place of the engine's FSR 3 frame generation, as DLSS Frame
Generation, DLSS Multi Frame Generation or XeSS Frame Generation come in from a vendor's SDK
package. [main.cpp](main.cpp) adds `slide` with `render::add_frame_generator`, with `max_frames`
3, and draws four frames of the clustered renderer into a 48x16 render target on the software
device: a white card four pixels wide slides right eight pixels a frame.

Each frame's `Scene3D` sets `frame_generation` and names `slide` in `frame_generator`, so the
frame keeps copies of its prepass's depth and motion vectors for it. Then
`RenderDevice::generate_frames(target, between, "slide")` hands `slide` a
`render::GeneratedFrames`: the drawn frame, the depth and motion vectors at the render size, the
three targets to write, earliest first, and whether its history starts over. On a GPU those are
the native API's objects (a `VkImage`, an `ID3D12Resource`, an `MTLTexture`) with the command
buffer, which a vendor's library takes; the software device hands float pixels, so the output is
the same on every machine.

`slide` moves each pixel the meshes drew back along its motion vector by the part of a frame each
generated frame comes before the drawn one (3/4, 1/2 and 1/4), over the background. It prints
what it was handed, then the example prints the middle row of each generated frame and the drawn
one: `#` is the card, `.` the background.

```text
  slide: 3 frames, 48x16, depth and motion kept
generated  ........####....................................
generated  ..........####..................................
generated  ............####................................
frame 1    ..............####..............................
```

- The first frame starts the history over (`reset`), so `slide` gives the drawn frame three times.
- After that, the three frames put the card 2, 4 and 6 pixels on from where it was, a quarter of
  its 8-pixel step each, so a game drawn at 30 frames a second shows 120.
- The engine's own frame generation (`""`) makes one frame, so asking it for three is refused.

In a game, project.json's `rendering.frame_generator` and `rendering.generated_frames` name the
generator and the frames it makes; the canvas renderer shows them before each drawn frame and the
game paces them with `platform::Loop::wait_frame_part`
([project-format.md](../../docs/reference/project-format.md#frame-generation)).
