# Import a Godot game

`project.import` converts a Godot 4 project into this one. [godot/](godot/) holds "Coin Run", a
small platformer as Godot saves it: [project.godot](godot/project.godot), a level
([main.tscn](godot/main.tscn)) that instances the player ([player.tscn](godot/player.tscn)), a
`SpriteFrames` resource, an `AnimationPlayer` that bobs the coin, a tile set
([tiles.tres](godot/tiles.tres)), a theme, a bus layout and three GDScript files. [main.cpp](main.cpp) draws the textures, then
makes the calls an agent would make over MCP or JSON-RPC; [expected.txt](expected.txt) holds the
replies and the files the import wrote. From a shell, `slash-cli import godot` does the same.

```text
project.import {"source":"godot"}
{"converted": {"animation_libraries": 1, "assets": 6, "bus_layouts": 1, "input_actions": 3,
               "scenes": 2, "sprite_frames": 2, "themes": 1, "tile_sets": 1},
 "files": 11, "main_scene": "godot/main.scene.json", "report": "import/godot.json",
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
- The coin's `AnimationPlayer` becomes an entity whose `AnimationPlayer` plays a `.anim.json`
  file. Each track names the node it animates by its path from the player's root node, as
  Godot's do (`AnimatedSprite2D:position:y` becomes target `AnimatedSprite2D`, path
  `Transform2D/position/y`), and the player's `targets` maps each path to its entity. The method
  track that calls `glint` becomes an event track, so the coin emits a `glint` event each loop.
- The tile set becomes a `.tileset.json` file, its terrains and the terrain each tile's sides
  must touch included, and the `TileMapLayer`'s cells become the layer's legend and rows.
- The theme becomes a `.theme.json` file and the bus layout a `.bus_layout.json` file, each
  effect in SlashEngine's form.
- `project.godot`'s name, main scene, window size, physics, clear color, texture filter, theme,
  buses and input map become `project.json`.
- SlashEngine games are plain C++, so scripts don't convert. Each becomes a storyboard task with
  what the script declares; `import/godot.json` holds the values each node gave its exported
  variables, its signal connections, groups, autoloads and the entity ID of every node path.
  The original scripts stay where they are, linked from the tasks.
- The assets stay where they are, since `godot/` is inside the project; importing a folder
  outside the project copies them in. Importing again updates the files and keeps each task's
  status.
