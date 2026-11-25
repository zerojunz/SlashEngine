# Rendering

The renderer: devices, passes, materials, lighting and ray tracing.

Status: current · Updated: 2026-11-20

`engine/render` is the renderer's device layer: `render::RenderDevice`, a dozen calls over SDL3's
GPU API (Metal, Direct3D 12 or Vulkan), and a null device for headless runs that checks every
call the same way but draws nothing. `create_device_or_null` falls back to it, with a warning,
where no backend starts. `examples/draw-offscreen` draws into a texture and reads it back.

`src/gl_device.cpp` is the same device over OpenGL (Godot's compatibility tier): the `opengl`
driver is OpenGL 3.3's core profile on Windows, macOS and Linux, falling back to OpenGL ES 3.0
where that doesn't start, and `opengl_es` is OpenGL ES 3.0, which is all the web (WebGL 2, where
`auto` picks it when the browser has no WebGPU), Android and iOS have. Desktop OpenGL
compiles the committed GLSL ES 3.00 as GLSL 3.30, so one set of shaders serves both. It loads
its GL functions through SDL (links WebGL's on the web), needs a window made with
`platform::Config::opengl`, and draws every frame upside down into a texture so rows and
`gl_FragCoord` match the other backends, then copies window frames over turned back. As Godot's
`fallback_to_opengl3`, games ask `render::choose_driver` before making the window (through
`platform::Config::choose_opengl`, once video has started) and draw with `opengl` where the GPU
backend doesn't work (`rendering.fallback_to_opengl`).
`uniforms.hpp` fills the material shader's uniforms for both hardware devices; a change to how
the GPU device draws goes in this one too.

`src/webgpu_device.cpp` is the device over the browser's WebGPU, through Emscripten's
emdawnwebgpu port, which `auto` picks on the web where the browser has it (the `webgpu`
driver; web builds only). `web/webgpu.js` asks for the device before `main` runs, and the
device reports WebGPU's errors through error scopes when a read waits on the GPU. It draws
with the committed WGSL, which Tint compiles from the HLSL's SPIR-V; Tint drops SPIR-V's row
major matrices, so the view matrix is uploaded turned. Game shaders, and FSR, draw with the
WGSL `tools/shader_compiler` writes with Tint. A change to how the GPU device draws goes in this
one too. `tests/CMakeLists.txt` builds the drawing tests
for the web as `slash_web_tests`, which `tests/web/run.mjs` runs in Node.js on Dawn's WebGPU
(the `webgpu` npm package, `npm ci --prefix tests/web`) over Mesa's software Vulkan; CI's web
export job runs them.

`create_software_device` (`src/software_device.cpp`) draws on the CPU, into render targets
only: it batches as the GPU device does, rasterizes with the GPU's fill rules and shades with
the CPU twins (`run_program`, `render::shade`, `distance_field_coverage`, `sample_image`), so its
pixels are the same on every machine. The GPU tests draw on it too, and hold both devices to the
same pixels; a change to how the GPU device draws goes in both. `canvas::capture` draws a world's
frame on it at the viewport size, and backs `frame.capture` (pixel colors, a PNG or a file; MCP
sends the PNG as an image). `examples/frame-capture` checks a HUD by its pixels.

`frame.summary` reads the same frame as data. `CanvasOptions::contents` has `CanvasRenderer::draw`
fill a `canvas::FrameContents`: each sprite, layer, emitter and control drawn, as triangles in
window pixels over their textures' pixels (`ui::DrawList::spans` says which control drew which
vertices), and what couldn't draw. `canvas::summarize` (`src/summary.cpp`) draws on the null
device with it, then samples each thing's pixels against what was drawn after it, by texture
alpha times vertex alpha, to say how much shows, and fills the map. A change to what the
renderer draws shows up in summaries with no other change. `examples/frame-summary` walks a
player behind a bush and off the screen.

`engine/canvas` is 2D drawing, as Godot's canvas. `Sprite2D` draws a texture, or a frame of a
sprite sheet, at the entity's `Transform2D`, in `z_index` order, then the order entities were
made. `Flipbook2D` plays a `.frames.json` file's animations on it through the
`animated_sprite_2d` system (`$cli schema frames`; `animation_finished` events). `canvas::install`
adds the asset types and the systems (that one, `camera_2d`, `parallax_2d`, `cpu_particles_2d` and
`gpu_particles_2d`) to a world;
sessions and games call it. `CanvasRenderer`
draws a world through a render device over the project's `rendering.clear_color`: games and
`slash-cli run` call `before_tick` before each tick and draw between ticks with
`Loop::interpolation()`, so sprites move smoothly at any frame rate. On the null device it checks
and counts sprites but draws nothing. `examples/sprites` plays and draws a scene's sprites. The
current `Camera2D` (`engine/world/src/camera2d.cpp`, Godot's `get_camera_transform` line for
line) picks what the window shows: the renderer draws the world through its canvas transform
(each draw's `render::Material::transform`) and controls over it in screen space, the
`camera_2d` system (priority 1050) moves its drag margins and smoothing on, and `frame.summary`
reports the world rect it shows. `examples/camera-2d` follows a player through a level.

Canvas layers follow Godot's `CanvasLayer` and `Parallax2D` (`engine/world/src/canvas_layer.cpp`).
`world::Canvases` lists a world's canvases in drawing order (by `layer`, the world first among
layer 0) and says which one each entity draws in: that of the `ScreenLayer` whose `children` hold
it or an entity above it, else the world's. `CanvasRenderer` draws each canvas's sprites, tiles
and particles, then its controls, through its transform (`canvas_layer_transform`, after the
camera's when `follow_viewport_enabled`), each lit by its own `AmbientTint2D` and the lights
whose `range_layer_min`/`range_layer_max` hold its layer; `ui::layout` places each
control by its canvas's transform (`Placed::canvas`, `DrawCall::canvas`), so hit tests follow.
The `parallax_2d` system (priority 1060) sets each `Parallax2D`'s position from the camera as
Godot's `_update_scroll`, and the renderer draws a repeating one's children again at each
repeat. `canvas::to_world` and `to_screen` (and `canvas.convert`) turn points between the screen
and a canvas. `examples/canvas-layers` scrolls hills behind a level under a HUD.

Textures are RGBA8, or BC7 and ASTC 4x4 blocks from `.ktx2` files (`render::TextureFormat`,
`src/texture_codec.cpp`). A device samples the formats `supports()` names and decodes the rest
with `decode_texels` (bc7enc_rdo's and astc-encoder's decoders, exact on every machine), so the
software device and GPUs draw the same pixels. `render::Image` keeps the decoded `pixels` for the
CPU and the file's `blocks`, which `CanvasRenderer` uploads where the device samples them.
`encode_texels` (`slash::texture_encoder`, tools and tests only) compresses texels.

Materials are `.material.json` node graphs (`$cli schema material`) that `Sprite2D`'s `material`
names and `material_params` sets. `engine/canvas/src/material.cpp` compiles a graph on load into
a `render::MaterialProgram` (`engine/render/include/slash/render/material.hpp`): at most 64
instructions over 32 registers, which `material.hlsl` interprets on the GPU (its ops are in
`material_ops.hlsl`, which particle graphs share) and `run_program` runs on the CPU for
`material.eval`. A new op goes in all three, with a case in the test that runs every op on both.
`examples/materials` has three materials.

A material's `"shader"` names a game's own `.hlsl` file with Godot's `fragment()` and `vertex()`
(the built-ins are in `docs/reference/scene-format.md`). `tools/shader_compiler` wraps it in
`engine/render/shaders/canvas.hlsl`'s entry points and compiles it with glslang, SPIRV-Cross
and Tint (from Dawn, built with only its SPIR-V reader and WGSL writer) for every backend and for `render::ShaderVm`, which the software device and `material.eval`
run. slash-cli, slash-mcp and exports link it and install it with `render::set_shader_compiler`;
games never do, and draw the `"compiled"` code their export wrote into each material file.
Renderers recompile a shader when its file changes. `examples/custom-shaders` has a dissolve and
a sway.

A material with `"shader_type": "spatial"` is Godot's spatial shader: its `fragment()` gives a
mesh's surface (`ALBEDO`, `ROUGHNESS`, `NORMAL_MAP`, `EMISSION`, `ALPHA` and the rest, Godot's
built-ins in view space), and `Model3D.material` draws it (`Renderer::override_material`
in `engine/scene3d/src/scene3d.cpp`, through the `ShaderMaterialSource` `CanvasRenderer` gives it).
The compiler puts the game's code, as `game_surface`, before `shaders/mesh.hlsl` with
`GAME_SHADER` defined, and compiles that file's Compatibility, Forward+ and prepass fragment
stages with it (`ShaderType::Spatial`, `CompiledShader::clustered`, `normal_roughness` and
`motion`). A `vertex()` becomes `game_vertex`, which the mesh, motion (with the frame before's
model and time, `SpatialUniforms::previous_model` and the time constant's y) and shadow vertex
stages call with `GAME_VERTEX` defined; `shaders/shadow.hlsl` takes `game_surface` too, to cut
out by `ALPHA_SCISSOR_THRESHOLD` (`CompiledShader::shadows_used`, `shadow_spatial_uniforms` in
`src/shadow.cpp`). Its uniforms (`render::SpatialUniforms`) are the fourth fragment uniform
buffer (the third vertex one) and its textures take the material's first five slots. The
software device runs `game_surface` with `ShaderVm` on 2x2 quads (`game_surfaces` in
`software_device.cpp`), so derivatives work, and `game_vertex` per corner (`game_corners`), and
`shade_mesh` takes the surface (`MeshPixel::game`); a change to how `mesh.hlsl` or `shadow.hlsl`
uses the shader goes in both. A `light()` becomes `game_light`, which `add_light` calls with
`GAME_LIGHT` defined in place of `light_compute`, once a light, with Godot's light built-ins in
view space (`CompiledShader::light_used`); on the software device `shade_mesh` calls
`MeshPixel::game_light`, which runs the `cpu_light` module (`game_surface`, then `game_light`)
for the pixel and light.
`tests/test_spatial_shader.cpp` holds every device to the software device's pixels;
`examples/spatial-shaders` has a crystal, a fence and a glass ball, and
`examples/custom-lighting-3d` cel-shades a ball with `light()`.

Tile maps follow Godot's TileSet and TileLayer2D (`tilemap.hpp` in `engine/canvas`). A
`.tileset.json` file (`$cli schema tileset`) cuts atlas textures into tiles and names terrain
sets; a terrain tile's `peering` names the terrain each side and corner must touch. A
`TileLayer2D` stores its cells as text: `origin`, a `legend` of short keys and `rows` of keys,
`.` for empty, which `decode_cells` and `encode_cells` read and write in canonical form.
`paint_terrain` picks connecting tiles for painted cells and their neighbors; the commands are
`tilemap.paint`, `tilemap.generate` (by noise), `tilemap.set` and `tilemap.get`. `CanvasRenderer` draws layers in runs of one
texture (`tiles_drawn()`), and `check_tile_layers` checks a scene's layers for `scene validate`
and exports. `examples/tilemaps` paints an island with a pond. An atlas source's
`normal_texture` and `specular_texture` are a Godot `CanvasTexture`'s, which lights shade its
tiles with (`material_of` in `renderer.cpp`). Tiles have Godot's physics layers,
collision polygons and alternative tiles, and a cell can turn its tile with `:h`, `:v` and `:t`
after the tile (`slope:h`, `rock:1:ht`), as Godot's cell transform flags; drawing and collision
turn together (`tile_look`, `tile_collision`). `examples/tile-collision` rolls balls on tiles.
A tile set's `tile_shape`, `tile_layout` and `tile_offset_axis` are Godot's: `map_to_local`,
`local_to_map` and `neighbor_cell` port Godot's `TileSet` functions line for line, peering
directions are 16 compass points (`Neighbor`) of which each shape has its own sides and corners
(`peering_kind`, `godot_neighbor` for Godot's bits), and cells draw by `draw_order`, down the
screen. A tile's `texture_origin` moves its texture as Godot's does (`TileLook::position`),
turning with the cell. `tilemap.convert` turns cells into points and back;
`examples/isometric-and-hex-tiles` paints diamonds and sets hexagons. Tiles span
`size_in_atlas` texture cells and play `animation_frames` laid out after them as Godot's
(`tile_region`, `tile_frame`, and `tile_animation_offset` for random start times), on
`CanvasOptions::time`; a tile set refuses tiles whose cells or frames overlap.
`examples/animated-tiles` has water, torches and a bridge two cells wide. Tile sets have
Godot's custom data layers (`TileSet::custom_data` gives a tile's value or the type's default;
`tilemap.get` with `cells` returns them) and occlusion layers: `collect_lighting` places each
cell's `tile_occluders` as `Occluder2D`s with the layer's `light_mask`, unless the layer's
`occlusion_enabled` is off. `examples/tile-data-and-occluders` has walls that cast shadows and
lava that hurts.

2D lights follow Godot's: `PointLight2D`, `SunLight2D`, `Occluder2D` and
`AmbientTint2D`, with `Sprite2D`'s `light_mask`, `normal_texture` and `specular_texture` and a
material's `light_mode`. `engine/render/include/slash/render/light.hpp` is the device side: a
frame's `render::Lighting` (up to 64 lights, a light atlas and a shadow map) and a draw's
`Material::shading` (its canvas's layer, ambient color and `to_lights`, which maps a canvas
layer's pixels into the world's, where lights are), shaded in `material.hlsl`; `render::shade` is its CPU twin, used by
`light.sample`, and a test compares the two pixel for pixel. A change to the light math goes in
both. `engine/canvas/src/light.cpp` turns a world's lights into that: `collect_lighting` casts
each shadowed light's occluder edges on the CPU into one row of `render::kShadowMapWidth` texels
(a point light's goes round it, a directional light's across it), places lights and occluders
in canvas layers into the world's canvas and gives each canvas its `CanvasLighting`, and
`pack_light_atlas` packs the light textures. `CanvasRenderer` uploads both only when they change (`lights_drawn()`,
`shadows_drawn()`). `examples/lights-2d` has a torch, the moon and a shadowed crate, and
`examples/lit-tiles-and-layers` a tile set's normal map and a HUD with its own light.

Meshes are the render device's 3D, as Godot's Compatibility renderer draws them
(`slash/render/mesh.hpp`): `create_mesh` takes `Vertex3D` corners (position, normal, tangent,
texture coordinate, color) and indices, a frame's `scene` (`render::Scene3D`: a camera, a
projection from `render::perspective` or `orthogonal`, the ambient light and up to 64 `Light3D`s,
Godot's directional, omni and spot lights) sees and lights them, and `draw_mesh` draws one with a
transform and a `Material3D`, Godot's `StandardMaterial3D`. Space is Godot's (Y up, a camera looks
down its -Z, front faces wind clockwise on the target) and depth is reversed. Meshes draw in a
pass of their own over a depth buffer, before the frame's 2D draws: opaque ones by depth layer,
mesh and then nearest first, `Alpha` ones after them farthest first (`mesh_draw_order`). `shaders/mesh.hlsl` lights them with
Godot's formulas, and `src/mesh_shading.hpp` fills its uniforms for every device and holds the
CPU twins of its two stages (`mesh_vertex`, `shade_mesh`), which the software device draws with;
`tests/test_mesh.cpp` holds every device to the software device's pixels, so a change to the 3D
shading goes in both. `examples/meshes-3d` draws a lit scene.

`Material3D`'s `rim`, `clearcoat` and `anisotropy` are `Surface3D`'s `rim_*`,
`clearcoat_*` and `anisotropy_*` fields, which Godot shades hair cards, skin and eyes with:
`mesh.hlsl`'s `light_compute` and its CPU twin in `src/mesh.cpp` add them as Godot's scene shader
does, from a `Layers` that `material_layers` reads out of lanes the lightmap and VoxelLighting3D uniforms
leave free (`MeshSurfaceUniforms::lightmap`'s z and w, `voxel_gi`'s y, z and w), so the surface
uniforms keep their size; the clearcoat also reflects the sky and anisotropy bends the normal
reflections take. Their textures aren't read yet (texture slots are full).
`examples/hair-and-eye-shading` lights a cheek, an eye and hair.

Alpha hash is Godot's `"alpha_hash"` transparency (`Transparency::AlphaHash`,
`Material3D::alpha_hash_scale`): `mesh_surface_uniforms` sets `kMeshAlphaScissor` and
`kMeshAlphaHash` with the scale in the threshold's lane, and `mesh.hlsl` and `shadow.hlsl` cut at
`alpha_hash_threshold` (Godot's `compute_alpha_hash_threshold` over the place on the mesh, a
varying of its own, and its `ddx` and `ddy`), whose noise is PCG3D of whole numbers in place of
Godot's sines. `detail::alpha_hash_threshold` in `src/mesh.cpp` is its CPU twin, which the
software device feeds from its quads' coarse derivatives (`MeshRaster::hash_scale`,
`MeshFragment::alpha_hash`); rays take the surface as alpha (`traced_surface`). A change to one
goes in both; `tests/test_alpha_hash.cpp` holds every device to the software device and
`examples/alpha-hash` fades a hair card. The OpenGL devices draw other meshes and shadows with
`mesh_plain.hlsl` and `shadow_plain.hlsl` (GLSL only, `NO_ALPHA_HASH`), linking the hashed
programs at the first draw that uses alpha hash (`alpha_hashed` in `src/gl_device.cpp`), since
Apple's software OpenGL renderer takes minutes compiling the mesh shader with it.

