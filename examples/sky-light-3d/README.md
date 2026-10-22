# Sky Light 3D

The sky's own light on meshes, as Godot's sky radiance in its Compatibility renderer.
[main.scene.json](main.scene.json) is three balls on a gray plain under a procedural sky, seen
through a `Camera3D` in a 320x180 window: chrome (`metallic` 1, `roughness` 0.05), satin gold
(`metallic` 1, `roughness` 0.45) and chalk (`StandardMaterial3D`'s defaults: not metallic, fully
rough). The `environment` entity's `WorldEnvironment` has a `"sky"` background and leaves
`ambient_light_source` and `reflected_light_source` at `"background"`, so the sky behind the
balls also lights them and shines in them.

[calls.jsonl](calls.jsonl) drives it the way an agent does over MCP or JSON-RPC, and
[results.jsonl](results.jsonl) holds the replies:

```text
frame.capture {"points":[[97,80],[100,107],[150,90],[205,100],[160,160]],"path":"shots/sky.png"}
{"colors":[[0.141,0.337,0.741,1],[0.298,0.212,0.114,1],[0.376,0.353,0.267,1],
 [0.357,0.369,0.498,1],[0.373,0.396,0.494,1]],"drawn":{"meshes":4},"height":180,
 "path":"shots/sky.png","width":320}
```

- `points` reads the chrome ball's top, where it mirrors the blue sky, and its bottom, where it
  mirrors the sky's brown ground; the satin ball, which shows the same sky blurred by its
  roughness; the chalk ball's shaded side, blue from the sky above it; and the plain.
- `reflected_light_source` `"disabled"` takes the reflections away: the chrome turns black and
  the satin keeps only the sun's broad highlight.
- `ambient_light_color` with `ambient_light_sky_contribution` 0.5 mixes half an orange light
  into the sky's, so the chalk and the plain warm up.
- A `"color"` background with `ambient_light_source` and `reflected_light_source` `"sky"` hides
  the sky but keeps its light: the background turns dark, and the chrome and chalk read exactly
  as in the first frame.

The sky is drawn into a cube `sky_radiance_size` texels a side (256 here, Godot's default) and
blurred into smaller levels for rougher surfaces. It is drawn again only when the sky changes.
