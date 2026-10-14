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
| `--scene <file>` | Load a `.scene.json` file before the first tick |
| `--save <file>` | Save the scene to this file after the last tick |
| `--serve` | Serve the running game over HTTP between ticks (see [serve](#serve)) |
| `--port <n>` | Port for `--serve` (default: any free port) |
| `--token <token>` | Bearer token for `--serve` (default: random) |
| `--project <dir>` | Project folder for `--serve` (default `.`) |

Without `--headless` it opens a window and runs until the window is closed or `--ticks` is
reached. With no display, set `SDL_VIDEO_DRIVER=dummy` to run the windowed path anyway.

```bash
slash-cli run --headless --ticks 60 --scene tests/data/basic.scene.json
```

```json
{"entities":3,"headless":true,"quit_requested":false,"ticks":60}
```

With `--serve`, the result also carries the server's `url`.

### scene validate

Checks a scene against the scene schema and lists every error, each with its JSON Pointer path.
Exit code 1 if the scene is invalid.

```bash
slash-cli scene validate <file>
```

```json
{"entities":3,"valid":true}
```

### scene format

Rewrites a scene in [canonical form](scene-format.md#canonical-form).

```bash
slash-cli scene format <file>
slash-cli scene format --check <file>    # change nothing; exit 1 if not canonical
```

### schema

Prints a JSON Schema: for scene files (generated from the registered components), for
[`project.json`](project-format.md) or for
[`export_presets.json`](project-format.md#export-presets).

```bash
slash-cli schema scene
slash-cli schema project
slash-cli schema presets
```

### project validate

Checks a folder's `project.json` and, if there is one, its `export_presets.json`, and lists every
error with its file and JSON Pointer path. Exit code 1 if either is invalid. The folder defaults
to `.`.

```bash
slash-cli project validate tests/data/game
```

```json
{"valid":true}
```

```json
{"errors":[{"file":"export_presets.json","message":"unknown platform \"wbe\"; did you mean \"web\"?","path":"/presets/web/platform"},{"file":"export_presets.json","message":"secrets are read from environment variables, never from export_presets.json; \"slash-cli schema presets\" lists the variables","path":"/presets/windows-signed/windows/certificate_password"}],"valid":false}
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
window and no ticking. Use `run --serve` to serve a running game instead.

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
{"created":["project.json","AGENTS.md","CLAUDE.md",".mcp.json",".cursor/mcp.json",".codex/config.toml",".claude/skills/slashengine/SKILL.md",".github/workflows/export.yml","scenes/main.scene.json",".gitignore"],"skipped":[]}
```

The `project.json` it writes is named after the folder and points at `scenes/main.scene.json`.
`.github/workflows/export.yml` exports the game for every platform on GitHub Actions, each on a
native runner, runs each export headless and uploads the results. It pins the engine to the commit
`init` ran from (`SLASHENGINE_REF`), or to its version tag when that commit is unknown.
An existing `.gitignore` is never replaced, even with `--force`: `init` adds the `.slash/` and
`build/` lines if they are missing and lists the file under `updated`.

### export

Builds a game project for one platform, ready to give to players.

```bash
slash-cli export <preset> [options]
```

`<preset>` is a preset from the project's [`export_presets.json`](project-format.md#export-presets)
or a platform for its defaults: `macos`, `windows`, `linux` or `web`. iOS and Android are planned
(see the [roadmap](../roadmap.md)). The [`export.run`](commands.md#exportrun) command does the
same over MCP, JSON-RPC and REST.

| Flag | Meaning |
| --- | --- |
| `--project <dir>` | Game folder with `project.json` (default `.`) |
| `--out <dir>` | Output folder (default: the preset's `output`, else `<project>/build/export/<preset>`) |
| `--debug` | Debug build, even if the preset says Release |
| `--test` | Run the exported game headless afterwards, if this machine can |
| `--engine <dir>` | Engine sources (default: `SLASH_ENGINE_DIR`, an installed copy, or the checkout `slash-cli` was built from) |

Export validates the project, its presets and every scene, copies `project.json` and the
`.scene.json` files into a staging folder (nothing else, so stray files such as `.env` never
ship), builds the game with CMake, signs it and installs it into the output folder. Each host
builds its own platform; web exports need Emscripten (`EMSDK` set) on any host. An output folder
that a previous export did not make is never replaced. Missing signing tools or secrets are
reported before anything is built.

| Platform | Output |
| --- | --- |
| `macos` | A universal app bundle (Apple silicon and Intel), always signed: ad hoc unless the preset names a `codesign_identity` |
| `windows`, `linux` | An executable plus a `data` folder |
| `web` | `index.html` plus the script, WebAssembly and data files |

```bash
slash-cli export linux --project my-game --test
```

```json
{"bytes":5335403,"executable":"my-game/build/export/linux/mygame","log":"my-game/.slash/export/linux/export.log","output":"my-game/build/export/linux","platform":"linux","preset":"linux","seconds":66.6,"test":{"entities":0,"passed":true,"ticks":60}}
```

A signed export adds `signed`: `{"identity", "notarized"}` on macOS or
`{"certificate", "timestamped"}` on Windows.

The full build log is at `log`. A failure names the step that failed (`options`, `validate`,
`stage`, `configure`, `build`, `install`, `sign`, `notarize` or `publish`) and ends with the last lines of
the log in `tail`, so the log rarely needs opening. A step of `options` means the request itself
was wrong, and exits with 2:

```json
{"error":{"message":"no preset or platform called \"web-relase\"; did you mean \"web-release\"?","step":"options"}}
```

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
{"entities":3,"headless":true,"name":"Test Game","quit_requested":false,"scene":"scenes/main.scene.json","ticks":60}
```

## Environment variables

| Variable | Effect |
| --- | --- |
| `SDL_VIDEO_DRIVER` | SDL3's video driver. `dummy` runs the windowed path with no display |
| `SLASH_ENGINE_DIR` | Engine sources `slash-cli export` builds against |
| `EMSDK` | The Emscripten SDK, needed for web exports |
| `SLASH_APPLE_API_KEY_PATH`, `SLASH_APPLE_API_KEY_ID`, `SLASH_APPLE_API_ISSUER` | An App Store Connect API key (the `.p8` file, its ID and issuer) for notarizing macOS exports |
| `SLASH_WINDOWS_CERTIFICATE_PATH`, `SLASH_WINDOWS_CERTIFICATE_PASSWORD` | A `.pfx` code-signing certificate and its password for signing Windows exports |