Subsurface scattering is Godot 4.6's Forward+ screen-space blur (`Material3D::subsurface`, from
`Surface3D`'s `subsurf_scatter_*`; project.json's
`rendering.subsurface`, `render::SubsurfaceSettings`). `src/subsurface.hpp`
holds what devices share (`uses_subsurface`, `draws_subsurface`, the uniforms) and the CPU twin of
`shaders/subsurface.hlsl`'s two passes (`run_subsurface_pass`, `run_subsurface`, Jimenez's
kernels as Godot's `subsurface_scattering.glsl`). After the opaque meshes, each device draws the
scattering meshes again with `mesh_subsurface.hlsl` (`mesh.hlsl` with `SUBSURFACE`: emission,
diffuse and ambient light and the signed strength) into an RGBA16F texture over the kept depth,
blurs it across and down, and adds the change to the world before the blended meshes; the
software device's `draw_color` does the same. The transmittance is `light_compute`'s, with the
depth through the mesh from the light's shadow map (`shadow_thickness`, `ShadowAtlas::depth`) by
`Light3D::shadow_transmittance_bias`. A change to either goes in both. `tests/test_subsurface.cpp`
holds every device to the software device; `examples/subsurface-scattering` lights a wax candle
and an ear.

`engine/scene3d` puts Godot's 3D nodes in scenes. `Transform3D` (position, Euler rotation in
radians in Godot's YXZ order, scale, `visible`, `top_level`, `children`) makes trees that
`world::Tree3D` (`slash/world/tree3d.hpp`) walks, as `Tree2D` walks 2D ones. `Model3D`
draws its entity's primitive mesh component (`BoxGeometry`, `SphereGeometry`, `CylinderGeometry`,
`CapsuleGeometry`, `PlaneGeometry`, `QuadGeometry`, `PrismGeometry`, `TorusGeometry`, made as Godot's
`primitive_meshes.cpp` makes them, in `src/primitive.cpp`) with its `Surface3D`, seen
through the current `Camera3D` and lit by `SunLight3D`, `PointLight3D` and `SpotLight3D`,
over a `Environment3D`'s background and ambient light. `scene3d::Renderer` turns a world into
a frame's `Scene3D` and mesh draws, keeping one device mesh per set of primitive settings and
interpolating transforms between ticks; `CanvasRenderer` owns one and draws the 3D world under
every canvas, so games, `slash-cli run` and `frame.capture` draw it. `check_meshes` backs
`scene validate` and exports. Mesh building and cameras use `slash::cos_sin` and build with
`-ffp-contract=off`, so meshes are the same bits everywhere. `examples/scene-3d` is a lit
table.

Shadows are Godot's Compatibility renderer's shadow maps. A `Light3D` with `shadow` set takes a
slot (4 directional, 16 omni and spot), and `draw_mesh`'s `ShadowCasting` says whether a mesh
casts (`ShadowsOnly` casts without being drawn). At `end_frame` each device asks
`plan_shadows` (`src/shadow.cpp`) for the frame's passes: a directional light's cascades fitted
to slices of the view as Godot's `RendererSceneCull` fits them, in its share of the directional
atlas, and an omni light's six cube faces or a spot light's one view in tiles of the positional
atlas. It draws each pass with `shaders/shadow.hlsl` into a depth atlas
(`Scene3D::shadows`, from `rendering.shadows`), then the forward pass samples them
with a comparison sampler in `mesh.hlsl`, filtered by the soft shadow quality. `shadow_vertex`
and `ShadowAtlas::compare` are the CPU twins, and `tests/test_shadow.cpp` holds every device to
the software device's pixels. `Model3D.cast_shadow` and the lights' `shadow_*` fields
are Godot's; `examples/shadows-3d` is a courtyard with three shadowed lights.

