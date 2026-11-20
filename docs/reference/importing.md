# Importing games from other engines

Importing games from Godot, Unity and Unreal Engine: how the importer works.

Status: current · Updated: 2026-11-20

`tools/importer` converts a game made in another engine into a SlashEngine project: Godot 4,
Unity and Unreal Engine 5. It registers `project.import`, which
`slash-cli import <folder>` and slash-mcp serve; `--dry-run` and `"dry_run": true` return the
report and the files it would write. `src/godot_text.cpp` parses Godot's text formats
(`.tscn`, `.tres`, `project.godot`) and `src/godot_import.cpp` converts them: a `.scene.json`
beside each `.tscn`, a `.frames.json` per `SpriteFrames`, a `.tileset.json` per `TileSet`, a
`.theme.json` per `Theme`, a `.bus_layout.json` per `AudioBusLayout`, a `.anim.json` per
`AnimationPlayer` (tracks keep their node paths as targets), a `.process_material.json` per
`ParticleProcessMaterial` (read in 3D for a `GPUParticles3D`, `process_material_file`'s
`three_d`; a vector field's slices come from its texture's `.import` file), `project.godot`'s settings,
input map and translations merged into `project.json`, and a storyboard task per script (`task/port-<name>`)
to port it to C++. Each GDScript and C# file also becomes a C++ skeleton (`src/scripts.cpp`,
public header `slash/importer/scripts.hpp`; `read_gdscript` and `read_csharp`, which reads
Godot's and Unity's C#): `<gameplay folder>/imported/<name>.cpp` with its variables as a component
the scenes fill in, its signals as `emit_<signal>` helpers, and a stub per function, keeping the
script in comments, that a system calls when its engine would (`_ready` and `Start` on
`entity.created` events, `_process` and `Update` once a tick, `_input` per input event, Unity's
trigger and collision messages on physics events, connected methods on their signal's events).
`src/skeletons.cpp` names and writes them for both importers: skeletons that exist are kept,
`imported/scripts.cpp` registers them and a game module is written when the project's code has
none. Properties convert by the target field's reflected type, Godot's enum numbers
through its `choices`, and a value that fails its file's schema (scene, tile set, theme, bus
layout or animation) is left out and reported, so one bad value can't stop a file. Everything that didn't
convert goes in `import/godot.json`, once per kind. `src/godot_bones.cpp` works out the
`Skeleton3D` nodes Godot's importer makes of a model's skins, as its `SkinTool` does, so a
scene's bone poses, IK modifiers, ragdolls and bone attachments under them convert onto the
entity that draws the model. `src/godot_terrain.cpp` reads the HTerrain plugin's
`data.hterrain` and height maps, so its `HTerrain` nodes become SlashEngine's `HTerrain` with a
`.height.png` file. `docs/reference/godot-import.md` maps each
node and setting; `examples/import-godot` imports a small platformer and
`examples/import-godot-scripts` ports a script from its skeleton.

Games exported with Godot 4 import too: `src/godot_unpack.cpp` finds the pack in a `.pck` file or
an executable and turns it back into the project (`godot::unpack_game`, in
`slash/importer/godot_pack.hpp`): `src/godot_binary.cpp` reads binary resources, values and
`project.binary`, `src/gdscript_tokens.cpp` turns `.gdc` tokens back into source, and textures
and sounds become PNG, WAV, Ogg and MP3 files. `unpack_to` writes the recovered project into the
SlashEngine project. Encrypted packs are refused and encrypted files listed, never decrypted.
`src/godot_pack.cpp` writes all of it as Godot's export does (`godot::export_project`,
`godot::write_pack`), checked against Godot 4.5's own exports: tests and examples make their
exported games in code with it rather than committing binary files. `examples/unpack-godot`
imports a shipped game.

Games built with Unity import through AssetRipper, which is GPL and so only ever runs as a
program of its own: `src/unity_build.cpp` (`slash/importer/unity_build.hpp`) finds a build
(a player folder on any desktop, an `.apk` or `.ipa` file, a web build, compressed or not),
refuses encrypted bundles, starts AssetRipper's headless server (`SLASH_ASSETRIPPER`, else the
`assetripper` toolchain, installed on first use), has it export the Unity project into
`.slash/import/unity/`, then tidies it: scripts of Unity's packages get the GUIDs they have in
every project (`unity::package_script_guid`) and their decompiled stand-ins are dropped, so the
Unity importer converts the project as one made in Unity. Unpacking needs the `build`
capability, and a running game refuses it. Tests and `examples/unpack-unity` run
`tests/fake_assetripper.cpp`, a stand-in that serves the same calls and exports a folder.

