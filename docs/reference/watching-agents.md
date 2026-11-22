# Watching agents

The work journal and watch servers: how a person sees what any agent is doing, and has done, in a
project, whichever vendor's agent it is.

Status: current · Updated: 2026-11-22

## The work journal

Every session that serves an agent keeps a journal: `slash-mcp`, `slash-cli rpc`, `slash-cli
serve` and `slash-cli run` (`slash-cli call` doesn't). Each appends a line of JSON per command to
`.slash/work/<session>.jsonl` in the project as it runs:

| Field | Meaning |
| --- | --- |
| `n` | The line's number in the session, from 1 |
| `time` | UTC, to the millisecond: `2026-11-22T13:05:03.120Z` |
| `session`, `start` | The first line only: the session's ID, and the program (`host`), engine `version` and `pid` that started it |
| `command`, `params` | The command and a short form of its params: long strings, lists and objects cut, and any param whose name holds a word such as key, token, secret, password or credential replaced with `"[left out]"` |
| `client` | Who sent it: the MCP client's name (`clientInfo.name`), when there is one |
| `ms` | How long it took |
| `changes` | The entities it `added` and `removed`, and the components and field names it `changed` by entity, without values (20 entities each at most, the rest counted in `added_more` and the like) |
| `files` | Project paths it wrote or removed |
| `error` | Its `code` and `message`, when it failed |
| `result` | The result of `test.run`, `build.check`, `playtest.run` and every `*.check` command, which say whether the work is good (cut to its numbers past 16 KiB) |
| `reads` | A run of commands that changed nothing and wrote no file (reads and dry runs) folded into one line: each command's count, with `time` the first and `until` the last. A run is written when another line comes, after a second with no read, or at 200 |
| `event` | Something that changed the world outside a command, such as `asset.reload` or `gameplay.reload`, with its `changes` |
| `end` | The last line, when the session ends |

A session that runs no command leaves no file. Session IDs (`20261122-130503-4f2a`) start with
the UTC time the session started, so they sort by age.

A journal moves to `<session>.1.jsonl` once it holds a quarter of the project's share
(`work.max_mb`, 64 MiB by default), replacing the part before. When a session starts, the oldest
sessions' journals are removed, never a running one's, until the project's take less than half
the share.

## Watch servers

Each such session also serves itself read-only on the loopback interface, over the same
[HTTP server](protocols.md#http) as `slash-cli serve`, with a token of its own. It lists itself in
`.slash/work/sessions.json` (readable by its owner only, as it holds tokens) with its ID, program,
process ID, URL, token, journal and start time, and takes itself out when it ends; sessions whose
process ended are dropped the next time the file is read. `slash-cli run` serves watchers only
with `--serve`.

Watchers' requests run between the agent's commands, as every server's do, so watching never
races the agent. They can run only commands that read and need only the `read` capability;
anything else fails with `-32003`. They are not written in the journal, and they don't change the
session's client name.

## Watching

`slash-cli watch --text` follows a session line by line in plain words until it ends, and `slash-cli
work digest` sums up the project's work over a stretch ([CLI](cli.md#watch)):

```text
13:05:02  claude-code  entity.create {"id":"floor"}  +floor
13:05:04  claude-code  read: entity.get, world.query
13:05:07  claude-code  component.set {"components":{"Transform2D":{"position":[64,180]}},"id":"crate"}  ~crate(Transform2D)
13:05:08  claude-code  entity.get {"id":"crat"}  FAILED: /id: no entity has the id "crat"; did you mean "crate"?
13:05:09  claude-code  storyboard.check  -> ok
13:05:10  claude-code  scene.save {"path":"main.scene.json"}  wrote main.scene.json
```

Agents read the same with [`work.feed`](commands.md#workfeed) and
[`work.digest`](commands.md#workdigest), to review another agent's work or say what was done.

## The watch window

`slash-cli watch` with no flags opens a window on the session ([CLI](cli.md#watch)). It asks the
session's watch server for its world with `scene.get` whenever the journal grows, and every
second otherwise (a running game moves things with no command), and shows it as a game's window
would, without stepping it. Over it:

- A bar along the top: the session's ID, its program and client, the agent's last command in the
  journal's plain words, and "ended" once the session ends.
- An outline round each entity the journal says a command or event touched: green round one it
  added, amber round one it changed, named by its ID, at full strength for 3 seconds and then
  fading over 2. An entity removed loses its outline at once.
- A 2D entity's outline covers what it and its children drew in the frame (sprites, tiles,
  polygons, particles, controls); a 3D entity's covers its own and its children's meshes and
  shapes as the current `Camera3D` sees them (the geometry a navigation bake reads). An entity
  that draws nothing gets a small square at its origin.

The window keeps showing the last world after the session ends, until it is closed.

## Settings

`project.json`'s [`work`](project-format.md) block turns either part off:

```json
{"work": {"journal": true, "serve": true, "max_mb": 64}}
```

## Engine notes

`slash/command/work.hpp` is the journal: `work::Journal` writes it, `Session::call` hands it a
`work::Call` for each outermost command (`SessionOptions::journal`), with the world's diff
(`world::diff_scenes`) worked out only while a journal is set, and the paths
`Session::write_file` and `commit_files` touched. `list_journals`, `read_journal`, `digest` and
`describe` read it back, and `live_sessions`, `add_session` and `remove_session` keep
`sessions.json`, written whole through `platform::write_private_file`. `slash/server/watch.hpp`
is the hosts' side: `server::Work` opens the journal from the settings before the session and
serves it with `Watched`, a `watch_only` server that runs each request with
`Session::set_watching`; `StdinLines` reads stdin on a thread so `slash-mcp` and `slash-cli rpc`
answer watchers while they wait for the agent. `server::Follower` follows a session for the
window: the journal's new lines, read again when its files grow, and the world, fetched from the
watch server on a thread of its own, since the session answers only between the agent's commands.
`debug::WatchView` (`slash/debug/watch_view.hpp`) is the window's UI, controls in a world of their
own drawn with `CanvasOptions::overlay`: it places outlines from the frame's
`canvas::FrameContents` in 2D, and in 3D from `navigation::world_geometry`'s boxes projected
through `scene3d::camera_view`. `examples/work-journal` journals an agent's edits and reads them
back, and `examples/watch-window` follows a session and prints what the window outlines.
