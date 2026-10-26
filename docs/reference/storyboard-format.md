# Storyboard format

The storyboard holds a game's facts that are not code: its style, world, story, design rules,
names, decisions and open work. Each fact is a typed entry under a stable ID, so an agent looks it
up with `storyboard.query` instead of remembering or guessing it, and changes it with
`storyboard.edit` (see the [command reference](commands.md#storyboard)). This page describes
format version 1. `slash-cli schema storyboard` (or `schema.get` with `{"kind": "storyboard"}`)
prints the JSON Schema.

## Files

A project's storyboard is up to eight files in its `storyboard/` folder. Each kind of entry lives
in one file, so `storyboard.edit` always knows where an entry goes. A missing file has no entries.
`slash-cli init` starts a new game with two: `glossary.json`, holding the game's title as a
`term`, and `work.json`, holding an open task to fill in the rest.

```text
storyboard/
  style.json       # palette, tone, reference, budget
  world.json       # character, place, faction, item, lore
  story.json       # act, beat, quest
  design.json      # mechanic, rule, tuning, control
  music.json       # theme, cue, loudness
  glossary.json    # term
  decisions.json   # decision
  work.json        # task
  journal.jsonl    # who changed which entries, when and why
```

`tests/data/game/storyboard/world.json`:

```json
{
  "entries": {
    "character/pilot": {
      "links": ["scenes/main.scene.json"],
      "name": "Pilot",
      "summary": "The player: a courier pilot on her first run."
    },
    "place/landing_pad": {
      "links": ["prefabs/marker.prefab.json", "scenes/main.scene.json"],
      "name": "Landing pad",
      "summary": "Where every run starts."
    }
  },
  "format": 1
}
```

| Key | Required | Meaning |
| --- | --- | --- |
| `format` | Yes | Format version. Must be `1` |
| `entries` | Yes | Entries keyed by ID |
| `entries.<id>.<field>` | `name` only | A field of the entry's kind |

## IDs

An ID is a kind and a name, such as `character/mira` or `task/fix-jump`. The name is 1 to 64
lowercase letters, digits, `_` and `-`. A misspelled kind is an error with a "did you mean" hint.

## Fields

Every entry has `name` and may have the common fields:

| Field | Type | Meaning |
| --- | --- | --- |
| `name` | string | What it is called, or a short title. Required |
| `summary` | string | One line on what it is |
| `text` | string | Anything longer, in full |
| `refs` | list of IDs | Other entries it relates to |
| `links` | list of paths | Scenes, assets, source files and folders it is about, relative to the project folder; a folder covers everything in it |
| `tags` | list of strings | Labels for filtering |

Each kind adds its own fields. A field that names another entry must name one that exists, and of
the kind shown.

| Kind | File | Fields besides the common ones | For |
| --- | --- | --- | --- |
| `palette` | style | `colors`: names mapped to `"#rrggbb"`; `tolerance` (integer, 0 to 255) | Named colors the art uses; the textures in its `links` may use only these |
| `tone` | style | | The mood and voice of the game, or of a part |
| `reference` | style | | Art or writing to follow; put the file in `links` |
| `budget` | style | `triangles`, `texture_size`, `texel_density` (integers) | Limits for the assets in its `links`, or for all; `texel_density` is the texels a meter models' textures should have, from half to twice it |
| `character` | world | `faction` (faction), `home` (place), `traits` (strings), `voice` | A person or creature |
| `place` | world | `region` (place) | A location |
| `faction` | world | `leader` (character) | A group, side or organization |
| `item` | world | `owner` (character) | An object that matters |
| `lore` | world | | A fact about the world's history or nature |
| `act` | story | `order` (integer) | A major part of the story |
| `beat` | story | `act` (act), `order` (integer), `place` (place), `characters` (characters) | One event in the story |
| `quest` | story | `giver` (character), `steps` (strings) | A goal the player can take on |
| `mechanic` | design | `controls` (controls) | A system the player interacts with |
| `rule` | design | | A design rule every change must keep |
| `tuning` | design | `value`, `min`, `max` (numbers), `unit` | A gameplay number and its limits |
| `control` | design | `action`, `keys` (strings), `gamepad` (strings) | An input action and its bindings |
| `theme` | music | `subjects` (any), `key`, `tempo` (number), `bar_beats` (integer), `instruments`, `mood` (strings) | A musical theme for characters, places or factions |
| `cue` | music | `theme` (theme), `scenes` (paths), `beats` (beats), `states`, `layers` (strings), `plays` (`loop`, `once`), `after` (cues), `music` (paths) | A piece of music and when it plays: one line of the cue sheet |
| `loudness` | music | `lufs`, `true_peak`, `tolerance` (numbers) | A loudness target for the sounds in its `links`, or for all music |
| `term` | glossary | `aliases`, `avoid` (strings) | A canonical name; `name` is the spelling to use |
| `decision` | decisions | `status` (`proposed`, `accepted`, `superseded`), `by`, `date`, `why`, `supersedes` (decision) | Something decided, by whom and why |
| `task` | work | `status` (`open`, `in_progress`, `blocked`, `done`), `owner`, `questions` (strings) | Open work; `refs` and `links` say what it touches |

## StoryRef

The built-in `StoryRef` component says which entries an entity stands for, so a scene and the
storyboard stay tied together:

```json
{
  "entities": {
    "player": {
      "components": {
        "StoryRef": {
          "ids": ["character/pilot"]
        }
      }
    }
  },
  "format": 1
}
```

`storyboard.brief` with the entity, or with its scene, returns those entries. A prefab can carry a
`StoryRef` too, and every entity that extends it inherits it. An entry can point the other way
with `links`, naming the scenes, prefabs and assets it is about.

## Music

The music file plans a game's soundtrack before any is written. A `theme` is a musical idea for
the characters, places or factions in its `subjects`: a `key` such as `"D minor"`, a `tempo` in
beats per minute, `bar_beats` (4 if unset), `instruments` named as
[scores](scene-format.md#scores) name General MIDI instruments, and a `mood`. A `cue` is one line
of the cue sheet: the `theme` it plays, and when it plays, in `scenes` (files or folders), during
story `beats` and in game `states` such as `"combat"`. Its `layers` are intensity levels, lowest
first, `plays` is `loop` (if unset) or `once`, `after` names the cues a cue that plays once
follows, as a sting, as the music leaves them, and `music` names the `.score.json`,
`.audio.json`, `.ogg`, `.wav` or `.mp3` files that play it once they exist, one per layer for a
cue with layers. A `loudness` entry sets a target, in LUFS, and
the highest true peak, in dBTP, for the sounds in its `links`, or with no `links` for all music.

```json
{
  "entries": {
    "cue/lighthouse": {
      "beats": ["beat/storm"],
      "name": "Lighthouse",
      "scenes": ["scenes/lighthouse.scene.json"],
      "theme": "theme/keeper"
    },
    "loudness/music": {
      "lufs": -16,
      "name": "Music",
      "true_peak": -1
    },
    "theme/keeper": {
      "instruments": ["accordion"],
      "key": "A minor",
      "mood": ["patient"],
      "name": "The Keeper",
      "subjects": ["character/keeper"],
      "tempo": 72
    }
  },
  "format": 1
}
```

[`soundtrack.plan`](commands.md#soundtrackplan) drafts the cues a game is missing from its scenes
and story, and `storyboard.brief` with a scene returns its cue with the cue's theme in full, so
whoever writes the music starts from the plan. [`soundtrack.arrange`](commands.md#soundtrackarrange)
turns the cues into interactive music once they have music, and
[`asset.generate`](commands.md#assetgenerate) with a `theme` asks a music generator for the
theme's key, tempo, meter, instruments and mood.
[examples/soundtrack-plan](../../examples/soundtrack-plan/) plans a small game's music, and
[examples/soundtrack-arrange](../../examples/soundtrack-arrange/) arranges and plays one.

## Checks

`storyboard.check` (see the [command reference](commands.md#storyboardcheck)) checks the whole
storyboard after a task. It reports as errors:

- a storyboard file that is invalid;
- a reference to an entry that does not exist, or is of the wrong kind;
- a word that a `term` lists in `avoid`, anywhere in another entry's text, matched as a whole
  word and ignoring case;
- a `StoryRef` ID in a scene or prefab that names no entry, with the closest existing ID;
- a texture (`.png` or `.bmp`) larger on a side than a `budget`'s `texture_size`;
- a texture with pixels in a color no `palette` covering it has, naming the first few colors,
  where each first appears and the nearest palette color. Fully transparent pixels don't count,
  and `tolerance` lets each channel be that far off, for art that was filtered or compressed.
- a model (`.gltf`, `.glb`, `.obj`, `.fbx`, `.model.json`, and `.blend` where Blender may run)
  with more triangles than a `budget`'s `triangles`, or holding an image larger on a side than
  its `texture_size` (an image in a file of its own is a texture, checked as above), or whose
  albedo textures have less than half or more than twice a budget's `texel_density` texels a
  meter, measured over every textured triangle. Palettes leave models alone, since lights change
  their colors.

A link to a file that does not exist is a warning, since open work can link files still to be
made. So, once the storyboard has a `cue`, is each scene no cue plays in and each story beat no
cue plays during. `slash-cli project validate` runs the same check, so the workflow `slash-cli init` writes
fails a pull request that breaks the storyboard, and `slash-cli scene validate` checks a scene's
`StoryRef` IDs:

```json
{"errors":[{"message":"unknown storyboard entry \"character/pilto\"; did you mean \"character/pilot\"?","path":"/entities/player/components/StoryRef/ids/0"}],"valid":false}
```

A `palette` covers the textures in its `links`, files or folders; with no links it guides the art
(`storyboard.brief` sends it with every task) but checks nothing, since painted art, normal maps
and light textures rarely keep to a palette. A `budget` covers its links too, or, with neither
`links` nor `refs`, every texture in the project. When several palettes cover a texture, a pixel
may use any of their colors. A pixel-art game might keep its sprites to the PICO-8 colors and
every texture to 256 pixels a side:

```json
{
  "entries": {
    "budget/textures": {
      "name": "Textures",
      "texture_size": 256
    },
    "palette/pico": {
      "colors": {"black": "#000000", "dark_blue": "#1d2b53", "red": "#ff004d", "white": "#fff1e8"},
      "links": ["art/sprites"],
      "name": "PICO-8"
    }
  },
  "format": 1
}
```

```json
{"errors":[{"file":"art/sprites/crate.png","message":"has 12 pixels in 1 color not in palette/pico: #ff0000 at (3, 4), nearest \"red\" #ff004d","path":""},{"file":"art/title.png","message":"is 512x128 pixels; budget/textures allows 256 on a side","path":""}],"valid":false}
```

A `budget` covers models as it covers textures: by its links, or every model without them.

### Refining the style from accepted assets

The style section is meant to follow the art the game keeps, not only what was written first.
[`asset.style`](commands.md#assetstyle) reads the library assets the project uses, takes those a
person or agent last judged `accepted` with [`asset.feedback`](commands.md#assetfeedback) (a
check's verdict doesn't count), and drafts edits for `storyboard.edit`:

- a `palette` covering an accepted image gains the colors it uses that the palette lacks, named
  in words (`dark_blue`, `light_gray`); with no palette at all, `palette/main` takes the colors of
  the images drawn in at most 16 flat colors, with a `tolerance` of 8, linked to their folders;
- a `budget` gains `triangles` and `texture_size` where no budget sets them for an accepted model
  or image: the most an accepted one has, rounded up to two significant digits or a power of two,
  on the budget with no links or refs, else on a new `budget/assets`;
- each accepted asset given a reason that no `reference` links gets a `reference/<file name>`
  linking it, its description as the name and the reason as the summary.

The asset library sends the references with every request for an asset of the same type, so
what worked before shapes what is made next, and `storyboard.check` holds new models to the
budgets. [examples/asset-style](../../examples/asset-style/) drafts a cafe's style this way.

## Model review

Rules catch broken references and words to avoid, but not dialogue that calls Oren Mira's sister
when the storyboard says brother. `storyboard.review` (see the
[command reference](commands.md#storyboardreview)) sends new material (text, a scene's interface
text, other files, images, or entries just written) with the entries it touches to a model from
[`providers.json`](providers.md), and returns what likely contradicts them:

```json
{"advisory":true,"facts":4,"findings":[{"confidence":"high","entry":"character/oren","path":"/entities/greeting/components/Label/text","problem":"Oren is Mira's brother, not her sister.","quote":"My sister Oren","source":"scenes/tavern.scene.json"}],"items":1,"model":"llama3.2","usage":{"input_tokens":310,"output_tokens":48}}
```

The entries sent are those `storyboard.brief` returns for the material, and every entry whose
`name` or `aliases` it mentions. Findings are advice, not checks: a model can miss a
contradiction or see one that isn't there, so the review never fails a pull request as
`storyboard.check` does. A finding must quote the material and name an existing entry, or it is
dropped and counted in `unverified`.

## Versions

Every entry has a version: eight hex digits that change whenever its fields do. `storyboard.query`
returns it, and `storyboard.edit` takes the version it read. If another agent changed the entry
in between, the edit is refused with `-32005` and the entry as it is now, so nothing newer is
overwritten. Versions are computed, not stored, so hand edits change them too.

## Journal

`storyboard.edit` appends one line to `storyboard/journal.jsonl` per edit, and so do its undo and
redo:

```json
{"by":"claude-code","changes":{"character/pilot":"45b24094","character/vex":"a77d1847"},"time":"2026-10-15T02:17:20Z","why":"Give the pilot a rival"}
```

| Key | Meaning |
| --- | --- |
| `by` | Who: the edit's `by`, else the MCP client's name, else `unknown` |
| `changes` | Each changed entry's new version, or `null` if it was removed |
| `time` | When, in UTC |
| `why` | The edit's `why`, if it gave one; undo and redo add `undo: ` and `redo: ` |

Commands return `seq`, the journal's line count, so an agent can tell whether anything changed
since it last looked. The journal is append-only: commit it with the storyboard.

## Canonical form and checks

`storyboard.edit` writes files in canonical form: sorted keys, two-space indent and a newline at
the end, as for [scenes](scene-format.md#canonical-form). Every storyboard command reads all the
files first and refuses to run while one is invalid, listing every error with its file and JSON
Pointer:

```json
{"error":{"code":-32002,"data":{"errors":[{"file":"storyboard/story.json","message":"missing required property \"name\"","path":"/entries/beat~1x"}]},"message":"storyboard/story.json at /entries/beat~1x: missing required property \"name\"; fix the storyboard file, then retry"}}
```

Agents should change the storyboard through `storyboard.edit`, which checks fields and references,
records the journal and can be undone. A hand edit is checked by the next storyboard command.