Post-processing follows the scene's renderer. `Scene3D::post` (`render::PostProcess`, from
`Environment3D`'s `tonemap_*`, `glow_*` and `adjustment_*` fields) picks a tonemapper (linear,
Reinhard, filmic, ACES, AgX), the exposure and white, glow, and brightness, contrast, saturation
and a lookup texture. `color_stage` (`src/post.cpp`) tells `mesh.hlsl` and `sky.hlsl` where their
colors go: linear for Forward+, untonemapped for Compatibility's post pass, or tonemapped as they
draw.

Forward+ is Godot 4.6's: each device draws the sky and meshes into an RGBA16F texture cleared to
`forward_clear`, then runs `shaders/forward_post.hlsl`'s passes at `end_frame`: glow's levels (up
to seven, each half the last's size, as many as `forward_glow_plan` says), then the tonemap pass
over the target, before the 2D draws. `src/forward_post.hpp` holds the uniforms and the CPU twins
(`run_forward_glow_pass`, `run_forward_tonemap_pass`, `round_half`); the software device keeps its
world as half floats too, and `tests/test_forward_post.cpp` holds the GPU to its pixels. The
OpenGL devices always draw as Compatibility. `examples/glow-3d` is neon signs at night.

Auto exposure is Godot 4.6's: `CameraLens` (the camera's, else the
`Environment3D`'s; `scene3d::camera_attributes`) sets `PostProcess::auto_exposure_*`, and
its `exposure_multiplier` scales lights, emission, the background and the sky in
`scene3d::environment` and `Renderer::prepare`. Forward+ devices measure each frame with
`forward_post.hlsl`'s luminance passes (`luminance_plan`, `run_luminance_pass`) into R32F
textures down to a 1x1 brightness they keep between frames, moving `auto_exposure_adjust` of the
way each frame, which `prepare` works out from game time; `auto_exposure_version` starts the
measure over. `auto_exposure_metering`'s `"log_average"` (SlashEngine's own, `auto_exposure_log`,
`options.y` in the luminance passes) averages the brightness's natural logarithm instead, as
Unreal meters, which the Unreal importer sets. `examples/auto-exposure-3d` walks out of a tunnel.

Depth of field is Godot 4.6's too: `CameraLens`'s `dof_blur_*` fields set
`PostProcess::dof_*`, and project.json's `rendering.depth_of_field` the bokeh shape,
quality and jitter (`render::DepthOfFieldSettings`). Forward+ devices keep the depth buffer and
run `forward_post.hlsl`'s depth of field passes before glow, as `dof_plan` lists them: blur sizes
from depth, the box, hexagon or circle blur, and the composite into the world. `run_dof_size_pass`,
`run_dof_blur_pass` and `run_dof_composite_pass` are the CPU twins. `examples/depth-of-field-3d`
racks focus across a garden.

SSAO is Godot 4.6's Forward+ SSAO (Intel's ASSAO): `Environment3D`'s `ssao_*` fields set
`Scene3D::ssao` (`render::Ssao3D`), and project.json's `rendering.ssao` its quality,
size, blur and fade (`render::SsaoSettings`). Forward+ devices draw the opaque meshes' depth and
normals in a prepass (`mesh_normal.hlsl`), then `ssao.hlsl`'s passes as `ssao_plan` lists them
(`src/ssao.hpp`): linear depth in four deinterleaved slices, the gather (ultra's base pass,
importance map and adaptive gather), the edge-aware blur and the interleave; Direct3D 12 makes
a shader for each group of them (`kSsaoShaderGroups`), since WARP loses the device running one
with every pass in it. `mesh.hlsl` applies the result as Godot's scene shader does. The passes load texels and filter in code, so
`run_ssao_pass`, their CPU twin, matches every device. `examples/ssao-3d` darkens a room's
corners.

SSR is Godot 4.6's Forward+ screen-space reflections: `Environment3D`'s `ssr_*` fields set
`Scene3D::ssr` (`render::Ssr3D`), and project.json's `rendering.ssr`
its size (`render::SsrSettings`). After the same prepass, Forward+ devices run `ssr.hlsl`'s passes
as `ssr_plan` lists them (`src/ssr.hpp`): the depth (halved at half size) and a Hi-Z pyramid, the
trace (`ssr_trace.hlsl`, the same file with two targets) reading the frame before, the filtered
levels and the resolve. Each level is drawn into a texture of its own and copied into one with
mipmaps. Each target keeps its frame before at SSR's size (`ssr_last_frame`), and its first frame
with SSR is drawn once without it. `mesh.hlsl` puts the result over the sky's reflections as
Godot's scene shader does. `run_ssr_pass` is the CPU twin. `examples/ssr-3d` reflects a room in a
polished floor.

Reflection probes are Godot 4.6's Forward+ `ReflectionProbe`. `scene3d::Renderer::reflect` sorts
a frame's probes largest first, gives each a cube of one atlas (a texture made with
`TextureDesc::cubes`, sized by project.json's `rendering.reflections`) and draws
a probe's six faces as frames whose `Scene3D::capture` (`render::ProbeCapture`) names the cube and
face, with `probe_face_view` and `probe_face_projection`. At the end of a capture's sixth face the
device filters the cube's levels with `radiance.hlsl`, as it does the sky's. `Scene3D::reflections`
(`render::Reflections3D`) lists the probes a frame reflects; `mesh.hlsl` runs Godot's
`reflection_process` over them, and `src/reflection.cpp` holds the checks, the uniforms and its
CPU twin, `reflect_probes`. SSAO and SSR share one two-layer screen texture the mesh pass reads.
`examples/reflection-probes-3d` reflects a lit room in a metal floor.

Baked lightmaps are Godot 4.6's `BakedLighting3D` (`slash/scene3d/lightmap.hpp`). `bake_lightmap`
gathers a BakedLighting3D's static meshes and baking lights (`src/lightmap.cpp`), unwraps each mesh
with xatlas (`unwrap_lightmap`; `src/xatlas_build.cpp` builds xatlas and stb_rect_pack with the
engine's sines and a stable sort, so charts are the same everywhere), then `src/lightmapper.cpp`
packs them into an atlas's slices and traces them on the CPU, following Godot's `LightmapperRD`
and its `lm_compute.glsl` pass by pass (direct light with soft shadows, bounces, the environment,
JNLM denoising, seams) on the job pool; results don't depend on the thread count. With
`LightmapSettings::device` (`lightmap.bake`'s `device` `"gpu"`), the direct light, bounces and
probes are traced by `RenderDevice::trace_lightmap` (`slash/render/bake.hpp`) instead: on Vulkan
with ray queries, `shaders/lightmap_light.comp` ports those three passes with the same random
numbers over one structure of the bake's triangles, a dispatch per region of 512x512 texels or
pass of rays, waited for so none runs long; a change to those passes goes in both. A
`.lightmap.json` file (`$cli schema lightmap`) is its `LightmapGIData`, with the texels in an
`.hdr` beside it; `lightmap.bake` (`engine/command/src/lightmap.cpp`) writes both. `scene3d::Renderer::find_lightmaps` loads them, gives each lightmapped mesh its UVs (`uv2`)
by the unwrap stored for its mesh's hash, and draws it with `Material3D::lightmap`
(`render::LightmapShading`, texture slot 10); `mesh.hlsl`'s `lightmap_light` samples it
bicubically, and `render::detail::lightmap_light` is its CPU twin. Light probes for moving meshes
are in `src/lightmap_probes.cpp`, each part a port of Godot's: `place_lightmap_probes`
(`LightmapGI::bake`'s octree), `tetrahedralize` (`Delaunay3D`, with an exact in-sphere test on
Shewchuk's expansion arithmetic so probes in flat layers don't overlap and tetrahedra are the same
everywhere), `lightmap_bsp_tree`,
`tap_lightmap_probes` (`lightmap_tap_sh_light`), `rotate_sh` and `lightmap_capture`
(`_update_instance_lightmap_captures`); the lightmapper traces the probes after the bounces. The
renderer eases each `"dynamic"` mesh's capture and draws it with `Material3D::capture`, which
`mesh.hlsl`'s `capture_light` and its CPU twin evaluate per pixel. `examples/lightmaps-3d` bakes a
courtyard.

VoxelLighting3D is Godot 4.6's (`slash/scene3d/voxel_gi.hpp`, `src/voxel_gi.cpp`). `voxelize` follows
Godot's `Voxelizer` (an octree down to `subdiv` cells, each with albedo, emission and normal), and
`voxel_lighting.bake` (`engine/command/src/lightmap.cpp`) writes the `.voxellight.json` file
(`$cli schema voxel_light`), Godot's `VoxelGIData`. `light_voxel_gi` injects lights on the CPU on the
job pool, following `voxel_gi.glsl`'s passes (direct light with shadows marched through the
distance field, mipmaps, the second bounce), and `voxel_gi_atlas` lays its 3D texture's levels
out as z slices in one RGBA8 atlas. `plot_voxel_gi_objects` plots moving meshes into that light as
Godot's dynamic objects (a material pass from six sides at 16 times the cells, lit per pixel, then
shrunk and blended into each level), and `voxel_gi_meshes` reads an entity's meshes for it and the
bake. `scene3d::Renderer::trace_voxel_gis` injects again when the lights or settings change, plots
the `"dynamic"` meshes in each box again whenever they move, stacks every VoxelLighting3D's atlas into one
texture and gives each draw the
VoxelGIs its bounds touch as `Material3D::voxel_gi` (`render::VoxelGIShading`, texture slot 5);
`mesh.hlsl`'s `trace_voxel_gis` follows `gi.glsl`'s `voxel_gi_compute` with manual trilinear
sampling of the slices, and `render::detail::trace_voxel_gis` (`engine/render/src/voxel_gi.cpp`)
is its CPU twin. `examples/voxel-gi-3d` bounces a sun around a room.

