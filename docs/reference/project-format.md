# Project format

A game is a folder with a `project.json` at its root, plus its scenes. The file works like
Godot's `project.godot`: it names the game and says which scene starts it. `slash-cli init`
writes one, `slash-cli schema project` prints its JSON Schema, and `slash-cli project validate`
checks it.

```json
{
  "format": 1,
  "main_scene": "scenes/main.scene.json",
  "name": "Test Game",
  "tick_hz": 50,
  "version": "1.2.3",
  "window": {
    "height": 240,
    "width": 320
  }
}
```

| Key | Required | Default | Meaning |
| --- | --- | --- | --- |
| `format` | Yes | | Format version. Must be `1` |
| `name` | Yes | | The game's name, 1 to 100 characters, shown in its title bar. Exported files use it with only letters and digits kept (`Test Game` becomes `TestGame`) |
| `main_scene` | Yes | | The scene loaded at startup, relative to the project folder |
| `id` | No | `com.example.<name>` | Reverse-DNS identifier for bundles and app stores, e.g. `com.example.pong` |
| `version` | No | `0.1.0` | Version as major.minor.patch |
| `tick_hz` | No | `60` | Fixed simulation ticks per second, above 0 and at most 1000 |
| `window.width` | No | `1280` | Initial window width in pixels, 1 to 16384 |
| `window.height` | No | `720` | Initial window height in pixels, 1 to 16384 |

Unknown keys are errors, with a "did you mean" suggestion for a misspelled one.

## Layout

```text
my-game/
  project.json
  scenes/
    main.scene.json
  build/           # exports land in build/export/<platform>/ (gitignored)
  .slash/          # caches, export logs, a running server's token (gitignored)
```

Only `project.json` and `.scene.json` files ship in an export. See
[cli.md](cli.md#export) for `slash-cli export` and [scene-format.md](scene-format.md) for scenes.
