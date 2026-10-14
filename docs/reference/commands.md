# Command reference

Every command the engine registers, with its params, the capabilities it needs and a real example.
The same commands are available everywhere:

| Front end | How to call `entity.create` |
| --- | --- |
| MCP | Tool `entity_create` with the params as arguments |
| JSON-RPC | `{"jsonrpc":"2.0","id":1,"method":"entity.create","params":{...}}` |
| REST | `POST /v1/commands/entity.create` with the params as the body |
| CLI | `slash-cli call entity.create '{...}'` |

The engine is the source of truth for this page: `engine.describe` with `"full": true`, or
`schema.get` with `"kind": "command"`, returns each command's live params schema. Examples below
run against `tests/data/basic.scene.json`, which holds `camera` (a `Transform3D`), `player` (a
`Transform2D`) and `spawn_point` (no components).

Rules that apply to every command (see [concepts.md](../concepts.md#sessions)):

- Params are a JSON object, checked against the command's schema before it runs. Unknown params
  are errors. Left out, params take the defaults listed here.
- A failed command changes nothing.
- Paths are relative to the project folder and cannot leave it.
- Component fields are written as in scene files: vectors as arrays (`[x, y]`), quaternions as
  `[x, y, z, w]`.

## Contents

| Group | Commands |
| --- | --- |
| Describe | [`engine.describe`](#enginedescribe), [`schema.get`](#schemaget) |
| Scene | [`scene.get`](#sceneget), [`scene.new`](#scenenew), [`scene.open`](#sceneopen), [`scene.save`](#scenesave) |
| Entities | [`entity.create`](#entitycreate), [`entity.destroy`](#entitydestroy), [`entity.get`](#entityget) |
| Components | [`component.set`](#componentset), [`component.remove`](#componentremove) |
| Query | [`world.query`](#worldquery) |
| Run | [`step`](#step) |
| History | [`undo`](#undo), [`redo`](#redo), [`history.get`](#historyget) |
| Several at once | [`batch`](#batch) |
| Observe | [`log.tail`](#logtail) |
| Export | [`export.run`](#exportrun) |

Planned commands from the [roadmap](../roadmap.md), not yet available: `input.inject`, `frame.capture`, `replay.run`, `play`, `pause`, `asset.list`,
`events.subscribe` and `perf.stats`.

## Describe

### engine.describe

Lists the components with their default field values, the command names, the granted
capabilities and the open scene. Call this first.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `full` | boolean | `false` | Also return each command's description, capabilities and params schema, and each component's field schema with docs and ranges |

Needs: `read`.

```json
{}
```

```json
{"capabilities":["read","edit","run","filesystem","build"],"commands":["batch","component.remove","component.set","engine.describe","entity.create","entity.destroy","entity.get","export.run","history.get","log.tail","redo","scene.get","scene.new","scene.open","scene.save","schema.get","step","undo","world.query"],"components":[{"defaults":{"position":[0.0,0.0],"rotation":0.0,"scale":[1.0,1.0]},"doc":"Position, rotation and scale in 2D. Units are pixels and y points down.","name":"Transform2D"},{"defaults":{"position":[0.0,0.0,0.0],"rotation":[0.0,0.0,0.0,1.0],"scale":[1.0,1.0,1.0]},"doc":"Position, rotation and scale in 3D. Units are meters and y points up.","name":"Transform3D"}],"engine":{"name":"SlashEngine","version":"0.1.0"},"scene":{"entities":0,"path":"","tick":0},"systems":[]}
```

`scene` describes the open scene: how many entities it has, its path (empty for an unsaved one)
and the current tick.

### schema.get

Returns a JSON Schema: for scene files, one component or one command.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `kind` | `"scene"`, `"component"` or `"command"` | required | Which schema |
| `name` | string | | Component or command name, for `component` and `command` |

Needs: `read`.

```json
{"kind": "command", "name": "step"}
```

```json
{"additionalProperties":false,"properties":{"tick_seconds":{"default":0.016666666666666666,"description":"Seconds per tick.","exclusiveMinimum":0,"maximum":1,"type":"number"},"ticks":{"default":1,"description":"Ticks to run.","maximum":100000,"minimum":1,"type":"integer"}},"type":"object"}
```

## Scene

### scene.get

Returns the whole open scene in scene-file form, with default fields left out. Prefer
`world.query` or `entity.get` for parts of it.

No params. Needs: `read`.

```json
{"path":"basic.scene.json","scene":{"entities":{"camera":{"components":{"Transform3D":{"position":[0.0,2.0,-5.0]}}},"player":{"components":{"Transform2D":{"position":[64.0,32.0],"rotation":0.5}}},"spawn_point":{}},"format":1}}
```

### scene.new

Replaces the open scene with an empty one and clears undo history.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `path` | string | | Where `scene.save` will write it, e.g. `"scenes/level2.scene.json"`. Nothing is written yet |

Needs: `edit`.

```json
{"path": "scenes/two.scene.json"}
```

```json
{"path":"scenes/two.scene.json"}
```

### scene.open

Loads a `.scene.json` file in place of the open scene and clears undo history. An invalid file
changes nothing and every error is listed.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `path` | string | required | Scene file, e.g. `"scenes/main.scene.json"` |

Needs: `edit`, `filesystem`.

```json
{"path": "basic.scene.json"}
```

```json
{"entities":3,"path":"basic.scene.json"}
```

### scene.save

Writes the open scene to a `.scene.json` file in canonical form, creating folders as needed.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `path` | string | the scene's current file | File to write |

Needs: `read`, `filesystem`.

```json
{"path": "out/level.scene.json"}
```

```json
{"entities":3,"path":"out/level.scene.json"}
```

A path outside the project folder fails with `-32003`:

```json
{"code":-32003,"data":{"path":"/path"},"message":"/path: path \"../x.scene.json\" leaves the project folder"}
```

## Entities

### entity.create

Creates an entity, optionally with components. Returns its ID.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `id` | string | generated | Stable ID: 1 to 64 letters, digits, `_` or `-` |
| `components` | object | | Components keyed by type name. Fields left out keep their defaults |

Needs: `edit`.

```json
{"id": "enemy_01", "components": {"Transform2D": {"position": [200, 40]}}}
```

```json
{"id":"enemy_01"}
```

Left out, the ID is generated (`entity_1`, `entity_2` and so on). An ID already in use is an
error.

### entity.destroy

Destroys an entity and its components.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `id` | string | required | Entity ID |

Needs: `edit`.

```json
{"id": "camera"}
```

```json
{"destroyed":"camera"}
```

### entity.get

Returns an entity's components.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `id` | string | required | Entity ID |
| `components` | array of strings | all | Only these components |
| `omit_defaults` | boolean | `false` | Leave out fields equal to their defaults |

Needs: `read`.

```json
{"id": "player", "omit_defaults": true}
```

```json
{"components":{"Transform2D":{"position":[64.0,32.0],"rotation":0.5}},"id":"player"}
```

An unknown ID fails with `-32001` (not found):

```json
{"code":-32001,"data":{"path":"/id"},"message":"/id: no entity has the id \"nobody\""}
```

## Components

### component.set

Sets component fields on an entity, adding components it lacks. Returns only the fields whose
values changed.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `id` | string | required | Entity ID |
| `components` | object | required | Fields to set, keyed by component type. Fields left out are unchanged; missing components are added |

Needs: `edit`.

```json
{"id": "player", "components": {"Transform2D": {"rotation": 1.0, "position": [64, 32]}}}
```

```json
{"changed":{"Transform2D":{"rotation":1.0}},"id":"player"}
```

`position` was already `[64, 32]`, so only `rotation` is reported. A misspelled component name
gets a suggestion:

```json
{"code":-32001,"data":{"path":"/components/Transfrom2D"},"message":"/components/Transfrom2D: unknown component \"Transfrom2D\"; did you mean \"Transform2D\"?"}
```

### component.remove

Removes components from an entity. Returns the ones it had.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `id` | string | required | Entity ID |
| `components` | array of strings | required | Component types to remove (at least one) |

Needs: `edit`.

```json
{"id": "player", "components": ["Transform2D"]}
```

```json
{"id":"player","removed":["Transform2D"]}
```

## Query

### world.query

Finds entities by the components they have and their field values. Returns IDs unless `include`
asks for components, plus the total count.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `with` | array of strings | | Only entities with all of these components |
| `without` | array of strings | | Only entities with none of these components |
| `where` | array of conditions | | Field conditions; an entity must meet all of them |
| `include` | `true` or array of strings | | Components to return with each entity: `true` for all. Left out, only IDs are returned, which is cheapest |
| `omit_defaults` | boolean | `false` | Leave out fields equal to their defaults |
| `limit` | integer, 1 to 10000 | `100` | Most entities to return; `count` still covers all |

Each `where` condition is an object:

| Key | Type | Meaning |
| --- | --- | --- |
| `component` | string | Component type, e.g. `"Transform2D"` (required) |
| `field` | string | Field name. Add `.x`, `.y`, `.z`, `.w` or `.<index>` for one element of a vector, e.g. `"position.y"` (required) |
| `op` | `==`, `!=`, `<`, `<=`, `>`, `>=` | Comparison; `==` if left out. Ordering operators compare numbers or strings |
| `value` | any | Value to compare against (required) |

Needs: `read`.

```json
{"where": [{"component": "Transform2D", "field": "position.x", "op": ">", "value": 10}]}
```

```json
{"count":1,"entities":["player"],"truncated":false}
```

With components, and with defaults dropped:

```json
{"with": ["Transform2D"], "include": ["Transform2D"], "omit_defaults": true}
```

```json
{"count":1,"entities":[{"components":{"Transform2D":{"position":[64.0,32.0],"rotation":0.5}},"id":"player"}],"truncated":false}
```

`truncated` is `true` when `limit` cut the list short:

```json
{"limit": 1}
```

```json
{"count":3,"entities":["camera"],"truncated":true}
```

## Run

### step

Advances the simulation by running every system once per tick. Undo reverts the entities to how
they were before.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `ticks` | integer, 1 to 100000 | `1` | Ticks to run |
| `tick_seconds` | number, above 0 and at most 1 | `1/60` | Seconds per tick |

Needs: `run`.

```json
{"ticks": 120}
```

```json
{"tick":120,"ticks":120}
```

`tick` is the world's tick counter after the step. Undo restores entities but not the counter.

## History

### undo

Reverts the last change to the scene. No params. Needs: `edit`.

```json
{"can_redo":1,"can_undo":2,"undone":"entity.destroy"}
```

With nothing to undo, it fails with `-32002` and the message `nothing to undo`.

### redo

Reapplies the last change that `undo` reverted. No params. Needs: `edit`.

```json
{"can_redo":0,"can_undo":3,"redone":"entity.destroy"}
```

### history.get

Lists the changes `undo` and `redo` would revert or reapply, by command name. The last `undo`
entry and the first `redo` entry are next. No params. Needs: `read`.

```json
{"redo":[],"undo":["batch","component.remove","entity.destroy"]}
```

## Several at once

### batch

Runs several commands in one call. If any fails, none of their changes to the scene are kept and
the error names the failing command. One undo reverts the whole batch.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `commands` | array of 1 to 1000 `{"command", "params"}` objects | required | Commands to run in order |

Needs no capability itself; each command inside it is checked as usual. A batch cannot contain
another `batch`.

```json
{"commands": [
  {"command": "entity.create", "params": {"id": "enemy_01", "components": {"Transform2D": {"position": [200, 40]}}}},
  {"command": "component.set", "params": {"id": "player", "components": {"Transform2D": {"rotation": 1.0}}}}
]}
```

```json
{"results":[{"id":"enemy_01"},{"changed":{"Transform2D":{"rotation":1.0}},"id":"player"}]}
```

When one command fails, the error gives its index, its name and the JSON Pointer of the bad value
inside the batch:

```json
{"code":-32002,"data":{"command":"entity.create","index":1,"path":"/commands/1/params/id"},"message":"/commands/1 (entity.create) failed, so nothing was changed. /id: entity id \"a\" is already in use"}
```

## Observe

### log.tail

Returns the most recent engine log lines as JSON objects, oldest first.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `lines` | integer, 1 to 1000 | `50` | How many lines |
| `level` | `"debug"`, `"info"`, `"warn"` or `"error"` | all | Only lines at this level or above |

Needs: `read`.

```json
{"lines": 2, "level": "warn"}
```

The result is `{"lines": [...]}`, each line an object such as
`{"level":"info","msg":"server listening","url":"http://127.0.0.1:34467"}`. A session captures
only the lines logged while it is open, so a fresh one returns `{"lines":[]}`.

## Export

### export.run

Builds the project with an export preset, as [`slash-cli export`](cli.md#export) does, and
returns the same result. Registered by `slash-cli` and `slash-mcp`; a game served with
`slash-cli run --serve` leaves it out, because an export would stall its loop.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `preset` | string | required | A preset from `export_presets.json`, or a platform (`macos`, `windows`, `linux`, `web`, `android`) for its defaults |
| `debug` | boolean | `false` | Debug build instead of Release |
| `test` | boolean | `false` | Run the exported game headless afterwards, where this machine can |

Needs: `filesystem`, `build`. It changes no scene, so there is nothing to undo.

```json
{"preset": "linux", "test": true}
```

```json
{"bytes":5335403,"executable":"build/export/linux/mygame","log":".slash/export/linux/export.log","output":"build/export/linux","platform":"linux","preset":"linux","seconds":66.6,"test":{"entities":0,"passed":true,"ticks":60}}
```

A bad preset name fails with `-32002` before anything is built; any other failure, including a
failed headless test, fails with `-32004`. Either way `data` carries the export's error details:
the failed `step`, the validation `errors` or the end of the build log in `tail`, and the `log`
path.

```json
{"code":-32002,"data":{"step":"options"},"message":"no preset or platform called \"macso\"; did you mean \"macos\"?"}
```

## Adding a command

Register it in `engine/command/src/builtin.cpp` (or a registry of your own) with a name, a short
description, its capabilities, an `Effect` and a params schema with
`"additionalProperties": false`, add a test to `tests/test_command.cpp`, and add a section to this
page. See [AGENTS.md](../../AGENTS.md#commands-mcp-and-json-rpc).
