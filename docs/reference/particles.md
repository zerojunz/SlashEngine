# Particles

CPU and GPU particles.

Status: current · Updated: 2026-11-20

`Emitter2D` is Godot's, with Godot's field names and angles in degrees. The
`cpu_particles_2d` system (priority 1100, after physics; `engine/canvas/src/particles.cpp`)
emits, moves and ages each emitter's particles on the CPU and emits `finished` when a one-shot's
last particle dies; `CanvasRenderer` draws them (`particles_drawn()`). Each `_min`/`_max` pair
is a random range per particle and each `_curve` is `[offset, value]` points over its life.
Random values hash the entity ID, `seed`, cycle and slot, so runs repeat everywhere: keep
particle math in `engine/canvas` (built with `-ffp-contract=off`) and use `slash::cos_sin`
(`core/math.hpp`), not libm. Particles live beside the components, kept by entity ID, so saving,
snapshots and undo leave them alone. `particles.eval` returns live particles, or runs the
settings for a time with `canvas::simulate`; `examples/particles` has a fountain, sparks, smoke
and a portal.

`GpuEmitter2D` is Godot's too, its particles moved on the device as a `.particles.json`
file (Godot's `ParticleProcessMaterial`; `$cli schema particles`) says. The
`gpu_particles_2d` system (priority 1110; `engine/canvas/src/gpu_particles.cpp`) ports Godot's
node and particles storage: each tick it works out each emitter's `render::ParticleStep`s
(`fixed_fps`, preprocess, the emission cycle, one-shot timing) and keeps those that can still
have made a live particle (`canvas::particle_steps`). `CanvasRenderer` runs the steps its device
hasn't with `RenderDevice::process_particles` and draws with `draw_particles`, so a late
renderer catches up from the steps kept. `engine/render/shaders/particles_process.hlsl` is the
compute shader and `render::process_particles` (`engine/render/src/particles.cpp`) its CPU twin,
which the software and OpenGL ES devices run; a test holds them to the same particles, so a
change to the particle math goes in both. Turbulence is Godot's curl noise, with lattice
gradients from an integer hash so both make the same noise. Collision follows Godot's 2D
collision with its signed distance field, measured exactly instead: the system gathers the
outlines of `Occluder2D`s and tile occluders with `sdf_collision` into each step's
`render::ParticleShape`s, in the world canvas's coordinates, and `render::particle_contact` (and
its twin in the shader) finds the way out and the depth. A `sub_emitter` chains emitters (up
to `render::kMaxParticleChain`), which step together through
`RenderDevice::process_particle_chain`: each link's step writes the particles it starts to an
emission buffer, `particles_emit.hlsl` numbers what the next link takes (its twin is
`take_emissions`), and that link's step starts them. `canvas::check_sub_emitters` reports the
links left out, for `scene validate` and exports. Trails are Godot's: a system made with a
trail (`create_particles(amount, trail)`) keeps `trail - 1` copies of each particle after them,
which wait when it starts and then run the steps `trail_steps * j / trail` behind it, so a run
starts with `history` steps it ran before (`ParticleRun::history`) for them to read;
`particles_draw.hlsl` and `particle_vertices` draw each as Godot's ribbon. The steps kept and the
emitter's idle time cover the copies running on after their particles. Particle graphs are
Godot's particle shaders as node graphs: a process material's `start` and `process` compile with
the canvas material compiler (`canvas::detail::compile_particle_graphs` in
`engine/canvas/src/material.cpp`) into `render::ParticleProgram`s, material programs over the
registers `render/particles.hpp` lays out, which the compute shader runs through
`material_ops.hlsl` (shared with the material shader) and the CPU twin through
`run_instructions` with `portable` set (`slash::exp`, `slash::ln`, `cos_sin` and `atan2`, the
same bits everywhere). `particles.eval` runs the steps kept on the CPU
(`canvas::gpu_particles`); `examples/gpu-particles` has a fire, sparks, snow, smoke, rain,
fireworks, comets with trails and fireflies that graphs move.

