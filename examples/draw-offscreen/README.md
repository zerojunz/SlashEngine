# Draw offscreen

Drawing into a texture and reading its pixels back, with no window. [main.cpp](main.cpp) draws
two squares and a checker into a 64x64 render target, then reads it.

- `create_device_or_null` uses the GPU backend that works on the machine (Vulkan, Metal or
  Direct3D 12) and falls back to the null device, with a warning, where none does. The null
  device keeps textures and counts draws but draws nothing: reading a target returns the color
  it was cleared to. Both refuse the same mistakes with the same errors.
- `draw` takes triangles, every three vertices one, with a texture or `TextureId::None` for plain
  color. Vertex positions are pixels from the top left unless the frame sets `view`.
- Draws in a row with the same texture are sent as one GPU draw: three draws here make two.
- Textures are straight-alpha RGBA. `Filter::Nearest` keeps texels sharp, for pixel art.
- The output only shows what is the same on every device, so CI can check it exactly on
  machines with and without a GPU. `tests/test_render.cpp` checks the drawn pixels on a GPU.
