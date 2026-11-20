# Procedural generation

Procedural generation: noise, textures, scatter rules and PCG graphs.

Status: current · Updated: 2026-11-20

`engine/procedural` is Godot's noise module and procedural texture resources. `procedural::Noise`
(`slash/procedural/noise.hpp`) is Godot's `FastNoiseLite` resource on Auburn's FastNoiseLite
(header-only, MIT): settings by Godot's names and defaults, sampled as Godot samples them (offset,
then domain warp, then noise), the same bits everywhere since FastNoiseLite calls libm only for
square roots and the module builds with `-ffp-contract=off`. Gameplay code samples it while a game
runs. `procedural::Gradient` (`gradient.hpp`) is Godot's `Gradient`, with its sRGB, linear and
Oklab blending through `slash::exp` and `ln`. A `.texture.json` file (`texture.hpp`;
`$cli schema texture`) is a `NoiseTexture2D` (`Noise::_get_image` and
`_generate_seamless_image`, the color ramp and `bump_map_to_normal_map` ported line for line),
`GradientTexture1D` or `GradientTexture2D`, made into pixels as it loads: `canvas::add_asset_types`
and `ui::add_asset_types` register it as a texture, and code that reads texture files itself
(scene3d's material, terrain and vector field checks) goes through `procedural::decode_texture`
and `is_texture_path`, so any texture field takes one. Exports ship the file and validate it.
`texture.generate` (`engine/command/src/procedural.cpp`) makes one from a file or inline settings
and measures it (channels' range, and `wrap`, which says whether it tiles), returns pixels and
writes a PNG. `examples/procedural-textures` tunes water that tiles.

Gameplay code makes textures and meshes while a game runs, as Godot's `ImageTexture` and
`ArrayMesh`: `asset::Assets::put` keeps a value at a `made:<name>` path (`asset::is_made_path`)
until `drop`, bumping its version when put again so renderers upload it again, and never reloads
it; `canvas::install` attaches a world's assets (`asset::attach`, `asset::assets_of`), so
`procedural::make_texture` and `scene3d::make_mesh` reach them from a system. `scene3d::SurfaceTool`
(`src/surface_tool.cpp`) is Godot's for triangles (`generate_normals` with smooth groups, MikkTSpace
`generate_tangents`, `index`, `commit` into a `Model`). Validators skip `made:` paths (only the
running game has them). `examples/runtime-meshes` grows hills from noise.

Scatter rules are `.scatter.json` files (`scatter.hpp`; `$cli schema scatter`), layers of what to
place over terrains, meshes, tile maps and areas, as Unreal's PCG and Godot's MultiMesh *Populate
Surface*: `procedural::scatter` picks a triangle by area and a point in it as Godot's
`Face3::get_random_point_inside`, keeps it by area, slope, height, noise and spacing (from its own
places and the layers it `avoid`s), and turns and scales it as `MultiMeshEditor::_populate`, with a
seeded PCG32 so places repeat everywhere. `scatter.run` (`engine/command/src/scatter.cpp`) reads
the ground as `navigation::world_geometry` (3D) or tile map cells (2D) and puts each layer under
`into` as entities extending a prefab or drawing a mesh, or as one `MultiMeshInstance3D`, Godot's
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
FastNoiseLite's values (`TerrainNoise::values`) in place of HTerrain's Perlin noise before its
curve, island and erosion. Both take a NoiseTexture2D's `.texture.json` file or inline settings
(`read_noise_source` and `sample_noise_source` in `engine/command/src/procedural.cpp`, sampled
and normalized as Godot's `Noise::_get_image`), so the noise an agent tuned with
`texture.generate` makes the map. `examples/noise-maps` makes an archipelago and dunes.

Procedural content from other tools comes in as those files. The Godot importer writes each
`NoiseTexture2D`, `GradientTexture1D` and `GradientTexture2D` as a `.texture.json` file
(`procedural_texture_file` in `tools/importer/src/godot_import.cpp`, its `FastNoiseLite` and
`Gradient` by Godot's property names), and `terrain.import` (`engine/command/src/terrain.cpp`)
reads a terrain tool's heightmap (16-bit PNG, `.r16`/`.raw`, `.r32` floats, or EXR through
tinyexr, `slash_tinyexr`, tools only) and splat and color maps into an `HTerrain` as HTerrain's
import dialog does, sizing it by `adjusted_map_size`. `examples/procedural-imports` brings in a
pond's water and a valley. glTF's `EXT_mesh_gpu_instancing`, as Blender's geometry nodes and
Houdini export scattered copies, loads each instance as a child node drawing the mesh
(`SceneReader::add_instances` in `engine/scene3d/src/model.cpp`); `examples/gltf-instances` is an
orchard.
