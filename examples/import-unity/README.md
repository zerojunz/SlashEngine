# Import a Unity game

`project.import` converts a Unity project into this one. [unity/](unity/) holds "Pond Hop", a
small 2D game as Unity 2022.3 saves it: its project settings, a scene
([Pond.unity](unity/Assets/Scenes/Pond.unity)) that instances a frog prefab and coin prefabs and
has a tile map, 2D lights and a HUD canvas, a prefab variant
([Gold Coin.prefab](unity/Assets/Prefabs/Gold%20Coin.prefab)), sprites with their import
settings, tile assets, an Animator controller
([Frog.controller](unity/Assets/Animations/Frog.controller)) and its clips, an audio mixer
([Pond.mixer](unity/Assets/Audio/Pond.mixer)), a physics material, an Input System actions file
and four C# scripts. Unity writes more settings than these files keep; they keep the ones the import reads.
[main.cpp](main.cpp) makes the textures and sounds, then makes the calls an agent would make over
MCP or JSON-RPC; [expected.txt](expected.txt) holds the replies and the files the import wrote.
From a shell, `slash-cli import unity` does the same.

```text
project.import {"source":"unity"}
{"converted": {"animation_libraries": 1, "animation_trees": 1, "assets": 9, "bus_layouts": 1,
               "input_actions": 12, "prefabs": 3, "scenes": 1, "tile_sets": 1},
 "files": 19, "main_scene": "unity/Assets/Scenes/Pond.scene.json", "pixels_per_unit": 32.0,
 "report": "import/unity.json",
 "module": "src/game.cpp",
 "scripts": [{"code": "src/imported/camera_follow.cpp", "component": "CameraFollow",
              "entities": 1, "script": "unity/Assets/Scripts/CameraFollow.cs",
              "task": "task/port-camera-follow"}, ...],
 "unsupported": [{"count": 1, "what": "component EventSystem",
                  "where": "Assets/Scenes/Pond.unity: EventSystem",
                  "why": "not needed: SlashEngine's gui system sends input to controls itself"},
                 ...]}
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
- The lantern hangs from a `HingeJoint2D` with limits of 60 degrees either way, which becomes a
  `PinJoint2D` on a `lantern_hinge` entity under it, pinning it to the world at the hinge's
  anchor. A second in, it has swung about 25 degrees.
- The `Bank` tile map, on a `Grid`, becomes a `TileMapLayer`, and the sprites its cells show a
  tile set, `tiles.tileset.json`: the `bank.png` sheet is one atlas source of three tiles. The
  slope's custom physics shape is its collision polygon, the dirt and grass, whose cells use the
  Grid collider type, get the cell's square, and the mirrored slopes at the right are `:h`
  cells. The tiles are solid: a ray down onto the first slope stops halfway up it.
- URP's 2D lights become Godot's. The dusky `Global Light 2D` is the scene's `CanvasModulate`,
  its color times its intensity, and the lantern's spot light a `PointLight2D` that swings with
  it, through `spot_17_50.png`, a texture of its falloff (full inside a sixth of its radius, the
  inner radius, then fading linearly) scaled to its 12-unit radius. It lights all three sorting
  layers, so its z range covers their sprites. The lily pad's `ShadowCaster2D` is a
  `LightOccluder2D` with its shape: beside the lantern, `light.sample` finds the lantern's light
  on top of the dusk, and under the lily pad only the dusk.
- The frog's `Animator` becomes an `AnimationTree`, its controller an animation library,
  `Frog.anim.json`, with an animation per state, and its state machine a tree,
  `Frog.animtree.json`. `Idle`, which plays first, swaps the frog's sprite to blink and calls
  `Ribbit` (an event track), and `Hop` stretches its feet. The sprite tracks target `#sprite`,
  the entity drawing the frog's sprite, and the feet track `Feet`, both bound in the tree's
  `targets`. A second in, the frog has ribbited, and at 1.75 seconds it blinks. The controller's
  `Hop` trigger and `Grounded` bool are the tree's trigger and variable: Idle goes to Hop on
  `Hop`, and Hop back to Idle at its end when `Grounded`. Setting `Hop` with
  `animation.set_parameters`, as gameplay code does where Unity's called `SetTrigger("Hop")`,
  makes the frog hop. The storyboard task for the controller lists the parameters to set.
- An `AudioSource` becomes an `AudioStreamPlayer`, or an `AudioStreamPlayer2D` when it is
  spatial. The music loops, so it plays a `.audio.json` file that loops the sound.
- The audio mixer becomes a bus layout, `Pond.bus_layout.json`, which `project.json` sets: its
  Music and SFX groups are buses, the music and the frog's croak play on them, and SFX's
  high-pass filter comes across. Music's Duck Volume, which SFX sends to, becomes a compressor
  with SFX as its sidechain, so the music ducks when the frog croaks. Its exposed `MusicVolume`
  parameter is reported, since gameplay code sets buses itself.
- The `HUD` canvas becomes controls drawn over the game, as Godot's Control nodes: `hud` covers
  the window (its `CanvasScaler`'s reference resolution is the window's, so it isn't scaled) and
  draws above the scene by its sort order. Each rect's anchors, position, size and pivot become
  a `Control`'s anchors and offsets, so the coin count sits at the top left, the pause button at
  the top right and the hint along the bottom, as `ui.layout` shows. The coin count's
  `HorizontalLayoutGroup` is a `BoxContainer` holding the coin's `Image` (a `NinePatchRect`
  showing the sprite's region of the sheet) and its TextMesh Pro text (a `Label`). The pause
  button's color tint becomes its style boxes, Unity's rounded built-in sprite a flat style box
  with rounded corners, and its On Click call to `Hud.TogglePause` a line in the `Hud` script's
  task. The hint's `Shadow` is the label's shadow. The event system isn't needed: the gui system
  sends input to controls itself.
- The player, time, physics and input settings become `project.json`: the name, version, window,
  tick rate, gravity, clear color, texture filter, and both the Input Manager's axes (`Jump`,
  `Horizontal/negative`) and the Input System's actions (`Player/Move/up`).
- SlashEngine games are plain C++, so each script becomes a C++ skeleton in `src/imported/` and a
  storyboard task with what the script declares. The skeleton's component holds the script's
  fields, and the objects that had the script hold it with the values they gave it: the frog's
  `jumpSpeed` and its `feet`, as an entity ID. Its methods are stubs keeping the C#, called when
  Unity would call them ([import-unity-scripts](../import-unity-scripts/) ports one).
  `import/unity.json` holds those values too, with tags, layers, sorting layers and the entity ID
  of every object. The original scripts stay where they are, linked from the tasks.
- The assets stay where they are, since `unity/` is inside the project; importing a folder
  outside the project copies them in. Importing again updates the files and keeps each task's
  status.
