# Compositor effects

A pipeline file, as Godot 4.3's `Compositor` and `CompositorEffect`: it turns the frame's own
passes off and adds screen passes of the game's own. [main.scene.json](main.scene.json) is a yard
with a crate and a ball under the sun, and its `Environment3D` names
[look.pipeline.json](look.pipeline.json), which:

- turns glow off for this scene, whatever the environment says;
- inks the edges between things with [outline](fx/outline.material.json) after the opaque meshes
  (`post_opaque`), reading the depth and the prepass's normals
  ([shaders/outline.hlsl](shaders/outline.hlsl));
- then tints the frame warm and darkens its corners with [grade](fx/grade.material.json) after the
  blended meshes, the default point ([shaders/grade.hlsl](shaders/grade.hlsl)).

```hlsl
// shaders/grade.hlsl, a compositor shader: COLOR starts as the screen's color.
void fragment() {
    float2 from_center = SCREEN_UV - 0.5;
    float fade = saturate(1.0 - vignette * dot(from_center, from_center));
    COLOR.rgb *= tint.rgb * fade;
}
```

[calls.jsonl](calls.jsonl) checks it as an agent would, with `frame.capture` at a point on the
crate's left edge, one in the sky's top left corner and one in the middle of the sky:

- With the look, the crate's edge is inked almost black and the sky's corner is darker than its
  middle.
- `component.set` of the environment's `compositor` to `""` draws the plain frame: the edge shows
  the crate's brown, and the sky is one blue in the corner and the middle.
- Naming [night.pipeline.json](night.pipeline.json) on the camera, whose `compositor` comes before
  the environment's, grades the same frame deep blue with no outline (the edge is the crate's,
  darkened) and a vignette strong enough to turn the corner black, through the grade material
  with other `material_params`.

- A compositor shader reads `SCREEN_TEXTURE` through `screen_texture()`, the depth through
  `depth_texture()` and, for an effect with `needs_normal_roughness`, the prepass's normals and
  roughness through `normal_roughness_texture()`.
- Effects draw in Forward+ only, as Godot's: the Compatibility renderer and OpenGL draw none.
- An effect's `code` names a C++ callback the game registers with
  `scene3d::add_compositor_code`, which changes the effect before each frame, as Godot's
  `_render_callback`.
- [Compositor shaders and pipeline files](../../docs/reference/scene-format.md#compositor-shaders-and-pipeline-files)
  in the scene format reference lists the built-ins and the file's fields.
