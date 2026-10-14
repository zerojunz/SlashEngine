# Getting started

This walks through building SlashEngine, running a scene, changing it with commands, and
connecting an AI agent to it. It takes a few minutes once the build is done. Every result shown
here is real output from the engine.

SlashEngine is early: it runs worlds of entities and components and saves them as JSON scenes, but
it does not draw sprites or meshes yet (see the [roadmap](roadmap.md)). What works today is the
part agents use most: scenes, the command API and the servers that expose it.

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
{"entities":3,"headless":true,"quit_requested":false,"ticks":60}
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
{"capabilities":["read","edit","run","filesystem"],"commands":["batch","component.remove","component.set","engine.describe","entity.create","entity.destroy","entity.get","history.get","log.tail","redo","scene.get","scene.new","scene.open","scene.save","schema.get","step","undo","world.query"],"components":[{"defaults":{"position":[0.0,0.0],"rotation":0.0,"scale":[1.0,1.0]},"doc":"Position, rotation and scale in 2D. Units are pixels and y points down.","name":"Transform2D"},{"defaults":{"position":[0.0,0.0,0.0],"rotation":[0.0,0.0,0.0,1.0],"scale":[1.0,1.0,1.0]},"doc":"Position, rotation and scale in 3D. Units are meters and y points up.","name":"Transform3D"}],"engine":{"name":"SlashEngine","version":"0.1.0"},"scene":{"entities":0,"path":"","tick":0},"systems":[]}
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
{"created":["AGENTS.md","CLAUDE.md",".mcp.json",".cursor/mcp.json",".codex/config.toml",".claude/skills/slashengine/SKILL.md","scenes/main.scene.json",".gitignore"],"skipped":[]}
```

Each config launches `slash-mcp`, the engine's MCP server, with the project's main scene open.
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

## Next

- [concepts.md](concepts.md) explains the world, commands, sessions and servers.
- [reference/commands.md](reference/commands.md) lists every command with examples.
- [reference/protocols.md](reference/protocols.md) covers REST, JSON-RPC and MCP in detail.
- [roadmap.md](roadmap.md) shows what comes next.
