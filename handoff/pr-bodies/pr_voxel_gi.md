Before: a 3D scene's bounced light came only from baked lightmaps, which have to be baked again whenever a light moves or changes, and Godot's `VoxelGI` had no counterpart.

After: a `VoxelGI` entity lights the meshes in its box with bounced light that follows the lights as they change, as Godot 4.6's `VoxelGI`. `voxel_gi.bake` voxelizes the static meshes once into a `.voxelgi.json` file; while the game runs, the renderer injects the lights into the cells whenever they change, and every mesh in the box takes its ambient light and reflections from cones traced through them, on every Forward+ device, the software one included.

How:
- `engine/scene3d/src/voxel_gi.cpp` ports Godot's `Voxelizer` (an octree down to `subdiv` cells, each with albedo, emission and normal, the same on every machine), `VoxelGIData` as `.voxelgi.json` (`slash-cli schema voxel_gi`, `scene validate`, `scene format`, exports), and `voxel_gi.glsl`'s light injection on the CPU on the job pool: direct light with shadows marched through the cells' distance field, mipmaps up the octree, the second bounce. The light goes into one RGBA8 atlas of each level's z slices.
- `scene3d::Renderer` loads each VoxelGI's cells, injects the lights again when they, its place or its settings change (omni and spot lights whose `light_bake_mode` isn't `"disabled"` and whose range reaches the box, then directional lights, at most 32, as Godot pairs them), stacks every VoxelGI's atlas into one texture and gives each draw the VoxelGIs its bounds touch, at most two. Lightmapped, light-probe, terrain and shadow-only draws take none, as in Godot.
- `mesh.hlsl` follows `gi.glsl`'s `voxel_gi_compute` (four 45-degree cones, or six with `rendering.global_illumination.voxel_gi.quality: "high"`, plus a reflection cone by roughness), blending over the environment's ambient light and sky reflections before reflection probes; `render::detail::trace_voxel_gis` is its CPU twin, and a test holds every Forward+ device to the software device's pixels. Shader headers regenerated.
- `voxel_gi.bake` writes the file and sets `data`, with dry runs, undo and "unchanged" like `lightmap.bake`.
- `examples/voxel-gi-3d` bakes a room and shows its bounce following the sun live; docs (scene format, commands, project.json, AGENTS.md) and the roadmap are updated. Moving meshes don't light the cells yet (Godot's dynamic objects), and Godot import of VoxelGI comes next; both are listed on the roadmap.

🤖 Generated with [Claude Code](https://claude.com/claude-code)

https://claude.ai/code/session_01A7FAm2JgA9jtumg74YFzUN
