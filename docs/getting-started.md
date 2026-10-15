# Getting started

This walks through building SlashEngine, running a scene, changing it with commands, and
connecting an AI agent to it. It takes a few minutes once the build is done. Every result shown
here is real output from the engine.

SlashEngine is early: it runs worlds of entities and components, saves them as JSON scenes and
draws 2D sprites with animations, materials, tile maps and lights, but has no physics, audio or
3D yet (see the [roadmap](roadmap.md)). What works today is the part agents use most: scenes, the command API,
the servers that expose it, and exporting a game for macOS, Windows, Linux, the web, Android and
iOS.

## 1. Build

You need CMake 3.25 or newer, Ninja and a C++20 compiler (Apple Clang, Clang 16+, GCC 12+ or MSVC
19.3x). Everything else is downloaded by CMake at pinned versions. On Linux, SDL3 also needs the
X11 and Wayland development packages listed under `LINUX_SDL_DEPS` in
[.github/workflows/ci.yml](../.github/workflows/ci.yml).

```bash
cmake --preset dev               # configure; the first run fetches the dependencies
cmake --build --preset dev       # build the engine, slash-cli, slash-mcp and the tests
ctest --preset dev               # run the tests
```

The tools land in `build/dev/tools/`. The rest of this guide uses two shell variables for them:

```bash
cli=./build/dev/tools/slash-cli/slash-cli
mcp=./build/dev/tools/slash-mcp/slash-mcp
```

## 2. Run the engine

```bash
$cli version
```

```json
{"name":"SlashEngine","version":"0.1.0"}
```

Run a scene for 60 fixed ticks with no window or GPU. Logs go to stderr as JSON lines; the result
goes to stdout as one JSON object:

```bash
$cli run --headless --ticks 60 --scene tests/data/basic.scene.json
```

```json
{"entities":3,"headless":true,"quit_requested":false,"renderer":"null","ticks":60}
```

`$cli run` with no flags opens a window and ticks until you close it. On a machine with no
display, set `SDL_VIDEO_DRIVER=dummy` to try the windowed path anyway.

## 3. Look at a scene

A scene is a `.scene.json` file of entities keyed by stable IDs, each holding components. Fields
equal to their defaults are left out. This is `tests/data/basic.scene.json`:

```json
{
  "entities": {
    "camera": {
      "components": {
        "Transform3D": {
          "position": [0.0, 2.0, -5.0]
        }
      }
    },
    "player": {
      "components": {
        "Transform2D": {
          "position": [64.0, 32.0],
          "rotation": 0.5
        }
      }
    },
    "spawn_point": {}
  },
  "format": 1
}
```

Check a scene against the schema the engine generates from its own components:

```bash
$cli scene validate tests/data/basic.scene.json
```

```json
{"entities":3,"valid":true}
```

