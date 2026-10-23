# Import a Unity 3D scene

`project.import` converts a Unity project's 3D scenes too. [unity/](unity/) holds "Courtyard" as
Unity 2022.3 saves it: its project settings and one scene,
[Courtyard.unity](unity/Assets/Scenes/Courtyard.unity), with a grass plane, a crate that falls onto
it, a lamp post from an OBJ file ([Lamp.obj](unity/Assets/Models/Lamp.obj)) with a light under it,
the sun, the main camera and Unity's Default-Skybox. [calls.jsonl](calls.jsonl) imports it, opens
the scene, lets the crate fall and looks at the frame, as an agent would over MCP or JSON-RPC;
[results.jsonl](results.jsonl) holds the replies.

- The scene has more of Unity's 3D components than 2D ones, so it converts in 3D: objects nest
  through `Transform3D` `children`, and sizes stay in meters.
- Unity's axes are left-handed and Godot's right-handed, so the scene is mirrored in x: the crate
  at x 1 in Unity is at x -1, and its 30-degree turn about y is -30 (`-0.523599`). The picture
  stays as Unity draws it, crate on the right of the lamp post.
- Unity's cameras and lights face along +z and Godot's along -z, so the camera and the sun turn
  half a turn about y: the camera, tilted 15 degrees down in Unity, has rotation
  `[-0.261799, 3.141593, 0.0]`.
- The plane and the cube become a `PlaneMesh` 10 meters across (scaled by 2) and a `BoxMesh`.
  The crate's material, of Unity's Standard shader, and the grass's, of URP's Lit shader, become
  `StandardMaterial3D`s: their colors, and roughness as 1 minus their smoothness.
- The lamp post is an instance of the OBJ model, so it becomes a `MeshInstance3D` whose `mesh` is
  the file; the `Glow` object Unity added under it stays under it, an `OmniLight3D`.
- The `Post` object names the lamp's mesh out of the OBJ file, as objects of a prefab made from a
  model's parts do, so the import writes that mesh to a glTF file of its own,
  `Lamp_Post.gltf`, which its `MeshInstance3D` names. Its one material, the crate's, is its
  `StandardMaterial3D`, and its convex mesh collider becomes a `convex_polygon` of the post's
  eight corners on a `StaticBody3D`.
- The crate's `Rigidbody` and `BoxCollider` become a `RigidBody3D` and its shape, with Unity's
  default friction of 0.6. The plane's mesh collider, with no `Rigidbody`, becomes a
  `StaticBody3D` of the plane's two triangles.
- Default-Skybox becomes a `ProceduralSkyMaterial` in the colors Unity's procedural sky has, and
  the camera clearing to it makes the `WorldEnvironment`'s background the sky, which lights the
  scene too.
- The reflection probe doesn't convert, and the reply lists it under `unsupported`, with the
  skybox, which is approximated.

After 120 ticks the crate rests on the grass, its center about half a meter (`0.48`) above the
ground. `frame.capture` reads the sky, the grass, the crate, the lamp post, the sky's ground
beyond the plane and the post in the crate's colors from the frame the camera sees.
