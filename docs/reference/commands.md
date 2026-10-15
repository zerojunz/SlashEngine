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
- Paths are relative to the project folder and cannot leave it. Names Windows keeps for devices
  (`con`, `prn`, `aux`, `nul`, `com1` to `com9`, `lpt1` to `lpt9`, with any extension) are
  refused on every platform, since opening one on Windows reaches the device.
- Component fields are written as in scene files: vectors as arrays (`[x, y]`), quaternions as
  `[x, y, z, w]`.

## Dry runs

Add `"dry_run": true` to the params of any command that changes the world (`scene.new`,
`scene.open`, `entity.create`, `entity.destroy`, `component.set`, `component.remove`, `step`,
`run.until`, `world.restore`, `storyboard.edit` and `batch`) to see what it would do. The command runs, then
the world, the scene's path, the next generated ID and the tick counter are put back, and no undo
step is recorded. An error is returned as usual. The result holds the command's own result and the
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
| Materials | [`material.eval`](#materialeval) |
| Animations | [`animation.eval`](#animationeval) |
| Tile maps | [`tilemap.paint`](#tilemappaint), [`tilemap.set`](#tilemapset), [`tilemap.get`](#tilemapget) |
| Lights | [`light.sample`](#lightsample) |
| Storyboard | [`storyboard.query`](#storyboardquery), [`storyboard.brief`](#storyboardbrief), [`storyboard.check`](#storyboardcheck), [`storyboard.edit`](#storyboardedit) |
| Components | [`component.set`](#componentset), [`component.remove`](#componentremove) |
| Query | [`world.query`](#worldquery) |
| Snapshots | [`world.snapshot`](#worldsnapshot), [`world.diff`](#worlddiff), [`world.restore`](#worldrestore) |
| Run | [`step`](#step), [`run.until`](#rununtil), [`play`](#play), [`pause`](#pause) |
| History | [`undo`](#undo), [`redo`](#redo), [`history.get`](#historyget) |
| Several at once | [`batch`](#batch) |
| Observe | [`events.subscribe`](#eventssubscribe), [`events.read`](#eventsread), [`events.unsubscribe`](#eventsunsubscribe), [`log.tail`](#logtail) |
| Display | [`display.list`](#displaylist), [`display.set`](#displayset), [`perf.stats`](#perfstats) |
| Export | [`export.run`](#exportrun) |

Planned commands from the [roadmap](../roadmap.md), not yet available: `input.inject`, `frame.capture` and `replay.run`.

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
{"capabilities":["read","edit","run","filesystem","build"],"commands":["animation.eval","asset.list","batch","component.remove","component.set","engine.describe","entity.create","entity.destroy","entity.get","events.read","events.subscribe","events.unsubscribe","export.run","history.get","light.sample","log.tail","material.eval","prefab.save","redo","run.until","scene.get","scene.new","scene.open","scene.save","schema.get","step","storyboard.brief","storyboard.check","storyboard.edit","storyboard.query","tilemap.get","tilemap.paint","tilemap.set","undo","world.diff","world.query","world.restore","world.snapshot"],"components":[{"defaults":{"position":[0.0,0.0],"rotation":0.0,"scale":[1.0,1.0]},"doc":"Position, rotation and scale in 2D. Units are pixels and y points down.","name":"Transform2D"},{"defaults":{"position":[0.0,0.0,0.0],"rotation":[0.0,0.0,0.0,1.0],"scale":[1.0,1.0,1.0]},"doc":"Position, rotation and scale in 3D. Units are meters and y points up.","name":"Transform3D"},{"defaults":{"ids":[]},"doc":"The storyboard entries this entity stands for.","name":"StoryRef"},{"defaults":{"centered":true,"flip_h":false,"flip_v":false,"frame":0,"hframes":1,"light_mask":1,"material":"","material_params":{},"modulate":[1.0,1.0,1.0,1.0],"normal_texture":"","offset":[0.0,0.0],"region":[0.0,0.0,0.0,0.0],"specular_color":[1.0,1.0,1.0,1.0],"specular_shininess":1.0,"specular_texture":"","texture":"","texture_filter":"inherit","vframes":1,"visible":true,"z_index":0},"doc":"A texture drawn at the entity's Transform2D. Sprites draw in order of z_index, then in the order their entities were made.","name":"Sprite2D"},{"defaults":{"animation":"default","frame":0,"frame_progress":0.0,"playing":true,"speed_scale":1.0,"sprite_frames":""},"doc":"Plays an animation from a .frames.json file on the entity's Sprite2D, which it adds if missing. Emits animation_finished when an animation that doesn't loop ends.","name":"AnimatedSprite2D"},{"defaults":{"animation":"","backwards":false,"library":"","playing":true,"speed_scale":1.0,"time":0.0},"doc":"Plays a keyframe animation from a .anim.json file on the entity's components. Emits animation_finished when an animation that doesn't loop ends, and the events of its event tracks.","name":"AnimationPlayer"},{"defaults":{"legend":{},"light_mask":1,"modulate":[1.0,1.0,1.0,1.0],"origin":[0,0],"rows":[],"texture_filter":"inherit","tile_set":"","visible":true,"z_index":0},"doc":"A grid of tiles from a .tileset.json file, drawn at the entity's Transform2D if it has one. Cells are text: rows of legend keys, \".\" for empty. Edit them with tilemap.paint and tilemap.set.","name":"TileMapLayer"},{"defaults":{"blend_mode":"add","color":[1.0,1.0,1.0,1.0],"enabled":true,"energy":1.0,"height":0.0,"offset":[0.0,0.0],"range_item_cull_mask":1,"range_z_max":1024,"range_z_min":-1024,"shadow_color":[0.0,0.0,0.0,0.0],"shadow_enabled":false,"shadow_filter":"none","shadow_filter_smooth":0.0,"shadow_item_cull_mask":1,"texture":"","texture_scale":1.0},"doc":"A light shining from the entity's Transform2D through a texture. It lights sprites and tile map layers whose light_mask shares a bit with range_item_cull_mask; with shadow_enabled, LightOccluder2D polygons cast shadows from it.","name":"PointLight2D"},{"defaults":{"blend_mode":"add","color":[1.0,1.0,1.0,1.0],"enabled":true,"energy":1.0,"height":0.0,"max_distance":10000.0,"range_item_cull_mask":1,"range_z_max":1024,"range_z_min":-1024,"shadow_color":[0.0,0.0,0.0,0.0],"shadow_enabled":false,"shadow_filter":"none","shadow_filter_smooth":0.0,"shadow_item_cull_mask":1},"doc":"Light from one direction over everything, such as the sun. It shines down the screen, turned by the entity's Transform2D rotation. Masks, ranges and shadows work as PointLight2D's.","name":"DirectionalLight2D"},{"defaults":{"closed":true,"cull_mode":"disabled","occluder_light_mask":1,"polygon":[]},"doc":"A polygon that casts shadows from lights with shadow_enabled, placed by the entity's Transform2D.","name":"LightOccluder2D"},{"defaults":{"color":[1.0,1.0,1.0,1.0]},"doc":"Tints everything lit: the light a scene has where no light reaches. One per scene.","name":"CanvasModulate"}],"engine":{"name":"SlashEngine","version":"0.1.0"},"scene":{"entities":0,"path":"","tick":0},"systems":["animation_player","animated_sprite_2d"]}
```

`scene` describes the open scene: how many entities it has, its path (empty for an unsaved one)
and the current tick. In a running game, which has [`play`](#play) and [`pause`](#pause), it also
says whether the game is `playing`.

`systems` lists the systems in the order they run. With `"full": true`, each is an object:

```json
{"name":"movement","doc":"Moves things by their velocity.","priority":0}
```

### schema.get

Returns a JSON Schema: for scene, prefab, [`.frames.json`](scene-format.md#sprite-frames),
[`.material.json`](scene-format.md#materials), [`.anim.json`](scene-format.md#animations),
[`.tileset.json`](scene-format.md#tile-sets) or storyboard files, one component or one command.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `kind` | `"scene"`, `"prefab"`, `"frames"`, `"material"`, `"animation"`, `"tileset"`, `"storyboard"`, `"component"` or `"command"` | required | Which schema |
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

## Materials

### material.eval

Returns the colors a [material](scene-format.md#materials) draws at texture coordinates, before
blending, from 0 to 1 and computed on the CPU as the GPU computes them: a check that needs no
screenshot. Given an entity, it uses its `Sprite2D`'s material, texture, `material_params` and
`modulate`; the other params try other values. A sprite with no material gives its texture times
its modulate.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `id` | string | | An entity with a `Sprite2D`. Give `id` or `path` |
| `path` | string | | A `.material.json` file |
| `uv` | array of `[u, v]` | required | The texture coordinates, 1 to 64 of them |
| `texture` | string | the entity's, or none | The sprite's texture path; none samples white |
| `params` | object | | Param values by name, over the entity's `material_params` |
| `color` | `[r, g, b, a]` | the entity's `modulate`, or white | What the `color` input reads |
| `frag_coord` | `[x, y]` | `[0, 0]` | What `fragcoord` reads, in pixels |
| `time` | number | `0` | What `time` reads, in seconds |

Needs: `read`.

```json
{"id": "hero", "uv": [[0.5, 0.5]], "params": {"flash": 0.5}}
```

```json
{"colors":[[1.0,0.625,0.5,1.0]]}
```

Colors are rounded to three decimals. A param the material doesn't have fails with the closest
name, and a value of the wrong type or outside the param's range says what it expected; the
error's `path` is `/params/<name>` for a param given here and `/id` for one of the entity's.

## Animations

### animation.eval

Returns the values an [animation](scene-format.md#animations)'s value tracks set at the times
given, as `AnimationPlayer` sets them, without playing it: a check that needs no stepping. Given
an entity, it uses its `AnimationPlayer`'s library and, unless `animation` names another, its
animation.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `id` | string | | An entity with an `AnimationPlayer`. Give `id` or `path` |
| `path` | string | | A `.anim.json` file |
| `animation` | string | the entity's | The animation's name; required with `path` |
| `times` | array of numbers | required | Seconds into the animation, 1 to 100 of them, each from 0 to its length |
| `tracks` | array of strings | all | The value track paths to return |

Needs: `read`.

```json
{"id": "hero", "times": [0, 0.125, 0.25, 0.375]}
```

```json
{"length":0.5,"loop":"linear","tracks":{"Sprite2D/frame":[0,1,2,3],"Sprite2D/offset/y":[0,-2,-4,-2]}}
```

`tracks` holds one value per time for each value track, keyed by path. Numbers are rounded to
four decimals. A discrete track gives the value of the last key at or before each time. An
unknown animation or track path fails with the closest name, and a time past the animation's
length fails with its `path`, `/times/<n>`. Event tracks are left out; read them in the file.

## Tile maps

These commands edit an entity's [`TileMapLayer`](scene-format.md#tile-map-layers) through its
[tile set](scene-format.md#tile-sets), and write its cells back in canonical form. Each takes
the cells as `cells`, a list of `[column, row]` (up to 4096), or `rect`,
`[column, row, width, height]` (up to 65536 cells); give one. Columns and rows are from -32768 to
32767. A layer whose tile set doesn't load, or whose legend or rows are wrong, fails with `path`
`/id` and the message names the bad value, as `"map"'s TileMapLayer/rows/2: ...`.

### tilemap.paint

Paints a terrain on cells and picks tiles for them and the terrain cells around them so their
edges connect, as Godot's terrain painting does. `null` erases the cells and fixes up their
neighbors. Returns how many cells changed.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `id` | string | required | An entity with a `TileMapLayer` |
| `cells` | array of `[column, row]` | | The cells to paint. Give `cells` or `rect` |
| `rect` | `[column, row, width, height]` | | Every cell in it |
| `terrain` | string or `null` | required | A terrain of the tile set, or `null` to erase |

Needs: `edit`.

```json
{"id": "ground", "rect": [2, 1, 8, 6], "terrain": "grass"}
```

```json
{"changed":48,"id":"ground"}
```

Each cell gets the tile of its terrain whose `peering` fits its neighbors best, first where its
own terrain stops and then which terrain lies beyond; empty cells count as no terrain, so
painting in empty space draws edges. Equal fits are picked by the cell's position, weighted by
`probability`, so the same paint gives the same tiles. An unknown terrain fails with the
closest name.

### tilemap.set

Sets cells to one tile, or erases them, without changing the tiles around them. Returns how
many cells changed.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `id` | string | required | An entity with a `TileMapLayer` |
| `cells` | array of `[column, row]` | | The cells to set. Give `cells` or `rect` |
| `rect` | `[column, row, width, height]` | | Every cell in it |
| `tile` | string or `null` | required | A tile by name or as `"source:column,row"`, or `null` to erase |

Needs: `edit`.

```json
{"id": "ground", "cells": [[8, 2]], "tile": "rock"}
```

```json
{"changed":1,"id":"ground"}
```

An unknown tile or source fails with the closest name.

### tilemap.get

Returns a layer's cells as rows of legend keys from an origin, with `.` for an empty cell: the
whole layer as stored, or a window of it in the layer's own keys.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `id` | string | required | An entity with a `TileMapLayer` |
| `rect` | `[column, row, width, height]` | the whole layer | Only these cells |

Needs: `read`.

```json
{"id": "ground", "rect": [3, 2, 3, 3]}
```

```json
{"legend":{"G":"grass_n","a":"grass_s","b":"grass_w","c":"grass_e","j":"grass_in_se","k":"grass_in_sw","l":"grass_in_ne","m":"grass_in_nw","w":"water"},"origin":[3,2],"rows":["jak","cwb","lGm"]}
```

With `rect`, `origin` is the rect's top left, there is a row for each of its rows, and each row
ends at its last tile. `legend` holds only the keys the rows use.

## Lights

### light.sample

Returns how lit points are: the color a white pixel of a lit sprite gets there from the scene's
[2D lights](scene-format.md#2d-lights), computed on the CPU as the GPU shades it, and each light
that reaches it. A check that needs no screenshot. Given an entity, it samples at its
`Transform2D` position with its `Sprite2D`'s (or `TileMapLayer`'s) `light_mask` and `z_index`.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `points` | array of `[x, y]` | | The points, 1 to 256 of them. Give `points` or `id` |
| `id` | string | | An entity with a `Transform2D` |
| `light_mask` | integer | the entity's, or `1` | The draw's light mask, 0 to 1048575 |
| `z_index` | integer | the entity's, or `0` | The draw's z index, for the lights' z ranges |

Needs: `read`. In the scene of [examples/lights-2d](../../examples/lights-2d/), a point by the
torch and one behind the crate from it:

```json
{"points": [[96, 60], [200, 90]]}
```

```json
{"ambient":[0.12,0.12,0.2],"lit":true,"points":[{"color":[1.244,1.02,0.9],"lights":[{"color":[0.125,0.15,0.25],"id":"moon"},{"color":[0.999,0.75,0.45],"id":"torch","shadow":0}]},{"color":[0.245,0.27,0.45],"lights":[{"color":[0.125,0.15,0.25],"id":"moon"},{"color":[0,0,0],"id":"torch","shadow":1}]}]}
```

- `ambient` is the `CanvasModulate` color, and `lit` says whether anything lights the scene;
  when nothing does, draws are unshaded and every point is white.
- Each point's `color` is `[r, g, b]` after the ambient color and every light, before clamping,
  so a value above 1 means the light is saturated there.
- `lights` lists the lights that reach the point, in draw order, each with its color there times
  its alpha (what it adds with `blend_mode` `add`). A light with `shadow_enabled` also has
  `shadow`, from 0 (lit) to 1 (hidden by occluders). A light whose texture is clear at the point
  is left out.
- Normal maps aren't sampled: the pixel faces the viewer. Values are rounded to three decimals.
- A light that can't shine, such as one whose texture doesn't load, is left out and listed in
  `warnings` with its entity's ID and why.

## Storyboard

The game's facts as typed entries in `storyboard/*.json`; see the
[storyboard format](storyboard-format.md). Examples run against a copy of `tests/data/game`.

An agent calls `storyboard.brief` before a task, so it works from the game's recorded facts
instead of guessing them, records what it decides with `storyboard.edit`, and calls
`storyboard.check` after the task.

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

### storyboard.brief

The facts a task needs, in one call: name the scenes, entities, files or entries it touches.
Returns, each with every field:

- the entries named, and the entries named by the `StoryRef` components in those scenes or on
  those entities;
- entries whose `links` name those files, those scenes, or the prefabs their entities extend;
- rules, tones, palettes and budgets that apply everywhere (those with no `links` or `refs`);
- open tasks and live decisions that link to those files or name those entries.

`related` lists the other entries those name, with name and summary only. With `since`, `changed`
maps each entry changed after that journal `seq` to its new version, or `null` if it was removed,
so an agent that briefed earlier rereads only what moved.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `scenes` | array of paths | | Scene files, e.g. `["scenes/main.scene.json"]`. The open scene is read as it is now |
| `entities` | array of IDs | | Entities in the open scene |
| `files` | array of paths | | Other project files: assets, prefabs, source |
| `ids` | array of IDs | | Storyboard entries |
| `since` | integer | | Also list entries changed after this journal `seq` |

An unknown entity or entry, or a scene that does not exist, fails with `-32001` and a "did you
mean" hint. Needs: `read`.

```json
{"scenes": ["scenes/main.scene.json"]}
```

```json
{"entries":[{"fields":{"links":["scenes/main.scene.json"],"name":"Pilot","summary":"The player: a courier pilot on her first run."},"id":"character/pilot","version":"f6ce5b73"},{"fields":{"links":["prefabs/marker.prefab.json","scenes/main.scene.json"],"name":"Landing pad","summary":"Where every run starts."},"id":"place/landing_pad","version":"4bdb955e"},{"fields":{"name":"One hit","summary":"Any hazard ends the run; there is no health bar."},"id":"rule/one_hit","version":"7b97241a"}],"related":[],"seq":0}
```

After another agent's edit:

```json
{"files": ["prefabs/marker.prefab.json"], "since": 0}
```

```json
{"changed":{"character/pilot":"80d23357","term/courier":"d1f7edb3"},"entries":[{"fields":{"links":["prefabs/marker.prefab.json","scenes/main.scene.json"],"name":"Landing pad","summary":"Where every run starts."},"id":"place/landing_pad","version":"4bdb955e"},{"fields":{"name":"One hit","summary":"Any hazard ends the run; there is no health bar."},"id":"rule/one_hit","version":"7b97241a"}],"related":[],"seq":1}
```

### storyboard.check

Checks the storyboard against itself and the project. Errors: a storyboard file that is invalid,
a reference to an entry that does not exist or is of the wrong kind, a word a glossary `term`
says to `avoid` (as a whole word, ignoring case), and a `StoryRef` ID in a scene or prefab that
names no entry. Every `.scene.json` and `.prefab.json` file in the project is read, except those
in `build/` and in folders whose names start with `.`; the open scene is checked as it is now.
Warnings: a link to a file that does not exist, since open work can link files still to be made.
`slash-cli project validate` runs the same check.

Takes no params. Needs: `read`.

```json
{}
```

```json
{"errors":[{"file":"storyboard/world.json","message":"uses \"driver\", which term/courier says to avoid; write \"courier\"","path":"/entries/character~1pilot/summary"},{"file":"scenes/main.scene.json","message":"unknown storyboard entry \"character/pilto\"; did you mean \"character/pilot\"?","path":"/entities/player/components/StoryRef/ids/0"}],"valid":false}
```

`errors` and `warnings` are left out when empty, so a clean project returns `{"valid":true}`.

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

### run.until

Steps the simulation until an entity matches a condition, or until none does, or the tick limit
passes, then returns once. Use it instead of stepping and querying after every tick: "when does
the ball reach the floor?", "do all enemies die within ten seconds?". Undo reverts the entities to
how they were before.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `with`, `without`, `where` | as in [`world.query`](#worldquery) | | The entities to wait for. At least one is required |
| `until` | `"any"` or `"none"` | `"any"` | Stop when any entity matches, or when none does |
| `ticks` | integer, 1 to 100000 | `600` | Most ticks to run |
| `tick_seconds` | number, above 0 and at most 1 | `1/60` | Seconds per tick |
| `include`, `omit_defaults`, `limit` | as in [`world.query`](#worldquery) | | What to return about the matching entities |

Needs: `run`. Takes `dry_run`.

The condition is checked before each tick, so one that already holds runs no ticks. The result is
`world.query`'s for the entities that match when it stops, plus `matched` (whether the condition
held), `ticks` (how many ran) and `tick` (the world's tick counter). With `"until": "none"` a
match means nothing is left, so `entities` is empty.

From [examples/run-until](../../examples/run-until/), where a `gravity` system makes `ball` fall
and a `despawn` system removes bodies below y = 1000:

```json
{"with": ["Body"], "include": ["Transform2D", "Body"], "omit_defaults": true, "tick_seconds": 0.015625,
 "where": [{"component": "Transform2D", "field": "position.y", "op": ">", "value": 600}]}
```

```json
{"count":1,"entities":[{"components":{"Body":{"velocity":[0.0,1104.0]},"Transform2D":{"position":[40.0,603.75]}},"id":"ball"}],"matched":true,"tick":69,"ticks":69,"truncated":false}
```

```json
{"with": ["Body"], "until": "none", "tick_seconds": 0.015625}
```

```json
{"count":0,"entities":[],"matched":true,"tick":89,"ticks":10,"truncated":false}
```

When the limit passes first, `matched` is `false`:

```json
{"with": ["Body"], "ticks": 10, "tick_seconds": 0.015625,
 "where": [{"component": "Transform2D", "field": "position.x", "op": "!=", "value": 40}]}
```

```json
{"count":0,"entities":[],"matched":false,"tick":79,"ticks":10,"truncated":false}
```

In a running game it runs between frames, so the game waits for it, as it does for `step`.

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

## Display

A game served with `slash-cli run --serve` (or reached through `slash-mcp --live`) also has these
three, for its displays, how it presents frames and how evenly they arrive. Other sessions have
no window or loop, so they leave them out. [examples/frame-pacing](../../examples/frame-pacing/)
calls all three.

### display.list

Lists the displays, primary first, with the game's V-Sync mode and frame cap. A headless game
has no displays.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `modes` | boolean | `false` | Also list each display's fullscreen modes (`width`, `height`, `refresh_rate`, `pixel_density`) |

Each display has its `id`, `name`, whether it is `primary` and whether the game's `window` is on
it, its `bounds` (`[x, y, width, height]` in screen coordinates), its UI `scale`, the current
mode's `refresh_rate` and `pixel_density`, and `refresh_rates`: every rate its fullscreen modes
offer, fastest first, for an options menu. A refresh rate of `0` means the system doesn't say.
Needs: `read`. On SDL's offscreen video driver, which has one display and reports no refresh
rate:

```json
{}
```

```json
{"displays":[{"bounds":[0,0,1024,768],"id":1,"name":"1","pixel_density":1.0,"primary":true,"refresh_rate":0.0,"refresh_rates":[],"scale":1.0,"window":true}],"max_fps":120,"vsync_mode":"mailbox"}
```

### display.set

Changes the V-Sync mode or the frame cap from the next frame, as a game's options menu would,
and returns the settings in effect.

| Param | Type | Default | Meaning |
| --- | --- | --- | --- |
| `vsync_mode` | `"disabled"`, `"enabled"`, `"adaptive"` or `"mailbox"` | unchanged | How frames wait for the display; see [project-format.md](project-format.md) |
| `max_fps` | integer, 0 to 1000 | unchanged | Frames per second at most; `0` means no cap |

A mode the GPU or window lacks falls back to `enabled`, which every GPU has, and the result
shows the mode in effect. SDL's GPU API has no adaptive mode yet, so `adaptive` always falls back
on a window. Needs: `run`. It changes nothing in the world, so there is nothing to undo. Here
Mesa's software Vulkan on an offscreen window has no mode that skips V-Sync:

```json
{"vsync_mode": "disabled", "max_fps": 0}
```

```json
{"max_fps":0,"vsync_mode":"enabled"}
```

### perf.stats

Frame pacing over the last 120 frames, with the settings that shape it. No params. Needs: `read`.

```json
{"catch_up_frames":0,"fps":120.0,"frame_ms":{"avg":8.33,"max":8.67,"min":7.99},"frames":932,"max_fps":120,"renderer":"vulkan","tick_hz":60.0,"ticks":465,"vsync_mode":"mailbox"}
```

`frame_ms` is the time from one frame's start to the next: a `max` far above `avg` is a hitch.
`catch_up_frames` counts, since the game started, frames that ran more than one fixed tick
because the frame before was late. `frames` and `ticks` also count from the start, and
`renderer` names the render device (`null` when nothing is drawn). A headless game runs one tick
per frame and never waits, so its frame times only show how long its ticks take.

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
