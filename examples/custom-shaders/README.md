# Custom shaders

A game's own shaders, as Godot's canvas_item shaders, for what a material's node graph can't
say. A `.material.json` file's `"shader"` names an `.hlsl` file with a `fragment()` function, a
`vertex()` function or both. [main.scene.json](main.scene.json) has two sprites:
[dissolve](dissolve.material.json) burns the crate away with
[shaders/dissolve.hlsl](shaders/dissolve.hlsl), and [wave](wave.material.json) sways the flag
with [shaders/wave.hlsl](shaders/wave.hlsl). [main.cpp](main.cpp) writes their textures, checks
them with `material.eval` and `frame.capture` as an agent would, then edits a shader.

```hlsl
void vertex() {
    VERTEX.x += sin(TIME * speed) * amplitude * (1.0 - UV.y);
}

void fragment() {
    COLOR *= tint;
}
```

- The code reads and writes Godot's built-ins as globals: `COLOR`, `UV`, `VERTEX`, `TIME`,
  `TEXTURE`, `TEXTURE_PIXEL_SIZE`, `FRAGCOORD`, `NORMAL_MAP` and `SPECULAR_SHININESS`.
  `texture(TEXTURE, UV)` samples a texture, as in Godot. Helper functions are plain HLSL.
- Each param in the material file is a global of the same name in the shader: a `float`,
  `float2`, `float3` or `float4` (colors too), or a texture. Sprites set them through
  `Sprite2D`'s `material_params`, as for graph materials.
- `material.eval` runs `fragment()` on the CPU; a discarded pixel comes back as `[0, 0, 0, 0]`.
  `frame.capture` draws the frame on the CPU with `vertex()` too, so the flag's top has moved
  half a second in.
- slash-cli and slash-mcp compile shaders with glslang and SPIRV-Cross, for Vulkan, Metal,
  Direct3D 12, OpenGL ES and WebGL 2 and the CPU. Games can't, so `slash-cli export` writes each
  material's shader, compiled for the platform, into the material file.
- A mistake is reported with its file and line, by `slash-cli scene validate` on the material,
  by exports and by `material.eval`, and a sprite whose shader doesn't compile is left out with a
  warning. Shaders reload when their files change.
- [Shaders](../../docs/reference/scene-format.md#shaders) in the scene format reference lists
  the built-ins and limits.