`Emitter3D` is Godot 4.6's, in `engine/scene3d/src/particles.cpp`
(`slash/scene3d/particles.hpp`): the `cpu_particles_3d` system (priority 1100, added by
`canvas::install`) runs each emitter as `Emitter2D`'s are run, with the same timing and
hashed random values, and Godot's 3D emission shapes, cone spread, basis flags and scale.
`scene3d::Renderer::add_particles` draws each emitter's mesh (its `mesh` file or primitive mesh
component) as one CPU mesh per surface, refilled each frame with `update_mesh`: a copy of the
mesh at each live particle, turned by `billboard` where its `Surface3D` has a
`billboard` (as Godot's `BaseMaterial3D` vertex code, which `face_camera` also applies to
`Model3D`s) and colored by the particle. `particles.eval` lists them in 3D, and
`examples/particles-3d` has a fountain, a fire and snow.

`GpuEmitter3D` is Godot 4.6's: `.particles.json` files read in 3D too
(`canvas::ProcessMaterial` holds both forms; `parse_process_material_3d` takes Godot's 3D fields
and per-axis curves, a 2D file's two numbers keeping the 3D default's z) into a
`render::ParticleProcess3D`. The `gpu_particles_3d` system (`engine/scene3d/src/gpu_particles.cpp`,
priority 1110) steps each emitter as `gpu_particles_2d` does and keeps its
`render::ParticleStep3D`s; `scene3d::Renderer::add_gpu_particles` runs the steps its device
hasn't (`RenderDevice::process_particles_3d`) and has `fill_particle_mesh` write each draw pass's
mesh at every live particle into a mesh of world-space corners, culled by the visibility box.
The GPU and WebGPU devices run them in compute shaders: `particles_process_3d.hlsl` steps the
particles a thread each, `particles_sort_3d.hlsl` sorts a view depth pass's slots with a bitonic
sort (keys of depth bits and index, so ties keep slot order) and `particles_mesh_3d.hlsl` writes
the mesh's vertex buffer a thread a vertex, its box `ParticleMeshDraw3D::box_low` and `box_high`
(the visibility box, which every device gives the mesh through `set_mesh_bounds`, so blended
draws order alike); `src/particle_layout_3d.cpp` lays out the words they read. On Direct3D 12
the meshes are placed on the CPU for now, since SDL's device is lost running the mesh pass on
WARP.
`render::process_particles_3d` and `particle_mesh_vertices_3d` (`engine/render/src/particles3d.cpp`,
built with `-ffp-contract=off`) are their CPU twin, which the software and OpenGL devices run,
and `tests/test_particle_compute_3d.cpp` holds every device to it; a change to one goes in both,
and `check_particle_process_3d` checks what every device takes. `particles.eval` runs the steps
kept on the CPU; `examples/gpu-particles-3d` has sparks, leaves and streaks. Attractors are Godot's
`ParticleAttractorSphere3D`, `Box3D` and `VectorField3D`: the system pairs each emitter
with those visible whose bounds touch its visibility box and whose `layer_filter` meets its
`layers` (Godot's scene cull), and puts them in its steps in the particles' space
(`render::ParticleAttractor3D`, `for_particles` in `src/gpu_particles.cpp`); a vector field's
texture is cut into slices by `particle_field` and read through the `ImageLoader`
`canvas::install` gives it, and packed after the steps for the shaders
(`src/particle_layout_3d.cpp`). `examples/particle-attractors-3d` gathers dust in a well.
Colliders are Godot's `GPUParticlesCollisionSphere3D` and `Box3D`, paired as attractors are and
placed by `collider_for_particles` (`render::ParticleCollider3D`); `collider_contact` in
`engine/render/src/particles3d.cpp` and its twin in the shader find the way out and the depth
as `particles.glsl`'s collider loop, and the process material's `collision_mode` bounces or
hides the particle. `examples/particle-collision-3d` bounces sparks off a floor.
`ParticleColliderSdf3D` and `HeightField3D` are Godot's too
(`slash/scene3d/particle_collision.hpp`, `src/particle_collision.cpp`): `bake_particle_sdf`
ports `GPUParticlesCollisionSDF3D::bake` (its bounding volume hierarchy and distance code, cells
on the job pool), `particles.bake_sdf` (`engine/command/src/lightmap.cpp`) writes the `.sdf.png`
file of half floats it makes, and `particle_height_field` draws the ground under a box on the
CPU as Godot's height field camera does, which the system draws again when the box moves and
steps after the camera with `follow_camera_enabled`. Both go to the devices in the step's words
(`src/particle_layout_3d.cpp`; an SDF's distances as halves, read with filtering in code).
`examples/particle-sdf-collision-3d` settles snow on a yard and sparks on a statue.

Visual effects are `.vfx.json` recipes, after Unreal's Niagara systems (Godot has none):
`slash/world/vfx.hpp` (`engine/world/src/vfx.cpp`, built with `-ffp-contract=off`;
`$cli schema vfx`) lays layers of components (particles, decals, lights, sounds, screen flashes,
`CameraShake`s, anything a scene holds) on one timeline, with `curves` that move their fields
and values written as `Expression`s over the recipe's `parameters` (`vfx_value`,
`vfx_components`, which also thins emitters by detail, and `vfx_curve_value`). The `vfx` system
(priority 1090, added by `canvas::install`) plays each `VisualEffect3D` and `VisualEffect2D`: a
layer plays as `<effect>_<layer>` with a `VfxPart`, a child of the effect (`world` layers left
where they appeared), emitters stop and linger until their particles are gone, levels of detail
come from the current camera's distance (`vfx_detail`), and `CameraShake`s shake the current
cameras' offsets, taking off what they added (`camera_shake_offsets`). `vfx_cost` and
`vfx_over_budget` hold a recipe to its `budget`. Damage profiles' `effects` name a `vfx`
recipe that `leave_effects` plays where a hit lands. `vfx.play`, `vfx.state`, `vfx.check` and
`vfx.preview` (a stage in a private `Session`, sampled as numbers and drawn into a PNG) are in
`engine/command/src/vfx.cpp`. `examples/vfx-effects` blows up a barrel yard and slashes in 2D. Simulation stages run
over CPU particles, after Niagara's: a `ParticleSimulation3D` or `ParticleSimulation2D` beside a
`Emitter3D` or `Emitter2D` (`slash/world/particle_sim.hpp`,
`engine/world/src/particle_sim.cpp`, built with `-ffp-contract=off`: `NeighborIndex` sorts points
along a Morton curve, no grid; `curl_noise`; `run_stages` flocks, swirls, limits, collides through
a `SimInputs::contact` callback, works out attributes as `Expression`s and fires events) is run by
the `particle_simulation_3d` and `particle_simulation_2d` systems (priority 1105, in
`engine/scene3d/src/particles.cpp` and `engine/canvas/src/particles.cpp`) over the emitters'
slots, colliding with `GPUParticlesCollision*3D`s (`scene3d::particle_surroundings_3d`), what the
current camera sees (`camera_depth_view`, `particle_depth_contact`) or 2D occluders
(`canvas::particle_collision_shapes`), and spawning events' particles into other emitters as
extra slots. Beside a `GpuEmitter3D`, a `ParticleSimulation3D`'s neighbor, curl, drag and
speed stages run on the device after each step (`scene3d::gpu_stages` fills each step's
`render::ParticleStages3D`): `shaders/particles_stages_3d.hlsl` keeps the particles and writes
Morton keys, `particles_sort_3d.hlsl` sorts them and a second pass flocks with binary searches,
and `render::run_particle_stages_3d` is its CPU twin, which `tests/test_particle_compute_3d.cpp`
holds every device to; a change to one goes in both. `examples/particle-simulation` flocks
fireflies and bees and settles sand. Screen distortion
is `ScreenDistortion3D` and `ScreenDistortion2D` (heat, shockwaves, ripples, lenses;
`slash/canvas/distortion.hpp`, `src/distortion.cpp`): `CanvasRenderer` draws the world into
`distortion_target_` and shows it through a grid whose texture coordinates `distortion_offset`
moves, so devices need no shader of their own. The creature, vehicle, character and foliage
recipes' `vfx` name an effect per world event; `play_hooks` in `engine/world/src/vfx.cpp` plays
them (`play_vfx`) and keeps vehicles' `running` effects, and the foliage system emits
`foliage_parted`. `examples/vfx-tool-effects` feeds a dog, runs a van and parts grass.

Fluids and smoke are SlashEngine's own, after Niagara Fluids but with no grid (Godot has none):
a `.fluid.json` recipe (`slash/world/fluid.hpp`, `engine/world/src/fluid.cpp`, built with
`-ffp-contract=off`; `$cli schema fluid`) is a liquid or a gas in 3D or 2D, and a `Fluid3D` or
`Fluid2D` steps one with `FluidEmitter3D`/`2D`s pouring in and `FluidSink3D`/`2D`s draining it.
`world::step_fluid` is the solver both dimensions share (2D at z 0): liquids as position-based
fluids (a density constraint, XSPH viscosity, cohesion, vorticity confinement), gases as
particles carrying temperature, smoke and fuel that rise, cool, burn, diffuse and swirl round
vortex particles, neighbors along a Morton curve (`NeighborIndex`, the particle simulation's)
and the work split over the job pool by particle, so steps don't depend on the thread count. The
`fluids` system (priority 958, added by `canvas::install`) keeps particles by entity ID
(`world::fluid_particles`); physics3d and physics2d set a `world::FluidHost`
(`src/fluid.cpp` in each: spheres against Jolt bodies, circles against Box2D shapes by their
manifolds, impulses back on rigid bodies, the project's gravity). `scene3d::Renderer::add_fluid`
and `CanvasRenderer::gather_fluids` draw them: a liquid as the surface the camera sees,
splatted, smoothed and meshed on the CPU (`scene3d::fluid_surface`; `fluid_surface_2d` traces
a 2D liquid by marching squares) with its spray (`world::fluid_is_spray`) as discs
(`scene3d::fluid_quads`), so every device draws it, and a gas as discs lit through the smoke
between them and the light (`world::fluid_transmittance`, gridless: particles found where they
fall on a plane square to the light). `fluid.place`, `fluid.pour`, `fluid.state`, `fluid.check`
and `fluid.preview` (stages in a private `Session`) are in `engine/command/src/fluid.cpp`. The hosts
also collide particles with soft bodies' and `Cloth3D`s' faces (`collide_soft`, pushing the
nearest face's points back, `push_soft`) and `Cloth2D`s' outlines (`Cloth2DBody::outline`, which
the cloth step keeps); `PhysicsField3D`s and `2D`s with `particles` act on each particle through
`particle_effect` (forces, velocities, anchors, kills); an emitter with no `fluid` pours into the
fluid on its entity or above it, throwing particles with its motion by `inherit_velocity`
(`EmitterState`); and a VFX layer's emitters stop at its end while a part with its own fluid
lingers until the particles are gone (`end_part` in `engine/world/src/vfx.cpp`).
`examples/fluids` splashes water, lights a campfire and pours 2D water.

Oceans, lakes and rivers are SlashEngine's own, after Unreal's Water plugin: a `.water.json`
recipe (`slash/world/water.hpp`, `engine/world/src/water.cpp`, built with `-ffp-contract=off`;
`$cli schema water`) gives Gerstner waves (or a wind that raises them round the
Pierson-Moskowitz peak), a current or a river's flow, ripples, buoyancy and drag, and the look.
`world::sample_water` gives the surface over a point (the waves' sideways shift undone in fixed
steps), its normal, the water's velocity, crest and shore foam and the depth to the ground,
which `fill_ground` measures once under a grid through the `world::WaterHost` physics3d and
physics2d set (rays down past what moves); `water_bodies` makes each `Water3D` (a lake's box, an
ocean round the camera or a river along its `Spline3D`) and `Water2D` one. The `water` system
(priority 959, added by `canvas::install`) keeps the clock (`water_time`) and rings (bodies going
in and moving through, liquid fluid particles it takes in) and emits `water_entered` and
`water_exited`. Each space's `apply_water` (`src/water.cpp`) floats rigid bodies at points
through their shapes and drags them toward the water's velocity, as Unreal's pontoons.
`scene3d::Renderer::add_water` draws a displaced grid and `CanvasRenderer::gather_water` a 2D
surface and body. `water.place` and `water.sample` are in `engine/command/src/water.cpp`.
`examples/water` floats crates on a lake, a river and a 2D pond.
