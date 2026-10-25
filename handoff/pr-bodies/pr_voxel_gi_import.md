Before: `project.import` left a Godot scene's `VoxelGI` nodes out and reported them as unsupported, so a room lit by bounced light in Godot came over without it.

After: a `VoxelGI` imports with its settings, its camera attributes and the cells Godot baked for it, written as a `.voxelgi.json` file, so the room lights as it did in Godot with no new bake. Godot's VoxelGI quality setting carries over too. This finishes milestone 7's global illumination item.

How:
- `tools/importer/src/godot_import.cpp` maps `VoxelGI` to `Transform3D` and `VoxelGI` (`subdiv` 0 to 3 as `"64"` to `"512"`, `size`), its `camera_attributes` to a `CameraAttributesPractical` on an entity of its own, and its `VoxelGIData`'s settings (`dynamic_range`, `energy`, `bias`, `normal_bias`, `propagation`, `use_two_bounces`, `interior`) to the component's fields.
- The data's `_data` (bounds, octree size, level counts and the 16-byte cells, base64 or not, in a `.tscn`, `.tres` or binary `.res`) becomes a `.voxelgi.json` file named after the resource, written once however many VoxelGIs share it. Its cells have the same layout as `voxel_gi.bake`'s, and their emission was baked at the camera attributes' exposure multiplier, as Godot's `_get_camera_exposure_normalization`, so that is the file's `baked_exposure`. Data with no cells, or cells that fail `validate_voxel_gi`, is reported and left out, with a hint to bake again.
- `rendering/global_illumination/voxel_gi/quality` becomes `rendering.global_illumination.voxel_gi.quality` (`"low"` or `"high"`).
- A test imports a `.tscn` with sub-resource data and camera attributes, a binary `.res`, data with no cells and data with a cell missing, and checks the written cells against a bake. `examples/import-godot-voxel-gi` imports a room from a binary `.res` and checks the frame with and without the cells. Docs and the roadmap are updated.

🤖 Generated with [Claude Code](https://claude.com/claude-code)

https://claude.ai/code/session_01A7FAm2JgA9jtumg74YFzUN
