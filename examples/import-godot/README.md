# Import a Godot game

`project.import` converts a Godot 4 project into this one. [godot/](godot/) holds "Coin Run", a
small platformer as Godot saves it: [project.godot](godot/project.godot), a level
([main.tscn](godot/main.tscn)) that instances the player ([player.tscn](godot/player.tscn)), a
`SpriteFrames` resource and three GDScript files. [main.cpp](main.cpp) draws the textures, then
makes the calls an agent would make over MCP or JSON-RPC; [expected.txt](expected.txt) holds the
replies and the files the import wrote. From a shell, `slash-cli import godot` does the same.

```text
project.import {"source":"godot"}
{"converted": {"assets": 5, "input_actions": 3, "scenes": 2, "sprite_frames": 2},
 "files": 7, "main_scene": "godot/main.scene.json", "report": "import/godot.json",
 "scripts": [{"entities": 2, "script": "godot/player.gd", "task": "task/port-player"}, ...],
 "unsupported": [{"count": 1, "what": "node type Timer", "where": "main.tscn: Timer",
                  "why": "timers are gameplay code: count the ticks in a C++ system"}, ...]}
```

- Each `.tscn` file becomes a `.scene.json` file beside it, a node becoming an entity named after
  it (`AnimatedSprite2D` is `animated_sprite2d`). Node2D nodes nest through `Transform2D`
  `children`, controls through `Control` `children`, and each node's properties go to the
  components of the same names.
- The level instances the player scene, and stays an instance: the level's `player` entity
  extends `player.scene.json` with only the level's overrides (`position`), and the player's
  nodes come with it as `player/animated_sprite2d`, `player/camera2d` and the rest. Editing the
  player scene changes the player in every level.
- `SpriteFrames` become `.frames.json` files: whole textures stay a list of files, and
  `AtlasTexture` regions on one grid become frame numbers of a sheet (the coin).
- `project.godot`'s name, main scene, window size, physics, clear color, texture filter and input
  map become `project.json`.
- SlashEngine games are plain C++, so scripts don't convert. Each becomes a storyboard task with
  what the script declares; `import/godot.json` holds the values each node gave its exported
  variables, its signal connections, groups, autoloads and the entity ID of every node path.
  The original scripts stay where they are, linked from the tasks.
- The assets stay where they are, since `godot/` is inside the project; importing a folder
  outside the project copies them in. Importing again updates the files and keeps each task's
  status.