SDFGI is Godot 4.6's fully dynamic GI, a research spike on the CPU
([docs/reference/sdfgi.md](sdfgi.md)): `Environment3D`'s `sdfgi_*` fields set
`Scene3D::sdfgi` (`render::Sdfgi3D`), and the software device, after the prepass, keeps a
`detail::SdfgiState` per target (`src/sdfgi.cpp`, after `gi.cpp`'s `GI::SDFGI` and its shaders):
`update_sdfgi` moves the cascades with the camera (a moved one keeps the cells it still holds
while the triangles stay the same), makes the cells that came in or changed from the path
tracer's scene (`path_surface_at`, summed in fixed point) with an exact distance field (an
integer Euclidean distance transform; `SdfgiCascade` keeps cells, field, light, occlusion and
probes as the 32-bit words a GPU's buffers hold, and every step that sums or rounds does so in
whole numbers, so a compute shader can make the same bits) (and, with
`field_lighting_use_occlusion`, `make_sdfgi_occlusion`'s flood from each probe), lights the cells
through it and has the probes trace their rays into spherical harmonics history, then
`run_sdfgi` (`gi.glsl`'s `sdfgi_process`) gives each pixel its ambient light and reflections as
`MeshMaps::sdfgi`, which `shade_mesh` puts under VoxelGIs' light. The GPU device (Vulkan, Metal,
Direct3D 12) and the WebGPU device run the same frame in compute (`src/sdfgi_gpu.hpp`: the buffers' layout, the items
buffer and `sdfgi_steps`, the dispatches in update_sdfgi's order): `shaders/sdfgi_voxelize.hlsl`
sums each draw's triangles into the cells with atomic adds, `shaders/sdfgi.hlsl` runs the other
steps as passes, and `sdfgi_gather.hlsl` writes the screen texture's layers 5 and 6, which
`mesh.hlsl` reads when `scene_voxel_gi.z` is set; a change to the CPU twin goes in the shaders
too. OpenGL draws without it.
project.json's `rendering.field_lighting` (`render::SdfgiSettings`) sets every frame's
probe rays and how fast it follows changes.
`examples/sdfgi-3d` lights a room with it.

Path tracing makes reference stills on the CPU (`Scene3D::path_trace`, `render::PathTrace3D`;
`frame.capture`'s `path_trace`). The software device, at `end_frame`, has `build_path_scene`
(`src/path_trace.cpp`) put the frame's triangles in the world into a bounding volume hierarchy
(median splits, ties by triangle index) and `trace_paths` trace them on the job pool: each path
shades what it meets with `mesh.hlsl`'s CPU twins (`path_surface`, `light_response` and
`light_reach` in `src/mesh.cpp`), adds each light it sees, traces shadows by ray for lights with
`shadow`, and bounces by a mix of the diffuse lobe and GGX's visible normals, taking the sky
(`sky_ray_toward`, `shade_sky`) or the ambient light where it leaves. Random numbers hash the
pixel, the sample's number and the bounce, so a still is the same on any number of threads and
samples summed over frames match those traced at once. Each target keeps its sums
(`path_samples`) while `path_scene_key` and its textures' write counts stay the same, and
forward_post runs over the result. `CanvasRenderer` turns off TAA, FSR 2, occlusion culling and
LODs when tracing. On Vulkan with ray queries (see Ray tracing below) the GPU device traces them
too: `begin_frame` makes a path-traced frame Forward+ without its screen-space and ray-traced
effects, `draw_paths` draws the sky into the world texture, copies it as the background, and runs
`ray_light.comp`'s passes 3 and 4 (`trace_paths`' twin, its random numbers counted the same way,
summing into a float4 storage buffer per target, then averaging over the background) before the
post passes; it keys the sums by `path_scene_key` and a hash of the draws. `display.set`'s
`path_trace` (`LiveGame::path_trace`) path traces a running game's window, and
`CanvasRenderer::path_samples` reports its sums. `examples/path-tracing` traces the lightmaps room.

ReSTIR direct lighting is a research spike on the software device
([docs/reference/restir.md](restir.md)): with `rendering.ray_tracing.many_lights`
(`RayTracing3D::many_lights`), after the prepass `run_restir` (`src/restir.cpp`, built with
`-ffp-contract=off`) keeps `many_light_samples` reservoirs a pixel of one omni or spot light
each: `many_light_candidates` drawn from the pixel's cluster (`LightClusters::sources` names
their scene lights) and resampled by their unshadowed light, last frame's reservoir where the
surface was (`RestirState`, kept per target with the camera), then three neighbors' on the same
surface, with a shadow ray (`path_blocked`) to the light kept for lights with `shadow`.
`shade_mesh` lights the pixel with those lights, weighed by their reservoirs, in place of its
cluster's (`MeshMaps::restir`). Other devices light every light as before.
`examples/many-lights` lights a hall with 64 shadowed lamps.

Gaussian splats are a research spike on the software device
([docs/reference/gaussian-splats.md](gaussian-splats.md)): a `Splat3D`
names a 3D Gaussian Splatting `.ply` file (`render::read_splat_ply` and `write_splat_ply` in
`engine/render/src/splats.cpp`, loaded as an asset by `scene3d::add_splat_type`), which
`scene3d::Renderer::add_splats` puts in `Scene3D::splats`; in Forward+ the software device's
`draw_color` blends them after the opaque meshes and subsurface scattering and before the blended
meshes with `detail::run_splats` (EWA projection through the view projection's Jacobian, colors
from spherical harmonics, sorted nearest first, tiles of 16 pixels on the job pool, tested against
the opaque depth; built with `-ffp-contract=off`). Other devices draw none, and splats are
neither lit nor in the depth buffer. `examples/gaussian-splats` places a boulder made in code.

`lighting.check` (`engine/command/src/lighting.cpp`) judges a 3D or 2D frame's lighting as
numbers for agents: it draws the frame with `canvas::capture`, measures its exposure (mean,
percentiles, clipped and crushed shares), draws it again with each light's energy at 0 (put back
after) to measure what that light lights, and names problems such as a light that lights nothing
in view. `examples/lighting-check` fixes a flood light and a lamp out of view. `light.tune`
(same file) scales lights toward a mean brightness, a clipping limit and per-light shares,
learning from each step how brightness follows energy and bisecting once it has seen too dark
and too bright, with one undo step. Physical light units are Godot's
(`rendering.physical_light_units`, `scene3d::physical_light_units`):
`render::color_temperature` and the unit factors (`engine/render/src/light_units.cpp`, built
with `-ffp-contract=off`) tint and scale lights in `light_base` (`engine/scene3d/src/scene3d.cpp`)
and in the lightmap and VoxelLighting3D bakes, with the background's and emission's nits and the
camera's exposure normalization; light.tune then scales lux and lumens. 2D lights' `temperature`
is SlashEngine's own (`fill_common` in `engine/canvas/src/light.cpp`). Light projectors are
Godot 4.6's Forward+ `light_projector`: `scene3d::Renderer::add_projectors` loads each omni and
spot light's texture into `render::Light3D::projector`, baking a `light_ies` profile
(`slash/render/ies.hpp`, `src/ies.cpp`: LM-63 files, `bake_ies_spot` and `bake_ies_omni`, the
same bytes everywhere) into it and scrolling it by `light_projector_scroll` (`projector_offset`);
the decal atlas holds the textures (`decal_images(const Scene3D&)`), `build_clusters` appends a
record per projector after the decals (`projector_record`, `LightClusters::decal_count`) and
marks its place in the light's `shadow[1]` above the baked bit, and `mesh.hlsl`'s
`projector_light` tints the light, `detail::projector_light` its CPU twin; a change to one goes
in both. A 2D light's `texture_scroll` rolls its texels as the atlas is packed
(`pack_light_atlas`, `light_atlas_key`). `examples/light-tools` tunes a dusk street and a 2D
alley and casts a street lamp's IES profile and a stained glass window.

