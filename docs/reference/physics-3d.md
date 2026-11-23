# 3D physics

3D physics on Jolt.

Status: current · Updated: 2026-11-20

`engine/physics3d` is 3D physics on Jolt Physics, as Godot 4.4's Jolt module. Bodies and areas
are components: `StaticBody3D`, `MovingBody3D`, `RigidBody3D`, `CharacterMover3D` and
`Zone3D`, with `Collider3D` shapes (`box`, `sphere`, `capsule`, `cylinder`,
`convex_polygon`, `concave_polygon`, `world_boundary`), built as Godot's `JoltShape3D`
subclasses build them (`src/shapes.cpp`). As in 2D, the ECS holds the truth: the `physics3d`
system (priority 950) brings Jolt's system up to date with the components, applies Godot's
gravity and damping to rigid bodies, steps it on one thread, writes rigid bodies back, moves
characters with Godot's `move_and_slide` (`src/character.cpp`, ported line for line onto
Godot's Jolt `body_test_motion` in `src/motion.cpp`), finds what is in each area and emits the
2D events. Object layers encode each body's collision layer and mask as Godot's `JoltLayers`
does, so layers and masks collide when either mask matches. Areas take no part in Jolt's step:
the space collides their shapes after it. `project.json`'s `physics_3d` sets gravity, damping
and `areas_detect_static_bodies`; `physics3d::raycast`, `overlap_point`, `overlap_shape`,
`contacts` and `gravity` answer queries, and the `physics.*` commands ask 3D physics when given
three-number points, a 3D shape or a 3D body. Jolt is built with
`CROSS_PLATFORM_DETERMINISTIC` and the space adds and changes bodies in entity order, so results
are the same on every platform. `examples/physics-3d` has every kind of body.

Joints are Godot's `PinJoint3D`, `HingeJoint3D`, `SlideJoint3D`, `ConeJoint3D` and
`SixAxisJoint3D` as components on an entity of their own, built into Jolt constraints as
Godot's `JoltJoint3D` subclasses build them (`src/joints.cpp`): made at the entity's
`Transform3D` between `body_a` and `body_b` (a missing one is the world), made again when that
transform or the bodies change and rebuilt in place for other changes. `WheeledBody3D` on a
rigid body rolls it on the `Wheel3D` entities among its children, with Godot's
`WheeledBody3D` code ported line for line in `src/vehicle.cpp`: its inputs go to the wheels
before the step, each wheel's ray cast, suspension and friction push the body after it, and the
wheels' `Transform3D` and contact fields are written back. `examples/joints-and-vehicles` has a
lamp, a bell, a door, a drawer and a car.

