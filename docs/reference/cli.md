# Command-line tools

SlashEngine has three programs, built into `build/<preset>/tools/`:

- **`slash-cli`** runs games, checks scenes and projects, calls commands, serves them over HTTP
  and exports games.
- **`slash-mcp`** serves the engine's commands to MCP clients over stdio.
- **`slash-game`** runs a game project exactly as its export would.

They all print results to stdout as JSON and logs to stderr as JSON lines, one object per line, so
their output can always be parsed. Run either with `--help` (or `slash-cli help`) for the same
usage text as below.

## Exit codes

| Code | Meaning |
| --- | --- |
| `0` | Success |
| `1` | The command ran and failed: an invalid scene or project, a command error, a scene that is not canonical under `--check`, a failed export |
| `2` | Usage error: an unknown subcommand, flag or platform, or a missing value |

## slash-cli

### version

```bash
slash-cli version
```

Prints `{"name":"SlashEngine","version":"0.1.0"}`.

### run

Starts the engine and ticks the world at a fixed rate.

```bash
slash-cli run [options]
```

| Flag | Meaning |
| --- | --- |
| `--headless` | Run with no window or GPU. Requires `--ticks` |
| `--ticks <n>` | Stop after `n` fixed ticks |
| `--hz <rate>` | Fixed tick rate in Hz (default 60) |
| `--vsync <mode>` | `disabled`, `enabled` (default), `adaptive` or `mailbox`; see [project-format.md](project-format.md) |
| `--max-fps <n>` | Frames per second at most, 0 to 1000; `0` (default) means no cap |
| `--scene <file>` | Load a `.scene.json` file before the first tick |
| `--save <file>` | Save the scene to this file after the last tick |
| `--record <file>` | Record the run to a [`.replay.json`](scene-format.md#replays) file inside the project, written when the game stops |
| `--debug-overlay` | Show the [debug panel](#the-debug-overlay) from the start; F3 shows and hides it |
| `--debug-collisions` | Draw every collision shape over the world, as Godot's `--debug-collisions` |
| `--serve` | Serve the running game over HTTP between ticks (see [serve](#serve)) |
| `--port <n>` | Port for `--serve` (default: any free port) |
| `--token <token>` | Bearer token for `--serve` (default: random) |
| `--project <dir>` | Project folder for `--serve` and `--record` (default `.`) |

Without `--headless` it opens a window and runs until the window is closed or `--ticks` is
reached. With no display, set `SDL_VIDEO_DRIVER=dummy` to run the windowed path anyway.

```bash
slash-cli run --headless --ticks 60 --scene tests/data/basic.scene.json
```

```json
{"entities":3,"headless":true,"quit_requested":false,"renderer":"null","ticks":60}
```

`renderer` names the device that drew the window: `metal`, `d3d12` or `vulkan`, or `null` when
headless or when no GPU backend works (a warning in the log says why). The window shows the
scene's [sprites](scene-format.md#built-in-components) over Godot's default gray, drawn between
ticks as the game moves (see [`rendering.interpolation`](project-format.md)). The keyboard,
mouse, touches and gamepads press the [input actions](project-format.md#input-actions) of the
project folder's `project.json`, or only Godot's `ui_*` actions without one. Its
[sound](scene-format.md#audio) plays on the default output device; with none, a warning says so
and the game runs silent. Headless runs mix sound but play it nowhere.

With `--serve`, the result also carries the server's `url`, and the game takes the `play` and
`pause` commands: while paused it keeps serving requests but does not step the world, and
`--ticks` still counts the loop's ticks. It also takes
[`display.list`, `display.set`, `perf.stats` and `debug.overlay`](commands.md#display), which
list the displays, change V-Sync and the frame cap while it runs, report frame pacing and show
the debug overlay.

Every 15 ticks it checks the files the game has loaded and reloads the ones that changed: an
edited prefab updates its instances and keeps their overrides (see
[hot reload](../concepts.md#assets)).

With `--record`, everything the player and any served agent do is recorded from the first tick,
and the result carries the [`replay.stop`](commands.md#replaystop) reply as `replay`.
[`replay.run`](commands.md#replayrun) plays the file back in any session:

```bash
slash-cli run --headless --ticks 90 --scene tests/data/basic.scene.json --record runs/basic.replay.json
slash-cli call replay.run '{"path": "runs/basic.replay.json"}'
```

```json
{"entities":3,"headless":true,"quit_requested":false,"renderer":"null","replay":{"commands":0,"inputs":0,"path":"runs/basic.replay.json","ticks":90},"ticks":90}
{"end":90,"matched":true,"start":0,"tick":90}
```

#### The debug overlay

F3 shows and hides a panel in the window's top left corner, for the person watching the game:

- the frame rate and average frame time, the tick, and whether the game plays or is paused;
- the entity count and what the last frame drew (sprites, tiles, particles, lights, controls
  and collision shapes);
- the mouse's position over the game, with the IDs of the bodies and areas whose shapes are
  under it;
- Pause (Play while paused), Step and Shapes buttons.

The buttons run the `pause`, `play` and `step` commands through the game's session, as an agent
would, so a recording keeps the steps and `undo` takes one back. Shapes draws every
`CollisionShape2D` and `CollisionPolygon2D` over the world as Godot's Visible Collision Shapes
does, whether or not the panel shows. The game never sees F3, or clicks and touches on the
panel. An agent shows the panel and the shapes with
[`debug.overlay`](commands.md#debugoverlay), and sees the shapes itself in
[`frame.capture`](commands.md#framecapture) with `collisions`. Headless runs draw nothing, so
the flags change nothing there.

### scene validate

Checks a scene, a `.prefab.json` file, a [`.frames.json`](scene-format.md#sprite-frames) file, a
[`.material.json`](scene-format.md#materials) file, a [`.anim.json`](scene-format.md#animations)
file, a [`.tileset.json`](scene-format.md#tile-sets) file, a [`.font.json`](scene-format.md#fonts)
file, a [`.theme.json`](scene-format.md#themes) file, an
[`.audio.json`](scene-format.md#audio-streams) file, a
[`.bus_layout.json`](scene-format.md#bus-layouts) file, a
[`.replay.json`](scene-format.md#replays) file or a [`.test.json`](scene-format.md#tests) file
and lists every error, each with its JSON Pointer path. Exit code 1 if the file is invalid. The [prefabs](scene-format.md#prefabs) a file
extends, the tile sets its layers use and the project's window size and theme are read from its
project: the nearest folder above it with a `project.json`, else the current folder.

```bash
slash-cli scene validate <file>
```

```json
{"entities":3,"valid":true}
```

For a prefab the result counts its `components` instead of `entities`, for a `.frames.json` or
`.anim.json` file its `animations`, for a `.tileset.json` file its `tiles`, for a `.theme.json`
file its `types`, for a `.bus_layout.json` file its `buses`, for a `.replay.json` file the
`ticks` it covers, for a `.test.json` file its `tests`, for a `.font.json` or `.audio.json` file nothing more, and for a `.material.json` file the
`instructions` its graph compiles to, of the 64 a material can run:

```json
{"instructions":5,"valid":true}
```

Each [`TileMapLayer`](scene-format.md#tile-map-layers) in a scene or prefab must use a tile set
that loads, and its legend and rows must name tiles there:

```json
{"errors":[{"message":"the tile set has no tile named \"watr\"; did you mean \"water\"?","path":"/entities/map/components/TileMapLayer/legend/w"}],"valid":false}
```

A scene's [controls](scene-format.md#ui-and-text) must lay out as written: each `children`
entry names a control with no other parent and doesn't make a loop, an entity has one
container, theme override names are ones the control's types have, style box overrides are
valid, and themes, theme type variations, fonts and textures load:

```json
{"errors":[{"message":"a control can't be its own child","path":"/entities/body/components/Control/children/0"},{"message":"Label has no colors item \"font_colour\"; did you mean \"font_color\"?","path":"/entities/hint/components/Control/theme_override_colors/font_colour"}],"valid":false}
```

A theme file's types must be control types or variations of them, with the items those types
have:

```json
{"errors":[{"message":"Label has no colors item \"font_colour\"; did you mean \"font_color\"?","path":"/types/Label/colors/font_colour"}],"valid":false}
```

A bus layout's effects must be [types the engine has](scene-format.md#effects), with their
fields, and each bus must send to a bus before it:

```json
{"errors":[{"message":"expected one of \"amplify\", \"low_pass_filter\", ... \"stereo_enhance\", got \"revreb\"; did you mean \"reverb\"?","path":"/buses/1/effects/0/type"}],"valid":false}
```

When the project has a
[storyboard](storyboard-format.md#storyref), each `StoryRef` ID must name one of its entries:

```json
{"errors":[{"message":"unknown storyboard entry \"character/pilto\"; did you mean \"character/pilot\"?","path":"/entities/player/components/StoryRef/ids/0"}],"valid":false}
```

### scene format

Rewrites a scene, prefab, `.frames.json`, `.material.json`, `.anim.json`, `.tileset.json`,
`.font.json`, `.theme.json`, `.audio.json`, `.bus_layout.json`, `.replay.json` or `.test.json` file in [canonical form](scene-format.md#canonical-form).

```bash
slash-cli scene format <file>
slash-cli scene format --check <file>    # change nothing; exit 1 if not canonical
```

### schema

Prints a JSON Schema: for scene or prefab files (generated from the registered components), for
[`.frames.json` files](scene-format.md#sprite-frames), for
[`.material.json` files](scene-format.md#materials), for
[`.anim.json` files](scene-format.md#animations), for
[`.tileset.json` files](scene-format.md#tile-sets), for [`.font.json` files](scene-format.md#fonts),
for [`.theme.json` files](scene-format.md#themes), for
[`.audio.json` files](scene-format.md#audio-streams), for
[`.bus_layout.json` files](scene-format.md#bus-layouts), for
[`.replay.json` files](scene-format.md#replays), for [`.test.json` files](scene-format.md#tests),
for [storyboard files](storyboard-format.md),
for [`project.json`](project-format.md) or for [`export_presets.json`](project-format.md#export-presets).

```bash
slash-cli schema scene
slash-cli schema prefab
slash-cli schema frames
slash-cli schema material
slash-cli schema animation
slash-cli schema tileset
slash-cli schema font
slash-cli schema theme
slash-cli schema audio
slash-cli schema bus_layout
slash-cli schema replay
slash-cli schema test
slash-cli schema storyboard
slash-cli schema project
slash-cli schema presets
```

### project validate

Checks a folder's `project.json`, its `export_presets.json` if there is one, the theme its
`gui.theme` names, the bus layout its `audio.bus_layout` names, and its storyboard with [`storyboard.check`](commands.md#storyboardcheck), and
lists every error with its file and JSON Pointer path. Exit code 1 if any is invalid. Storyboard warnings, such as a link to a file
that does not exist yet, are listed under `warnings` and do not fail. The folder defaults to `.`.

```bash
slash-cli project validate tests/data/game
```

```json
{"valid":true}
```

```json
{"errors":[{"file":"project.json","message":"no file \"ui/gmae.theme.json\"","path":"/gui/theme"}],"valid":false}
```

```json
{"errors":[{"file":"export_presets.json","message":"unknown platform \"wbe\"; did you mean \"web\"?","path":"/presets/web/platform"},{"file":"export_presets.json","message":"secrets are read from environment variables, never from export_presets.json; \"slash-cli schema presets\" lists the variables","path":"/presets/windows-signed/windows/certificate_password"}],"valid":false}
```

### test

Runs the project's [`.test.json` tests](scene-format.md#tests) headless with
[`test.run`](commands.md#testrun) and prints only the checks that failed. Exit code 1 if any
did. Paths are files and folders inside the project; with none, every test file runs, and so
does [`storyboard.check`](commands.md#storyboardcheck).

```bash
slash-cli test [path...] [--project <dir>] [--test <name>]...
```

| Flag | Meaning |
| --- | --- |
| `--project <dir>` | The project folder (default `.`) |
| `--test <name>` | Run only the test with this name; repeat it for more |

```json
{"failed":[],"passed":3,"tests":3}
```

```json
{"failed":[{"file":"tests/main.test.json","message":"expected 1 item, got 0 items: []","path":"/tests/crate_lands/steps/0/expect/entities","test":"crate_lands","tick":600}],"passed":0,"tests":1}
```

### call

Runs one [command](commands.md) and prints its result.

```bash
slash-cli call <command> [params] [options]
```

`params` is a JSON object, or `-` to read it from stdin. Left out, it is `{}`.

| Flag | Meaning |
| --- | --- |
| `--scene <file>` | Open this scene first, or start a new one that will be saved there |
| `--save` | Save the scene after the command succeeds |
| `--project <dir>` | Folder that scene paths are relative to (default `.`) |
| `--allow <list>` | Capabilities: `read`, `edit`, `run`, `filesystem`, `build` or `all` (default `all`) |

```bash
slash-cli call world.query '{"with": ["Transform2D"]}' --scene tests/data/basic.scene.json
```

```json
{"count":1,"entities":["player"],"truncated":false}
```

A failed command prints `{"error": {"code", "message", "data"}}` to stdout and exits with 1.

### rpc

Serves commands as [JSON-RPC 2.0](protocols.md#json-rpc), one message per line on stdin and
stdout, until stdin closes. Takes `--scene`, `--project` and `--allow` as `call` does.

```bash
echo '{"jsonrpc":"2.0","id":1,"method":"entity.get","params":{"id":"player","omit_defaults":true}}' \
    | slash-cli rpc --scene tests/data/basic.scene.json
```

```json
{"id":1,"jsonrpc":"2.0","result":{"components":{"Transform2D":{"position":[64.0,32.0],"rotation":0.5}},"id":"player"}}
```

### serve

Serves commands over [HTTP](protocols.md#http) on this machine until stopped (Ctrl+C), with no
window and no ticking. Use `run --serve` to serve a running game instead. Edited prefab and
asset files reload while it runs.

| Flag | Meaning |
| --- | --- |
| `--scene <file>` | Open this scene first |
| `--port <n>` | Port (default: any free port) |
| `--token <token>` | Bearer token clients must send (default: random) |
| `--project <dir>` | Project folder (default `.`) |
| `--allow <list>` | Capabilities to grant (default `all`) |

It prints where it listens, and writes the same to `.slash/server.json`:

```json
{"endpoint_file":".slash/server.json","host":"127.0.0.1","port":18765,"token":"t0k","url":"http://127.0.0.1:18765"}
```

### init

Sets a game folder up for Claude Code, Codex and Cursor. See
[concepts.md](../concepts.md#agent-setup-files) for what each file is for.

```bash
slash-cli init [dir]
```

| Flag | Meaning |
| --- | --- |
| `--force` | Replace files that already exist (by default they are skipped) |
| `--mcp-command <cmd>` | How agents launch `slash-mcp` (default `slash-mcp`, found on `PATH`) |

```json
{"created":["project.json","AGENTS.md","CLAUDE.md",".mcp.json",".cursor/mcp.json",".codex/config.toml",".claude/skills/slashengine/SKILL.md",".github/workflows/export.yml","scenes/main.scene.json","tests/main.test.json","storyboard/glossary.json","storyboard/work.json",".gitignore"],"skipped":[]}
```

The `project.json` it writes is named after the folder and points at `scenes/main.scene.json`.
The starter [storyboard](storyboard-format.md) records the game's title in the glossary and an
open task to fill in the rest, and `AGENTS.md` and the skill tell agents to call
`storyboard_brief` before a task and `storyboard_check` after it. `tests/main.test.json` is a
starter [test](scene-format.md#tests) that runs the main scene for a second, and `AGENTS.md` and
the skill tell agents to run `test_run` after a change and add tests as they build. `AGENTS.md`
also links the engine's [examples](../../examples/README.md) at the commit the project pins.

`.github/workflows/export.yml` exports the game for every platform on GitHub Actions, each on a
native runner, runs each export headless and uploads the results. It runs
`slash-cli project validate` and `slash-cli test` first, so a change that breaks the storyboard
or a test fails the run. It pins
the engine to the commit `init` ran from (`SLASHENGINE_REF`), or to its version tag when that
commit is unknown.
An existing `.gitignore` is never replaced, even with `--force`: `init` adds the `.slash/` and
`build/` lines if they are missing and lists the file under `updated`.

### import

Converts a game made with Godot 4 into a SlashEngine project: its scenes, sprite frames, input
map and settings, with each script turned into a storyboard task to port it to C++. It runs
[`project.import`](commands.md#projectimport) and prints the same report;
[godot-import.md](godot-import.md) says what converts to what.

```bash
slash-cli import <folder> [options]
```

| Flag | Meaning |
| --- | --- |
| `--project <dir>` | The SlashEngine project to import into (default `.`); it needn't exist yet |
| `--dry-run` | Print the report and the files it would write; write nothing |

`<folder>` holds the Godot project's `project.godot`. A folder inside the project stays where it
is: the converted files are written beside the originals and use their textures, sounds and
fonts. A folder anywhere else is copied in, its assets and scripts keeping the paths they had
under `res://`. Either way `project.json`, `storyboard/work.json` and the full report,
`import/godot.json`, are written at the top of the project; an existing `project.json` keeps
its other settings, and importing again keeps each task's status.

```bash
slash-cli import ~/games/coin-run-godot --project coin-run
```

```json
{"converted":{"assets":5,"input_actions":3,"scenes":2,"sprite_frames":2},"engine":"godot","files":15,"main_scene":"main.scene.json","project":"Coin Run","report":"import/godot.json","scripts":[{"entities":0,"script":"game_state.gd","task":"task/port-game-state"},{"entities":1,"script":"main.gd","task":"task/port-main"},{"entities":2,"script":"player.gd","task":"task/port-player"}],"unsupported":[{"count":1,"what":"autoload","where":"project.godot: GameState","why":"autoloads are gameplay code; see the storyboard task"},{"count":1,"what":"file type .svg","where":"icon.svg","why":"textures are PNG or BMP files; save it as PNG"},{"count":1,"what":"node type Timer","where":"main.tscn: Timer","why":"timers are gameplay code: count the ticks in a C++ system"}]}
```

It exits 1 if the folder isn't a Godot 4 project.

### export

Builds a game project for one platform, ready to give to players.

```bash
slash-cli export <preset> [options]
```

`<preset>` is a preset from the project's [`export_presets.json`](project-format.md#export-presets)
or a platform for its defaults: `macos`, `windows`, `linux`, `web`, `android` or `ios`. The
[`export.run`](commands.md#exportrun) command does the same over MCP, JSON-RPC and REST.

| Flag | Meaning |
| --- | --- |
| `--project <dir>` | Game folder with `project.json` (default `.`) |
| `--out <dir>` | Output folder (default: the preset's `output`, else `<project>/build/export/<preset>`) |
| `--debug` | Debug build, even if the preset says Release |
| `--test` | Run the exported game headless afterwards, if this machine can |
| `--engine <dir>` | Engine sources (default: `SLASH_ENGINE_DIR`, an installed copy, or the checkout `slash-cli` was built from) |

Export validates the project, its presets and every scene and prefab, copies `project.json` and
the `.scene.json` and `.prefab.json` files into a staging folder (nothing else, so stray files such as `.env` never
ship), builds the game with CMake, signs it and installs it into the output folder. A Mac builds every
platform: macOS and iOS with Xcode's command-line tools and SDKs, and Windows and Linux with the
toolchains in [toolchain](#toolchain), which the first such export downloads. A Linux machine
builds Linux and Windows the same way, and a Windows machine builds Windows with MSVC. Web
exports need Emscripten (`EMSDK` set) on any host, and Android exports need the Android SDK
(build tools 35 or newer and the Android 36 platform), NDK r27 or newer and JDK 17 or newer on
any host. An output folder that a previous export did not make is never replaced. Missing signing
tools, identities, profiles or secrets are reported before anything is built; signing a Windows
export needs signtool, so only a Windows machine does it.

| Platform | Output |
| --- | --- |
| `macos` | A universal app bundle (Apple silicon and Intel), always signed: ad hoc unless the preset names a `codesign_identity` |
| `windows` | An executable plus a `data` folder, with the C++ runtime linked in |
| `linux` | An executable plus a `data` folder, for x86_64 or arm64 (`linux.arch`). Built against glibc 2.31 and GCC 10's libstdc++, so it runs on Ubuntu 20.04, Debian 11 and newer. It uses X11, which Wayland desktops run through XWayland |
| `web` | `index.html` plus the script, WebAssembly and data files |
| `android` | `<binary>.apk` for arm64-v8a and x86_64, signed with the release keystore from the environment or else the debug keystore |
| `ios` | For the Simulator, `<binary>.app` (Apple silicon and Intel), signed ad hoc. For devices, `<binary>.ipa` (arm64), signed with the team's identity and provisioning profile |

```bash
slash-cli export linux --project my-game --test
```

```json
{"bytes":5335403,"executable":"my-game/build/export/linux/mygame","linux":{"arch":"x86_64"},"log":"my-game/.slash/export/linux/export.log","output":"my-game/build/export/linux","platform":"linux","preset":"linux","seconds":66.6,"test":{"entities":0,"passed":true,"ticks":60}}
```

A signed export adds `signed`: `{"identity", "notarized"}` on macOS,
`{"certificate", "timestamped"}` on Windows, `{"keystore", "release"}` on Android or
`{"identity", "profile"}` on iOS. Linux exports also return `linux`: the `arch`; their `--test`
runs only on a Linux machine of that architecture. Android exports also return `android`: the
`package`, `version_code` and `abis`. Their `--test` installs the APK on the connected device or emulator
(`ANDROID_SERIAL` picks one), runs it headless and reads its summary from logcat; with no device
connected the test is skipped and says so. iOS exports return `ios`: the `bundle_id` and
`destination`. Their `--test` installs a Simulator build on a booted iPhone Simulator, else boots
the one with the newest iOS and shuts it down afterwards, runs the game headless and reads its
summary; device builds and Macs with no Simulator skip the test and say so.

The full build log is at `log`. A failure names the step that failed (`options`, `validate`,
`toolchain`, `stage`, `configure`, `build`, `install`, `package`, `sign`, `notarize` or `publish`; Android
build failures also name the `abi`) and ends with the last lines of
the log in `tail`, so the log rarely needs opening. A step of `options` means the request itself
was wrong, and exits with 2:

```json
{"error":{"message":"no preset or platform called \"web-relase\"; did you mean \"web-release\"?","step":"options"}}
```

### toolchain

```bash
slash-cli toolchain list
slash-cli toolchain install [name|platform ...]
```

Exports to Windows and Linux from a Mac or a Linux machine build with open-source toolchains
that `slash-cli` downloads, pinned to exact versions and SHA-256 hashes:

| Toolchain | Used for | License |
| --- | --- | --- |
| `llvm-mingw` | Clang and LLD; mingw-w64 for Windows exports | Apache 2.0 with LLVM exception, ZPL, MIT and public domain |
| `linux-sysroot-x86_64`, `linux-sysroot-arm64` | Debian 11's glibc 2.31, GCC 10's libstdc++ and the X11, ALSA and PulseAudio headers that Linux exports build against | Debian's: glibc under the LGPL and libstdc++ under the GPL with the GCC Runtime Library Exception, both used from the player's system; X11 under MIT |

An export downloads what it needs the first time, so `toolchain install` is only needed to get
them ahead of time, for example before going offline. With no names it installs every toolchain
for this machine; a name can also be a platform (`windows`, `linux`) or `linux-arm64`. Each
download is checked against its pinned hash and deleted if it differs. They are installed in
`SLASH_TOOLCHAINS_DIR`, else `~/.slash/toolchains`; delete a folder there to remove one.

```json
{"dir":"/Users/me/.slash/toolchains","toolchains":[{"description":"Clang and LLD with mingw-w64, for Windows and Linux exports","installed":true,"name":"llvm-mingw","version":"20260922"},{"description":"Debian 11 sysroot (glibc 2.31) for x86_64 Linux exports","installed":false,"name":"linux-sysroot-x86_64","version":"bullseye"},{"description":"Debian 11 sysroot (glibc 2.31) for arm64 Linux exports","installed":false,"name":"linux-sysroot-arm64","version":"bullseye"}]}
```

`install` returns the `installed` toolchains and those `already_installed`, each with its `path`.
Windows machines have no toolchains to install.

## slash-mcp

Serves SlashEngine to MCP clients over stdio. MCP clients start it themselves; you rarely run it
by hand.

```bash
slash-mcp [options]
```

| Flag | Meaning |
| --- | --- |
| `--project <dir>` | Project folder; scene paths are relative to it (default `.`) |
| `--scene <file>` | Open this scene at startup, or start a new one saved there |
| `--allow <list>` | Capabilities to grant: `read`, `edit`, `run`, `filesystem`, `build` or `all` (default `all`), e.g. `--allow read,run` |
| `--live` | Drive the game already running in the project (`slash-cli run --serve` or `slash-cli serve`) instead of an engine of its own |
| `--version` | Print the engine version |
| `--help` | Show the usage text |

With `--live`, `slash-mcp` reads `.slash/server.json` for each message and forwards it to that
server, so it reconnects on its own when the game restarts. If no game is running, each request
gets an error saying so.

### Client configs

`slash-cli init` writes these for you. To add the server by hand:

Claude Code:

```bash
claude mcp add slashengine -- slash-mcp --project .
claude mcp add slashengine-live -- slash-mcp --live --project .
```

Codex (`.codex/config.toml` in a trusted project, or `~/.codex/config.toml`):

```toml
[mcp_servers.slashengine]
command = "slash-mcp"
args = ["--project", ".", "--scene", "scenes/main.scene.json"]
```

Cursor (`.cursor/mcp.json`):

```json
{ "mcpServers": { "slashengine": { "command": "slash-mcp", "args": ["--project", "${workspaceFolder}"] } } }
```

## slash-game

Runs a game project the same way an exported game runs. Every export is this program built for
its platform, with the project's data next to it.

```bash
slash-game [options]
```

| Flag | Meaning |
| --- | --- |
| `--headless` | Run with no window or GPU (needs `--ticks`) |
| `--ticks <n>` | Stop after `n` fixed ticks and print a JSON summary |
| `--data <dir>` | Folder with `project.json` (default: the exported data folder) |
| `--help` | Show the usage text |

```bash
slash-game --data tests/data/game --headless --ticks 60
```

```json
{"entities":3,"headless":true,"name":"Test Game","quit_requested":false,"renderer":"null","scene":"scenes/main.scene.json","sprites":0,"ticks":60}
```

`sprites` counts the sprites the last frame drew. Headless and on the null device nothing is
drawn, but the sprites are still checked and counted, so a test can see that they would draw.

## Environment variables

| Variable | Effect |
| --- | --- |
| `SDL_VIDEO_DRIVER` | SDL3's video driver. `dummy` runs the windowed path with no display; on Linux, `offscreen` does too and still draws on the GPU |
| `SDL_GPU_DRIVER` | SDL3's GPU backend (`vulkan`, `metal` or `direct3d12`), overriding the project's `rendering.driver` |
| `SLASH_ENGINE_DIR` | Engine sources `slash-cli export` builds against |
| `EMSDK` | The Emscripten SDK, needed for web exports |
| `SLASH_TOOLCHAINS_DIR` | Where `slash-cli toolchain` installs cross toolchains (default `~/.slash/toolchains`) |
| `SLASH_APPLE_API_KEY_PATH`, `SLASH_APPLE_API_KEY_ID`, `SLASH_APPLE_API_ISSUER` | An App Store Connect API key (the `.p8` file, its ID and issuer) for notarizing macOS exports |
| `SLASH_WINDOWS_CERTIFICATE_PATH`, `SLASH_WINDOWS_CERTIFICATE_PASSWORD` | A `.pfx` code-signing certificate and its password for signing Windows exports |
| `SLASH_ANDROID_KEYSTORE_PATH`, `SLASH_ANDROID_KEYSTORE_PASSWORD`, `SLASH_ANDROID_KEY_ALIAS`, `SLASH_ANDROID_KEY_PASSWORD` | The keystore, its password, the key's alias and the key's password (default: the keystore's) for signing release Android exports |
| `SLASH_IOS_PROVISIONING_PROFILE` | The `.mobileprovision` file iOS device exports embed (default: the newest installed one for the team and bundle ID) |
| `ANDROID_HOME`, `ANDROID_NDK_HOME`, `JAVA_HOME` | Where Android exports find the SDK, the NDK and the JDK when they aren't in Android Studio's default places |
| `ANDROID_USER_HOME` | The folder holding `debug.keystore` (default `~/.android`) |
| `ANDROID_SERIAL` | The device or emulator `export --test` uses when several are connected |
