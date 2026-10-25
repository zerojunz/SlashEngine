Before: a mesh whose `gi_mode` is `"dynamic"` took a `VoxelGI`'s bounced light but gave none back: a glowing or brightly lit object moving through a room lit nothing around it.

After: the renderer plots dynamic meshes into the VoxelGIs they touch each frame they move or change, as Godot 4.6's VoxelGI dynamic objects, so a lantern carried through a room lights the walls beside it and a lit ball bounces its color onto the floor, with no bake.

How:
- `engine/scene3d/src/voxel_gi.cpp` ports `GI::VoxelGIInstance::update`'s dynamic object pass: each mesh's box in cells (Godot's `multiplier` of 16), drawn from six sides into a material map (albedo, alpha, normal, emission, depth) by an orthographic rasterizer with the GPU's fill rules and the material's cull mode, lit at each texel by the same lights and shadows as the cells (`MODE_DYNAMIC_LIGHTING`), shrunk level by level as `MODE_DYNAMIC_SHRINK_WRITE` and `_PLOT` do, then plotted into a copy of the static light so the cells keep their baked part. The light code the injection and the plotter share moved into one `LightField`.
- `scene3d::voxel_gi_meshes` reads a mesh entity's meshes and materials as the bake does, and `plot_voxel_gi_objects` plots them; both run on the job pool and give the same texels on every machine.
- `scene3d::Renderer` keeps each dynamic mesh it has read (re-read when its mesh, model or material changes), finds those whose bounds touch each VoxelGI's box, and plots them again only when one moves, changes or leaves, or when the lights change. The canvas renderer hands it CPU images for materials' textures.
- Tests cover the plotted texels (lit tops, dark undersides, emission without lights, every level, pool and no pool), cull modes, reading entities, and the renderer re-plotting a ball as it moves and leaving unchanged frames alone. `examples/voxel-gi-3d` makes its ball glow and moves it across the room; docs and the roadmap are updated. Godot import of VoxelGI comes next.

🤖 Generated with [Claude Code](https://claude.com/claude-code)

https://claude.ai/code/session_01A7FAm2JgA9jtumg74YFzUN