Soft bodies are Godot's `SoftBody3D` on its Jolt module (`src/soft_body.cpp`, after
`JoltSoftBody3D`): `sync_soft_bodies` makes a Jolt soft body of the primitive mesh the entity's
`Model3D` draws, its vertices welded by position into points (`SoftBody::mesh_to_physics`)
with Godot's stiffness, shrinking and edge springs, moves pinned points to their attachments
before each step and keeps each body's `scene3d::SoftBodyPose` after it. Soft bodies feel Jolt's
own gravity, which the space sets to the project's (every other body's `mGravityFactor` is 0, as
the space applies their gravity itself), and `OnSoftBodyContactValidate` keeps Godot's one-way
layers and `parent_collision_ignore`. `scene3d::set_soft_body_source` (called by
`physics3d::install`, as scene3d doesn't depend on physics3d) hands the poses to
`scene3d::Renderer`, which draws the mesh from them (`soft_mesh`), and `check_soft_bodies` backs
`scene validate`. `physics.soft_body` and `physics3d::soft_body_points` read them;
`examples/soft-bodies` drapes a tablecloth, flies a flag and drops a ball full of air.

Destruction is SlashEngine's own (Godot has none), after Unreal's geometry collections:
`scene3d::fracture` (`slash/scene3d/fracture.hpp`, `make_fracture` in
`engine/scene3d/src/csg.cpp`) cuts a closed mesh into Voronoi cells around points a PCG32 seeds
in its bounds, each a padded cube trimmed by bisector planes and intersected with the mesh in
Manifold, and `fracture_of` keeps an entity's cut in the CSG cache. The space measures each
contact's impulse (reduced mass times closing speed) for bodies with a `Destructible3D`
`damage_threshold` (`Space::note_impact`), lets a breaking contact through as a sensor, and
after the step `detail::break_and_age` (`src/destruction.cpp`) breaks them with `break_body`
(rigid pieces `<id>_<n>` with convex hulls, mass by volume, the body's velocity) and counts down
`FracturePiece3D` lifetimes; `Renderer::add_piece` draws a piece's part of the cut.
`physics.break` breaks one by hand, and `check_destructibles` backs `scene validate`.
`examples/destruction-3d` breaks a wall and a crate. Damage is after Chaos Destruction: a
`.damage.json` profile (`slash/world/damage.hpp`, `engine/world/src/damage.cpp`, built with
`-ffp-contract=off`; `$cli schema damage`), named by a `Destructible3D`'s or `Destructible2D`'s
`profile` with `overrides` over it, gives a material's defaults, health, resistances by kind,
damage states (`show_state` tints, swaps materials and meshes), a fracture pattern
(`world::fracture_sites`: voronoi, impact, radial, splinters, slices, clusters), levels of pieces
that break again (`damage_cut`; scene3d's `fracture` takes chains of `FractureCut`s, each piece
of one cut the region of the next, and finds each piece's neighbors) and linked pieces standing
on anchors: physics3d's `damage_body`, `damage_area` and `settle_group` (`src/destruction.cpp`)
freeze them with `FracturePiece3D` links, loosen those near the hit and, after each break, let
`world::settle_stress` (the connection graph's load, down a tree from the anchors) break the
links too weak and drop what no link holds. `Space::note_impact` damages profiled bodies by
their `impact` settings. physics2d does the same (`src/destruction.cpp`): Box2D's hit events
(`Space::collect_hits`) and `damage_body_2d` cut a body's outline into cells with Clipper2,
each a `RigidBody2D` with a `PolygonCollider2D` and a `Polygon2D` of its part of the look
(`FracturePiece2D`). `damage.apply`, `damage.state` and `damage.check`
(`engine/command/src/damage.cpp`) drive and read them; `examples/destruction-profiles` breaks a
garden wall, a window and 2D crates. Damage short of breaking deforms by the profile's `deform`:
physics3d's `deform_body` adds a `Destructible3D`'s `dents` and `carves` (in its mesh's space),
which scene3d's `deform` (in `src/csg.cpp`: carves as hulls of rings subtracted in Manifold,
dents by `Warp` on a refined mesh) applies, `damaged_piece_of` keeps and the renderer draws;
`reshape` makes a carved body collide as what is left, and a break moves its damage into the new
`FractureCut`, so pieces keep it. An `Terrain3D`'s `craters` are dug into its heights by
`scene3d::dig_crater` as the data is made. physics2d's `deform_body_2d` cuts a 2D outline with
Clipper2 (`open_holes` slots holes to the edge), drops the parts cut off as debris and carves
tile map layers through the `TileCarver` the canvas sets (`set_tile_carver`). Cloth tears:
`Cloth2D`'s `torn` edges (`world::cloth_grid_2d`) hold nothing in `step_cloths`, parts torn off
every held point fall free, and drawing leaves out their triangles (`cloth_triangles_2d`); a
`SoftBody3D` is made again without its `torn` triangles, keeping its points (`keep_points`,
`restore_points` in `engine/physics3d/src/soft_body.cpp`); both tear by `tear_stretch` after a
step, and `tear_cloth_2d` and `tear_soft_body` tear them where `damage.apply` cuts. A `Cloth3D`
is made again the same way (`create_cloth` leaves out its `torn` triangles and frees from the
skin each part no skinned point holds; `tear_cloth`, `tear_stretched` in
`engine/physics3d/src/cloth.cpp`), and the renderer's `cloth_mesh` leaves out each surface's
`SoftBodyPose::Surface::torn`. `examples/damage-deform` dents, holes, craters and tears.
A profile's `effects` (`world::DamageEffects`) say what hits and breaks leave: `leave_effects`
in each `src/destruction.cpp` makes `<id>_debris`, `_dust`, `_decal` and `_sound` entities
(one-shot CPU particles, a `Decal` or `Sprite2D`, a positional audio player) with a
`DamageEffect` that `break_and_age` removes when its lifetime runs out. `kind_of` sorts bodies
into things, the living and vehicles: the living are struck through `hit_ragdoll` and die
(`finish`: limp ragdolls, stopped creatures, `killed`), vehicles are wrecked; `damage.apply`'s
`by` goes into the events, which the `fauna` system answers (fleeing or attacking) and the
`npc` system turns into a brain's `health` and `dead` facts (`feel_health`). `break_budget`
(`may_cut`) lets the first break of a step cut and queues the rest in `DestructionState::pending`
for later steps, counting pieces so replays repeat; scene3d's `cut_cells` cuts a body's cells on
the world's job pool (`fracture_of` passes it), in the same order on any number of threads.
`examples/break-effects` scorches a crate, fells a deer and wrecks a van.

Wind is SlashEngine's own, after Unreal's `WindDirectionalSource` (Godot's only wind is an
`Zone3D`'s for soft bodies, which physics3d ports too: `area_winds` and `blow_soft_body` in
`engine/physics3d/src/soft_body.cpp`, after `_compute_area_windforce`). `world::WindField`
(`slash/world/wind.hpp`, `engine/world/src/wind.cpp`, built with `-ffp-contract=off`) sums the
enabled `WindSource3D`s, directional along the entity's -z or `point_wind` out to `radius`, with
gusts that travel downwind and turbulence across it from a noise of whole numbers hashed with
each source's ID, on the clock the `wind` system (priority -650, added by `canvas::install` and
`physics3d::install`) keeps (`world::wind_time`, reset with the world's state). Soft bodies take
its drag by their `drag_coefficient` before each step, `Emitter3D` add it times
`wind_influence` to gravity, `GpuEmitter3D` get it as a box attractor that pushes everywhere
(`wind_attractor` in `engine/scene3d/src/gpu_particles.cpp`, so devices need no change), and
spatial shaders read it at the mesh's origin as `WIND` (`render::kShaderWindConstant`, filled in
`scene3d::Renderer::prepare`). In 2D, `world::WindField2D` sums `WindSource2D`s along their x
axis in pixels, which `Emitter2D` feel by `wind_influence` and canvas shaders read as `WIND`
(filled by `CanvasRenderer::draw`'s `blow`). `wind.sample` returns either at points;
`examples/wind` blows a meadow.

Foliage is Unreal's Foliage tool (Godot has none): a `.foliage.json` type
(`slash/world/foliage.hpp`; `$cli schema foliage`) says what a plant or rock draws, how thickly a
brush paints it, how it bends and parts, whether it blocks and what harvesting it gives, and a
`Foliage3D` or `Foliage2D` holds a group of instances per type as flat numbers (seven or four an
instance). `engine/procedural`'s `slash/procedural/foliage.hpp` is what drawing, collision and
commands ask: `foliage_type` (types read through the world's assets, kept by version),
`paint_foliage` (a brush stroke through `procedural::scatter`), `foliage_lean_3d` and `_2d` (wind
times `bend` with sway, each instance's phase and rate hashed from its place by the type's
`phase_spread` and `frequency_spread`, gusts rolling downwind by `wave_length`, `flutter` across
the wind and an `idle` rock in still air, plus the push of characters, capped) and `install_foliage`, the
`foliage` system (priority 970) springing instances that `CharacterBody`, `RigidBody` and
`WheeledBody3D` entities push. `scene3d::Renderer` draws each instance's model bent about its
foot, fading by alpha hash past `cull_distance` and as an impostor far off; `CanvasRenderer`
draws 2D instances as quads leaned by their height. Blocking types are a static body of the
entity's: physics3d takes `scene3d::foliage_bodies` (`src/foliage.cpp`) as grid bodies with a
cell per instance, physics2d takes polygons through the canvas's tile shape source, so
`physics.raycast` names the `instance` hit. `foliage.paint`, `erase`, `get` and `harvest` are in
`engine/command/src/scatter.cpp`, with `foliage.sway`, which measures a type's sway along a row
downwind and tunes its file (`set`, or `lean` for the bend a wind gives). `examples/foliage` paints a meadow in 3D and 2D.

Hair and fur are Unreal's Groom and its Chaos hair (Godot has none): a `.groom.json` file
(`slash/world/hair.hpp`, `engine/world/src/hair.cpp`, built with `-ffp-contract=off`;
`$cli schema groom`) says how strands grow, draw and move. `world::step_strands` is the solver
both dimensions share (2D at z 0): small steps of position-based dynamics with a pull toward the
groomed shape that weakens toward the tips, bends, follow-the-leader lengths with Müller's
damping correction, wind, fields and the `StrandCollider`s `strand_colliders_3d` and `_2d` find
under the hair's tree root. `scene3d::hair_growth` (`src/hair.cpp`) grows a `Hair3D`'s
guides over `model_section` (cloth's section, which now carries normals and texture
coordinates) by area with hashed randoms, and the strands drawn that `world::follow_guide`
places, kept in the world by key (`world::keep_hair_3d`); `hair_frames` carries each root by its
joints' blend. physics3d's `Space::step_hair` and physics2d's `step_hair_2d` (each
`src/hair.cpp`) step the guides after the bodies and hand them to drawing through
`world::set_hair_source_3d` and `_2d`; `scene3d::Renderer::add_hair` draws strands and cards
(`hair_ribbons`) and fur shells (`hair_shell`, the pattern `hair_shell_image` at
`made:hair/shells`), and `CanvasRenderer::gather_hair` 2D ribbons. Maps read through
`world::set_hair_image_reader`, which `canvas::install` sets. `hair.get`
(`engine/command/src/hair.cpp`) reads them; `examples/hair-and-fur` grows a head, a creature and
a mane.

Characters are MetaHuman Creator's, as recipes (Godot has none): a `.character.json` file
(`slash/world/character.hpp`, `engine/world/src/character.cpp`; `$cli schema character`) gives
a body's measures, a face, hair (and a `.groom.json`), clothing in layers (one garment may hang
as cloth) and props at points, and `extends` another, merged as JSON Merge Patch.
`world::load_character` reads it through the world's assets (the `.character.json` suffix is a
model's, so it reads the text with `Assets::read`). In 3D, `scene3d::build_character`
(`engine/scene3d/src/character.cpp`) builds it into a skinned `Model` of Godot's whole
humanoid profile (fingers, eyes and jaw) plus twist bones, each turned as the profile's and then onto its next bone
(`skeleton`), so animations retarget onto it, its skin weighted smoothly across joints
(`Body::along`, `trunk_at`, `jaw_weights`; `rig::mix_weights`), with hand poses and `jaw_open`
as its own animations (`add_clips`), of primitive meshes placed by `character_measures`, each
garment's coverage (`garment_coverage`) pushed out by its layer, the groom's `scalp`, a cloth
garment's free part as the material a `Cloth3D` names and props loaded at attachment nodes;
`load_model` and `check_model` take recipes, so `Model3D` draws them. In 2D,
`character_cutout` makes a side-on cut-out of bones and shapes, which the `character_2d` system
(priority 880) keeps as `<id>_<bone>` children of each `Character2D` with a `Polygon2D` each,
drawn by `CanvasRenderer::gather_polygons`. `scene3d::silhouette` and `world::color_difference`
(Oklab) measure how alike two look; `character.check` (`engine/command/src/character.cpp`, run
by `test.run`) holds the cast to the storyboard's `character_rules` (outlines and main colors
apart, heights, palette, props), `character.generate` keeps the most distinct of seeded
candidates within them, and `character.place` puts one in a 3D or 2D scene.
`character.place` adds the twist bones' `BoneCopy3D` and, with `look_at`, the eyes'
`BoneLookAt3D`s, and a `Character2D`'s `look_at`, `jaw_open` and hands pose its cut-out
(`pose_character`). `examples/character-design` designs a cast and `examples/character-rig`
looks, grips and twists.
Faces are MetaHuman Creator's too: a `.face.json` file or a recipe's inline `face`
(`slash/world/face.hpp`, `engine/world/src/face.cpp`, built with `-ffp-contract=off`;
`$cli schema face`) names 46 sculpt controls (`face_controls`, each -1 to 1), carves (dents,
bumps, creases and ridges) at 27 landmarks, `blend`s of other faces' controls by weight, brows,
asymmetry by a seed and a style, and `extends` another (a recipe's face comes `from` one), merged
as JSON Merge Patch. `sculpt_head` shapes a head from it: a grid of rings and segments denser
over the face, sized by the style, pushed by the controls and carves, its lids wrapped over the
eyeballs (`face_eyes`, which the rig's eye bones follow) and its lips' seam tucked in at
`face_mouth` and split (`SculptedHead::seam`: the lower lip's copies move with the jaw), so the
jaw opens it, each triangle marked skin, lips, brows, mouth or hidden and bearded or not. `scene3d::build_character` draws its parts with their own materials
(`head_part` in `engine/scene3d/src/character.cpp`) and `character_cutout` draws its side-on
outline, `face_profile`, as the 2D head. Sculpts carry ARKit's 52 expression shapes
(`face_expressions`, `face_expression_names`: lids turned about the eye, the jaw about its hinge,
lips, cheeks and brows moved by soft weights), which `build_character` gives the head, lower
teeth and tongue as blend shapes (`ModelBuilder::add_shaped`) and which a `Character2D`'s
`expressions` move its profile by (`face_profile`'s weights). `face_skin_maps` paints the 3D
head's albedo, normal and roughness maps over the sculpt's uvs with integer noise: the style's
pores, blotches and shine, the face's `skin` wrinkles, the lips (sampled finer than texels) and
`marks` (moles, freckles, scars, wounds, burns, bruises, rashes, birthmarks, tattoos and paint,
placed at landmarks as carves are, `mark_texel`), which `build_character` draws as the `face`
material; `face_marks_2d` gives the marks the profile shows as polygons for the cut-out.
`examples/character-faces` sculpts an elder and a toon, has them smile, blink and talk, and
marks a fighter.
The face tools (`engine/command/src/face.cpp`) design a cast against the storyboard's
`face_rules` (shape and profile differences, styles, control ranges, marks): `face.check`
sculpts each face file and designed recipe face on one head and measures each pair by rays at
the front and `face_profile`'s rows (`test.run` runs it), `face.generate` reads a brief into
controls, marks and a style and keeps the first seeded candidate far enough from the cast, and
`face.portrait` draws one in 3D or as a cut-out in a private `Session`. `examples/face-tools`
designs a ship's crew.
Characters have anatomy and flesh, after Chaos Flesh (Godot has none): `slash/world/flesh.hpp`
(`engine/world/src/flesh.cpp`, `flesh_solver.cpp` and `flesh2d.cpp`, built with
`-ffp-contract=off`) places a recipe's bones and 28 kinds of muscle on a body's `BodyFrames`
(`body_frames_3d` from its measures, `body_frames_2d` the cut-out's, turned), sized by its
body's `muscle` and `fat` (`make_anatomy`), and `build_flesh` makes the muscles rings of points
joined into tetrahedra (triangles in 2D). `step_flesh` is an XPBD solver both dimensions share:
end rings pinned to their bones, elements keeping shape and volume, fibers pulling by
activation, points springing toward the muscle's shape between its moved ends and staying out of
its bone, and the root's acceleration felt; `bind_skin` and `skin_offsets` move skin by it. The
`flesh_3d` system (`engine/scene3d/src/flesh.cpp`, priority 957, added by `canvas::install`)
steps each `Flesh3D` on a character's `Model3D` from `pose_of` and `apply_modifiers`,
and `Renderer::skinned_mesh` adds its `FleshPose` after skinning; `flesh_2d` steps each
`Character2D`'s `Flesh2D` and `gather_polygons` adds `flesh_offsets_2d` to its parts, with a
`<id>_anatomy` view. A character model's `::skeleton` and `::muscles` forms
(`engine/scene3d/src/anatomy.cpp`) draw the anatomy on its rig. `character.place`'s `flesh` adds
them and `flesh.get` (`engine/command/src/character.cpp`) reads the muscles.
`examples/character-anatomy` flexes an arm in 3D and 2D.

Animals are creature recipes, after Unreal's Mutable and its animal AI (Godot has none): a
`.creature.json` file (`slash/world/creature.hpp`, `engine/world/src/creature.cpp`, built with
`-ffp-contract=off`; `$cli schema creature`) gives a body plan (quadruped, bird, fish or insect),
measures, a look, parts and a behavior, and `extends` another. `world::creature_measures`,
`creature_skeleton`, `creature_animations` and `creature_pose` are what both dimensions share:
`scene3d::build_creature` (`engine/scene3d/src/creature.cpp`, on the rig helpers
`src/rig_builder.hpp` shares with characters) builds a skinned `Model` with its animations,
which `load_model` and `check_model` take as recipes, and `creature_cutout` a side-on cut-out
that the `character_2d` system keeps as `<id>_<bone>` children of each `Creature2D`. The `fauna`
system (`engine/npc/src/fauna.cpp`, priority 964, added by `npc::install`) runs each `Creature3D`
and `Creature2D` from its `CreatureMind`: fleeing what hunts it and people by temperament,
attacking, hunting when hungry, following its owner, carrying its rider, and roaming, grazing
and herding by boids with choices hashed from its ID and the tick, moving its transform or
character body, playing its animations and posing its cut-out, and setting an `NpcAgent`'s
`threatened`, `hungry` and `tamed` facts. `npc::feed`, `ride`, `steer` and the rest
(`slash/npc/fauna.hpp`) are gameplay code's; `creature.place`, `creature.populate`,
`creature.interact` and `creature.state` (`engine/command/src/creature.cpp`) an agent's.
`examples/fauna` keeps a meadow and a pond.
A recipe's `motion` block tunes its animations by name (`world::AnimationMotion`: length,
speed, loop, amplitude, each bone's `scale`, `shift` and `hold`, added `waves`, and new animations
`from` a built-in one), which `creature_pose` and `creature_animations` play, so the model, the
cut-out and the `fauna` system follow it; a `Creature3D`'s or `Creature2D`'s own `motion` is a
merge patch over it (`load_creature`'s overload, `with_motion`), which `tune_creature_clips`
plays on the recipe's model by retiming its clips, so it can't add an animation.
`animation.preview` (`engine/command/src/creature.cpp`) measures one as numbers (each bone's
range by axis and, from the feet's places by forward kinematics, the `gait`: stride, how fast a
planted foot sweeps back and the `slide` against the body's speed) and draws a `filmstrip` in a
private `Session`; `animation.tune` sets a motion, scales bones to `ranges` and plants the feet
by `slide` (`length` or `swing`), writing the instance's motion or the recipe file.
`examples/animation-tuning` plants a deer's feet and tunes a stag and a 2D fox.

Vehicles are recipes, after Unreal's Chaos Vehicles (Godot has only `WheeledBody3D`): a
`.vehicle.json` file (`slash/world/vehicle.hpp`, `engine/world/src/vehicle.cpp`, built with
`-ffp-contract=off`; `$cli schema vehicle`) says what a car, bike, truck, tracked vehicle, boat
or aircraft looks like (body, seats, doors, wheels, wings, and `parts` of its own: shapes or
model files placed in its space) and its own physics (mass and center
of mass, the engine's torque curve, gears, differential, steering, brakes, suspension, tires,
buoyancy, lift), what it does to whoever it hits and the `design` a test drive must meet, and
`extends` another. `world::drive_step` is the drivetrain both dimensions share (rpm from the
wheels, an automatic gearbox, throttle against the way it rolls braking, steering eased and
reduced at speed), and `vehicle_side_view` its 2D profile. `scene3d::build_vehicle`
(`engine/scene3d/src/vehicle.cpp`) builds its model, with `seat_` and `door_` points and convex
collision, which `load_model` and `check_model` take as recipes. The `vehicles_3d` system
(`engine/physics3d/src/vehicle_tool.cpp`, priority 945) keeps each `Vehicle3D`'s `RigidBody3D`
(the recipe's mass and `center_of_mass`, a field `RigidBody3D` and `RigidBody2D` now have, as
Godot's) and `WheeledBody3D` with a `Wheel3D` child a wheel, drives the wheels from its
`controls` (tracked vehicles slewing on tracks that slide), floats boats at four corners, flies
aircraft by thrust, lift and control torques through `constant_force` and `constant_torque`,
keeps bikes upright, carries `VehicleRider`s in their seats and hits characters, creatures and
ragdolls (`vehicle_hit`); `vehicles_2d` (`engine/physics2d/src/vehicle_tool.cpp`) does the same
for a `Vehicle2D` from the side, its wheels `RigidBody2D` circles on `TrackJoint2D`s and
`SpringJoint2D`s. `enter_vehicle` and `exit_vehicle` are gameplay code's; `vehicle.place`,
`vehicle.control`, `vehicle.tune` (a merge patch kept in the vehicle's `overrides`),
`vehicle.check` (a test drive in a private `Session` held to the design), `vehicle.design`
(writes a recipe an agent designed after fitting its torque, drag, brakes and steering to
targets by test drives, the unwritten recipe handed to each drive's session through
`SessionOptions::files`) and `vehicle.state` (`engine/command/src/vehicle.cpp`) an agent's.
`examples/vehicles` drives a car, a boat and a tank and a 2D pickup, and designs a dune buggy.

Physics materials and fields make one simulation, after Unreal's Chaos. A
`.physics_material.json` file (`slash/world/physics_material.hpp`: Godot's `PhysicsMaterial`
with Chaos's density, strength and surface; `$cli schema physics_material`) is named by a
body's `physics_material`; each space reads it through the world's assets
(`Space::material`, kept by version) and gives the body its computed friction and bounce
(Box2D takes no negative values, so 2D keeps rough and absorbent in the shapes'
`userMaterialId` for `combine_friction` and `combine_bounce`), its mass by density times its
shapes' volume or area, its `strength` as a `Destructible3D`'s fallback threshold and its
`surface`, which `RayHit` and `physics.raycast` report. `world::PhysicsFields3D` and
`PhysicsFields2D` (`slash/world/physics_field.hpp`, built with `-ffp-contract=off`) sample the
enabled `PhysicsField3D`s and `PhysicsField2D`s at a place; `Space::apply_fields`
(`engine/physics3d/src/fields.cpp`, `engine/physics2d/src/fields.cpp`) applies them before each
step (forces, velocities, anchors as kinematic bodies, strain as impacts, kill after the step
as `field_killed`, sleep), soft bodies' points included, and `Emitter3D` and
`Emitter2D` add `particle_force` per particle. `physics_3d.substeps` and
`physics_2d.substeps` set Jolt's collision steps and Box2D's substeps.
`examples/physics-fields` blasts crates and slides a 2D puck. While `world::record_physics`
records (`physics.debug`'s `record`; `slash/world/physics_debug.hpp`), each space adds a
`PhysicsDebugStep` per step, as Chaos's Visual Debugger: contacts (3D from the contact
listener's manifolds, `note_debug_contact`; 2D from `b2Body_GetContactData`), joints' force and
stretch and bodies awake after it (`finish_debug` in each `src/debug.cpp`) and fields' effects
where `apply_fields` applies them. `physics.cache` records rigid bodies' transforms after each
step (`world::PhysicsCache`, `slash/world/physics_cache.hpp`, added to by both spaces) and
writes them as a `.anim.json` animation that freezes them kinematic and keys them where their
motion bends, as Chaos Cache, for an `Animator` or a cinematic to play back
(`engine/command/src/physics_debug.cpp`). `examples/physics-debugger` reads a blast and replays
the collapse.

Character cloth is Chaos Cloth's: `Cloth3D` makes the surfaces of its entity's model drawn
with its `section` material one Jolt soft body (`engine/physics3d/src/cloth.cpp`) skinned by
Jolt's `SoftBodySharedSettings::Skinned`: `scene3d::cloth_section` (`src/cloth.cpp`) gathers
the surfaces, welded vertices, skin weights and rest joints, `cloth_shares` gives each vertex
its share of `max_distance` (uniform, a gradient or a vertex color channel), and before each
step `cloth_joints` poses the skeleton as drawing does and `SkinVertices` moves the skinned
points, the free ones held within their max distance, out of the backstop sphere and tethered
by long range attachments. The pose goes back in `SoftBodyPose::surfaces`, in the model's
space, which `Renderer` draws in place of the skin (`cloth_mesh`). `Cloth2D` is a sheet of
points on a cut-out that physics2d steps after the bodies with position-based dynamics of its
own (`engine/physics2d/src/cloth.cpp`: edges, bends, tethers, max distance, Box2D rays and
closest points for collision, `WindField2D` and `PhysicsFields2D`), handing the points to the
canvas through `world::set_cloth_2d_source` (`slash/world/cloth2d.hpp`), which draws them with
its texture. `examples/character-cloth` swings both capes.

World streaming is SlashEngine's own (Godot has none), after Unreal's World Partition: the
`world_streaming` system (`engine/scene3d/src/streaming.cpp`, priority -700, added by
`canvas::install`) measures from each enabled `StreamingSource3D` to each `StreamedScene3D`'s
box and loads its `.scene.json` file onto a new entity `<id>_scene` with
`world::instantiate_prefab`, as a child of the cell, or destroys it past `unload_margin`,
emitting `streamed_in`, `streamed_out` and `stream_failed` events. `streaming.state` lists the
cells and `check_streaming` backs `scene validate`. `examples/world-streaming` walks a road.

Double precision is Godot's `precision` build option: `-DSLASH_PRECISION=double` (the `double`
preset; [docs/reference/precision.md](precision.md)) makes `slash::real_t`, and
with it `Vec2`, `Vec3` and `Quat`'s fields, doubles (`engine/core` defines
`SLASH_REAL_T_IS_DOUBLE`), builds Jolt with `DOUBLE_PRECISION` (`to_jolt_place` and
`from_jolt_place` in `engine/physics3d/src/space.hpp` carry places as `JPH::RVec3`) and adds
`, double precision` to `SLASH_COMPILER_ID`, so gameplay libraries match. What the render device
takes stays float: `Vec2f` and `Vec3f` (`slash/core/math.hpp`, the same types as `Vec2` and `Vec3`
in single builds) and `to_float`. `scene3d::Renderer` draws around a render origin, the camera's
place rounded to 1024 meters (`render_origin_for`; zero in single builds): `global` in `prepare`
places everything relative to it. Code that mixes the two narrows with `static_cast<float>` or
`to_float` and passes `<real_t>` to `std::min`, `max` and `clamp`; the `double` CI job builds
with warnings as errors and runs the unit tests. `examples/large-world-precision` drops a crate
ten thousand kilometers out.
