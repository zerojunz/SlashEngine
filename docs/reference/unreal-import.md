# Importing Unreal Engine projects

[`project.import`](commands.md#projectimport) and [`slash-cli import`](cli.md#import) convert an
Unreal Engine 5 project's levels into a SlashEngine project in one pass: static meshes, lights,
the sky and fog, cameras, physics bodies and collision, trigger and blocking volumes, sounds,
both input systems and the project's settings. Blueprints and C++ classes don't convert, since
SlashEngine games are plain C++: each becomes a storyboard task to port it.
[examples/import-unreal](../../examples/import-unreal/) imports a small 3D game.

## How it reads the project

Unreal's levels and assets (`.umap`, `.uasset`) are binary packages that only Unreal reads, so
the import runs the project's own Unreal Editor in the background, as Godot runs Blender for
`.blend` files: no Unreal code is linked or shipped, and Unreal's license covers running the
editor you installed. The editor runs `UnrealEditor-Cmd <project>.uproject -run=pythonscript
-script=unreal_export.py` with the Python Editor Script Plugin and the glTF Exporter plugin
(both ship with Unreal 5 and are enabled for the run), headless and unattended. The script
(`tools/importer/src/unreal_export.py`) opens each level and writes into
`<project>/.slash/import/unreal/`:

| File | What |
| --- | --- |
| `export.json` | Each level's actors and their components with their settings, the Blueprints, the Enhanced Input actions and mapping contexts, and each mesh's bounds and collision |
| `meshes/<package>.glb` | Each static mesh the levels place, as Unreal's glTF exporter writes it |
| `sounds/<package>.wav` | Each sound wave the levels play |
| `unreal.log` | The editor's log; the end of it is in the error when the export fails |

The importer then converts what the script wrote. It reads `Config/DefaultEngine.ini`,
`DefaultGame.ini` and `DefaultInput.ini` and the C++ headers in `Source/` itself.

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
| `<sound>.wav` beside each sound's `.uasset`, and `<sound>.audio.json` for a looping one | The sound wave |
| `project.json` | The project's name, version, main level, gravity and input, merged into the project's own if it has one |
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
to. Hidden components and actors are `visible: false`. The report's `entities` maps each actor's
name to its entity.

| Unreal | SlashEngine |
| --- | --- |
| `StaticMeshComponent` | `MeshInstance3D` naming the mesh's `.glb`; `bCastShadow` off is `cast_shadow: "off"` |
| `InstancedStaticMeshComponent`, `HierarchicalInstancedStaticMeshComponent` | A child `MeshInstance3D` entity per instance (up to 2048) |
| `SkeletalMeshComponent` | `MeshInstance3D` in its bind pose (its animations don't convert yet) |
| `CameraComponent`, `CameraActor` | `Camera3D`: the field of view across (`keep_aspect: "width"`), or `orthogonal` with its width; the camera of the pawn player 0 possesses is `current` |
| `SpringArmComponent` | Its children placed at the end of the arm (`TargetArmLength` back, plus `SocketOffset`) |
| `DirectionalLightComponent` | `DirectionalLight3D`: energy is lux / 10, so Unreal's default sun is 1; `LightSourceAngle` is `light_angular_distance` |
| `PointLightComponent` | `OmniLight3D`: energy is candelas / 8 (lumens are divided by 4π, EV by Unreal's formula); `AttenuationRadius` is the range and `SourceRadius` the size |
| `SpotLightComponent` | `SpotLight3D`, its outer cone angle as `spot_angle` |
| `RectLightComponent` | A `SpotLight3D` of 89 degrees |
| `SkyLightComponent` | The `WorldEnvironment`'s ambient light, from the sky or its color, at its intensity |
| `SkyAtmosphereComponent` | A `sky` background with a `PhysicalSkyMaterial` |
| `ExponentialHeightFogComponent` | The `WorldEnvironment`'s height fog: density / 10, its inscattering color, its height and falloff, and volumetric fog |
| `AudioComponent`, `AmbientSound` | `AudioStreamPlayer3D` with attenuation (`unit_size` the inner radius, `max_distance` its end), else `AudioStreamPlayer`; volume in dB, pitch and auto activate |
| `BoxComponent`, `SphereComponent`, `CapsuleComponent` | `CollisionShape3D` (see [Collision](#collision)) |
| `CharacterMovementComponent` | `CharacterBody3D` with its walkable floor angle; its speeds and jump in the report |
| `ArrowComponent`, `BillboardComponent` | Nothing: they show only in Unreal's editor |

The level's `WorldEnvironment` is an `environment` entity, made when the level has a sky, a sky
light or fog. A level with no camera views from the eyes of the pawn player 0 possesses, or else
the player start: a `Camera3D` 64 cm up (Unreal's `BaseEyeHeight`), named in the report's
`camera_added`. The world settings' `KillZ` is the report's `kill_y` and its game mode
`game_mode`.

## Collision

Each mesh's `.glb` gets the collision Unreal gives it, as Godot's importer makes it (see
[`model.cleanup`](commands.md#modelcleanup)): its triangles when it uses its complex collision as
simple (and isn't simulated), a convex decomposition when it has several simple shapes, a convex
hull for one, and none when it has no simple collision (reported). A static mesh that collides is
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

## Blueprints and C++

Each Blueprint becomes a storyboard task (`task/port-<name>`) linking to its `.uasset` and the
scenes its actors are in, to write what it does as C++ components and systems; a widget
Blueprint's task is to rebuild it as controls, and an animation Blueprint's as an
`AnimationTree`. The task says which class it derives from and which entities its actors became.
Each `UCLASS` in `Source/` becomes a task linking to its header, listing its `UPROPERTY` fields,
its `UFUNCTION`s and the entities of its actors. The report's `class` gives each actor's class,
so the port knows which entities it runs on; actors' `tags` are kept there too.

## Not yet converted

Landscapes, foliage, BSP brushes (convert them to static meshes in Unreal first), Niagara and
Cascade particles, materials (meshes draw with what the glTF exporter wrote; overrides are
reported), skeletal animation and Animation Blueprints, Nanite meshes without a fallback,
movement components such as `RotatingMovementComponent`, light temperature and IES profiles,
level streaming and World Partition data layers (a partitioned level's actors are loaded all at
once), and Blueprint and C++ logic. What a project has of these is listed in the result's
`unsupported`, once per kind with where it was first seen and why.
