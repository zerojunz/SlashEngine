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
13:05:12  checkpoint 1 (scene.save)
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

## The watch page

Each watch server also serves a page for people at `GET /watch`. `slash-cli watch --browser`
opens it for a running session, and `--url` prints its link, `http://127.0.0.1:<port>/watch#token=<token>`,
to open elsewhere on the same machine ([CLI](cli.md#watch)). The token rides in the link's
fragment, which browsers never send, so the page itself needs none; it calls the session's
commands with it as any watcher does, so it can only read. It shows, and keeps up to date every
second while the session runs:

- The world now: `frame.capture`'s picture, drawn again after each command that changed
  something and every 10 seconds otherwise, or with "Draw again".
- What the agent did, newest first, from [`work.feed`](commands.md#workfeed): each command with its
  params, chips for the entities it added, removed and changed (with the components) and the
  files it wrote, and each failure's message.
- Tests and checks: the last result of each command that returned one, judged as the journal
  judges it (`ok`, `passed`, `valid`, `failed` and lists of problems).
- So far: [`work.digest`](commands.md#workdigest)'s summary of the session.
- The agent's plan ([`work.plan`](commands.md#workplan)): its task and steps, ticked as they finish.
- Steer, while the session runs: Pause with a reason, Resume, and a note for the agent
  ([steering](#steering)).
- Checkpoints: a strip of each [checkpoint](#checkpoints)'s frame with its label, newest last.
  Picking one sets it beside the one before with what changed between them
  ([`work.compare`](commands.md#workcompare)).

The page follows the system's light or dark theme and fits a phone's screen. It is one
self-contained file with no outside scripts, styles or fonts, served with a Content Security
Policy that lets it reach only its own server.

## Checkpoints

A journaled session keeps checkpoints of its work in `.slash/work/checkpoints/<session>/`, so a
person can see how it got where it is, compare two points and go back to one. It takes one after
each `scene.save`, each `test.run` with no failures and each
[`work.checkpoint`](commands.md#workcheckpoint), which an agent calls with a `label` when its
work reaches a point worth keeping. A checkpoint holds:

- The files the session wrote since the last one, as they are then (the first time the session
  writes a file, it keeps the file as it was before too, so the project can go back to before the
  session started).
- The world, as `scene.save` writes it, and a frame of it drawn on the CPU, at most 480 pixels
  wide.

Each is a line in the journal too (`checkpoint 2: crate on the shelf`). Checkpoints share the
journals' room (`work.max_mb`) and go with their session's journal.

```bash
slash-cli work checkpoints            # the timeline: number, time, label, files
slash-cli work compare 2 5            # what changed from checkpoint 2 to 5
slash-cli work restore 3              # the files back as they were at checkpoint 3
slash-cli work video --out work.ivf   # the frames as a video, one a second
```

[`work.checkpoints`](commands.md#workcheckpoints) lists them and
[`work.compare`](commands.md#workcompare) compares two: the files added, removed and changed,
with lines, and the entities as `world.diff` gives them, with both frames when asked
(`image`). `work restore` ([CLI](cli.md#work)) is the CLI's alone, never a command, since it runs only
while no session does, an agent's included: it keeps the files as they are now as a new checkpoint
first, then writes each one the session touched back as it was at the checkpoint (removing those
it made after), and refuses while any session in the project runs. It names the checkpoint's
world file, which [`scene.open`](commands.md#sceneopen) loads.

## Steering

People watching can steer the agent without its vendor's app, from the CLI or the watch page,
both through the session's watch server
([`work.pause`](commands.md#workpause), [`work.resume`](commands.md#workresume),
[`work.note`](commands.md#worknote)):

```bash
slash-cli work pause "Wrong level: use level_2"   # its edits fail with this; reads still work
slash-cli work note "The crate goes left of the door"
slash-cli work resume
slash-cli work plan                               # its plan, ticked as it goes
```

While paused, each command a watcher couldn't run fails with `-32006` and the person's words, so
the agent's next edit tells it why it stopped; it can still read, say what it was doing and keep
its plan. A note rides on the agent's next reply, in `notes`, once. Notes are the person's words
for the agent to weigh, never commands the engine runs. Only watchers can pause, resume and leave
notes, so an agent can't lift a pause itself. Each is a line in the journal, and `work.digest`
says when a session is paused.

The agent keeps its own plan with [`work.plan`](commands.md#workplan): a task, steps it ticks as
they finish and a status (`working`, `blocked` when it needs a person, `done`), which the watch
page, `work.feed`, `work.digest` and `slash-cli work plan` show. A step can name the storyboard
`task/...` it finishes: ticking it hands back the `storyboard.edit` that closes the task. Games
made by `slash-cli init` tell agents to keep a plan, read notes and stop editing when paused.

## Remote viewing

A person away from the machine checks on the work by asking an agent, in any chat on any device:
that agent looks in on the session through the engine, whichever vendor's agent does the work.

- [`work.view`](commands.md#workview) is the session in one call: its world drawn now (an image
  over MCP), its plan, whether it is paused, its last lines and its tests and checks. Once the
  session ends, the last checkpoint's frame and the end of its journal.
- [`work.timeline`](commands.md#worktimeline) is the way there in one picture: the checkpoints'
  frames in rows, numbered, with their labels.
- [`work.report`](commands.md#workreport) writes one HTML page of the run (plan, summary, each
  checkpoint's frame and changes, the feed) for the agent to hand on, as an artifact or a file.
- `work.pause`, `work.resume` and `work.note` take `session` too, so the person steers through
  their agent as from the watch page.

A call naming another live session goes through that session's watch server, as a watcher's:
it runs between the agent's commands, read-only, and goes in no journal. An agent can't steer its
own session that way. Hosts install the forwarder at start (`server::install_work_forwarder`).

`slash-mcp --watch` ([CLI](cli.md#slash-mcp)) is the server for that agent: it offers only these
tools and the reading ones (`work_digest`, `work_feed`, `work_checkpoints`, `work_compare`,
`work_plan`), in a session with only the `read` capability, and names the session in each call,
`--session`'s or the newest live one's. `slash-cli work timeline --out <file.png>` and
`slash-cli work report` do the same from a terminal. `examples/remote-viewing` checks on a yard
from a second agent's session.

## Settings

`project.json`'s [`work`](project-format.md) block turns each part off:

```json
{"work": {"journal": true, "serve": true, "checkpoints": true, "max_mb": 64}}
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
The watch page is `watch_page` in `engine/server/src/watch_page.cpp`, which the server serves when
`ServerOptions::watch_session` names the session. Remote viewing is
`engine/command/src/work_view.cpp`: `work.view`, `work.timeline` (`work::timeline_strip`) and
`work.report` (`work::write_report`), and `forward_elsewhere`, which hands a call naming another
live session to the `work::Forwarder` that `server::install_work_forwarder` sets
(`server::call_watched`). `mcp::Tools::Watch` is `slash-mcp --watch`'s tool set
(`mcp::watch_commands`).
Steering is `Session::steering()`: `Session::call_command` refuses what a watcher couldn't run
while `paused` is set, with `code::kPaused`, and `Session::call` moves the waiting `notes` into
the agent's next reply; `Journal::steer` writes the lines, and `server::call_watched` runs a
command on a live session through its watch server for the CLI.
Checkpoints are `engine/command/src/checkpoints.cpp`: `Journal::before_write` keeps a file's
first contents in the session's `base/` folder before `Session::write_file` or `commit_files`
writes it, `take_checkpoint` saves the world and draws it with `canvas::capture`, and
`Journal::checkpoint` stores the files written since the last one in `<n>/` with a
`checkpoint.json`, so a file's contents at checkpoint n are those of the last checkpoint up to n
that holds it, else `base/`'s. `list_checkpoints`, `compare_checkpoints` and
`restore_checkpoint` read them back.
`debug::WatchView` (`slash/debug/watch_view.hpp`) is the window's UI, controls in a world of their
own drawn with `CanvasOptions::overlay`: it places outlines from the frame's
`canvas::FrameContents` in 2D, and in 3D from `navigation::world_geometry`'s boxes projected
through `scene3d::camera_view`. `examples/work-journal` journals an agent's edits and reads them
back, and `examples/watch-window` follows a session and prints what the window outlines.
