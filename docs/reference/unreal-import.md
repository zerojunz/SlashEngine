# Importing Unreal Engine projects

[`project.import`](commands.md#projectimport) and [`slash-cli import`](cli.md#import) convert an
Unreal Engine 5 project's levels into a SlashEngine project in one pass: static meshes, landscapes,
lights, the sky and fog, cameras, physics bodies and collision, trigger and blocking volumes,
sounds, both input systems and the project's settings. Blueprints and C++ classes don't run, since
SlashEngine games are plain C++: each becomes a storyboard task to port it, and each C++ class a C++
skeleton whose component the level's actors fill in.
[examples/import-unreal](../../examples/import-unreal/) imports a small 3D game.

## How it reads the project

Unreal's levels and assets (`.umap`, `.uasset`) are binary packages that only Unreal reads, so
the import runs the project's own Unreal Editor in the background, as Godot runs Blender for
`.blend` files: no Unreal code is linked or shipped, and Unreal's license covers running the
editor you installed. The editor runs `UnrealEditor-Cmd <project>.uproject -run=pythonscript
-script=unreal_export.py` with the Python Editor Script Plugin and the glTF Exporter plugin
(both ship with Unreal 5 and are enabled for the run), headless and unattended, with
`-AllowCommandletRendering` so the glTF exporter can bake materials. The script
(`tools/importer/src/unreal_export.py`) opens each level and writes into
`<project>/.slash/import/unreal/`:

| File | What |
| --- | --- |
| `export.json` | Each level's actors and their components with their settings and the values of their C++ classes' properties, the Blueprints, the Enhanced Input actions and mapping contexts, and each mesh's bounds and collision |
| `meshes/<package>.glb` | Each static and skeletal mesh the levels place, as Unreal's glTF exporter writes it |
| `materials/<package>.glb` | Each material a component overrides a mesh's slot with, on a preview mesh |
| `text/<package>.t3d` | Base materials and material functions as Unreal writes them as text, read for world-aligned materials |
| `animations/<package>.glb` | Each animation sequence a skeletal mesh component plays, on its skeleton |
| `sounds/<package>.wav` | Each sound wave the levels play |
| `landscapes/<level>/<actor>.r16` | Each landscape's heights, its streaming proxies' too, as Unreal's own `.r16` heightmaps are |
| `unreal.log` | The editor's log; the end of it is in the error when the export fails |

The importer reads the C++ headers and sources in `Source/` itself first, and hands the script a
`request.json` in the same folder naming each class's `EditAnywhere` and `EditInstanceOnly`
properties, so it writes the values each actor and component of the class holds (a class the
editor can't load, say because the project's code isn't built, is reported as "C++ class the
editor did not load"). It then converts what the script wrote, and reads
`Config/DefaultEngine.ini`, `DefaultGame.ini` and `DefaultInput.ini` itself.

Which editor runs: the importer's `unreal_editor` option (`--unreal-editor` on the command
line: an `UnrealEditor-Cmd` program or an engine folder such as `UE_5.4`), else
`SLASH_UNREAL_EDITOR`, else an engine the Epic Games Launcher installed (its
`LauncherInstalled.dat`, then the default install folders), preferring the version the
`.uproject` file's `EngineAssociation` names and else the newest. A source build registered under
a GUID is found through `Install.ini` on macOS and Linux; on Windows, set `SLASH_UNREAL_EDITOR`.
Unreal Engine 5 only.

A folder imports as an Unreal project when it holds a `.uproject` file (and is neither a Godot
nor a Unity project); the `.uproject` file itself names one too. Running the editor takes
minutes and needs the session's `build` capability, and a running game refuses it.

## What an import writes

| File | From |
| --- | --- |
| `<level>.scene.json` beside each `<level>.umap` | The level's actors, as entities and components |
| `<mesh>.glb` beside each static mesh's `.uasset` | The mesh, with the collision Unreal gives it (see [Collision](#collision)) |
| `<mesh>_<materials>.glb` beside a mesh's `.glb` | The mesh drawn with a component's override materials (see [Materials](#materials)) |
| `<sequence>.glb` beside each animation sequence's `.uasset`, and `<sequence>.anim.json` for a looping one | The sequence (see [Animation](#animation)) |
| `<sound>.wav` beside each sound's `.uasset`, and `<sound>.audio.json` for a looping one | The sound wave |
| `<level>_<entity>.height.png` and `<level>_<entity>.color.png` beside the level | A landscape's heights and holes (see [Landscapes](#landscapes)) |
| `project.json` | The project's name, version, main level, gravity and input, merged into the project's own if it has one |
| `src/imported/<name>.cpp` | A C++ skeleton per C++ class, in the gameplay sources (see [Blueprints and C++](#blueprints-and-c)) |
| `src/imported/scripts.cpp`, `src/game.cpp` | The file registering every skeleton, and a game module calling it when the project's code has none |
| `storyboard/work.json` | A task per Blueprint and per C++ class, merged into the project's own tasks |
| `import/unreal.json` | The full report: every actor's entity and class, the axes, kill height, game mode, character movement settings, and everything that didn't convert |

Unreal's paths map to the project's folders: `/Game/X` is `Content/X`, `/Engine/X` is
`Engine/Content/X` and a plugin's `/<Plugin>/X` is `Plugins/<Plugin>/Content/X`. When the Unreal
project is inside the SlashEngine project, the converted files go beside the originals
(`/Game/Maps/Main` in `unreal/` is `unreal/Content/Maps/Main.scene.json`); otherwise they are
written at their paths in the Unreal project, and the Blueprints and C++ sources the tasks link
to are copied in.

## Units and axes

Unreal measures in centimeters with z up and is left-handed; SlashEngine's 3D is Godot's, in
meters with y up. The glTF exporter writes Unreal's x, z and y as the file's x, y and z, times
0.01. The importer confirms that from the meshes, by matching each exported file's bounds to the
bounds Unreal reported for it, and places the whole level the same way, so meshes need no turn:
Unreal's x stays x, its z is y and its y is z, and positions are times 0.01. The report's `axes`
and `meters_per_unit` say what it found. Rotations convert with the axes. Unreal's cameras and
directional, spot and rect lights face along their x, Godot's along their -z, so those turn a
quarter about y, and what hangs from them is turned back.

## Actors and components

Each actor becomes an entity named after its label in snake case (`Crate Stack` is
`crate_stack`); its root component is that entity, and every other scene component a child
entity named `<actor>_<component>`. Attached actors hang under the component they are attached
to. Hidden actors are `visible: false`, and so are hidden components with nothing under them; a hidden component that holds others, such as a character's capsule, stays shown, since Unreal hides only the component itself and a `Transform3D` hides its children too. The report's `entities` maps each actor's
name to its entity.

| Unreal | SlashEngine |
| --- | --- |
| `StaticMeshComponent` | `MeshInstance3D` naming the mesh's `.glb`; `bCastShadow` off is `cast_shadow: "off"`. With `OverrideMaterials`, a copy of the mesh's `.glb` drawn with them (see [Materials](#materials)) |
| `InstancedStaticMeshComponent`, `HierarchicalInstancedStaticMeshComponent` | A child `MeshInstance3D` entity per instance (up to 2048) |
| `SkeletalMeshComponent` | `MeshInstance3D`; one that plays an animation sequence ("Use Animation Asset") gets an `AnimationPlayer` playing it (see [Animation](#animation)), and one run by an Animation Blueprint gets an `AnimationTree` of its sequences (see [Animation](#animation)) |
| `CameraComponent`, `CameraActor` | `Camera3D`: the field of view across (`keep_aspect: "width"`), or `orthogonal` with its width; the camera of the pawn player 0 possesses is `current` |
| `SpringArmComponent` | Its children placed at the end of the arm (`TargetArmLength` back, plus `SocketOffset`) |
| `DirectionalLightComponent` | `DirectionalLight3D`: energy is lux / 10, so Unreal's default sun is 1; `LightSourceAngle` is `light_angular_distance` |
| `PointLightComponent` | `OmniLight3D`: energy is candelas / 8 (lumens are divided by 4π, EV by Unreal's formula); `AttenuationRadius` is the range and `SourceRadius` the size |
| `SpotLightComponent` | `SpotLight3D`, its outer cone angle as `spot_angle` |
| `RectLightComponent` | A `SpotLight3D` of 89 degrees |
| `SkyLightComponent` | The `WorldEnvironment`'s ambient light, from the sky or its color, at its intensity |
| `SkyAtmosphereComponent` | A `sky` background with a `PhysicalSkyMaterial`. Godot's defaults are Unreal's air in Godot's color space, so only a changed Rayleigh color (made linear at the default's brightness), Rayleigh scale (`rayleigh_coefficient`), Mie scale (`mie_coefficient`) and anisotropy (`mie_eccentricity`) are written. Godot's physical sky is far dimmer for a sun than Unreal's, so `energy_multiplier` makes its zenith, with the sun 45° up, a sixth as bright as a white surface facing the level's first directional light, as a clear sky is |
| `ExponentialHeightFogComponent` | The `WorldEnvironment`'s height fog: density / 10, its inscattering color (when that is black, as Unreal 5.8's default is, the sky's color through `fog_aerial_perspective` 1, as Unreal takes it from the sky atmosphere), its height and falloff, and volumetric fog |
| `AudioComponent`, `AmbientSound` | `AudioStreamPlayer3D` with attenuation (`unit_size` the inner radius, `max_distance` its end), else `AudioStreamPlayer`; volume in dB, pitch and auto activate |
| `BoxComponent`, `SphereComponent`, `CapsuleComponent` | `CollisionShape3D` (see [Collision](#collision)) |
| `CharacterMovementComponent` | `CharacterBody3D` with its walkable floor angle; its speeds and jump in the report |
| `NiagaraComponent` | `CPUParticles3D` or `GPUParticles3D` per enabled emitter (see [Particles](#particles)) |
| `ArrowComponent`, `BillboardComponent` | Nothing: they show only in Unreal's editor |

The level's `WorldEnvironment` is an `environment` entity. It tonemaps with `aces` (white 6), as
Unreal's filmic tonemapper does, and unless the project's `r.DefaultFeature.AutoExposure` is off it
has a `CameraAttributesPractical` with Unreal's auto exposure: `auto_exposure_scale` is Godot's
0.4 at Unreal's default exposure compensation (`r.DefaultFeature.AutoExposure.Bias`, 1), doubled
for each stop over it, and the sensitivities span Unreal's range (EV100 from -10 to 20 with
`ExtendDefaultLuminanceRange`, 0.03 to 8 cd/m² without) in the import's light units. Godot meters
the whole frame's average where Unreal leaves out its brightest and darkest tenth, so a level lit
mostly by a few bright pools comes out darker than in Unreal. A `PostProcessVolume`'s own settings
aren't read. A level with no camera views from the eyes of the pawn player 0 possesses, or else
the player start: a `Camera3D` 64 cm up (Unreal's `BaseEyeHeight`), named in the report's
`camera_added`. The world settings' `KillZ` is the report's `kill_y` and its game mode
`game_mode`.

## Collision

Each mesh's `.glb` gets the collision Unreal gives it, as Godot's importer makes it (see
[`model.cleanup`](commands.md#modelcleanup)): its triangles when it uses its complex collision as
simple (and isn't simulated), a convex decomposition when it has several simple shapes, a convex
hull for one, and none when it has no simple collision (reported). The script counts the simple
shapes in the mesh's body setup, since a commandlet has no static mesh editor subsystem. A
static mesh that collides is
then a static body, as every model with collision is; a movable one is an
`AnimatableBody3D`, and one Unreal simulates a `RigidBody3D` with its mass and damping.

Box, sphere and capsule components become `CollisionShape3D`s, sized from their extents and
radii. Those with a trigger profile (`Trigger`, `OverlapAll`, `OverlapAllDynamic`,
`OverlapOnlyPawn`) are in an `Area3D`; blocking ones in a `StaticBody3D`, an
`AnimatableBody3D` when movable, or the body of the actor above them; a character's capsule is
its `CharacterBody3D`'s. A player start's capsule only shows where a player fits and is left out.

Volumes are boxes as big as their bounds: a `BlockingVolume` is a `StaticBody3D`, a
`TriggerVolume`, `PhysicsVolume` and other volumes an `Area3D` (what a volume does is the game's
code). Post-process, navigation bounds, Lightmass, level streaming, cull distance and audio
volumes are reported and placed as empty entities.

## Input

Legacy input from `DefaultInput.ini`: each action mapping is an action, with its shift, control,
alt and command modifiers, and each axis mapping two actions, `<Axis>/positive` and
`<Axis>/negative`, by the sign of its scale. Enhanced Input's actions are named after their
assets (`IA_Jump`), with each mapping context's keys: a boolean or 1D action is one action (a 1D
one `/positive` and `/negative`), and a 2D one four, `/right`, `/left`, `/up` and `/down`, by the
`Negate` and `Swizzle Input Axis Values` modifiers on each key. Dead zone, exponential response
curve and smoothing modifiers are left to the input system's own; other modifiers and triggers
are reported. Keys, mouse buttons and gamepad buttons and sticks take Godot's names (a stick's y
is flipped, since Unreal's points up); mouse movement is reported, since no binding moves it.

## Project settings

| Unreal | `project.json` |
| --- | --- |
| `DefaultGame.ini` `ProjectName`, else the `.uproject` file's name | `name` |
| `ProjectVersion`, its first three parts | `version` |
| `DefaultEngine.ini` `GameDefaultMap`, else the first level | `main_scene` |
| `DefaultGravityZ` | `physics_3d.gravity`, in meters |

## Materials

The export script writes each mesh with the materials its slots name, as the glTF exporter makes
them, and each material a component overrides a slot with as a `.glb` of its own (the exporter
writes it on a preview mesh). A component with `OverrideMaterials` draws a copy of its mesh's
`.glb`, beside it, named after the mesh and the override materials
(`SM_Crate_MI_CrateDark.glb`, or a hash when that would be long): each overridden slot's
material, found by the slot's material's name or, failing that, by its place, is replaced by the
override's, with its images, and the copy keeps the mesh's collision. Components with the same
overrides share one copy. A Godot scene has no per-surface override that `MeshInstance3D` takes,
so the copy stands for it. The result's `converted` counts the copies as `material_variants`.
The override's material is named after its package, not the exporter's preview mesh.

A material that places its textures by world position (its base material, or a material
function it calls two deep, has a `WorldPosition` node or calls `WorldAlignedTexture`,
`WorldAlignedNormal` or `WorldCoordinate3Way`) is baked over the mesh's texture coordinates like
any other, so its pattern loses its size in the world; the export script finds these in the
materials' text (T3D, as the editor copies nodes) and the result's `unsupported` lists each as a
`world-aligned material`.

## Animation

Each animation sequence a skeletal mesh component plays is exported on its skeleton as a `.glb`
where Unreal keeps it (`/Game/Characters/A_Idle` is `Content/Characters/A_Idle.glb`), and the
component's entity gets an `AnimationPlayer` that plays it on the mesh's bones by name: from where
the component's `AnimationData` left it (`SavedPosition` is `time`), at its play rate
(`speed_scale`), stopped when it isn't playing. A looping sequence plays through an `.anim.json`
library beside it that takes the animation `from` the `.glb` with `loop: "linear"`; one that
doesn't loop plays from the `.glb` itself. The report's `animations` maps each sequence to its
file, and `converted` counts them.

An Animation Blueprint's graphs don't convert, since Unreal's Python API doesn't read them, but
what they play does: the export lists the sequences the Blueprint references (and those of the
blend spaces, montages and composites it uses) and writes each as above. The importer makes them
an `<blueprint>.anim.json` library, each looping, and an `<blueprint>.animtree.json` state
machine with a state per sequence, starting at the first whose name holds "idle" (else the
first) and with no other transitions, and a component the Blueprint runs gets an `AnimationTree`
over the two, so the mesh idles rather than standing in its bind pose. The Blueprint's task links
the tree, to add the transitions, blends and parameters its graphs have; `converted` counts
`animation_trees`. Montages and blend spaces aren't played themselves.

## Landscapes

A `Landscape` actor becomes an entity with an `HTerrain` (the HTerrain plugin's terrain, which
SlashEngine has in Godot's place), its heights from the `.r16` file the export writes: 16-bit
numbers, 32768 at the landscape's own height and 128 a unit of its space, with its streaming
proxies' heights in it (the script writes it with the landscape's `ExportHeightmap`, or where the
editor lacks that, renders it with `LandscapeExportHeightmapToRenderTarget`). The HTerrain keeps the
numbers as they are: its `height_range` is -256 to 32767/128, so each value stands for the height
Unreal gives it, and its `map_scale` is the actor's scale in meters, so a vertex is as far from the
next as in Unreal. An HTerrain is square and a power of two plus one across, 65 to 4097, so the
landscape fills the corner of the smallest that holds it and the rest is a hole in the color map,
which neither draws nor collides; one over 4097 vertices across keeps every second (or fourth)
vertex, and says so. The entity is placed at the landscape's first vertex, in the world's axes; its
streaming proxies make no entities.

The landscape collides unless its collision is off. Its material and its paint layers' weights don't
convert, and are reported: an HTerrain shades with HTerrain's Classic4 shader and up to four
textures mixed by a splat map, which the port sets. Nor do its holes from a visibility layer,
splines, grass or foliage.

## Particles

A `NiagaraComponent`'s system becomes Godot's particles. The export reads each emitter's stack
through `NiagaraExternalEditUtilities`, which Unreal Engine 5.8 added (an older editor reports
"Niagara emitters" and the component emits nothing): its sim target, its emitter data, each
module of its four scripts with its inputs' values and dynamic input chains, and its renderers.
An emitter simulated on the CPU becomes a `CPUParticles3D`; one simulated on the GPU becomes a
`GPUParticles3D` and a `.process_material.json` file beside the system's `.uasset`, named
`<system>_<emitter>`. A system of one emitter is the component's entity; with several, each enabled
emitter is a child entity named `<component>_<emitter>`.

Niagara's particles have no gravity, spread or speed but what modules give them, so those start
at 0. Lengths go from centimeters to meters and vectors into Godot's axes, as everywhere else.
A random range (Niagara's `Uniform Ranged` dynamic inputs) is a `_min`/`_max` pair; a `User.`
parameter's link is read from the system's user variables.

| Niagara | SlashEngine |
| --- | --- |
| `EmitterState` | `one_shot` for `Loop Behavior` `Once`; a number of loops is reported and loops for ever |
| `SpawnRate`, `SpawnBurst_Instantaneous` | `amount` as the rate times the longest lifetime plus the burst; a burst alone is `explosiveness: 1` |
| `InitializeParticle` | `lifetime` (the longest) and `lifetime_randomness`, `color`, the sprite size or mesh scale as `scale_min`/`scale_max`, and the sprite rotation angle |
| `AddVelocity` (in particle spawn) | `direction`, `initial_velocity_min`/`max` and, for a random vector, the `spread` of a cone through its range's corners |
| `AddVelocityInCone` | `direction` along `Cone Axis`, `spread` half of `Cone Angle`, and the speed |
| `GravityForce` | `gravity` |
| `ShapeLocation`, `SphereLocation`, `BoxLocation`, `CylinderLocation` | The emission shape: `sphere` or `sphere_surface`, `box` (a plane is a flat box), and `ring` for cylinders, rings and discs |
| `SpriteRotationRate` | `angular_velocity_min`/`max` |
| `CurlNoiseForce` | `turbulence_enabled` on GPU emitters, at Godot's own strength and scale; reported on the CPU |
| `SolveForcesAndVelocity`, `ParticleState`, `EmitterLifeCycle` and the like | Nothing: Godot's particles move, age and die so anyway |
| Sprite renderer | A `QuadMesh` with a `StandardMaterial3D` billboarding as particles, unshaded and colored by the particles (`draw_pass_1` on the GPU) |
| Mesh renderer | Its first mesh's `.glb` as `mesh` (CPU) or `draw_pass_1` (GPU) |
| The system's `bDeterminism` and `RandomSeed` | `seed` (and `use_fixed_seed` on the GPU) |
| The system's warmup time | `preprocess` |
| The system's fixed bounds | The `GPUParticles3D`'s visibility box |
| The emitter's `bLocalSpace` | `local_coords` |
| The component's `bAutoActivate` off | `emitting: false` |

Over-life modules (`ScaleColor`, `ColorFromCurve`, `ScaleSpriteSize`, `SubUVAnimation` and the
like), event handlers, other renderers, sprite materials (particles draw in their color) and
every other module are reported, once per kind. Unreal 5.8's documentation doesn't spell out how
an input's value is written, so the converter reads Unreal's exported text loosely (numbers,
vectors, colors, enums and dynamic inputs' `Minimum`/`Maximum` by name) and reports a value it
can't read as "Niagara input that couldn't be read" rather than guessing it.

## Blueprints and C++

Each Blueprint becomes a storyboard task (`task/port-<name>`) linking to its `.uasset` and the
scenes its actors are in, to write what it does as C++ components and systems; a widget
Blueprint's task is to rebuild it as controls, and an animation Blueprint's as an
`AnimationTree`. The task says which class it derives from and which entities its actors became.
Each `UCLASS` in `Source/` becomes a task linking to its header, listing its `UPROPERTY` fields,
its `UFUNCTION`s and the entities of its actors. The report's `class` gives each actor's class,
so the port knows which entities it runs on; actors' `tags` are kept there too.

Each `UCLASS` also becomes a C++ skeleton for the agent to fill in, `src/imported/<name>.cpp` in
`project.json`'s gameplay sources, laid out as [a Unity script's](unity-import.md#scripts) is:
its `UENUM`s, its `static constexpr` members as constants, its member variables as a component
named after the class without its `A` or `U` (`ACoinPickup` is `CoinPickup`), an
`emit_<delegate>` helper per delegate it declares (`DECLARE_DYNAMIC_MULTICAST_DELEGATE_*`), a
stub per function with its body from the `.cpp` file in comments, the system calling them and
`imported::register_<name>`. The report's `classes` lists each one with its header, skeleton,
component, task and how many entities have it.

An actor of the class, of a Blueprint made from it, or holding a component of the class gets the
component, with the values its `EditAnywhere` and `EditInstanceOnly` properties hold in the level;
other properties are the component's state. `ClampMin` and `ClampMax` are a field's range and
its comment its doc. Until the code is built the component is a stand-in that keeps the values,
so the scenes open.

| Unreal | C++ |
| --- | --- |
| `bool`, `int32` and smaller, `int64`, `float`, `double`, `FString`, `FName`, `FText` | `bool`, `int`, `std::int64_t`, `float`, `double`, `std::string` |
| `FVector`, `FVector2D`, `FRotator`, `FQuat`, `FLinearColor`, `FColor` | `slash::Vec3` (in Unreal's axes and units, as written), `Vec2`, `Vec3` (pitch, yaw, roll), `Quat`, `Vec4` (`FColor` as 0 to 1) |
| `AActor*` and other actors, `U...Component*` | `entt::entity`, the entity it became |
| Assets (`UStaticMesh*`, `USoundBase*`, `TSoftObjectPtr`...), `TSubclassOf` | `std::string`, the asset's or class's path |
| `TArray<T>`, `TSet<T>`, `TMap<FString, T>` | `std::vector`, `std::map<std::string, T>` |
| Enums | `int`, the enum kept beside it |

The system calls the stubs when Unreal would call the functions:

| Function | When the system calls it |
| --- | --- |
| The constructor, `PostInitializeComponents`, `InitializeComponent`, `BeginPlay` | For each entity made since the last tick, in that order |
| `Tick`, `TickComponent` | Once a tick, with the tick's seconds |
| `NotifyActorBeginOverlap`, `NotifyActorEndOverlap` | On both entities when a body or area enters or leaves an `Area3D`, with the other entity |
| `NotifyHit` | On both entities when a `RigidBody3D` touches another body, with the other entity |
| `EndPlay`, `Destroyed` and Unreal's other overrides | Not yet; their stubs say so |

Importing again leaves a skeleton that exists as it is and rewrites `scripts.cpp`.

## Not yet converted

Landscapes' materials and paint layers, foliage, BSP brushes (convert them to static meshes in
Unreal first), Cascade particles and the Niagara modules [Particles](#particles) lists, material graphs (meshes draw with what the glTF
exporter bakes them to), Animation Blueprints' graphs, montages and blend spaces, Nanite meshes
without a fallback, movement components such as `RotatingMovementComponent`, light temperature and
IES profiles, level streaming and World Partition data layers (a partitioned level's actors are
loaded all at once), and Blueprint logic (C++ classes become skeletons to port). What a project has
of these is listed in the result's `unsupported`, once per kind with where it was first seen and
why.