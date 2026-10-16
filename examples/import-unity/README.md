# Import a Unity game

`project.import` converts a Unity project into this one. [unity/](unity/) holds "Pond Hop", a
small 2D game as Unity 2022.3 saves it: its project settings, a scene
([Pond.unity](unity/Assets/Scenes/Pond.unity)) that instances a frog prefab and coin prefabs and
has a tile map, a prefab variant ([Gold Coin.prefab](unity/Assets/Prefabs/Gold%20Coin.prefab)),
sprites with their import settings, tile assets, a physics material, an Input System actions file
and three C# scripts. Unity
writes more settings than these files keep; they keep the ones the import reads.
[main.cpp](main.cpp) makes the textures and sounds, then makes the calls an agent would make over
MCP or JSON-RPC; [expected.txt](expected.txt) holds the replies and the files the import wrote.
From a shell, `slash-cli import unity` does the same.

```text
project.import {"source":"unity"}
{"converted": {"assets": 8, "input_actions": 12, "prefabs": 3, "scenes": 1, "tile_sets": 1},
 "files": 9, "main_scene": "unity/Assets/Scenes/Pond.scene.json", "pixels_per_unit": 32.0,
 "report": "import/unity.json",
 "scripts": [{"entities": 1, "script": "unity/Assets/Scripts/CameraFollow.cs",
              "task": "task/port-camera-follow"}, ...],
 "unsupported": [{"count": 1, "what": "inactive object", "where": "Assets/Scenes/Pond.unity: Coin (2)",
                  "why": "converted hidden, with its shapes disabled; ..."}, ...]}
```

- Each `.unity` and `.prefab` file becomes a `.scene.json` file beside it, a GameObject becoming
  an entity named after it (`Lily Pad` is `lily_pad`) and nesting through `Transform2D`
  `children`.
- Unity measures in units and SlashEngine in pixels: the import takes the pixels per unit most of
  the project's sprites use (32 here) and scales positions, sizes and gravity by it, with y
  pointing down. The camera's orthographic size becomes `Camera2D` `zoom` for the project's
  window height.
- Prefabs stay prefabs. The scene's `frog` entity extends `Frog.scene.json` with only the scene's
  overrides (its position), and the prefab's objects come with it as `frog/feet` and the rest.
  The hat the scene adds under the frog is the scene's own `hat` entity. The gold coin is a
  prefab variant, so `Gold Coin.scene.json` extends `Coin.scene.json` with another sprite and
  color.
- A `SpriteRenderer` becomes a `Sprite2D`: a sprite cut from a sheet is a `region`, the pivot an
  `offset`, the sorting layer and order a `z_index`. Unity's sprites don't pass their order or
  color to the objects under them, and Sprite2D's do, so the frog, which has children, draws its
  sprite on a `frog_sprite` entity; so does the hat, whose texture has another pixels per unit.
- A `Rigidbody2D` and its colliders become a body with shapes: the frog a `RigidBody2D` with a
  capsule, the ground a `StaticBody2D` (a collider without a Rigidbody2D doesn't move) with a
  second shape on a child entity, the lily pad's `PlatformEffector2D` a one-way shape, and
  trigger colliders `Area2D`s. Layers and the layer collision matrix become collision layers and
  masks, and physics materials friction and bounce.
- The `Bank` tile map, on a `Grid`, becomes a `TileMapLayer`, and the sprites its cells show a
  tile set, `tiles.tileset.json`: the `bank.png` sheet is one atlas source of three tiles. The
  slope's custom physics shape is its collision polygon, the dirt and grass, whose cells use the
  Grid collider type, get the cell's square, and the mirrored slopes at the right are `:h`
  cells. The tiles are solid: a ray down onto the first slope stops halfway up it.
- An `AudioSource` becomes an `AudioStreamPlayer`, or an `AudioStreamPlayer2D` when it is
  spatial. The music loops, so it plays a `.audio.json` file that loops the sound.
- The player, time, physics and input settings become `project.json`: the name, version, window,
  tick rate, gravity, clear color, texture filter, and both the Input Manager's axes (`Jump`,
  `Horizontal/negative`) and the Input System's actions (`Player/Move/up`).
- SlashEngine games are plain C++, so scripts don't convert. Each becomes a storyboard task with
  what the script declares; `import/unity.json` holds the values each object gave its scripts
  (references to objects as entity IDs, to assets as paths), tags, layers, sorting layers and the
  entity ID of every object. The original scripts stay where they are, linked from the tasks.
- The assets stay where they are, since `unity/` is inside the project; importing a folder
  outside the project copies them in. Importing again updates the files and keeps each task's
  status.
