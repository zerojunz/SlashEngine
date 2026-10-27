# Import a Godot 3D scene

`project.import` converts a Godot 4 project's 3D scenes too. [godot/](godot/) holds
"Courtyard" as Godot saves it: [project.godot](godot/project.godot) and one scene,
[main.tscn](godot/main.tscn), with a grass ground, a crate that falls onto it, a lamp post from an
OBJ file ([lamp.obj](godot/models/lamp.obj)) with a light and smoke on top, a statue instanced from a glTF
file ([statue.gltf](godot/models/statue.gltf)), the sun, a camera and a `WorldEnvironment` with a
procedural sky. [calls.jsonl](calls.jsonl) imports it, opens the scene, lets the crate fall and
looks at the frame, as an agent would over MCP or JSON-RPC; [results.jsonl](results.jsonl) holds
the replies.

- Node3D nodes become entities that nest through `Transform3D` `children`. Each node's
  `Transform3D(...)` becomes a position, a rotation in radians and a scale, as Godot's
  `Basis::get_euler` splits it (the crate is turned 30 degrees, `0.523599`, about Y).
- A `MeshInstance3D` with a `BoxMesh` or `PlaneMesh` gets that component, and its material, here
  the mesh's own `StandardMaterial3D`, becomes the entity's `StandardMaterial3D` with the same
  field names. A material override on the node wins over the mesh's, as in Godot.
- The lamp's mesh is an OBJ file and the statue an instance of a glTF scene; both become
  `MeshInstance3D.mesh` naming the file, which SlashEngine reads as Godot's importer does
  (see [obj-models](../obj-models/) and [models-3d](../models-3d/)). The statue keeps the scale
  the level gave its instance.
- Bodies, collision shapes and lights keep Godot's names and fields: the ground's `BoxShape3D`
  becomes its `CollisionShape3D` `size`, and the glow's `OmniLight3D` keeps its color, energy and
  range.
- The lamp's smoke is a `CPUParticles3D`: its settings keep their names, `draw_order = 2` becomes
  `"view_depth"`, its `Gradient` becomes `color_ramp` points, and its `QuadMesh` and the quad's
  material (`billboard_mode = 3`, `"particles"`) become the entity's own, so each puff faces the
  camera. `particles.eval` finds all 16 alive after two seconds, rising from the lamp's top.
- The `WorldEnvironment`'s `Environment` (its SSAO included) and `Sky` become its
  `WorldEnvironment` fields and a `ProceduralSkyMaterial` component; `project.godot`'s
  `3d/default_gravity` becomes `project.json`'s `physics_3d` gravity.
- The fireflies are a `GPUParticles3D`: its `ParticleProcessMaterial` becomes a
  `.process_material.json` file in 3D (its sphere emission, `direction`, `spread`, zero gravity
  and color), `visibility_aabb` becomes `visibility_aabb_position` and `visibility_aabb_size`, and
  `draw_pass_1`'s `QuadMesh` and its material become the entity's own. The `Lure`
  (`GPUParticlesAttractorSphere3D`) pulls them toward the lamp and the `Roof`
  (`GPUParticlesCollisionBox3D`) keeps them under it, both with Godot's fields. `particles.eval`
  finds 8 alive after two seconds, two thirds of the 12 a three-second lifetime keeps, glowing
  yellow. Nothing is left in the reply's `unsupported` list.

After 120 ticks the crate rests on the grass, its center about half a meter (`0.48`) above the
ground. `frame.capture` reads the sky, the grass, the crate and the statue from the frame the
camera sees.
