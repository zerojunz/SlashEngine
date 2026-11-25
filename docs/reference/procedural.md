# Procedural generation

Procedural generation: noise, textures, scatter rules and PCG graphs.

Status: current · Updated: 2026-11-25

Parts of this area are ported from [Godot Engine](https://godotengine.org) (MIT); see [THIRD_PARTY.md](../../THIRD_PARTY.md).

`engine/procedural` is noise and procedural textures. `procedural::Noise`
(`slash/procedural/noise.hpp`) is a noise resource on Auburn's FastNoiseLite (header-only, MIT):
sampled by offset, then domain warp, then noise, the same bits everywhere since FastNoiseLite calls
libm only for square roots and the module builds with `-ffp-contract=off`. Gameplay code samples it
while a game runs. `procedural::Gradient` (`gradient.hpp`) is a color ramp with sRGB, linear and
Oklab blending through `slash::exp` and `ln`. A `.texture.json` file (`texture.hpp`; `$cli schema
texture`) is a `noise` texture (with a color ramp, seamless generation and bump maps to normal
maps), or a `gradient_1d` or `gradient_2d` texture, made into pixels as it loads:
`canvas::add_asset_types` and `ui::add_asset_types` register it as a texture, and code that reads
texture files itself (scene3d's material, terrain and vector field checks) goes through
`procedural::decode_texture` and `is_texture_path`, so any texture field takes one. Exports ship the
file and validate it. `texture.generate` (`engine/command/src/procedural.cpp`) makes one from a file
or inline settings and measures it (channels' range, and `wrap`, which says whether it tiles),
returns pixels and writes a PNG. `examples/procedural-textures` tunes water that tiles. A
`.texture.json` file of `"type": "graph"` is a texture graph (`slash/procedural/texture_graph.hpp`,
`src/texture_graph.cpp`): nodes over parameters, `extends` and named outputs, checked and compiled
into a `render::TextureGraphProgram` of passes over shared slots (`compile_texture_graph`).
`RenderDevice::bake_texture_graph` runs it: the GPU and WebGPU devices dispatch
`shaders/texture_graph.hlsl` per pass (with `fast_noise_lite.hlsl`, FastNoiseLite's HLSL, its tables
in group memory), and every other device (and Direct3D 12, and any device inside a frame) runs the
CPU twin, `render::run_texture_graph` (`engine/render/src/texture_graph.cpp`, on the job pool, built
with `-ffp-contract=off`). A change to an op goes in both, and `tests/test_texture_graph_gpu.cpp`
holds every device to the twin over every op. `examples/texture-graphs` tunes a brick wall.
`texture.inspect`, `texture.edit` and `texture.check` (`engine/command/src/texture_tools.cpp`) work
on graphs as text: `procedural::describe_texture_graph` says what reads each parameter and what each
output needs, an edit is a JSON Merge Patch that returns each output measured (`measure`: luminance,
channels, contrast and whether it tiles), and the check holds each graph to its `expect` ranges and
the storyboard's palettes and budgets (`storyboard::check_textures`); `test.run` and `slash-cli
project validate` run it. `examples/texture-graph-tools` tunes the wall to a budget. A material's
`shader` can name a graph, which then draws live: `procedural::texture_graph_shader`
(`src/texture_graph_shader.cpp`) writes a shader with a function per pass, each
`shaders/texture_graph_ops.hlsl` included with the pass's op, size and inputs as macros
(`texture_graph.hlsl`, the compute pass, includes it once for every op; both share
`texture_graph_common.hlsl`, and fast_noise_lite.hlsl reads its tables where they are with
`FNL_STATIC_TABLES`), and the shader tools' compiler has the three files built in. Settings that
material params (and `time`, as `TIME`) move are found by compiling the graph again with each moved,
and must follow in a straight line. `examples/live-texture-graphs` flows lava.
`model.bake_maps` (`engine/command/src/mesh_maps.cpp`) bakes a model's mesh maps for graphs to
mask with (`slash/scene3d/mesh_maps.hpp`): ambient occlusion, curvature, thickness, sky, world
normal and position, part and material ids, and from a high model its height and normal, as PNG
files in a `.maps` folder beside it that `image` nodes read. Rays trace on ray queries where a
device has them (see [rendering](rendering.md)) and on the CPU twin elsewhere; curvature comes from
the angles of nearby edges. `examples/mesh-maps` bakes a crate's maps and wears its edges with them.
Layer stacks (`.paint.json`, `slash/procedural/paint.hpp`, `src/paint.cpp`) paint a model or a
sprite as layers over masks: `resolve_paint` merges what a stack `extends` (a file, or a smart
material it ships as `builtin:paint/<name>`), and `compile_paint` turns it into a texture graph of
a blend per layer and channel. What the graph can't work out per texel on its own (the mesh maps,
world normal and position by axis, 3D noise in the model's space, part and material ids, strokes,
decals) is worked out on the CPU over a `PaintSurface` and fed in as float images
(`GraphReader::graph_image`, up to `render::kMaxGraphImages`, 64), so a stack bakes as any graph
does. A model's surface is its low mesh rasterized into its texture
(`scene3d::mesh_map_surface`: place, normal, part and material per texel, padded as the maps are),
and a sprite's is its pixels, its alpha and its outline's edge (`paint_sprite_surface`).
`paint.edit`, `paint.bake` and `paint.preview` (`engine/command/src/paint.cpp`) bake the maps a
stack needs with `model.bake_maps`' code and cache, write the textures and a material, and
`texture.check` holds stacks to their `expect` too. `examples/layer-stacks` paints a worn crate and
a 2D shield.

A `weather` layer expands (`weather_layers`) into its rain, rust, dust and moss layers before it
compiles, each an ordinary layer whose mask reads `facing`, `streaks`, the mesh maps and noise
with thresholds moved by `amount * age`. `point` terms read `PaintSurface::points` (part centers,
then node origins, filled by `paint.cpp`'s `prepare`). `paint_variant` draws a variant's
parameters with splitmix64 over a hash of the name, the variants' seed and its number, so a
variant is the same everywhere. Text stamps come in through `GraphReader::text_image`, which the
commands set to draw with the UI's fonts (`draw_text_stamp` in `procedural.cpp`);
`examples/weathering` ages a market stall.

Gameplay code makes textures and meshes while a game runs: `asset::Assets::put` keeps a value at a
`made:<name>` path (`asset::is_made_path`) until `drop`, bumping its version when put again so
renderers upload it again, and never reloads it; `canvas::install` attaches a world's assets
(`asset::attach`, `asset::assets_of`), so `procedural::make_texture` and `scene3d::make_mesh` reach
them from a system. `scene3d::SurfaceTool` (`src/surface_tool.cpp`) builds triangles
(`generate_normals` with smooth groups, MikkTSpace `generate_tangents`, `index`, `commit` into a
`Model`). Validators skip `made:` paths (only the running game has them). `examples/runtime-meshes`
grows hills from noise.

Scatter rules are `.scatter.json` files (`scatter.hpp`; `$cli schema scatter`), layers of what to
place over terrains, meshes, tile maps and areas, like Unreal's PCG: `procedural::scatter` picks a
triangle by area and a point in it, keeps it by area, slope, height, noise and spacing (from its own
places and the layers it `avoid`s), and turns and scales it, with a seeded PCG32 so places repeat
everywhere. `scatter.run` (`engine/command/src/scatter.cpp`) reads the ground as
`navigation::world_geometry` (3D) or tile map cells (2D) and puts each layer under `into` as
entities extending a prefab or drawing a mesh, or as one `ModelInstances3D`
(`Renderer::add_multimesh` draws its model at each instance), replacing what `into` held.
`examples/scatter-rules` grows a forest.

Procedural graphs drive every tool: a `.pcg.json` file (`slash/procedural/pcg.hpp`,
`src/pcg.cpp`; `$cli schema pcg`) is a list of nodes over points, after Unreal's PCG graphs.
Samplers (`surface` through `procedural::scatter`, `grid`, `points`, `path` along a curve,
`entities` by ID, component or storyboard place) make points; modifiers keep, thin, move and
turn them (`filter`, `density`, `prune`, `difference`, `near`, `transform`, `merge`: `pcg_filter`,
`pcg_prune` and the rest, every random choice hashed from the graph's seed, the node and the
point's own seed with `pcg_hash`); spawners place things at them. `pcg.run`
(`engine/command/src/pcg.cpp`, `Effect::Compound`) places `spawn` entities through
`entity.create`, a `multimesh`, a `foliage` group, a `route` path, and calls any edit command a
point with `$` names in its params (`$local`, `$yaw`, `$id`, `{"$pick": [...]}`), so creatures,
characters, vehicles, lights, wind, fields, fluids, water and effects all come from graphs;
what it made goes under `into` and the next run replaces it. `pcg.preview` returns counts,
bounds and a top-down PNG, and `pcg.check` holds nodes to the budget and their `expect` (counts,
density and reachability by `navigation::query_path`). scatter.cpp's ground gathering is shared
through `src/scatter_ground.hpp`. `examples/procedural-worlds` fills a valley and a 2D island.

Maps come from noise too: `tilemap.generate` (`engine/command/src/builtin.cpp`) samples a noise
over a rect of cells and gives each the first band its value is under, painting each band's
terrain with `paint_terrain` in turn or setting its tile, and `terrain.generate`'s `noise` puts a
FastNoiseLite's values (`TerrainNoise::values`) in place of Terrain3D's Perlin noise before its
curve, island and erosion. Both take a noise texture's `.texture.json` file or inline settings
(`read_noise_source` and `sample_noise_source` in `engine/command/src/procedural.cpp`, sampled
and normalized), so the noise an agent tuned with `texture.generate` makes the map.
`examples/noise-maps` makes an archipelago and dunes.

Procedural content from other tools comes in as those files. The Godot importer writes each
`NoiseTexture2D`, `GradientTexture1D` and `GradientTexture2D` as a `.texture.json` file
(`procedural_texture_file` in `tools/importer/src/godot_import.cpp`, its `FastNoiseLite` and
`Gradient` by Godot's property names), and `terrain.import` (`engine/command/src/terrain.cpp`)
reads a terrain tool's heightmap (16-bit PNG, `.r16`/`.raw`, `.r32` floats, or EXR through
tinyexr, `slash_tinyexr`, tools only) and splat and color maps into an `Terrain3D` as Terrain3D's
import dialog does, sizing it by `adjusted_map_size`. `examples/procedural-imports` brings in a
pond's water and a valley. glTF's `EXT_mesh_gpu_instancing`, as Blender's geometry nodes and
Houdini export scattered copies, loads each instance as a child node drawing the mesh
(`SceneReader::add_instances` in `engine/scene3d/src/model.cpp`); `examples/gltf-instances` is an
orchard.
