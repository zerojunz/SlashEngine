# Digital humans (research note)

A research note with no commitment to ship ([roadmap](../roadmap.md#research)). Digital humans in
the style of Unreal's MetaHuman are out of scope for releases. This page records what such
characters need, what SlashEngine already has, and what would come first if the work starts.
No MetaHuman code, assets or file formats are used or read: MetaHuman is closed and its assets
are licensed for Unreal only.

## What a realistic human needs

A believable close-up character is several systems working together:

1. **A body rig** that animates, retargets between characters and reacts to the world.
2. **A face** driven by many blend shapes (morph targets) and face bones, so it can speak and
   show expression. Face capture apps usually record Apple ARKit's 52 named shapes.
3. **Skin** that scatters light under its surface, so it doesn't look like plastic.
4. **Eyes** with refraction through the cornea, a wet layer and soft shadowing.
5. **Hair** as strands or cards, shaded with an anisotropic highlight.
6. **Level of detail**, since a full face is costly and is rarely seen up close.

## What SlashEngine has

- Skinned meshes, skeletons and model animations, as Godot's importer and `Skeleton3D`
  ([scene-format.md](scene-format.md)).
- Godot's humanoid skeleton profile, bone mapping and retargeting (`model.retarget`), and rigging
  of unrigged figures (`asset.refine`'s `rig`).
- Godot's IK modifiers, ragdolls and bone attachments.
- Animation trees with blend spaces and state machines.
- Mesh LODs and virtualized geometry ([virtualized-geometry.md](virtualized-geometry.md)).
- `StandardMaterial3D`'s base lighting model with Godot's rim, clearcoat, anisotropy, and
  subsurface scattering and transmittance.

## What is missing, in the order Godot would suggest

Godot has no digital human system either, but it has the parts below, so they come first and
follow Godot's design:

1. **Blend shapes** (done: [scene-format.md](scene-format.md#blend-shapes)). glTF morph targets loaded as Godot's mesh blend shapes, a
   `MeshInstance3D` field per shape (`blend_shapes/<name>`), animation tracks for them, and
   blending on the CPU before skinning, as Godot's skeleton shader does. This is useful well
   beyond humans, for faces, cloth, and any shape change an artist animates.
2. **Subsurface scattering and transmittance** (done:
   [scene-format.md](scene-format.md#subsurface-scattering)), as Godot's `StandardMaterial3D`
   `subsurf_scatter_*` and `subsurf_scatter_transmittance_*` fields, with its screen-space
   blur in Forward+. Their textures remain.
3. **Hair and eye shading**: Godot's `StandardMaterial3D` `anisotropy`, `rim` and `clearcoat`
   fields (done: [scene-format.md](scene-format.md#standardmaterial3d)) and alpha hash, which
   Godot uses for hair cards and eyes. Alpha hash and the fields' textures remain.
4. **Face capture data**: reading ARKit-style named blend shape tracks into animations, so a
   recorded performance can drive a face.

Strand hair simulation, a face rig editor and a library of scanned heads would come only after
these, and would need permissively licensed sources.

## Recommendation

If this work starts, start with blend shapes. They follow Godot directly, every later step needs
them, and games that never show a human still use them.
