# Import an Unreal Engine game

`project.import` converts an Unreal Engine 5 project into this one. [unreal/](unreal/) holds
"Crate Run", a small Unreal Engine 5.8 game: its `.uproject` file, config files with its settings
and legacy input mappings, and a C++ class. Unreal's `.umap` and `.uasset` files are binary and
only Unreal reads them, so the import runs the project's own Unreal Editor in the background, as
Godot runs Blender for `.blend` files, with a Python script that exports each level's actors and
components, the Enhanced Input assets, the meshes, override materials and animation sequences as
glTF, the landscape's heights, the Niagara systems' emitter stacks and the sounds as WAV. Here a
stand-in for the editor copies [unreal/FakeExport](unreal/FakeExport/), what the script writes
for this project, so the example runs where Unreal isn't installed. [main.cpp](main.cpp) makes
the glTF files, the heights and the sound in code, then makes the calls an agent would make over
MCP or JSON-RPC; [expected.txt](expected.txt) holds the replies. From a shell,
`slash-cli import unreal` does the same.

```text
project.import {"dry_run":true,"source":"unreal"}
{"classes": [{"class": "ACoinPickup", "code": "src/imported/coin_pickup.cpp", ...}],
 "converted": {"animation_trees": 1, "animations": 2, "blueprints": 5, "classes": 1,
               "input_actions": 10, "landscapes": 1, "material_variants": 1, "meshes": 4,
               "particle_systems": 1, "scenes": 1, "sounds": 1},
 "engine": "unreal", "main_scene": "unreal/Content/Maps/Main.scene.json", ...}
```

- The level becomes a `.scene.json` file beside the `.umap`, in Godot's axes and meters: meshes
  as `MeshInstance3D` entities over the exported glTF files with the collision Unreal gives
  them (and a copy drawn with a component's override material), lights in Unreal's units, the
  sky, fog, the camera on the player's spring arm, physics bodies, trigger and blocking volumes,
  and an ambient sound.
- The hero's skeletal mesh plays its animation sequence with an `AnimationPlayer`, and the
  guard's Animation Blueprint becomes an `AnimationTree` state machine of its sequences.
- The landscape becomes an `HTerrain`, its heights exactly as Unreal keeps them.
- The embers' Niagara system, simulated on the CPU, becomes a `CPUParticles3D`: its spawn rate,
  lifetime, color, sprite size, cone velocity and gravity, in meters.
- The project's settings and both input systems, legacy mappings and Enhanced Input, become
  `project.json`.
- The C++ class becomes a C++ skeleton in `src/imported/`, its properties a component the level's
  actors fill in; each Blueprint and C++ class becomes a storyboard task to port it to C++, since
  SlashEngine games are plain C++.
- The report, `import/unreal.json`, lists what didn't convert and why, once per kind.
- After the import the example steps the scene: the simulated crate falls onto the floor, rays
  land on the floor and the hill, and the embers rise and fall.

The editor only ever runs as a program of its own; nothing of Unreal's is linked.
[docs/reference/unreal-import.md](../../docs/reference/unreal-import.md) maps each component and
setting.