`src/unity_yaml.cpp` reads Unity's YAML (scenes, prefabs, assets, settings and `.meta` files)
and `src/unity_import.cpp` converts a project with `Assets/` and `ProjectSettings/`: a
`.scene.json` beside each `.unity` and `.prefab` file, prefabs staying prefabs that scenes
instance (an instance's objects take Unity's IDs, `(instance ^ source) & 0x7FFFFFFFFFFFFFFF`,
and stripped objects resolve to what they stand for), units scaled to pixels by the pixels per
unit most sprites use, tile maps as `TileMapLayer`s over a `.tileset.json` cut from the sprites
their cells show (isometric and hexagonal Grids as Godot's shapes, the layer moved by
`GridPlan::offset` so cells stay where Unity draws them, anchors and pivots as `texture_origin`),
each Animator controller as a `.anim.json` library with an animation per state
(Unity's curves keyed every 60th of a second where they bend; tracks target objects by their
paths from the Animator), every `.mixer` file as one `.bus_layout.json`, 2D joints as Godot's,
URP's 2D lights as `CanvasModulate` and `PointLight2D`s through generated falloff textures and
shadow casters as `LightOccluder2D`s, objects under a `Canvas` as controls (rects as anchors and
offsets, uGUI and TextMesh Pro widgets as Godot's, their parts folded into the control's theme
and fields), settings and both input systems merged into `project.json`, and a task per C#
script and per state machine. A file with more 3D components than 2D ones (`is_3d`, the project's
count breaking ties) converts in 3D (`convert_object_3d`): mirrored in x into Godot's
right-handed axes (`place_of`), cameras, lights and quads turned half a turn about y
(`place_turned`), built-in meshes, model instances (`instantiate_model`), meshes picked out of
models as glTF files of their own (`picked_mesh`), `.mat` files as `StandardMaterial3D`
(`convert_surface`), 3D bodies and colliders, particle systems as `CPUParticles3D`
(`particles_3d`, gravity turned into the object's axes for local particles), and `RenderSettings`
as an `environment` entity. Each script's skeleton takes its serialized fields, which scenes
fill in; its trigger and collision messages make the importer set `contact_monitor` on the
object's `RigidBody2D`, and UI events' persistent calls connect to its methods.
Objects a prefab brings are written whole, so `format_scene` keeps only what differs.
`src/common.cpp` holds what both importers share: paths, the report and the tasks.
`docs/reference/unity-import.md` maps each component and setting; `examples/import-unity`
imports a small platformer.

Unreal Engine 5 projects (a folder with a `.uproject` file) import through the project's own
Unreal Editor, which only ever runs as a program of its own, as Blender does:
`src/unreal_editor.cpp` (`slash/importer/unreal.hpp`) finds it (`ImportOptions::unreal_editor`,
`SLASH_UNREAL_EDITOR`, then the Epic Games Launcher's installs), reads Unreal's `.ini` files
(`parse_config`, `parse_struct`) and runs `UnrealEditor-Cmd <project> -run=pythonscript` on
`src/unreal_export.py` (embedded in the tools), which writes each level's actors and components,
Enhanced Input, meshes, override materials and animation sequences as glTF and sounds as WAV into
`.slash/import/unreal/`.
`src/unreal_import.cpp` converts that: it finds the exporter's axes and scale by matching the
glTF files' bounds to Unreal's, places the levels in them (so meshes need no turn; cameras and
lights turn to face Godot's way), gives each mesh's `.glb` the collision Unreal gives it with
`generate_collision`, draws a component's override materials with a copy of its mesh's `.glb`
(`mesh_with_overrides`), plays a skeletal mesh's animation sequence with an `AnimationPlayer`
(`convert_animation`) and an Animation Blueprint's sequences with an `AnimationTree` state machine
(`anim_tree`), makes each landscape an `HTerrain` from the `.r16` heights the script
writes (`convert_landscape`), makes each Niagara emitter a `CPUParticles3D` or a `GPUParticles3D` with
a `.process_material.json` from its module stack, which the importer reads from the text (T3D)
the script writes of the system (`niagara_from_text` in `src/unreal_text.cpp`, `convert_niagara`), converts lights by Unreal's units, the sky, fog, bodies, shapes, volumes,
sounds and both input systems, and makes a task per Blueprint and `UCLASS`. Each `UCLASS` is
also a C++ skeleton: `src/unreal_cpp.cpp` (`read_unreal_class`) reads its header and source into
a `Script` for `src/scripts.cpp` to write, as C# scripts are, and the export's `request.json`
asks the script for the values of its edited properties on each actor (`add_class_component`).
Unreal projects need
the `build` capability. `tests/fake_unreal.cpp` is the editor's stand-in for tests and
`examples/import-unreal`, copying a project's `FakeExport/` folder;
`docs/reference/unreal-import.md` maps each component and setting.