`Reflection2D` is SlashEngine's own 2D reflection (Godot draws water with a screen texture
shader): `CanvasRenderer::draw` keeps a render target per reflection
(`reflection_targets_`, the rect's size in pixels) and draws its rect in strips rippled
sideways and faded by their vertex colors, as an item among its canvas's; `submit` first
draws each one's `ReflectionPass` into its target, the items of its canvas that come before
it (`reflected`) through a view turned upside down about its top edge, with the frame's
lights, which shade the positions as given. It needs nothing of a device, so every device
draws it the same. `examples/reflections-2d` mirrors a pond's sky and boat.

Ray tracing is SlashEngine's own (Godot 4.6 has none): project.json's `rendering.ray_tracing`
(`render::RayTracing3D`, `Scene3D::ray_tracing`) turns on ray-traced shadows in Forward+ on
devices whose `RenderDevice::ray_tracing()` says so: the software device, and the GPU device on
Vulkan where the GPU has ray queries, on Metal where it supports ray tracing and on Direct3D 12
where it has DXR 1.1's inline ray queries.
`ray_shadow_slots` (`src/shadow.cpp`) gives the first `kMaxRayShadowLights` (8) shadowed lights a
slot (`MeshLightUniforms::shadow.w`, slot + 1). After the prepass, `run_ray_shadows`
(`src/ray_shadows.cpp`, built with `-ffp-contract=off`) puts each pixel back in the world from its
depth and normal and traces `shadow_samples` rays a light through the path tracer's hierarchy
(`path_blocked`), spread over the light's disk by `Light3D::size`, into `MeshMaps::ray_shadows`, which
`ray_shadow` in `src/mesh.cpp` reads in place of the shadow maps. `mesh.hlsl`'s `add_light` is its
twin, reading them from the screen texture's layers 2 and 3 (four lights a layer) when `screen.z`
is set. On Vulkan, `create_gpu_device` asks SDL for `VK_KHR_acceleration_structure` and
`VK_KHR_ray_query` (`VulkanRayOptions`; `cmake/patches/sdl-ray-tracing.cmake` chains their
features, which SDL's options don't take) and falls back to a device without them, and
`src/vulkan_rays.cpp` records into SDL's command buffer with Vulkan's own calls: a bottom-level
structure per mesh and LOD from copies of its SDL buffers (kept while `Mesh::version` stays), a
top-level one a frame and `shaders/ray_shadows.comp` (GLSL with `GL_EXT_ray_query`, as glslang's
HLSL has none; `cmake/compile_ray_shaders.cmake` writes `ray_shadows_shaders.hpp`), whose two
textures the GPU device copies into the screen's layers 2 and 3. `ambient_occlusion` traces the
environment's SSAO instead (`ray_occlusion` and `run_ray_occlusion` in `src/ray_shadows.cpp`:
`ambient_occlusion_samples` cosine-weighted rays a pixel up to `ssao_radius`, made occlusion with
Godot's intensity and power), into the screen's layer 0 where SSAO's goes; the compute shader
writes it to a third texture. A change to the ray math goes in both.
`reflections` traces the environment's SSR (`run_ray_reflections`: rays reflected about GGX's
visible normals, lit by `path_reflection` in `src/path_trace.cpp` as a path's bounce), into the SSR
layer. On Vulkan `shaders/ray_light.comp` is its twin: every draw is an instance (mask 1 for rays
from the camera's side, 2 for shadow rays), each with a `RayDrawRecord` of its material, its
textures' places in an array of 64 and the device addresses of the vertex and index copies its
structure was built from (`GL_EXT_buffer_reference`), and it lights hits as `path_reflection`
does, the sky from the radiance cube; the GPU device copies its texture into the screen's layer 1. `global_illumination` traces each
pixel's ambient light there too (`run_ray_gi`: cosine-weighted rays lit by `path_reflection` or
`path_environment`, smoothed by normal and plane), which `MeshMaps::gi` hands the shading when
`screen.w` is set; on Vulkan `ray_light.comp` traces and smooths it in two more pipelines (its
`kPass` specialization constant) into the screen's layer 4, which `mesh.hlsl` reads then.
On Metal (`src/metal_rays.mm`, macOS 13 or later, where `supportsRaytracing`) the same shaders
run as MSL that `slash-ray-msl` (`tools/shader_compiler/src/ray_msl.cpp`) makes from the GLSL
with glslang and SPIRV-Cross into `*_msl.hpp`, set 0 an argument buffer whose ids by binding it
writes beside it (`k<Name>MslIds`) and the prepass's depth a `depth2d`; Metal's encoders build the
structures (user ID instances) and dispatch into SDL's command buffer, reaching mesh copies by
`gpuAddress` (`SDL_SlashGPUNativeBuffer`, which the patch adds for Metal and Direct3D 12 too).
On Direct3D 12 (`src/d3d12_rays.cpp`, ray tracing tier 1.1 and shader model 6.5) they run as
inline ray queries: `slash-ray-msl --hlsl` compiles the GLSL with `SLASH_D3D12` (the meshes an
unbounded array of buffers, an address's x its place there, as HLSL has no buffer addresses) into
shader model 6.5 HLSL in `*_hlsl.hpp` with its root signature (a descriptor table of views, then
one of samplers, places by binding in `k<Name>HlslSlots`), DXC compiles each `kPass` to signed
DXIL in `*_dxil.hpp` (see Shaders), and the rays bind heaps of their own around their dispatches,
mark each frame's slot done with `WriteBufferImmediate` and put SDL's heaps back with
`SDL_SlashGPURestoreState`. The backends implement `GpuRays` (`src/gpu_rays.hpp`) and pack the
same uniforms and records (`src/ray_records.cpp`). `tests/test_ray_tracing.cpp` holds Vulkan,
Metal and Direct3D 12 to the software device, `tests/test_ray_records.cpp` the MSL and HLSL to
their GLSL and the DXIL to its HLSL, `examples/ray-traced-shadows-3d` traces an avenue, `examples/ray-traced-ao-3d` a room and
`examples/ray-traced-reflections-3d` a hall's floor and `examples/ray-traced-gi-3d` a room's walls.

TAA is Godot 4.6's `use_taa` (project.json's `rendering.anti_aliasing.taa`,
`Scene3D::use_taa`), in Forward+ only. `src/taa.hpp` holds what devices share: the jitter
(`taa_jitter`, Halton bases 2 and 3 over 16 frames, `jittered`), the mesh passes' camera with the
frame before's (`taa_camera_uniforms`) and the resolve's CPU twin, `run_taa`. `scene3d::Renderer`
gives each draw whose transform changed since the frame before its old one (`draw_mesh`'s
`previous`), and devices keep a skinned mesh's vertices from before its last `update_mesh`.
The prepass then draws with `mesh_motion.hlsl`, writing motion vectors to a second target. After
the meshes, `taa.hlsl` (Godot's `taa_resolve.glsl`) blends the frame with each target's history,
kept by the device. A frame without meshes starts it again. `examples/taa-3d` smooths a turning
card's edges.

FSR 2 is Godot's `scaling_3d` mode `fsr2` (`render::uses_fsr2`, `Scene3D::fsr2`), in Forward+
at a scale below 1 on every device but OpenGL; elsewhere the world scales with `bilinear`.
Devices draw the meshes at `fsr2_render_size` with a jitter over `fsr2_phase_count` frames
(`camera_jitter`) and motion vectors, then run AMD's FSR 2.2 passes, ported from its HLSL with
the notice kept: `fsr2_luma`, `fsr2_dilate`, `fsr2_reconstruct` (compute), `fsr2_depth_clip`,
`fsr2_lock`, `fsr2_accumulate` and `fsr2_rcas` in `engine/render/shaders/`. `src/fsr2.hpp` holds
what devices share, with `run_fsr2`, the passes' CPU twin, which `tests/test_fsr2.cpp` holds
every device to. Each target keeps its own history, and post-processing runs at the window's
size after. The prepass clears FSR 2's motion target to `kInvalidMotion`, and where it stays so
(the sky) the passes work the motion out from the depth with `Fsr2Uniforms::reprojection`, as
Godot's `derive_motion_vector`; the sky draws through TAA's or FSR 2's jitter, as Godot's.
`examples/fsr2-3d` rebuilds a card at twice its render size.

FSR 3 is the upscaler of AMD's FSR 3.1 (FidelityFX SDK 1.1.4), the scaling mode `fsr3`
(`Fsr2Scaling::fsr3`, `uses_fsr3`), which Godot doesn't have. It runs where FSR 2 runs, from the
same jittered frame, and ports AMD's passes as fragment passes and one compute pass:
`fsr3_prepare`, `fsr3_reconstruct`, `fsr3_pyramid` and `fsr3_reduce` (the shading change pyramid,
a level a pass, where AMD's downsampler makes them in one), `fsr3_shading_change`,
`fsr3_reactivity`, `fsr3_lock` (a gather where AMD's scatters), `fsr3_instability` and
`fsr3_accumulate`, then FSR 2's `fsr2_rcas`. `src/fsr3.hpp` holds the sizes, the uniforms (FSR 2's,
then the frame before's jitter) and `run_fsr3`, the CPU twin, which `tests/test_fsr3.cpp` holds
every device to; a change to one goes in both. `examples/fsr3-3d` rebuilds, turns and recolors a
card.

Frame generation is the frame interpolation and optical flow of AMD's FSR 3.1 (FidelityFX SDK
1.1.4), which Godot doesn't have. A Forward+ frame with `Scene3D::frame_generation`
(`uses_frame_generation`) draws motion vectors in its prepass (without a jitter unless TAA or FSR
2 adds one) and keeps them with its depth through AMD's prepare pass
(`prepare_frame_generation`); `RenderDevice::generate_frame(target, into)` then writes into
`into` the frame halfway between `target`'s last two: the optical flow's luma pyramid, scene
change detection and block search (`run_optical_flow`), the game's and the flow's vector fields,
the disocclusion mask, the blend of the two frames and the inpainting (`run_frame_generation`).
`src/frame_generation.hpp` holds the sizes and the CPU twin, which the software device runs, with
AMD's notice; a frame that drew no meshes for it (2D, or no scene) is moved only as its camera
turned, so the optical flow carries it. As AMD's, it goes by the game's vectors until ten frames
after a reset or scene change. It differs from AMD's where AMD's passes race (the game's field
takes every primary vector before any secondary one, the scene change pass reads its histograms
before clearing them, and pyramids are made a level a pass), so every device can make the same
pixels. The GPU device (Vulkan, Metal, Direct3D 12) and the WebGPU device run the passes as
compute dispatches of `shaders/frame_generation.hlsl`, the CPU twin's port, over two storage
buffers per target (`frame_generation_layout`: the frames, pyramids and fields as words, the
fields written by atomic max as atoms), in the order `frame_generation_prepare_plan` and
`frame_generation_plan` give, and `frame_generation_draw.hlsl` draws the result into `into`; a
change to the twin goes in the shader too. `tests/test_frame_generation.cpp` moves a card, a
picture and blocks, and holds every device to the software device's frames;
`examples/frame-generation-3d` slides a card past a post. Games show the frames with
project.json's `rendering.frame_generation` (`canvas::Settings::frame_generation`, `display.set`'s
`frame_generation`): with `CanvasOptions::generate`, `CanvasRenderer` draws the world and the
canvases below it into a target of its own (`scene3d::Renderer::set_frame_generation` gives its
meshes their transforms from the frame before), generates from it, and draws the generated frame,
then the drawn one, into the window with the controls, the canvases above and the overlay over
both; `CanvasOptions::between_frames` paces them, which `runtime::Game` and `slash-cli run` do
with `platform::Loop::wait_half_frame` where V-Sync doesn't. `tests/test_generated_frames.cpp`
reads the frames shown.

MetalFX is Godot's `metalfx_spatial` and `metalfx_temporal`, after its `metal_fx.mm`: Apple's
scalers run in the GPU device on Metal (`src/metalfx.mm`, `metalfx_none.cpp` elsewhere), encoded
into SDL's command buffer through the calls `cmake/patches/sdl-native-handles.cmake` adds to
SDL (`src/native.hpp`), with MetalFX linked weakly so games still start on macOS 11 and iOS 14.
`RenderDevice::metalfx()` says which scalers a device has and `render::scaling_mode_on` falls
back as Godot does where it lacks one. The spatial scaler is a built-in `render::Upscaler`
(`metalfx_spatial`, into a texture made `scaled_into`); the temporal one takes FSR 2's jittered
frame, depth and dilated motion (`upscale_metalfx` in `gpu_device.cpp`). The software device has
neither, so tests see the fallbacks; only a Mac draws them.

A game's own temporal upscaler (DLSS, XeSS or FSR 4 from an SDK package) is an `UpscalerInfo`
with `temporal` set and `make_temporal`; `temporal_upscaler_on` picks it where FSR 2 would run,
and `Fsr2Scaling::upscaler` names it to the device. The GPU device runs FSR 2's dilate pass, then
hands the upscaler a `render::TemporalFrame` of native objects (`upscale_external` in
`gpu_device.cpp`, through `src/native.hpp`) and puts back the Direct3D 12 descriptor heaps after
it (`SDL_SlashGPURestoreState`); the software device hands it float pixels
(`upscale_temporal` in `software_device.cpp`, motion from `run_fsr2_dilate`).
`tests/test_temporal_upscaler.cpp` checks both, copying with Vulkan or Direct3D 12 commands
where those run. `examples/temporal-upscaler` adds one.

Decals are Godot 4.6's Forward+ `Decal` (`render::Decals3D`, `Scene3D::decals`; the `Decal`
component, gathered by `scene3d::Renderer::add_decals`). `src/decal.hpp` holds what devices
share: `pack_decal_atlas` packs a frame's textures as Godot's `update_decal_atlas` (skyline
layout, 5 averaged levels) into a storage buffer the shader filters itself, so every device reads
the same texels; `place_decals` orders them as Godot's `update_decal_buffer`; `build_clusters`
gives each tile a decal section after its lights'; and `apply_decals` is the CPU twin of
`mesh.hlsl`'s decal loop. A change to the decal math goes in both. `check_decals` caps the atlas
at `kMaxDecalAtlasTexels` (2^25, WebGPU's 128 MiB storage binding); `add_decals` drops and reports
those past it. Compatibility draws none. `examples/decals-3d` paints a street.

Volumetric fog is Godot 4.6's Forward+ `volumetric_fog_*` (`render::VolumetricFog3D`,
`Scene3D::volumetric_fog`, from `Environment3D`; the volume's size from project.json's
`rendering.volumetric_fog`). `src/volumetric_fog.hpp` holds what devices share: the
froxel volume's size as Godot's `_update_volumetric_fog` (`fog_volume_size`), the uniforms and
the CPU twins of `shaders/volumetric_fog.hlsl`'s compute passes (`run_volumetric_fog`: each
froxel's density and light from the lights, their shadows, the clusters and the sky's radiance,
blended with the frame before's reprojected; the blur across and down with `use_filter`; the
gathering along each column by Beer's law). The volume is a storage buffer of a float4 a froxel
rather than Godot's 3D textures, since the mesh shader's samplers are taken: `mesh.hlsl` reads it
and filters it itself (`sample_fog_volume`), and `volumetric_fog_sky.hlsl` draws the sky through
its last slice by `sky_affect` (`fog_sky`). Each target keeps its density for the next frame
(`FogClock`); probe captures and frames without meshes have none, and Compatibility draws none.
A change to the fog math goes in both. `examples/volumetric-fog-3d` lights a misty courtyard.
`FogVolume`s and their `FogVolumeLook`s are Godot's (`render::FogVolume3D` in
`VolumetricFog3D::volumes`, gathered by `scene3d::Renderer::add_fog_volumes`, up to
`kMaxFogVolumes`): `fog_volume_froxels` picks each one's froxels as Godot's
`volumetric_fog_update` does, and the density pass (`fog_volumes` in the shader and its twin)
adds each one's shape, material and Godot's default material's cull in Godot's fixed point.
`examples/fog-volumes-3d` fills a cellar. The density pass also takes the light of the first
`kMaxFogVoxelGIs` VoxelGIs (`VolumetricFog3D::voxel_gis` and `voxel_gi_atlas`, which
`trace_voxel_gis` fills) by `gi_inject`, every level of each front to back as Godot's
`volumetric_fog_process.glsl` reads them (`fog_voxel_gi_light` in the shader and in
`src/voxel_gi.cpp`). Both reach the pass in a storage buffer (`fog_items`), not its uniforms:
SDL's Vulkan device shows a shader only the first 4 KiB of a uniform push.
`examples/fog-voxel-gi-3d` lights a misty hall with its bounce.

