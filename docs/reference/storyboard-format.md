# Storyboard format

The storyboard holds a game's facts that are not code: its style, world, story, design rules,
names, decisions and open work. Each fact is a typed entry under a stable ID, so an agent looks it
up with `storyboard.query` instead of remembering or guessing it, and changes it with
`storyboard.edit` (see the [command reference](commands.md#storyboard)). This page describes
format version 1. `slash-cli schema storyboard` (or `schema.get` with `{"kind": "storyboard"}`)
prints the JSON Schema.

## Files

A project's storyboard is up to seven files in its `storyboard/` folder. Each kind of entry lives
in one file, so `storyboard.edit` always knows where an entry goes. A missing file has no entries.
`slash-cli init` starts a new game with two: `glossary.json`, holding the game's title as a
`term`, and `work.json`, holding an open task to fill in the rest.

```text
storyboard/
  style.json       # palette, tone, reference, budget
  world.json       # character, place, faction, item, lore
  story.json       # act, beat, quest
  design.json      # mechanic, rule, tuning, control
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
| `links` | list of paths | Scenes, assets and source files it is about, relative to the project folder |
| `tags` | list of strings | Labels for filtering |

Each kind adds its own fields. A field that names another entry must name one that exists, and of
the kind shown.

| Kind | File | Fields besides the common ones | For |
| --- | --- | --- | --- |
| `palette` | style | `colors`: names mapped to `"#rrggbb"` | Named colors the art uses |
| `tone` | style | | The mood and voice of the game, or of a part |
| `reference` | style | | Art or writing to follow; put the file in `links` |
| `budget` | style | `triangles`, `texture_size` (integers) | Limits for a class of assets |
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

## Checks

`storyboard.check` (see the [command reference](commands.md#storyboardcheck)) checks the whole
storyboard after a task. It reports as errors:

- a storyboard file that is invalid;
- a reference to an entry that does not exist, or is of the wrong kind;
- a word that a `term` lists in `avoid`, anywhere in another entry's text, matched as a whole
  word and ignoring case;
- a `StoryRef` ID in a scene or prefab that names no entry, with the closest existing ID.

A link to a file that does not exist is a warning, since open work can link files still to be
made. `slash-cli project validate` runs the same check, so the workflow `slash-cli init` writes
fails a pull request that breaks the storyboard, and `slash-cli scene validate` checks a scene's
`StoryRef` IDs:

```json
{"errors":[{"message":"unknown storyboard entry \"character/pilto\"; did you mean \"character/pilot\"?","path":"/entities/player/components/StoryRef/ids/0"}],"valid":false}
```

Checking assets against `palette` colors and `budget` limits comes with sprites, textures and
meshes ([roadmap](../roadmap.md)).

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
