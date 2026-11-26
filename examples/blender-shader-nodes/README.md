# Blender shader nodes

`texture.from_blender` turns a Blender material's shader nodes into a
[texture graph](../../docs/reference/scene-format.md#texture-graphs): each node the material's
Principled BSDF reads is followed back and written as the graph's ops, so a material an artist
made in Blender becomes text an agent tunes, bakes on every device and uses in a layer stack.

The engine never reads a `.blend` file. With `"blend"` (and the `build` capability), it runs the
artist's own Blender in the background with a script that writes every material's node tree as
JSON, and reads that. [materials/barrel.nodes.json](materials/barrel.nodes.json) is what that
script wrote for a barrel's two materials, so this example runs without Blender:

- `Rusty`: a color ramp over noise, multiplied by a tint, mixed a quarter with a Magic Texture;
  roughness from Voronoi distance times 0.8; a bump from the noise.
- `Plain`: a red base color and nothing linked.

The calls in [calls.jsonl](calls.jsonl):

1. `texture.from_blender` writes `Rusty` as `textures/rusty.texture.json` at 128 pixels: its
   `albedo`, `roughness`, `metallic` and `normal` outputs, measured. The Magic Texture has no
   graph op, so it reads gray and comes back in `warnings`.
2. `texture.inspect` lists the graph's nodes and what each output needs.
3. `Plain` becomes a graph of constants, unmeasured.
4. A material name that isn't there names the nearest.
5. `texture.generate` bakes the roughness output to `build/rusty_roughness.png`.

Run it with `slash-cli rpc --project . < calls.jsonl`; [results.jsonl](results.jsonl) holds the
replies.