Compatibility's post-processing: without glow or adjustments, `mesh.hlsl` tonemaps as it draws.
With either, each device draws the meshes into an RGBA8 texture at `end_frame` and runs
`shaders/post.hlsl`'s passes: glow's filter, three downsamples and three upsamples through four
textures each half the last's size, then the post pass over the target. `shaders/tonemap.hlsl` is
shared by the shaders. `src/post_shading.hpp` holds the uniforms, the checks and the CPU twins
(`tonemap`, `run_post_pass`) the software device draws with; `tests/test_post.cpp` holds every
device to its pixels. `examples/post-processing-3d` is a lantern glowing at night.

The GPU and WebGPU devices record their screen-space passes as a frame graph, as Unity's render
graph and Godot's `RenderingDevice` graph do (`src/frame_graph.hpp`): each pass names the
resources it reads and writes, `FrameGraph::compile` leaves out those whose results nothing kept
reads, gives each transient texture (`transient`, by its `GraphShape`) an allocation from a
`TransientPool` that textures whose uses don't overlap share, and puts compute passes next to
each other that touch nothing another writes in one device pass; `import` names what lives
outside the frame (the world, the target, histories). Compatibility's post passes, Forward+'s
(depth of field, luminance, glow and the tonemap), SSAO, SSR and subsurface scattering run that
way (`run_graph` in each device), the pool keeping its textures from frame to frame, and
`render::Stats` and `perf.stats`' `frame_graph` report what the last frame's graph did.
`tests/test_frame_graph.cpp` checks the graph, and each feature's test the pool it shares.

Pipeline files are Godot 4.3's `Compositor` and `CompositorEffect`: a `.pipeline.json` file
(`$cli schema pipeline`; `slash/scene3d/pipeline.hpp`, `src/pipeline.cpp`) that the current
`Camera3D`'s `compositor`, else the first `Environment3D`'s, names turns the frame's passes off
over the environment's and the project's (`passes`, `apply_pipeline_passes`, after
`CanvasRenderer`'s `apply_settings`) and lists effects. An effect is a material with
`"shader_type": "compositor"`, whose `fragment()` reads `SCREEN_TEXTURE` through
`screen_texture()`, `depth_texture()` and `normal_roughness_texture()` and writes `COLOR`
(`ShaderType::Compositor`, `compile_compositor` in `tools/shader_compiler`), drawn over the
world at one of Godot's `EffectCallbackType` points (`render::EffectCallback`, `Scene3D::effects`,
`slash/render/compositor.hpp`), in the file's order within a point; `code` names a C++ callback
the game registers with `scene3d::add_compositor_code`, run before each frame's effect to change
it, as Godot's `_render_callback`. `Renderer::add_effects` loads the file and the materials
through the `ShaderMaterialSource`; each device draws the effects as screen passes in Forward+
(`src/compositor.cpp` holds the uniforms and the CPU twin, `run_compositor`, which
`ShaderVm` runs on the software device), and none in Compatibility or on OpenGL, as Godot's
Compatibility renderer has no compositor. `tests/test_compositor.cpp` holds every device to the
software device; `examples/compositor-effects` outlines and grades a yard.

Skies and fog are the Compatibility renderer's too. `Scene3D::sky` (`render::Sky3D`: a color,
procedural, physical or panorama sky, from a `"sky"` background and the sky material on the
`Environment3D`'s entity) draws before the meshes, as one triangle over the target with no
depth test: `shaders/sky.hlsl` works out each pixel's view direction and shades it as Godot's sky
shaders do, with the suns of up to four `SunLight3D`s. In Forward+ (`kSkyLinear`) it
works from linear colors and leaves the result linear, as Godot's RD sky does; in Compatibility
it works in sRGB and makes the result linear, as `drivers/gles3` does. `Scene3D::fog` (`render::Fog3D`)
fades meshes in `mesh.hlsl`'s `fog_process` and the sky by its `sky_affect`; fog over a color
background draws that color as a sky, as Godot's fog sky does. `src/sky_shading.hpp` holds the
uniforms, the checks and the CPU twins (`sky_ray`, `shade_sky`) the software device draws with;
`tests/test_sky.cpp` holds every device to its pixels. `examples/sky-3d` is a valley under three
skies.

The sky's light is Godot's sky radiance. `Scene3D::radiance` (`render::Radiance3D`, from
`Environment3D`'s `ambient_light_source`, `reflected_light_source` and `sky_radiance_size`)
names the sky material to light with; each device draws it with `sky.hlsl` into an RGB10A2 cube,
mipmaps it and filters each level with `shaders/radiance.hlsl`'s GGX samples, only when the sky
changes, and `mesh.hlsl` reads it for ambient light and reflections. Cube faces follow one table
on every API (`cube_direction`); `make_radiance` in `src/sky.cpp` is the CPU twin the software
device lights with, and `tests/test_sky.cpp` holds every device to its pixels.

Models are glTF 2.0 files that `Model3D.mesh` names, read with cgltf as Godot's glTF
importer reads them (`engine/scene3d/src/model.cpp`): `load_model` turns a `.gltf` or `.glb`
file into a `scene3d::Model` of meshes in Godot's winding, with MikkTSpace tangents where the
file has none, materials made `Surface3D`s as Godot's importer makes them, images and
the parts its scene's nodes place. `scene3d::add_asset_types` loads them as assets, so they
reload, and `check_model` backs `scene validate`, exports and `model.inspect`; a
`Surface3D` on the entity is Godot's `material_override`. `write_gltf`
(`src/gltf_writer.cpp`) writes a `Model` back out as a `.gltf` file that loads as written (its
buffer embedded, images by their paths or embedded as PNGs, skins and animations kept), which
importers use to pick one mesh out of a model, and `write_glb` as one `.glb` file holding everything. `model.cleanup`
(`engine/command/src/model_cleanup.cpp`, on `scene3d::cleanup_model` in `src/cleanup.cpp`) makes a
generated or imported model game-ready as a `.glb`: up turned to y, delit, simplified with
meshoptimizer to the storyboard's triangle budget (`storyboard::budget_for`) with LODs made again,
sized, moved to a pivot, unwrapped with xatlas where it has no texture coordinates and given
collision;
`examples/model-cleanup` cleans a CAD crate and a dense boulder. Models collide as Godot's
importer makes them (`slash/scene3d/collision.hpp`, `src/collision.cpp`): `apply_import_hints`
reads `-col`, `-convcol`, `-colonly` and `-convcolonly` on nodes' names into `Model::collision`
when a model loads, `generate_collision` makes a trimesh, a hull or V-HACD's convex
decomposition (`src/vhacd_build.cpp`, built with `-ffp-contract=off` and patched by
`cmake/patches/vhacd-deterministic.cmake` so hulls are the same everywhere), and `write_glb`
writes them back as hinted nodes. physics3d makes each `Model3D` whose model collides a
static body of `model_shapes`, or adds them to the body it is on or under (`add_model_shapes` in
`engine/physics3d/src/space.cpp`). `examples/model-collision` drops balls on a yard and an arch.
`delight_textures` (`src/delight.cpp`) takes the light baked into albedo textures out, as Unity's
De-Lighting tool: it places each covered texel on the model, traces its sky with the
`OccluderScene` of `src/occlusion.cpp`, fits an ambient and directional light to the texels by
least squares and divides it out, with `slash::exp` and `ln` for sRGB so results are the same
everywhere; `examples/model-delighting` delights a scanned crate. JPEG textures decode with stb_image's integer decoder
(`engine/render/src/jpeg.c`). `tests/gltf.hpp` writes test models in code, so none is committed;
`examples/models-3d` is a house from a `.gltf` file.

OBJ models are read as Godot's OBJ importer reads them as a scene (`src/obj.cpp`, which ports
`_parse_obj` and `_parse_material_library`, Godot's quirks included: `Ns` sets the metallic and
only `s off` turns smoothing off). It shares the mesh tools in `src/model_tools.cpp` with the
glTF reader: winding, merging equal corners as `SurfaceTool::index` does, MikkTSpace and
Godot's stand-in tangents. Numbers are read with `slash::parse_decimal`, the same in every
locale. Problems that leave an OBJ model drawing, such as a library it can't read, are
`check_model` errors, not load failures. `examples/obj-models` is a lamp post from an `.obj` file.

FBX models are read with ufbx as Godot 4.3's FBX importer reads them (`src/fbx.cpp`, which ports
`FBXDocument`'s load options, meshes and materials): meters and y up, geometric transforms and
pivots applied to the meshes, `vertex_color_use_as_albedo` on every material, and textures
found where `_get_texture_path` looks. ufbx builds with `-ffp-contract=off`; it calls libm only
for nodes turned by Euler angles, so examples keep their nodes unturned. `tests/fbx.hpp` writes
binary and ASCII FBX files in code; `examples/fbx-models` is a windmill from an `.fbx` file.

