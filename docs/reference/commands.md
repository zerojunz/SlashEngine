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
- Commands that change the world (marked "Takes `dry_run`" below) accept `"dry_run": true`; see
  [Dry runs](#dry-runs).
- Paths are relative to the project folder and cannot leave it.
- Component fields are written as in scene files: vectors as arrays (`[x, y]`), quaternions as
  `[x, y, z, w]`.

## Dry runs

Add `"dry_run": true` to the params of any command that changes the world (`scene.new`,
`scene.open`, `entity.create`, `entity.destroy`, `component.set`, `component.remove`, `step`,
`world.restore`, `storyboard.edit` and `batch`) to see what it would do. The command runs, then the world, the
scene's path, the next generated ID and the tick counter are put back, and no undo step is
recorded. An error is returned as usual. The result holds the command's own result and the
changes, in the same form as [`world.diff`](#worlddiff):

```json
{"id": "player", "dry_run": true, "components": {"Transform2D": {"rotation": 1.0}}}
```

```json
{"changes":{"changed":{"player":{"Transform2D":{"rotation":1.0}}}},"dry_run":true,"result":{"changed":{"Transform2D":{"rotation":1.0}},"id":"player"}}
```

A batch takes `dry_run` itself; the commands inside it cannot. In a dry-run batch, `scene.save`
and `prefab.save` check everything and return their usual result but write no file, and
`storyboard.edit` returns the versions it would write. Commands that change nothing, such
as `entity.get`, reject it as an unknown param.

## Contents

| Group | Commands |
| --- | --- |
| Describe | [`engine.describe`](#enginedescribe), [`schema.get`](#schemaget) |
| Scene | [`scene.get`](#sceneget), [`scene.new`](#scenenew), [`scene.open`](#sceneopen), [`scene.save`](#scenesave) |
| Entities | [`entity.create`](#entitycreate), [`entity.destroy`](#entitydestroy), [`entity.get`](#entityget) |
| Prefabs | [`prefab.save`](#prefabsave) |
| Assets | [`asset.list`](#assetlist) |
| Storyboard | [`storyboard.query`](#storyboardquery), [`storyboard.edit`](#storyboardedit) |
| Components | [`component.set`](#componentset), [`component.remove`](#componentremove) |
| Query | [`world.query`](#worldquery) |
| Snapshots | [`world.snapshot`](#worldsnapshot), [`world.diff`](#worlddiff), [`world.restore`](#worldrestore) |
| Run | [`step`](#step), [`play`](#play), [`pause`](#pause) |
| History | [`undo`](#undo), [`redo`](#redo), [`history.get`](#historyget) |
| Several at once | [`batch`](#batch) |
| Observe | [`events.subscribe`](#eventssubscribe), [`events.read`](#eventsread), [`events.unsubscribe`](#eventsunsubscribe), [`log.tail`](#logtail) |
| Export | [`export.run`](#exportrun) |

Planned commands from the [roadmap](../roadmap.md), not yet available: `input.inject`, `frame.capture`, `replay.run` and `perf.stats`.

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
{"capabilities":["read","edit","run","filesystem","build"],"commands":["batch","component.remove","component.set","engine.describe","entity.create","entity.destroy","entity.get","export.run","history.get","log.tail","redo","scene.get","scene.new","scene.open","scene.save","schema.get","step","undo","world.diff","world.query","world.restore","world.snapshot"],"components":[{"defaults":{"position":[0.0,0.0],"rotation":0.0,"scale":[1.0,1.0]},"doc":"Position, rotation and scale in 2D. Units are pixels and y points down.","name":"Transform2D"},{"defaults":{"position":[0.0,0.0,0.0],"rotation":[0.0,0.0,0.0,1.0],"scale":[1.0,1.0,1.0]},"doc":"Position, rotation and scale in 3D. Units are meters and y points up.","name":"Transform3D"}],"engine":{"name":"SlashEngine","version":"0.1.0"},"scene":{"entities":0,"path":"","tick":0},"systems":[]}
```

`scene` describes the open scene: how many entities it has, its path (empty for an unsaved one)
and the current tick. In a running game, which has [`play`](#play) and [`pause`](#pause), it also
says whether the game is `playing`.

`systems` lists the systems in the order they run. With `"full": true`, each is an object:

```json
{"name":"movement","doc":"Moves things by their velocity.","priority":0}
```

### schema.get

Returns a JSON Schema: for scene, prefab or storyboard files, one component or one command.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `kind` | `"scene"`, `"prefab"`, `"storyboard"`, `"component"` or `"command"` | required | Which schema |
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

Needs: `edit`. Takes `dry_run`.

```json
{"path": "scenes/two.scene.json"}
```

```json
{"path":"scenes/two.scene.json"}
```

### scene.open

Loads a `.scene.json` file in place of the open scene and clears undo history. An invalid file
changes nothing and every error is listed. Prefabs its entities extend are read from the project
folder.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `path` | string | required | Scene file, e.g. `"scenes/main.scene.json"` |

Needs: `edit`, `filesystem`. Takes `dry_run`.

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

Creates an entity, optionally from a prefab and with components. Returns its ID.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `id` | string | generated | Stable ID: 1 to 64 letters, digits, `_` or `-` |
| `extends` | string | | A `.prefab.json` file to start from, e.g. `"prefabs/enemy.prefab.json"` |
| `components` | object | | Components keyed by type name. Fields left out keep their defaults, or the prefab's |

Needs: `edit`. Takes `dry_run`.

```json
{"id": "enemy_01", "components": {"Transform2D": {"position": [200, 40]}}}
```

```json
{"id":"enemy_01"}
```

Left out, the ID is generated (`entity_1`, `entity_2` and so on). An ID already in use is an
error. With `extends`, the entity gets the prefab's components and stays linked to it, so saving
the scene writes only the fields that differ from the prefab ([prefabs](scene-format.md#prefabs)).
Each command reads prefab files afresh, so an edited prefab shows up in the next instance.

### entity.destroy

Destroys an entity and its components.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `id` | string | required | Entity ID |

Needs: `edit`. Takes `dry_run`.

```json
{"id": "camera"}
```

```json
{"destroyed":"camera"}
```

### entity.get

Returns an entity's components, and the prefab it extends, if any, as `extends`.

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

## Prefabs

### prefab.save

Writes an entity to a `.prefab.json` file that entities can extend, creating folders as needed.
An entity that extends a prefab is saved as `extends` plus what differs from it. The entity
itself is not linked to the new file; `entity.create` with `extends` makes instances.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `id` | string | required | Entity ID |
| `path` | string | required | File relative to the project folder, e.g. `"prefabs/enemy.prefab.json"` |

Needs: `read`, `filesystem`.

```json
{"id": "player", "path": "prefabs/player.prefab.json"}
```

```json
{"components":["Transform2D"],"path":"prefabs/player.prefab.json"}
```

Saving an entity to the prefab it extends fails, since the prefab would extend itself.

## Assets

### asset.list

Lists loaded assets and the prefab files entities extend. `version` is 1 when a file is first
loaded and one more each time it reloads; `refs` counts the handles to an asset, or the entities
that extend a prefab. Files reload on their own when they change (see
[hot reload](../concepts.md#assets)), so this command is for checking what is loaded and whether
an edit was picked up.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `type` | string | all | Only assets of this type: `prefab`, `json`, `text` or one a game added |

Needs: `read`.

```json
{}
```

```json
{"assets":[{"path":"data/tuning.json","refs":1,"type":"json","version":2},{"path":"prefabs/enemy.prefab.json","refs":3,"type":"prefab","version":1}]}
```

## Storyboard

The game's facts as typed entries in `storyboard/*.json`; see the
[storyboard format](storyboard-format.md). Examples run against a copy of `tests/data/game`.

### storyboard.query

Finds storyboard entries. Filters combine; with none, every entry is returned. Each result has the
entry's ID, its version (which `storyboard.edit` needs) and its name and summary, or every field
with `"full": true`. `seq` is the journal's line count.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `ids` | array of IDs | all | Only these entries. An unknown ID fails with `-32001` and a "did you mean" hint |
| `kind` | string | all | Only entries of this kind, e.g. `"character"` |
| `text` | string | | Only entries with this text in a field, ignoring case |
| `linked` | string | | Only entries whose `links` name this project path, or whose references name this ID |
| `full` | boolean | `false` | Return every field, not only `name` and `summary` |

Needs: `read`.

```json
{"linked": "scenes/main.scene.json"}
```

```json
{"entries":[{"fields":{"name":"Pilot","summary":"The player: a courier pilot on her first run."},"id":"character/pilot","version":"f6ce5b73"},{"fields":{"name":"Landing pad","summary":"Where every run starts."},"id":"place/landing_pad","version":"4bdb955e"}],"seq":0}
```

### storyboard.edit

Creates, changes or removes entries, all in one step: every edit applies or none does. It writes
the files in canonical form, appends a line to the journal, and records an undo step.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `edits` | array | required | 1 to 1000 edits, at most one per entry |
| `edits[].id` | string | required | The entry's ID |
| `edits[].version` | string or `null` | required | The version `storyboard.query` returned, or `null` to create the entry |
| `edits[].set` | object | | Fields to set; a `null` value removes a field |
| `edits[].remove` | boolean | `false` | Remove the entry; give this or `set` |
| `why` | string | | Why, for the journal |
| `by` | string | the MCP client's name | Who, for the journal |

Needs: `edit`, `filesystem`. Takes `dry_run`.

```json
{"why": "Give the pilot a rival", "by": "claude-code", "edits": [
  {"id": "character/vex", "version": null, "set": {"name": "Vex", "summary": "A rival courier."}},
  {"id": "character/pilot", "version": "f6ce5b73", "set": {"refs": ["character/vex"]}}]}
```

```json
{"changes":{"character/pilot":"45b24094","character/vex":"a77d1847"},"seq":1}
```

`changes` holds each entry's new version, or `null` for a removed one. Fields are checked against
the entry's kind, and every entry a set field names must exist, with the right kind:

```json
{"error":{"code":-32602,"data":{"path":"/edits/0/set/home"},"message":"/edits/0/set/home: unknown storyboard entry \"place/landing_pd\"; did you mean \"place/landing_pad\"?"}}
```

An edit with an old version fails with `-32005`, changes nothing, and returns the entry as it is
now in `data.current`; edit that version instead:

```json
{"error":{"code":-32005,"data":{"current":{"fields":{"links":["scenes/main.scene.json"],"name":"Pilot","refs":["character/vex"],"summary":"The player: a courier pilot on her first run."},"id":"character/pilot","version":"45b24094"},"path":"/edits/0/version"},"message":"/edits/0/version: \"character/pilot\" changed since version f6ce5b73 was read; it is now 45b24094 (see current), so edit that version"}}
```

Removing an entry that another entry still names fails; change both in one call. `undo` and
`redo` apply the reverse edit and the edit again, with the same version checks, so an undo that
would overwrite someone else's later change is refused.

## Components

### component.set

Sets component fields on an entity, adding components it lacks. Returns only the fields whose
values changed.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `id` | string | required | Entity ID |
| `components` | object | required | Fields to set, keyed by component type. Fields left out are unchanged; missing components are added |

Needs: `edit`. Takes `dry_run`.

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

Needs: `edit`. Takes `dry_run`.

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

## Snapshots

### world.snapshot

Saves a copy of every entity under a name, to compare against with `world.diff` or go back to
with `world.restore`. A name in use is replaced. A session keeps up to 100 snapshots, and they
last as long as the session.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `name` | string, 1 to 64 characters | `"default"` | Snapshot name |

Needs: `read`.

```json
{}
```

```json
{"entities":3,"name":"default"}
```

### world.diff

Lists the entities added, removed or changed since a snapshot, by ID. `changed` holds every added
or changed entity with the new value of each field that differs: from the snapshot, or from its
default for a component the entity did not have (`{}` when every field is at its default). A
removed component is `null`. Empty sections are left out, so no changes give `{}`.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `from` | string | `"default"` | Snapshot to compare from |
| `to` | string | the world now | Snapshot to compare to |

Needs: `read`. After a snapshot, moving `player` and destroying `spawn_point`:

```json
{}
```

```json
{"changed":{"player":{"Transform2D":{"position":[80.0,32.0]}}},"removed":["spawn_point"]}
```

An unknown name fails with `-32001` and the closest name:
`/from: no snapshot is named "defualt"; did you mean "default"?`.

### world.restore

Puts every entity back as it was in a snapshot. Undo reverts it; the tick counter keeps counting.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `name` | string | `"default"` | Snapshot to restore |

Needs: `edit`. Takes `dry_run`.

```json
{}
```

```json
{"entities":3,"restored":"default"}
```

## Run

### step

Advances the simulation by running every system once per tick. Undo reverts the entities to how
they were before.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `ticks` | integer, 1 to 100000 | `1` | Ticks to run |
| `tick_seconds` | number, above 0 and at most 1 | `1/60` | Seconds per tick |

Needs: `run`. Takes `dry_run`.

```json
{"ticks": 120}
```

```json
{"tick":120,"ticks":120}
```

`tick` is the world's tick counter after the step. Undo restores entities but not the counter.

### play

Starts the game ticking in real time, running every system each tick. No params. Needs: `run`.
Only a running game (`slash-cli run --serve`, or `slash-mcp --live` connected to one) has `play`
and `pause`; other sessions advance the world with `step`. A running game starts playing.

```json
{"playing":true,"tick":123}
```

### pause

Stops the game ticking, so the world holds still while you inspect or edit it. The game keeps
serving requests, and `step` still advances it. No params. Needs: `run`.

```json
{"playing":false,"tick":123}
```

`play` and `pause` change nothing in the world, so there is nothing to undo.

## History

### undo

Reverts the last change to the scene or the storyboard. No params. Needs: `edit`.

```json
{"can_redo":1,"can_undo":2,"undone":"entity.destroy"}
```

With nothing to undo, it fails with `-32002` and the message `nothing to undo`. Undoing a
`storyboard.edit` whose entries changed since fails with `-32005` and changes nothing.

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

Needs no capability itself; each command inside it is checked as usual. Takes `dry_run`, which
the commands inside it cannot. A batch cannot contain another `batch`, `undo` or `redo`.

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

### events.subscribe

Starts queueing world events for `events.read`, so an agent learns what happened without
re-querying the world. A session keeps up to 16 subscriptions, each with up to 1000 queued
events; when a queue is full the oldest event is dropped.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `kinds` | array of strings | all | Only these kinds of event (below) |
| `ids` | array of strings | all | Only events about these entity IDs |

| Kind | When | Fields |
| --- | --- | --- |
| `entity.created` | An entity with an ID is created, by a command or a system | `id` |
| `entity.destroyed` | An entity with an ID is destroyed | `id` |
| `command` | A command changed the world, including `step`, `undo` and `redo`, or a prefab reload did (`"command": "asset.reload"`) | `command`, and `changes` as [`world.diff`](#worlddiff) returns them |
| `asset.reloaded` | A loaded asset or prefab file changed on disk | `data`: `path`, `type`, and `version`, or `error` if it no longer loads |
| anything else | Gameplay code called `World::emit`, e.g. `"coin_collected"` | `id` and `data`, if the game gave them |

Every event also has `seq`, which counts up across the session, and `tick`, the world's tick when
it happened. A failed command or a dry run queues nothing. Opening or creating a scene, `undo`,
`redo` and `world.restore` queue one `command` event instead of an event per entity. An unknown kind starting with `entity.` fails
with "did you mean". Needs: `read`.

```json
{}
```

```json
{"subscription":"events_1"}
```

### events.read

Returns and removes a subscription's queued events, oldest first.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `subscription` | string | required | Name from `events.subscribe` |
| `limit` | integer, 1 to 1000 | `100` | Most events to return |

Needs: `read`. After creating `enemy_01` and turning `player`:

```json
{"subscription": "events_1"}
```

```json
{"dropped":0,"events":[{"id":"enemy_01","kind":"entity.created","seq":1,"tick":0},{"changes":{"added":["enemy_01"],"changed":{"enemy_01":{"Transform2D":{"position":[200.0,40.0]}}}},"command":"entity.create","kind":"command","seq":2,"tick":0},{"changes":{"changed":{"player":{"Transform2D":{"rotation":1.0}}}},"command":"component.set","kind":"command","seq":3,"tick":0}],"more":false}
```

`dropped` counts the events lost since the last read because the queue was full, and `more` is
`true` when `limit` left events in the queue.

### events.unsubscribe

Stops a subscription and drops its queued events.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `subscription` | string | required | Name from `events.subscribe` |

Needs: `read`.

```json
{"unsubscribed":"events_1"}
```

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
| `preset` | string | required | A preset from `export_presets.json`, or a platform (`macos`, `windows`, `linux`, `web`, `android`, `ios`) for its defaults |
| `debug` | boolean | `false` | Debug build instead of Release |
| `test` | boolean | `false` | Run the exported game headless afterwards, where this machine can |

Needs: `filesystem`, `build`. It changes no scene, so there is nothing to undo.

```json
{"preset": "linux", "test": true}
```

```json
{"bytes":5335403,"executable":"build/export/linux/mygame","linux":{"arch":"x86_64"},"log":".slash/export/linux/export.log","output":"build/export/linux","platform":"linux","preset":"linux","seconds":66.6,"test":{"entities":0,"passed":true,"ticks":60}}
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
