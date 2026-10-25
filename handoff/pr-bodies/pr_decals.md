Before: there was no way to put paint, dirt, puddles or signs on a surface without modelling them, and a Godot scene's `Decal` nodes were reported as unsupported on import.

After: a `Decal` component projects textures down its -Y onto the meshes in its box, as Godot 4.6's Forward+ `Decal`. Its albedo texture colors what it covers by `albedo_mix`, with its normal texture's bumps and its ORM texture's ambient occlusion, roughness and metallic; its emission texture glows on top. Fades (`upper_fade`, `lower_fade`, `normal_fade`, distance fade), `modulate`, `cull_mask`, `layers` and `sorting_offset` work as Godot's. Every Forward+ device draws decals, the software one included, so `frame.capture` shows them; Compatibility draws none, as Godot's. Godot's `Decal` nodes and the `rendering/textures/decals/filter` setting now import.

```json
"puddle": {"components": {"Transform3D": {"position": [1.2, 0, 0.8]},
  "Decal": {"size": [2, 1, 2], "texture_albedo": "puddle.png", "texture_orm": "puddle_orm.png",
            "albedo_mix": 0.7}}}
```

How:

- **Atlas** (`engine/render/src/decal.hpp`, `decal.cpp`): `pack_decal_atlas` packs a frame's textures as Godot's `TextureStorage::update_decal_atlas` does: its skyline layout with a 32-texel border, and 5 levels, each the rounded average of the one before. The atlas is a storage buffer of RGBA8 words that the shader filters itself, so every device reads the same texels. `DecalAtlasCache` packs again only when the textures change, and devices upload it only then.
- **Placement and clusters:** `place_decals` follows `update_decal_buffer` (the first 512 within their distance fade, nearest the camera first less their `sorting_offset`; none without an albedo or emission texture). `build_clusters` gives each tile a decal section after its lights', from the corners of each decal's box.
- **Shading:** `mesh.hlsl`'s `apply_decals` is Godot's decal loop in `scene_forward_clustered.glsl`, before lighting and on unshaded meshes too. Atlas levels are picked from how far the surface moves across a pixel, worked out from the camera so that every device picks the same ones. The project's filter (`rendering.textures.decals.filter`, Godot's six) picks nearest or linear, with or without mipmaps; the anisotropic ones sample as their mipmap twins. `apply_decals` in `decal.cpp` is its CPU twin, which the software device draws with.
- **Devices:** SDL GPU (Vulkan, Metal, Direct3D 12) and WebGPU bind the decals and the atlas as two more storage buffers. Storage buffers grow to at most 128 MiB, WebGPU's default binding limit, unless one frame needs more.
- **Limits:** `render::check_decals` caps the atlas at 2^25 texels (a texture 2048 across fits). `scene3d::Renderer` reports decals whose textures overflow it and keeps those before them, and reports missing textures at `/components/Decal/<field>`.
- **Scenes and import:** the `Decal` component (`engine/world`) is gathered by `scene3d::Renderer::add_decals`, with its textures loaded by path. Emission is scaled by the camera's exposure, and Godot's setters' ranges are kept. The Godot importer maps `Decal` (Godot 4.0's `extents` become a `size` twice as big) and the decal filter setting.
- **Tests:**
  - `tests/test_decal.cpp` checks the atlas layout against Godot's packing, the levels' texels, the checks, placement, the clusters, filter parsing, and pixels on the software device and every GPU device for four filters, plus cull masks and Compatibility.
  - `tests/test_scene3d.cpp` checks gathering, culling, missing textures, Compatibility and atlas overflow.
  - `tests/test_import_godot.cpp` checks the import.
- **Example:** `examples/decals-3d` paints a street with a dashed line, a puddle and a glowing sign, with textures made in code.
- **Docs:** `docs/reference/scene-format.md` (Decals), `project-format.md` and `godot-import.md`, AGENTS.md, the examples index and the roadmap, where clustered decals are now ticked.

🤖 Generated with [Claude Code](https://claude.com/claude-code)

https://claude.ai/code/session_01A7FAm2JgA9jtumg74YFzUN
