# Digital humans (research note)

A research note ([roadmap](../roadmap.md#research)) on what characters in the style of Unreal's
MetaHuman need. Every part it lists is now done, and the character, face, anatomy and hair tools
([roadmap](../roadmap.md#agent-built-worlds-and-smart-npcs), items 13, 14 and 20) build such
characters from recipes; see [Characters](scene-format.md#characters),
[Faces](scene-format.md#faces) and [Hair and fur](scene-format.md#hair-and-fur). The page stays
as the record of why the parts came in the order they did. No MetaHuman code, assets or file
formats are used or read: MetaHuman is closed and its assets are licensed for Unreal only.

Status: current · Updated: 2026-11-20

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

- Skinned meshes, skeletons and model animations
  ([scene-format.md](scene-format.md)).
- A humanoid skeleton profile, bone mapping and retargeting (`model.retarget`), and rigging
  of unrigged figures (`asset.refine`'s `rig`).
- IK modifiers, ragdolls and bone sockets.
- Animation trees with blend spaces and state machines.
- Mesh LODs and virtualized geometry ([virtualized-geometry.md](virtualized-geometry.md)).
- `Surface3D`'s base lighting model with rim, clearcoat, anisotropy, and
  subsurface scattering and transmittance.

## What was missing, in order

The engine had no digital human system, but it had the parts below, so they came first:

1. **Blend shapes** (done: [scene-format.md](scene-format.md#blend-shapes)). glTF morph targets
   loaded, a `Model3D` field per shape (`blend_shapes/<name>`), animation tracks for them, and
   blending on the CPU before skinning. This is useful well beyond humans, for faces, cloth, and any
   shape change an artist animates.
2. **Subsurface scattering and transmittance** (done:
   [scene-format.md](scene-format.md#subsurface-scattering)), with `Surface3D`'s
   `subsurf_scatter_*` and `subsurf_scatter_transmittance_*` fields, with its screen-space blur
   in the clustered renderer. Their textures remain.
3. **Hair and eye shading**: `Surface3D`'s `anisotropy`, `rim` and `clearcoat`
   fields and alpha hash, used for hair cards and eyes (done:
   [scene-format.md](scene-format.md#standardmaterial3d)). The fields' textures remain.
4. **Face capture data** (done: [scene-format.md](scene-format.md#face-tracking)): face trackers,
   which drive a face's blend shapes named in ARKit's,
   Unified Expressions', SRanipal's or Meta's standard live, and
   [`animation.import_face_capture`](commands.md#animationimport_face_capture), which reads a
   Live Link Face recording into an animation, so a recorded performance drives a face.

After these, the [hair and fur](scene-format.md#hair-and-fur) tool added strand hair and its
simulation, the [character](scene-format.md#characters) tool full rigs with fingers, eyes, jaw
and twist bones, and the [face](scene-format.md#faces) tool sculpted heads with ARKit's 52
expression shapes, skin maps and marks, all made in code with no scanned heads.

## Recommendation

The note recommended starting with blend shapes, as every later step
needs them, and games that never show a human still use them. That is the order the work took.