`.blend` files are imported as Godot imports them (`src/blend.cpp`, after
`editor_scene_importer_blend.cpp`): the engine runs the artist's own Blender in the background to
export a `.glb` file with Godot's default options and reads that, so no Blender code is linked
(Blender is GPL). `find_blender` looks where Godot does, after `SLASH_BLENDER_PATH`, and needs
3.0 or later. The glTF is kept in `.slash/imported/` under the file's name and a hash of its
bytes, with Blender's log beside it. `scene3d::with_blender` wraps a file source so `.blend`
files read as their glTF: sessions run Blender only with the `build` capability, and exports and
`slash-cli scene validate` always may; exports ship the glTF under the `.blend` file's name.
`tests/fake_blender.cpp` is Blender's stand-in for tests and `examples/blend-models`.

Model recipes are `.model.json` files (`$cli schema model`; `engine/scene3d/src/recipe.cpp`):
parameters, materials and parts of Godot's CSG shapes, capsules and prisms, combined with their
children in order, with bevels, arrays and numbers written as `Expression`s over the parameters
(`"=width / 2"`). `build_model_recipe` evaluates them, checks the result against the strict
schema and the shapes' fields, and builds each top-level part as a CSG root through
`csg_mesh(const CsgTree&)`, the same code CSG entities use; bevels are hulls of corner spheres
fed in as `SolidMesh3D`s. `load_model` and `check_model` take recipes, so `Model3D`,
`model.inspect`, LODs, hot reload and exports treat them as any model, and
`validate_model_recipe` backs `scene validate` and `scene format`.
A top-level part can be another model file (`"model": "props/crate.model.json"`), its nodes placed
under the part's as Godot instances a scene (`add_model` in `recipe.cpp`), so big assets are built
of small ones; the loader passed to `build_model_recipe` reads them, and a recipe reloads when one
changes, since an asset reloads when any file it read as it loaded does (`Assets::read` while a
loader runs). `examples/parametric-models` is a table and a vase, and `examples/composite-models`
a market stall of crates and a lantern. A recipe's `variants` and `quality` tiers are named forms
of it (`recipe_form`: parameters set, materials' and parts' fields changed, parts removed, as
Godot's inherited scenes override nodes); a model path picks a variant after `::`
(`asset::file_of` and `subresource_of`; the asset reloads with its file), and an export preset's
`model_quality` ships each recipe as its tier makes it (`with_quality` in
`tools/exporter/src/export.cpp`). `examples/model-variants` burns a crate and ships it for phones.
A recipe's `checks` (`scene3d::model_checks_schema`, `check_model_against` in
`src/model_checks.cpp`: size, pivot, triangles and the materials, nodes, bones and animations
other files name) run wherever it is validated, forms' over the recipe's; the library keeps a
model's checks from `asset.add` in its record and the project's `library.json`, every version
passes them, and `asset.check` checks the project's copy again, which `test.run` fails on.
`examples/model-checks` breaks a stool and a lamp. A recipe's `points` become nodes with no
mesh after the parts' nodes, under the part they name. `scene3d::summarize` (`model.cpp`)
describes any model in a few numbers and names: its size, `pivot` (the origin's place in its
bounds), triangles, each part's box and materials and its attachment points (nodes that place no
mesh, have no children and are no joint, as Godot's `Marker3D`), which `model.inspect`'s `brief`
returns and `BoneSocket3D` follows by name; `examples/model-parts` opens a cabinet's door and
stands a lamp on it.

