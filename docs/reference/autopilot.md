# Autopilot playtests

[Autopilot](https://github.com/ramenvr/autopilot) (Apache 2.0, by Ramen, the makers of
[Aura](https://www.tryaura.dev/autopilot/)) is an agent that plays a game to explore it for
bugs, check a claim about it or reach a goal in it, then writes a report. Its agent service runs
on the developer's machine and engine plugins connect out to it over a WebSocket; Autopilot ships
plugins for Unreal, Unity and Godot. SlashEngine's is built in: `slash::autopilot::Bridge`
(`engine/autopilot`), a port of the Godot addon's game side
(`plugin/godot/addons/autopilot`), which `slash-cli run` turns on with `--autopilot`.

The bridge says it is a Godot game (`"engine": "godot"` in its hello). SlashEngine's scenes
follow Godot's, so Autopilot plays a SlashEngine game with what it knows of Godot games, and needs
no change on its side. Where Godot has nodes, the bridge has entities: object paths are entity
IDs, `rc` reads and writes components, and the codex is the project's own files.

Nothing in SlashEngine depends on Autopilot: the bridge is SlashEngine's own code speaking
Autopilot's protocol, and a game that never connects is unchanged. SlashEngine also has its own
agent, which plays through the same bridge with a model from `providers.json` and needs no
Autopilot install or account: see [SlashEngine's own agent](#slashengines-own-agent).

## Running it

Install Autopilot as its [docs](https://www.tryaura.dev/documentation/autopilot/) say, signed in
with an Aura account or with your own model keys (BYOK), and start its agent service. Then:

```bash
slash-cli run --project path/to/game --autopilot-run verify \
    --autopilot-prompt "The door opens when the player has the key"
```

| Flag | Meaning |
| --- | --- |
| `--autopilot` | Connect to the agent service and serve it, with no run of our own: Autopilot's own tools (its editor dock, `autopilot mcp` for Claude Code or Codex) start runs |
| `--autopilot-run <kind>` | Ask it for a run when it welcomes the game, then exit with the run's code: `verify` (a claim), `goal` (a goal to reach), `explore` (an exploration, with an optional brief) or `auto` (a prompt it sorts itself) |
| `--autopilot-prompt <text>` | The claim, goal, brief or prompt. `verify` and `goal` need one |
| `--autopilot-minutes <n>` | How long an `explore` run plays (`minutes`), or any other run's `time_limit_minutes` |
| `--autopilot-url <url>` | The agent service's address (below) |
| `--autopilot-token <token>` | A bearer token to send (default: `AUTOPILOT_PLAYTEST_TOKEN`) |
| `--autopilot-report-dir <dir>` | Where `result.json` and the report's files go (default `.slash/autopilot/<UTC time>/` in the project) |
| `--autopilot-connect-timeout <s>` | Seconds to wait for the agent service to take the run (default 120) |
| `--autopilot-stay-open` | Keep playing and connected after the result, or after a failed connection |
| `--autopilot-share` | Share the run's data with Autopilot's developers (default: Autopilot's `preferences.json` `share_data`, else off) |
| `--autopilot-no-media` | Send no pictures of the game (`record` and `capture` fail) |

Any `--autopilot-*` flag turns on `--autopilot`. Ticks keep to real time while it is on, headless
too, as a player plays at the game's speed, so `--headless` needs no `--ticks`. The run's result
ends the game, and the exit code is the run's:

| Code | Meaning |
| --- | --- |
| 0 | Confirmed, achieved, or explored without bugs |
| 1 | An error: the agent service couldn't be reached in time, refused the game or went away before the result |
| 2 | Denied, or bugs found |
| 3 | Inconclusive |

A result's own `exit_code` wins, else its `verdict` (`CONFIRMED` 0, `DENIED` 2, anything else 3),
else 2 when it lists `bugs`. The JSON `slash-cli run` prints carries `autopilot`: the
`exit_code`, the `report_dir` and the agent service's `result`.

The address is, in order: `--autopilot-url`, `AUTOPILOT_PLAYTEST_URL`, the one a running
`autopilot serve` announces in `brain.json` (or the Aura app in `aura-brain.json`) in Autopilot's
settings folder (`%LOCALAPPDATA%\Autopilot`, `~/Library/Application Support/Autopilot` or
`$XDG_CONFIG_HOME/Autopilot`), and else `ws://127.0.0.1:8765/v1/bridge`. Only addresses on this
machine are taken from those files. It is looked up again at each try, and a connection that
fails or closes is tried again after 1 second, doubling to 15.

## SlashEngine's own agent

Autopilot plays with a hosted decision model that its service bills through an Aura plan.
`--autopilot-agent <role>` plays the run with SlashEngine's own agent instead, on a role or
provider from [providers.json](providers.md): Claude, GPT or Gemini with your own key, or a local
model through Ollama, llama.cpp or LM Studio with none.

```bash
slash-cli run --project path/to/game --headless --autopilot-run verify \
    --autopilot-prompt "The door opens when the player has the key" --autopilot-agent playtester
```

```json
{"providers": {"hosted": {"type": "anthropic", "model": "<model id>",
                          "api_key_env": "ANTHROPIC_API_KEY"},
               "local": {"type": "openai", "model": "llama3.2", "url": "http://localhost:11434/v1"}},
 "roles": {"playtester": {"provider": "hosted"}}}
```

The agent (`slash::autopilot::agent_connector`, `engine/autopilot/src/agent.cpp`) is an agent
service inside the game: it talks to the bridge over an in-process connection, so it asks for
everything Autopilot asks for, and the detectors, report files and exit codes are the same. Its
instructions, request kinds and rules of play follow Autopilot's agent session
(`autopilot/agents/session.py`), cut to a model that plays through tools itself where Autopilot's
writes a harness for its decision model.

- The game is paused while the model thinks and runs only inside the tools that take time, so a
  slow model plays as well as a fast one. Time is game time (`autopilot::game_clock`): keys held
  for seconds, waits and the play budget count only play, and headless runs tick as fast as they
  can.
- The first turn's brief holds the request, the game's `context`, the text and controls on
  screen, the codex's `summary.txt` and the notes earlier runs saved in
  `.slash/autopilot/notes.md`.
- Each turn's results end with what the detectors found since the last turn and the turns and
  seconds of play left. A turn with no tool calls is asked to go on; the last turn, and a turn
  once the play time, the turns or the tokens are spent, may only report. A run that ends without
  a report is `INCONCLUSIVE` (exit code 3), and a model that fails ends it with exit code 1.
- `rc` writes and calls need a reason, and the report lists them as direct changes to the game.

| Flag | Meaning |
| --- | --- |
| `--autopilot-agent <role>` | Play `--autopilot-run`'s run with the built-in agent on this role or provider from providers.json |
| `--autopilot-turns <n>` | The most turns (model replies), 60 by default |
| `--autopilot-minutes <n>` | Seconds of play, as for Autopilot (default 10 minutes) |
| `--autopilot-no-media` | Send the model no pictures |

| Tool | What it does (the op it asks for) |
| --- | --- |
| `look` | The player's pose, the text and controls on screen, and in 3D what the camera sees (`pose`, `screen`, `view`) |
| `context`, `find`, `transforms`, `ui` | As the ops of those names |
| `key`, `action` | A tap, a hold for `seconds` of play while the game runs, or a release, of a key by Unreal's name or an input action (`key`, `input`) |
| `move` | Walks the player along a kit direction for `seconds` (`move`) |
| `wait`, `watch` | Plays for `seconds`, and says what changed on the named entities (`watch`) |
| `ui_act` | Works a control (`ui_act`) |
| `screenshot` | A JPEG of the game, sent to a model that takes images (`capture`) |
| `files`, `read_files` | The project's text files, by project path (`codex_manifest`, `codex_read`) |
| `rc` | Reads, or with a reason writes, components and calls commands (`rc`) |
| `restart` | Starts the scene, or another, over (`session`) |
| `note` | Adds a line to `.slash/autopilot/notes.md` for later runs |
| `report` | Ends the run: `outcome` (`CONFIRMED`, `DENIED`, `INCONCLUSIVE`, `achieved`, `not_achieved` or `explored`), `summary`, `evidence` and `findings` |

The exit code is 0 for `CONFIRMED`, `achieved` and `explored` with no findings, 2 for `DENIED`,
`not_achieved` and `explored` with findings, and 3 for `INCONCLUSIVE`. `result.json` adds
`outcome`, `detector_findings`, `direct_changes` and `agent` (the model, turns, seconds of play
and tokens), and `report.md` is written from the report. In C++:

```cpp
#include "slash/autopilot/agent.hpp"

slash::autopilot::AgentOptions agent;
agent.provider = slash::ai::open(config, "playtester").value().provider;
agent.project_root = session.options().project_root;
agent.wait = true; // headless: wait for each reply, so runs repeat
slash::autopilot::Options options;
options.run = json::parse(R"({"kind": "goal", "goal": "Reach the flag"})").value();
options.clock = slash::autopilot::game_clock(session.world(), 1.0 / 60.0);
slash::autopilot::Bridge bridge(session, slash::autopilot::agent_connector(agent), options);
while (!bridge.finished()) {
    bridge.poll(1.0 / 60.0);
    if (session.playing()) {
        session.world().step(1.0 / 60.0);
    }
}
```

[examples/playtest-agent](../../examples/playtest-agent/) plays a run with a stand-in model.

## What the agent service asks

Each request is answered with the same names and shapes as the Godot addon's, positions and sizes
in Autopilot's "kit" frame, as the addon's are: Unreal's axes and centimeters, so in 3D
`(X, Y, Z) = (-z, x, y) * 100` and in 2D `(X, Y, Z) = (x, 0, -y)` in pixels.

| Op | What it does |
| --- | --- |
| `context` | The scene, the player (the body the current camera hangs from, else an entity named `player`, else the first character body), its camera, `2d` or `3d`, whether the game is paused, its game time and the controls `project.json` binds, by Unreal's key names |
| `session` | `status`, or `restart` the launch scene or a scene `map` by path or name; the answer waits two frames, until it runs |
| `pause` | Pauses or plays the world (`paused`) |
| `key` | Presses (`press`, a tap of 0.05 s and two frames), holds (`hold`, renewed within `lease` seconds, 1 by default, or for `seconds` of game time) or releases a key by Unreal's name (`D`, `SpaceBar`, `LeftMouseButton`, `MouseX`, `Gamepad_LeftX`, `Gamepad_FaceButton_Bottom`) as input events; `release_all` lets go of everything |
| `input` | Presses, holds or releases an input action by name, with a strength |
| `move` | Walks the player for `seconds` along a kit `direction`, relative to the camera in 3D, by the actions W, S, A and D are bound to (else the arrows); answers how far it went |
| `view` | What the 3D camera sees: things in front of it with their distance, bearing and size, nearest first |
| `find` | Entities by `name` or `class` (a component), with where they are |
| `transforms` | The places, turns and sizes of the entities named in `objects` |
| `watch` | Watches `objects` for `seconds` and answers what changed when: moved, turned, changed size, parts shown or hidden, lights on or off, animations started or ended |
| `pose` | The player's position, feet, eye, view direction, velocity and whether it is on the ground |
| `movement` | The player's tuning: walk speed, jump, gravity and size, from its components |
| `screen` | The text the window shows, by `<layer>.<entity ID>` (a `CanvasLayer`'s ID, else `Screen`); with `ui`, the controls too |
| `ui` | The controls on screen (buttons, check boxes, option buttons, sliders, text fields) with their state and where their middle is in the window |
| `ui_act` | Works a control by ID: `click` (the mouse, else the button pressed as its signal would be; an option button's next option or `option`), `right_click`, `double_click`, `set` (a slider's `value`, a text field's `text`, a toggle) or `drag` to another control |
| `rc` | Unreal Remote Control's calls: `property` reads or writes a component or a field (`objectPath` an entity ID, `propertyName` `Transform2D/position/x`), `call` runs a command (`functionName`, `parameters`, the entity as `id`; `Session` for commands on no entity), `describe` lists an entity's fields and the commands that take it |
| `record` | Starts or stops sending frames (`fps`, `max_dim`, `quality`) as JPEG pictures |
| `capture` | One JPEG picture of the frame |
| `detectors` | Turns the detectors on or off and sets their limits |
| `codex_manifest`, `codex_read` | The project's files (below) |
| `timestep`, `ping` | The tick rate, and the game and wall time |

Pictures are drawn on the software render device (`canvas::capture`), so headless games have them
too, the same on every machine. Answers that wait on frames (taps, moves, watches, restarts,
clicks) answer once they're done; the rest answer before the tick.

## Detectors

The bridge reports findings by itself, as the Godot addon's detectors do, each the 1st, 10th,
100th and 1000th time it happens:

| Detector | Finds |
| --- | --- |
| `log_error` | An error logged by the engine or the game (warnings are sent as log events) |
| `nan` | An entity whose transform holds a NaN or an infinity |
| `out_of_world` | A 3D body more than `out_of_world_margin` (50) meters below the lowest mesh when the scene started |
| `long_fall` | The player falling for more than `long_fall_seconds` (8) |
| `hitch` | A frame longer than `hitch_seconds` (0.75), after the first 3 seconds of a scene |
| `hud_fraction` | Text on screen such as `4/3`, a count over its stated maximum |

## The codex

Autopilot reads a game's code and files to plan its play. The agent service can't read this
machine's disk, so it asks the game: `codex_manifest` lists the project's text files (scenes,
prefabs and other JSON resources, gameplay sources and shaders, translations, notes; not `build/`,
`export/`, `node_modules/` or dot folders) with their sizes and SHA-256 hashes, and `codex_read`
sends the ones it asks for, up to 4 MB a call. Each is served under `.godot/autopilot_codex/`,
where Autopilot's Godot adapter looks for files a plugin makes, with `summary.txt` first: the
project in brief, its input actions and each scene's and prefab's entities and components. Paths
that leave the project, or name a folder left out, are reported `missing`.

## In C++

```cpp
#include "slash/autopilot/bridge.hpp"

slash::autopilot::Options options;
options.run = json::parse(R"({"kind": "explore", "minutes": 10})").value();
slash::autopilot::Bridge bridge(session, slash::autopilot::websocket_connector(), options);
while (!bridge.finished()) {
    bridge.poll(1.0 / 60.0); // before each tick
    session.world().step(1.0 / 60.0);
}
return bridge.exit_code();
```

A `Connector` makes each connection; tests and
[examples/autopilot-playtest](../../examples/autopilot-playtest/) give one of their own, a
scripted agent service on an in-process `Transport`, and `Options::clock` a clock of their own.
