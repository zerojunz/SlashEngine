# Command-line tools

SlashEngine has two programs, built into `build/<preset>/tools/`:

- **`slash-cli`** runs games, checks scenes, calls commands and serves them over HTTP.
- **`slash-mcp`** serves the engine's commands to MCP clients over stdio.

Both print results to stdout as JSON and logs to stderr as JSON lines, one object per line, so
their output can always be parsed. Run either with `--help` (or `slash-cli help`) for the same
usage text as below.

## Exit codes

| Code | Meaning |
| --- | --- |
| `0` | Success |
| `1` | The command ran and failed: an invalid scene, a command error, a scene that is not canonical under `--check` |
| `2` | Usage error: an unknown subcommand or flag, or a missing value. The usage text goes to stderr |

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

### schema scene

Prints the JSON Schema for scene files, generated from the registered components.

```bash
slash-cli schema scene
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
| `--allow <list>` | Capabilities: `read`, `edit`, `run`, `filesystem` or `all` (default `all`) |

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
{"created":["AGENTS.md","CLAUDE.md",".mcp.json",".cursor/mcp.json",".codex/config.toml",".claude/skills/slashengine/SKILL.md","scenes/main.scene.json",".gitignore"],"skipped":[]}
```

An existing `.gitignore` is never replaced, even with `--force`: `init` adds a `.slash/` line if
it is missing and lists the file under `updated`.

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
| `--allow <list>` | Capabilities to grant: `read`, `edit`, `run`, `filesystem` or `all` (default `all`), e.g. `--allow read,run` |
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

## Environment variables

| Variable | Effect |
| --- | --- |
| `SDL_VIDEO_DRIVER` | SDL3's video driver. `dummy` runs the windowed path with no display |