A broken scene lists every error with the [JSON Pointer](https://www.rfc-editor.org/rfc/rfc6901)
of the bad value, and suggests the right name for a misspelled one (exit code 1):

```bash
$cli scene validate tests/data/invalid.scene.json
```

```json
{"errors":[{"message":"expected number, got string","path":"/entities/player/components/Transform2D/position/1"},{"message":"unknown property \"rotaton\"; did you mean \"rotation\"?","path":"/entities/player/components/Transform2D/rotaton"},{"message":"unknown property \"Transfrom3D\"; did you mean \"Transform3D\"?","path":"/entities/player/components/Transfrom3D"},{"message":"expected 1, got 2","path":"/format"}],"valid":false}
```

`$cli scene format <file>` rewrites a valid scene in canonical form (sorted keys, defaults left
out), so diffs stay small. Run it before committing a scene you edited by hand.

## 4. Change a scene with commands

Every change to a world goes through a command. `slash-cli call` runs one command against a scene
and prints the result. Start by asking the engine what it has:

```bash
$cli call engine.describe
```

```json
{"capabilities":["read","edit","run","filesystem","build"],"commands":["animation.eval","asset.list","batch","component.remove","component.set","engine.describe","entity.create","entity.destroy","entity.get","events.read","events.subscribe","events.unsubscribe","export.run","history.get","light.sample","log.tail","material.eval","prefab.save","redo","run.until","scene.get","scene.new","scene.open","scene.save","schema.get","step","storyboard.brief","storyboard.check","storyboard.edit","storyboard.query","tilemap.get","tilemap.paint","tilemap.set","undo","world.diff","world.query","world.restore","world.snapshot"],"components":[{"defaults":{"position":[0.0,0.0],"rotation":0.0,"scale":[1.0,1.0]},"doc":"Position, rotation and scale in 2D. Units are pixels and y points down.","name":"Transform2D"},{"defaults":{"position":[0.0,0.0,0.0],"rotation":[0.0,0.0,0.0,1.0],"scale":[1.0,1.0,1.0]},"doc":"Position, rotation and scale in 3D. Units are meters and y points up.","name":"Transform3D"},{"defaults":{"ids":[]},"doc":"The storyboard entries this entity stands for.","name":"StoryRef"},{"defaults":{"centered":true,"flip_h":false,"flip_v":false,"frame":0,"hframes":1,"light_mask":1,"material":"","material_params":{},"modulate":[1.0,1.0,1.0,1.0],"normal_texture":"","offset":[0.0,0.0],"region":[0.0,0.0,0.0,0.0],"specular_color":[1.0,1.0,1.0,1.0],"specular_shininess":1.0,"specular_texture":"","texture":"","texture_filter":"inherit","vframes":1,"visible":true,"z_index":0},"doc":"A texture drawn at the entity's Transform2D. Sprites draw in order of z_index, then in the order their entities were made.","name":"Sprite2D"},{"defaults":{"animation":"default","frame":0,"frame_progress":0.0,"playing":true,"speed_scale":1.0,"sprite_frames":""},"doc":"Plays an animation from a .frames.json file on the entity's Sprite2D, which it adds if missing. Emits animation_finished when an animation that doesn't loop ends.","name":"AnimatedSprite2D"},{"defaults":{"animation":"","backwards":false,"library":"","playing":true,"speed_scale":1.0,"time":0.0},"doc":"Plays a keyframe animation from a .anim.json file on the entity's components. Emits animation_finished when an animation that doesn't loop ends, and the events of its event tracks.","name":"AnimationPlayer"},{"defaults":{"legend":{},"light_mask":1,"modulate":[1.0,1.0,1.0,1.0],"origin":[0,0],"rows":[],"texture_filter":"inherit","tile_set":"","visible":true,"z_index":0},"doc":"A grid of tiles from a .tileset.json file, drawn at the entity's Transform2D if it has one. Cells are text: rows of legend keys, \".\" for empty. Edit them with tilemap.paint and tilemap.set.","name":"TileMapLayer"},{"defaults":{"blend_mode":"add","color":[1.0,1.0,1.0,1.0],"enabled":true,"energy":1.0,"height":0.0,"offset":[0.0,0.0],"range_item_cull_mask":1,"range_z_max":1024,"range_z_min":-1024,"shadow_color":[0.0,0.0,0.0,0.0],"shadow_enabled":false,"shadow_filter":"none","shadow_filter_smooth":0.0,"shadow_item_cull_mask":1,"texture":"","texture_scale":1.0},"doc":"A light shining from the entity's Transform2D through a texture. It lights sprites and tile map layers whose light_mask shares a bit with range_item_cull_mask; with shadow_enabled, LightOccluder2D polygons cast shadows from it.","name":"PointLight2D"},{"defaults":{"blend_mode":"add","color":[1.0,1.0,1.0,1.0],"enabled":true,"energy":1.0,"height":0.0,"max_distance":10000.0,"range_item_cull_mask":1,"range_z_max":1024,"range_z_min":-1024,"shadow_color":[0.0,0.0,0.0,0.0],"shadow_enabled":false,"shadow_filter":"none","shadow_filter_smooth":0.0,"shadow_item_cull_mask":1},"doc":"Light from one direction over everything, such as the sun. It shines down the screen, turned by the entity's Transform2D rotation. Masks, ranges and shadows work as PointLight2D's.","name":"DirectionalLight2D"},{"defaults":{"closed":true,"cull_mode":"disabled","occluder_light_mask":1,"polygon":[]},"doc":"A polygon that casts shadows from lights with shadow_enabled, placed by the entity's Transform2D.","name":"LightOccluder2D"},{"defaults":{"color":[1.0,1.0,1.0,1.0]},"doc":"Tints everything lit: the light a scene has where no light reaches. One per scene.","name":"CanvasModulate"}],"engine":{"name":"SlashEngine","version":"0.1.0"},"scene":{"entities":0,"path":"","tick":0},"systems":["animation_player","animated_sprite_2d"]}
```

Find entities. Queries return only IDs unless you ask for components, which keeps answers small:

```bash
$cli call world.query '{"with": ["Transform2D"]}' --scene tests/data/basic.scene.json
```

```json
{"count":1,"entities":["player"],"truncated":false}
```

Make several changes in one call with `batch`, and save the result with `--save`. Copy the scene
first so the test data stays untouched:

```bash
mkdir -p build/play && cp tests/data/basic.scene.json build/play/level.scene.json
$cli call batch '{"commands": [
  {"command": "entity.create", "params": {"id": "enemy_01", "components": {"Transform2D": {"position": [200, 40]}}}},
  {"command": "component.set", "params": {"id": "player", "components": {"Transform2D": {"rotation": 1.0}}}}
]}' --project build/play --scene level.scene.json --save
```

```json
{"results":[{"id":"enemy_01"},{"changed":{"Transform2D":{"rotation":1.0}},"id":"player"}]}
```

A batch is all or nothing: if any command in it fails, none of its changes are kept and the error
names the command that failed. Paths are relative to `--project` (default: the current folder) and
cannot leave it.

## 5. Connect an agent

`slash-cli init` sets a game folder up for Claude Code, Codex and Cursor:

```bash
$cli init mygame
```

```json
{"created":["project.json","AGENTS.md","CLAUDE.md",".mcp.json",".cursor/mcp.json",".codex/config.toml",".claude/skills/slashengine/SKILL.md",".github/workflows/export.yml","scenes/main.scene.json",".gitignore"],"skipped":[]}
```

`mygame` is now a game project: `project.json` names the game and its main scene. Each config launches `slash-mcp`, the engine's MCP server, with the project's main scene open.
Put `slash-mcp` on your `PATH` (or pass `--mcp-command` with its full path to `init`), then start
the agent in `mygame/`; it finds the server on its next start. To add the server to Claude Code by
hand instead:

```bash
claude mcp add slashengine -- slash-mcp --project .
```

The agent now sees every command as a tool. Tool names use `_` where commands use `.`
(`entity_create` for `entity.create`), because some clients reject dots.

## 6. Change a game while it runs

Start a game with its HTTP server on, and point the agent at it with `slash-mcp --live`:

```bash
$cli run --serve --scene scenes/main.scene.json     # in the game folder
claude mcp add slashengine-live -- slash-mcp --live --project .
```

The server listens on `127.0.0.1` only and needs a bearer token. Its URL and token are in
`.slash/server.json` while it runs. Commands from the agent wait in a queue and run between ticks,
so they never race the game loop. Tools that don't speak MCP can use the REST mirror instead:

```bash
URL=$(jq -r .url .slash/server.json)
TOKEN=$(jq -r .token .slash/server.json)
curl -X POST -H "Authorization: Bearer $TOKEN" -d '{"with": ["Transform3D"]}' \
    "$URL/v1/commands/world.query"
```

```json
{"count":1,"entities":["camera"],"truncated":false}
```

## 7. Export the game

Build the game for players. On Linux, from the folder that holds `mygame`:

```bash
$cli export linux --project mygame --test
```

```json
{"bytes":5335402,"executable":"mygame/build/export/linux/mygame","log":"mygame/.slash/export/linux/export.log","output":"mygame/build/export/linux","platform":"linux","preset":"linux","seconds":64.2,"test":{"entities":0,"passed":true,"ticks":60}}
```

`--test` runs the exported game headless and reports whether it worked. Use `macos` or `windows`
on those machines, or `web` anywhere Emscripten is installed. To export every platform at once,
push the game to GitHub: `init` wrote `.github/workflows/export.yml`, which builds and tests each
one on its own runner. Presets in `export_presets.json` name output folders and signing settings;
see [reference/cli.md](reference/cli.md#export).

## Next

- [concepts.md](concepts.md) explains the world, commands, sessions and servers.
- [reference/commands.md](reference/commands.md) lists every command with examples.
- [reference/protocols.md](reference/protocols.md) covers REST, JSON-RPC and MCP in detail.
- [roadmap.md](roadmap.md) shows what comes next.