Models' surfaces get LODs when they load, as Godot 4.6's importer generates them
(`engine/scene3d/src/lod.cpp`: `generate_lods`, ported from `ImporterMesh::generate_lods`, on
meshoptimizer 1.0's simplifier, built with `-ffp-contract=off` so LODs are the same everywhere):
corners merged as Godot merges them, each LOD about half the last, skinned surfaces simplified
in their rest pose, and lightmapped meshes' LODs made again from their unwrap.
`RenderDevice::create_mesh` takes a mesh's LOD index lists and `draw_mesh` the LOD to draw, which
every device draws from one index buffer (`mesh_index_buffer`, `IndexRange`).
`scene3d::Renderer::pick_lods` picks each draw's LOD as Godot's `_fill_render_list` and
`mesh_surface_get_lod` do (`pick_lod`), by `Model3D.lod_bias` and project.json's
`rendering.lod.threshold_pixels`; shadow maps draw the camera's LODs and
reflection probes pick their own. `examples/mesh-lods` draws boulders near and far.

The camera's mesh draws are culled by the view on the GPU and drawn as instances, as Unity's
GPU-driven pipeline and Godot's Forward+ do: draws next to each other in the camera's order that
share a mesh, a material and their pipelines form a run (`cull_runs` in `src/mesh_cull.hpp`),
each device writes a record a draw (`push_cull_record`: the view projection times its transform,
its box and its indices), the runs and the order (`push_cull_runs`) and each draw's Instance
(`push_instance`), then `shaders/mesh_cull.hlsl` tests each draw in a thread (counts.w 0) and
lists each run's draws in view in a thread of its own (counts.w 1), writing the run's indexed
indirect arguments with as many instances as draws in view, which the camera's passes draw with
`shaders/mesh_instanced.hlsl` and `mesh_motion_instanced.hlsl` (`mesh.hlsl` with `INSTANCED`:
`InstanceBase` says where its run's list starts) on the GPU device and WebGPU (`cull_meshes`,
`prepare_cull` and `encode_cull`). `box_in_view` and `detail::cull_meshes` are its CPU twin,
which the software device skips draws by; OpenGL draws everything, and shadow passes draw every
caster. A draw with a game shader is never culled (an inside-out box), since `vertex()` moves its
corners, and draws alone with its own stages. Opaque draws go in Godot's order
(`mesh_draw_order`: by depth in `kMeshDepthLayers` slices of the far plane, then mesh, then LOD,
then nearest first), so a mesh's copies sit next to each other.
`tests/test_mesh_cull.cpp` holds every device to the software device.

Occlusion culling is Godot 4.6's raycast culler (`slash/scene3d/occlusion.hpp`,
`src/occlusion.cpp`, after `raycast_occlusion_cull.cpp`). `Occluder3D`'s quad, box,
sphere, polygon and array shapes (`occluder_mesh`, as Godot's `Occluder3D`s make them) go into an
`OccluderScene`, a bounding volume hierarchy of its own where Godot's uses Embree; with
project.json's `rendering.occlusion_culling` on, `scene3d::Renderer::prepare` has an
`OcclusionBuffer` cast a ray through each texel (sized and jittered as Godot's) and keep the
farthest depth in its mipmaps, then hides each draw whose placed bounds `occluded` says are
behind it, keeping shadow casters as `ShadowsOnly`. `bake_occluder` (`occluder.bake`) follows
`OccluderInstance3D::bake_scene`: meshoptimizer simplifies each opaque surface, then corners and
triangles are merged as Godot's `_bake_surface` does. `examples/occlusion-culling` hides houses
behind a wall.

Virtualized geometry is a research spike of SlashEngine's own, after Nanite
([docs/reference/virtualized-geometry.md](virtualized-geometry.md)):
`build_cluster_hierarchy` (`slash/scene3d/virtual_geometry.hpp`, `src/virtual_geometry.cpp`)
builds a surface's DAG of cluster groups with meshoptimizer's cluster LOD builder
(`demo/clusterlod.h`, compiled in `src/clusterlod_build.cpp` with the rest of
`slash_meshoptimizer`), locking open edges as Godot's LODs do, using only the regular
simplifier and dropping triangles folded over their twins; `select_clusters` cuts it for a
`ClusterView` by its groups' projected errors, so a cut covers the surface once with no cracks.
`Model3D.virtualized` has `scene3d::Renderer` cut each such surface for the camera in
`pick_lods` (`cut_clusters`; probes draw the last cut) and draw the cut as a mesh of its own,
made again when the clusters change; `frame.capture` counts them as `clusters`. The camera's
draw then leaves out the clusters out of view, facing away (`Cluster::cone`, from
`meshopt_computeClusterBounds`) or occluded (`cluster_culled`, `cull_clusters`), drawing the
rest as a second mesh and the whole cut into shadow maps (`Draw::shadow_mesh`), except where
rays, SDFGI or path tracing need every cluster (`set_cluster_culling`); `culled` counts them.
With project.json's `rendering.virtual_geometry.streaming_pool_kb`, each surface's hierarchy is a
page file (`write_cluster_pages`, `slash/scene3d/cluster_streaming.hpp`) whose groups' pages a
`ClusterStreamer` loads as cuts ask for them, coarser groups first, and drops least recently used
for room; `select` cuts from what is loaded, counting missing groups as fine enough, so cuts stay
crack-free (`stream_clusters` updates them after the camera's cuts; `pages`, `pending_pages`).
`examples/cluster-streaming` streams a moon in.
`examples/virtualized-geometry` cuts moons near and far.

Skinned meshes and model animations follow Godot's glTF importer, `Skeleton3D` and
`AnimationMixer` (`engine/scene3d/src/skeleton.cpp`). A glTF model keeps its nodes (named as
Godot's `validate_node_name` and made unique), skins (joints and inverse bind matrices),
vertices' joints and weights, and animations (node tracks; `-loop` and `-cycle` names loop); an
FBX model keeps the same from its skin deformers and its animation stacks, baked at 30 a second
with `ufbx_bake_anim` as Godot's FBX importer bakes them (`src/fbx.cpp`).
`pose_of` poses a model from its rest, then by its clips (`blend_animation`, Godot's blend of 3D
tracks with its deterministic slerp), then by the entity's `Skeleton3D`; poses are worked out
from components every time, never kept. The animation module tells scene3d what an entity's
`Animator` plays through `scene3d::set_clip_source`: a library can be a model file, and a
`.anim.json` animation can come `from` a model's and add its own tracks. The renderer skins
each surface into a mesh per entity and blends poses between ticks: where
`RenderDevice::gpu_skinning()` says so (the GPU device on Vulkan and Metal, and the WebGPU
device), as Godot's skeleton compute shader does, `create_skin` keeps the rest vertices, weights
and blend shapes on the device and `skin_mesh` moves them each frame with `shaders/skin.hlsl`
(`src/skin_gpu.hpp` lays out its words; `Renderer::skin_on_device` bounds the mesh by each
joint's box of the vertices it weighs), and elsewhere, or under flesh, on the CPU
(`blend_vertices`, then `skin_vertices`) into `RenderDevice::update_mesh`; the two are twins and
`tests/test_gpu_skinning.cpp` holds them to each other's pixels. `skeleton.get` lists and
poses bones; `examples/skinned-meshes` waves an arm. glTF morph targets are Godot's blend shapes
(`slash/scene3d/blend_shapes.hpp`): each surface keeps its shapes' offsets
(`ModelSurface::blend_shapes`, carried through merging corners by `Triangles::origins` and
`follow_origins`), animations' weights channels are `BlendShapeTrack`s, `blend_shape_weights`
blends them over `Model3D.blend_shapes` as Godot's mixer does, and the renderer moves the
vertices with `blend_vertices` before skinning; `examples/blend-shapes` makes a face smile.

Retargeting follows Godot 4.6's importer (`slash/scene3d/humanoid.hpp`): `humanoid_profile` is
its `SkeletonProfileHumanoid`, `auto_map_bones` its `BoneMapper::auto_mapping_process` (Godot's
name patterns matched by a small backtracking matcher in `src/humanoid.cpp`), and
`retarget_model` (`src/retarget.cpp`) its skeleton track organizer, renamer and rest fixer in the
order the importer runs them, on the bones `model_skeletons` (`slash/scene3d/skin_tool.hpp`,
Godot's `SkinTool`, shared with the Godot importer) names, fixing skins, nodes under bones, parts
and collision so the model draws as it did. Godot divides position tracks by the skeleton's
`motion_scale`, which glTF can't keep, so `blend_animation` scales another model's bone
position tracks by the ratio of the two models' `motion_scale` (the hips' height) instead.
`model.retarget` (in `engine/command/src/model_cleanup.cpp`) writes the result as a `.glb`;
`examples/humanoid-retarget` plays a Mixamo hero's wave on a Blender knight. Unrigged figures are
rigged by a generator that rigs (`GeneratorCapabilities::rigs`): `asset.refine`'s `rig` sends
the `.glb` to one (`meshy-rig` polls Meshy's task with `HttpRequest::method` `GET` and
`Transport::later`; the `test` generator calls `scene3d::rig_humanoid`, `src/auto_rig.cpp`, a
skeleton placed by average proportions), then `retarget_rig` in `engine/library/src/commands.cpp`
retargets the result before the library keeps it. `examples/humanoid-rigging` rigs a boxy knight
and plays a dancer's wave on it.

CSG shapes are Godot 4.6's CSG module (`engine/scene3d/src/csg.cpp`, after
`modules/csg/csg_shape.cpp`): `SolidBox3D`, `SolidSphere3D`, `SolidCylinder3D`, `SolidTorus3D`,
`SolidExtrusion3D`, `SolidMesh3D` and `SolidGroup3D`. Each shape's faces are made as Godot's
`_build_brush` makes them, packed into Manifold (built with `MANIFOLD_PAR=-1` and
`-ffp-contract=off`) and combined in Godot's order: a shape with its children, a child's
`operation` against what came before, hidden children left out. A root (a shape whose parent
isn't one) becomes one mesh with a surface per material, its normals and tangents as Godot's
`update_shape` makes them. `install_csg` (called by `canvas::install`) keeps each root's mesh in
the world until the shapes under it change; `csg_mesh` makes it on each call in a world without.
`Renderer` draws roots, physics3d makes a static body of each root with `use_collision` (its
triangles as a concave shape), navigation bakes take roots as Godot's
`CSGShape3D::navmesh_parse_source_geometry` does, `check_csg` reports shapes that can't be made
for `scene validate`, and `solid.get` returns a root's size, surfaces and problems.
`examples/csg-shapes` builds a hamlet.

Paths are Godot's `Path2D` and `Spline3D` with their curves, and the `SplineFollower2D` and
`SplineFollower3D` that ride them (`engine/world/src/path.cpp`, after `scene/resources/curve.cpp`
and `scene/2d/path_2d.cpp` and `scene/3d/path_3d.cpp`). A curve is baked as Godot bakes it,
each Bézier segment halved until its pieces are no longer than `bake_interval`, with the way it
goes at each point and, in 3D, its tilt and an up vector carried along as a parallel transport
frame; `world::Curve2D` and `Curve3D` sample it. The `path_follow` system (priority 930, after
animations and before physics; added by `canvas::install`) wraps each follower's `progress` and
places it on its parent's curve, turned as its `rotates` or `rotation_mode` says, so an
`Animator` track on `progress` moves it. `SolidExtrusion3D`'s `"path"` mode pushes its
polygon along the `Spline3D` its `path` names, as Godot's `MODE_PATH`. `path.get` samples a
curve and finds its nearest point, `check_paths` (run from `check_meshes`) reports followers
whose parent has no path, and `examples/paths` laps a cart round a rail.

Grid maps are Godot 4.6's `BlockMap3D` and `MeshLibrary` (`engine/scene3d/src/gridmap.cpp`, after
`modules/gridmap/grid_map.cpp`). A `.blocks.json` file (`$cli schema blocks`) holds items by
name, each a model's meshes or a primitive, a material, shapes and a navigation mesh in one
cell's space. A `BlockMap3D` writes its cells as a `TileLayer2D` does, a `legend` of keys
(`world/legend.hpp`, shared with tile maps) and `floors` of rows; `decode_grid_cells` and
`encode_grid_cells` read and write them in canonical form, and an item's legend value is its
name, then `:` and one of Godot's 24 orthogonal indices when it is turned
(`orientation_basis`). Cells are grouped into octants as Godot groups them: `grid_mesh` merges
each octant's items into one surface per material, `install_gridmap` (called by
`canvas::install`) keeps it until the cells, the library or a model change and keeps octants
whose cells didn't, `Renderer` draws them, physics3d makes the BlockMap3D one static body of every
cell's shapes (each sub-shape tagged with its cell, so `physics.raycast` returns the cell it
hit), with `bake_navigation` each cell whose item has a navigation mesh is a region, and
navigation bakes take its meshes and shapes as Godot's `GridMap::navmesh_parse_source_geometry`.
`blockmap.set` fills cells or a box of them, `blockmap.get` reads cells and points, and
`check_gridmaps` reports items the library doesn't have for `scene validate` and exports.
`examples/gridmap` lays out a room with a ramp.

Terrains follow the Terrain3D plugin for Godot (zylann.hterrain, MIT; Godot has none of its own),
in `engine/scene3d/src/terrain.cpp`. An `Terrain3D` names a 16-bit `.height.png` heightmap
(`render::Gray16`, `decode_gray16` and `encode_gray16_png`), a splat map and a color map, each
`resolution` pixels square; one that doesn't exist yet reads as flat, the first texture and
white. `terrain_data` (kept by `install_terrain`, called by `canvas::install`, until the
component or a map changes) holds the heights and Terrain3D's range map; `terrain_chunks` settles
its quad tree for a viewer, `terrain_indices` and `terrain_chunk_vertices` make chunks with
seams toward coarser neighbors as its mesher does, and `Renderer` draws them with
`render::TerrainShading` (Classic4 in `mesh.hlsl`, `terrain_texel` in `mesh.cpp` its CPU twin;
a change to one goes in both). physics3d makes it a Jolt height field with holes, navigation
bakes take `terrain_faces`, and `generate_heights` and `paint` are Terrain3D's generator and
brushes, behind `terrain.generate`, `terrain.brush` and `terrain.get`
(`engine/command/src/terrain.cpp`), which stage the map files and record their undo. Exports
ship heightmaps and the maps HTerrains name as they are, never in compressed forms.
`examples/terrain` makes an island.

Shaders are HLSL in `engine/render/shaders/`. Their SPIR-V, MSL, GLSL ES and WGSL are committed
in `engine/render/src/*_shaders.hpp`, so only someone changing a shader needs glslang,
SPIRV-Cross (`glslang-tools` and `spirv-cross` on Debian and Ubuntu, `glslang` and `spirv-cross`
in Homebrew) and Tint (Dawn's `tint` tool, built with `TINT_BUILD_CMD_TOOLS` and passed as
`-DSLASH_TINT=<path>`; games' own shaders compile with the copies built as libraries from
`cmake/dependencies.json`). The FSR shaders are written as games' shaders are, and
`slash-shader-header` (`tools/shader_compiler`) compiles them into `fsr_shaders.hpp`. After
editing one, regenerate and commit the header; the `shaders_up_to_date_*` tests and the FSR
header's test in `tests/test_upscaler.cpp` fail if the two differ:

```bash
cmake --build --preset dev --target shaders
```

Direct3D 12's ray tracing shaders need shader model 6.5, which Windows' d3dcompiler can't
compile, so their HLSL (`engine/render/src/*_hlsl.hpp`) is compiled to signed DXIL
(`*_dxil.hpp`) by Microsoft's DXC, which no one needs locally: the "DXIL shaders" workflow
(`.github/workflows/dxil.yml`) builds DXC from the source `cmake/dependencies.json` pins and runs
`cmake/compile_ray_dxil.cmake`, a DXIL shader per value of a shader's `kPass` (its header's
`// passes:` line), as Direct3D 12 has no specialization constants. On a pull request it checks the committed DXIL; run by hand on a
branch (`workflow_dispatch` with `ref`), it pushes a commit of the new DXIL to the branch
`dxil-shaders` to cherry-pick (`git fetch origin dxil-shaders && git cherry-pick FETCH_HEAD`).

The GPU tests in `tests/test_render.cpp` skip when no backend starts; `tests/devices.hpp` gives
them every device that works (`drawing_devices()`, `hardware_devices()`). On Linux, Mesa's
software Vulkan (`mesa-vulkan-drivers`), OpenGL and OpenGL ES (`libgl1`, `libgles2`,
`libegl-mesa0`, `libgl1-mesa-dri`) run them with no GPU, and `SDL_VIDEO_DRIVER=offscreen` draws
the windowed path on them too. CI sets `SLASH_EXPECT_GPU=1` on Linux so they fail there instead of skipping.

V-Sync and the frame cap follow Godot: `window.vsync` and `max_fps` in `project.json`,
`--vsync` and `--max-fps` on `slash-cli run`, and `display.set` while a served game runs
(`display.list` and `perf.stats` too; `engine/command/src/live.cpp`). `platform::Loop` paces
windowed frames to the cap, never headless ones, and `Loop::interpolation()` gives the fraction
of the next tick for drawing between ticks.

The render scale follows Godot's `scaling_3d` settings, applied to the whole world (`fsr2`
scales only the 3D world; see TAA above): `rendering.scaling` in `project.json` (`mode`, `scale`,
`sharpness`, `upscaler`) and
`display.set`'s `scaling`. `CanvasRenderer` draws the world, with the canvas layers below it,
into an upscaler's input at the render size, scales it over the screen, then draws the world's
controls and the layers above at full size (`scaled()`; `frame.capture` returns `upscaler` and
`render_size`). `engine/render/include/slash/render/upscaler.hpp` is the interface:
`render::Upscaler`, built-in `bilinear` and `fsr` (AMD's FSR 1, ported in
`engine/render/shaders/fsr_easu.hlsl` and `fsr_rcas.hlsl` and checked against a C++ port of
`ffx_fsr1.h` on every device), and `add_upscaler` for a game's or an SDK package's own, which
refuses temporal ones for now. `examples/render-scale` changes it while a game runs.
