# Protocols

The engine's [commands](commands.md) are served four ways: MCP, JSON-RPC 2.0, a REST mirror over
HTTP and the command line. All four are generated from one command registry and run through the
same session, so params, results and errors are identical; only the envelope differs. This page
covers the envelopes, the HTTP server and the error codes.

Status: current · Updated: 2026-11-20

## Choosing one

| If you are | Use |
| --- | --- |
| An MCP client (Claude Code, Codex, Cursor and others) | MCP over stdio with `slash-mcp`, or `slash-mcp --live` for a running game |
| A client that speaks MCP over HTTP | `POST /mcp` on a running server |
| A tool or script that keeps one engine open | JSON-RPC over stdio with `slash-cli rpc`, or `POST /rpc` |
| Anything that speaks HTTP and JSON | The REST mirror, `POST /v1/commands/<name>` |
| A shell script | `slash-cli call` (see [cli.md](cli.md#call)) |

## MCP

SlashEngine is an MCP server speaking protocol versions `2025-11-25`, `2025-06-18`, `2025-03-26`
and `2024-11-05`. It answers `initialize` with the newest version the client also speaks.

**Tools.** An agent starts with the commands most tasks need as tools (`mcp::core_commands`:
scenes, entities, queries, `batch`, undo, stepping, input, frame checks, tests, building gameplay
code, the storyboard and the asset library), and `tools_load`, a directory of the rest. OpenAI's
API takes at most 128 tools and some clients fewer, and every tool definition is read again on each
turn, so the engine's commands are loaded only when a task needs them. `tools_load` with no
arguments lists every command by group (its name up to the first dot):

```json
{"groups":{"audio":["audio.analyze","audio.capture","audio.play"],"ui":["ui.focus","ui.layout","ui.press"]}}
```

With `groups` (`["audio"]`) or `commands` (`["ui.layout"]`, or tool names), it adds them to the
tools, returns each one's name, description and `inputSchema`, and sends a
`notifications/tools/list_changed` notification, so the client lists the tools again. Over plain
HTTP (`POST /mcp`) there is no stream to send it on; `slash-mcp --live` sends it to its client
itself. A client that doesn't list tools again can still call a loaded command by its tool name or
through `batch`. Every command can be called by its tool name whether it is listed or not.
`slash-mcp --tools all` lists every command from the start instead, for clients that handle many
tools (Claude Code defers large tool sets on its own); then there is no `tools_load`.

Tool names replace dots with underscores (`entity_create` for
`entity.create`), because some clients only accept letters, digits, `_` and `-`. The tool's
`inputSchema` is the command's params schema, and its annotations say whether it is read-only and,
if not, whether it is destructive (opening a scene, writing files or building with `export.run`
cannot be undone; other edits can), and whether it reaches outside the machine (`openWorldHint`,
for commands that need the `network` capability, such as `ai.complete`).

**Results.** A successful call returns the command's result as text content and, for objects, as
`structuredContent`. A failed command is a tool result with `isError: true`, not a protocol
error, so the model sees the message and can fix its call:

```json
{"content":[{"text":"{\"error\":{\"code\":-32001,\"data\":{\"path\":\"/id\"},\"message\":\"/id: no entity has the id \\\"playr\\\"; did you mean \\\"player\\\"?\"}}","type":"text"}],"isError":true}
```

**Resources.**

| URI | Contents |
| --- | --- |
| `slash://scene` | The open scene in `.scene.json` form |
| `slash://schema/scene` | The JSON Schema for `.scene.json` files |
| `slash://log` | The most recent engine log lines |

Resources are read through the matching command (`scene.get`, `schema.get`, `log.tail`), so the
session's capabilities apply to them too.

**Instructions.** The `initialize` result carries short instructions that clients pass to the
model: call `engine_describe` first, use `batch` for many edits, query IDs before components, use
the tools rather than editing `.scene.json` files by hand, load a group with `tools_load` before
calling its commands, and call `storyboard_brief` before a task and `storyboard_check` after it.

Also supported: `ping`, `tools/list`, `resources/list`, `resources/templates/list` (empty) and
notifications. The tool list changes only when `tools_load` adds to it; the resource list never
changes while the server runs.

## JSON-RPC

JSON-RPC 2.0, with the command name as the method and its params as `params`:

```json
{"jsonrpc":"2.0","id":1,"method":"entity.get","params":{"id":"player","omit_defaults":true}}
```

```json
{"id":1,"jsonrpc":"2.0","result":{"components":{"Transform2D":{"position":[64.0,32.0],"rotation":0.5}},"id":"player"}}
```

- Over stdio (`slash-cli rpc`), each message is one line of JSON and each response is one line.
- Batches (a JSON array of requests) are answered with an array of responses. A JSON-RPC batch is
  only a transport convenience: each request in it is a separate command with its own undo step.
  Use the `batch` command when the changes must apply all together or not at all.
- Notifications (requests with no `id`) run but get no response.
- Errors use the standard `error` object, with the codes [below](#error-codes).

## HTTP

`slash-cli serve` serves a session with no game loop; `slash-cli run --serve` serves a running
game. Both write their URL and token to `.slash/server.json` in the project folder:

```json
{"host":"127.0.0.1","port":18765,"token":"t0k","url":"http://127.0.0.1:18765"}
```

| Endpoint | Does |
| --- | --- |
| `POST /v1/commands/<name>` | Runs a command. The body is its params as a JSON object (an empty body means `{}`) |
| `GET /v1/commands` | Every command with its description, capabilities and params schema |
| `GET /v1/openapi.json` | The same commands as an OpenAPI 3.1 document |
| `POST /rpc` | JSON-RPC 2.0, one message or batch per request |
| `POST /mcp` | MCP over Streamable HTTP. Responses are plain JSON; the server never opens an event stream, so `GET /mcp` returns 405 |
| `GET /watch` | A session's [watch server](watching-agents.md#the-watch-page) only: the watch page, for people |

### Security

- The server listens on the loopback interface only.
- Every request needs `Authorization: Bearer <token>`. A missing or wrong token gets 401. The
  one exception is a watch server's `GET /watch`, a page that holds no secret: it reads the token
  from its link's fragment (`/watch#token=...`), which browsers never send to servers, and sends
  it with each command it calls.
- A request whose `Host` or `Origin` header names another site gets 403, which blocks DNS
  rebinding attacks from web pages.
- `.slash/server.json` is owner-only on macOS and Linux, should stay out of git (`slash-cli init`
  adds `.slash/` to `.gitignore`) and is removed when the server stops.
- `--allow` limits what callers can do, as on every front end. A running game never serves
  `export.run`, so callers can't make it build (the `build` capability).

### REST

A successful command returns 200 with the result as the body:

```bash
curl -X POST -H "Authorization: Bearer $TOKEN" -d '{"with": ["Transform3D"]}' \
    "$URL/v1/commands/world.query"
```

```json
{"count":1,"entities":["camera"],"truncated":false}
```

A failed command returns `{"error": {"code", "message", "data"}}` with an HTTP status from its
error code:

| Error code | HTTP status |
| --- | --- |
| `-32700`, `-32600`, `-32602`, `-32002` | 400 |
| `-32003` | 403 (and 401 for a bad token) |
| `-32601`, `-32001` | 404 |
| `-32005` | 409 |
| `-32006` | 423 |
| Anything else | 500 |
| Request waited more than 30 s for the game loop, or the server stopped | 503 |

```json
{"error":{"code":-32601,"message":"unknown command \"histroy.get\"; did you mean \"history.get\"?"}}
```

### Timing

Requests arrive on a background thread but never run there. Each one waits in a queue until the
thread that owns the session calls `Server::poll()`; `run --serve` does that between ticks. So
commands never race the simulation, and each sees the world at a tick boundary. Engine code that
embeds the server must call `poll()` regularly; `tests/test_server.cpp` shows the pattern.

## Error codes

Every front end uses the same codes. The `-326xx` codes come from JSON-RPC; the `-320xx` codes are
SlashEngine's own.

| Code | Name | Meaning |
| --- | --- | --- |
| `-32700` | Parse error | The message or body is not valid JSON |
| `-32600` | Invalid request | The message is not a valid request |
| `-32601` | Method not found | No command with that name; the message suggests the closest one |
| `-32602` | Invalid params | The params failed the command's schema; `data.path` or `data.errors` say where |
| `-32603` | Internal error | A bug in the engine |
| `-32001` | Not found | The entity, component or file the params name does not exist |
| `-32002` | Invalid argument | The params are well formed but not allowed, such as an ID already in use or nothing to undo |
| `-32003` | Permission denied | The session lacks a capability, or a path leaves the project folder |
| `-32004` | Platform failure | The operating system or a tool refused, such as a failed file write or a failed export build |
| `-32005` | Conflict | The data changed since the caller read it, such as a storyboard entry another agent edited; read it again and retry |
| `-32006` | Paused | A person watching paused the session's edits ([`work.pause`](commands.md#workpause)); the message says why, and reads still work |

`data` carries structured details where they help:

| Key | When |
| --- | --- |
| `path` | The JSON Pointer of the bad value inside the params |
| `errors` | Several schema errors, each `{"path", "message"}` |
| `capability` | The capability a session lacked: `read`, `edit`, `run`, `filesystem` or `build` |
| `step`, `tail`, `log` | Where an `export.run` failed, the end of its build log and the full log's path |
| `command`, `index` | The command inside a `batch` that failed, and its position |

## Engine notes: Commands, MCP and JSON-RPC

Every change to a world goes through a command registered in `engine/command`. The same registry
backs three front ends, so they never drift apart:

```bash
mcp=./build/dev/tools/slash-mcp/slash-mcp
$mcp --project . --scene tests/data/basic.scene.json     # MCP over stdio; tools use '_' for '.'
$mcp --project . --tools all                             # every command as a tool, not core + tools_load
$cli rpc --project .                                     # JSON-RPC 2.0, one message per line
$cli call world.query '{"with": ["Transform2D"]}' --scene tests/data/basic.scene.json
$cli call batch '{"commands": [{"command": "entity.create", "params": {"id": "a"}}]}' \
    --scene build/out.scene.json --save
$cli init path/to/game                                   # MCP configs for Claude Code, Codex, Cursor, opencode, Antigravity, Hermes, OpenClaw
```

A session checks each command's capabilities (`--allow read,edit,run,filesystem,build,network`),
validates its params against the command's JSON Schema, runs it, and records an undo step if it
changed the world. A failed command changes nothing. Paths are relative to `--project` and cannot
leave it. Commands that change the world take `"dry_run": true`, which returns what would change
and changes nothing; `world.snapshot`, `world.diff` and `world.restore` save, compare against and
go back to a copy of the entities. `events.subscribe` and `events.read` report entities created and
destroyed, each change to the world and events gameplay code emits with `World::emit`. Systems
react to them through `World::events()`, the events of the tick before, which each system sees
once whatever its priority; sessions put them back when a command fails or is a dry run.

MCP clients start with the core commands as tools (`kCoreCommands` in
`engine/command/src/mcp.cpp`) and `tools_load`, a directory that adds the rest by group as a task
needs them, since OpenAI's API takes at most 128 tools and each definition costs tokens every turn
([docs/reference/protocols.md](protocols.md#mcp)). A command most sessions need
belongs in that list; the bench's setup counts it.

Commands are built for agents, so keep them cheap to call:

- Return only what the caller needs: IDs before full components, changed fields after an edit.
- Report every error with the JSON Pointer of the bad value and, for a misspelled name, the
  closest valid one ("did you mean").
- Keep descriptions short; they are sent to the model as tool definitions.

The benchmark in `bench/` holds commands to that. `slash-cli bench` plays standard agent tasks
through MCP and counts each one's calls and estimated tokens, and `ctest` fails when a task
stops passing its checks or costs more than `bench/baseline.json` (setup included, so a new
command's definition counts). When a change costs more or less on purpose, run
`slash-cli bench --baseline bench/baseline.json --update` and commit the file. With `--live`, a
model does the tasks from their prompts instead; see
[docs/reference/benchmark.md](benchmark.md).

To add a command, register it in `engine/command/src/builtin.cpp` (or your own registry) with a
name, description, capabilities, an `Effect` and a params schema with
`"additionalProperties": false`, then add a test to `tests/test_command.cpp`.

## Engine notes: HTTP server and live games

`engine/server` serves a session over HTTP on the loopback interface, so agents can drive a game
while it runs:

```bash
$cli serve --scene tests/data/basic.scene.json     # until Ctrl+C; prints the URL and token
$cli run --serve                                   # a running game, served between ticks
$mcp --live --project .                            # MCP over stdio, forwarded to that game
```

Endpoints: `POST /v1/commands/<name>` (REST, params as the body), `GET /v1/commands`,
`GET /v1/openapi.json`, `POST /rpc` (JSON-RPC) and `POST /mcp` (MCP over Streamable HTTP). Every
request needs `Authorization: Bearer <token>`; a `Host` or `Origin` naming another site gets 403.
The URL and token are written to `.slash/server.json` (owner-only on macOS and Linux) and removed
on exit.

Requests are queued and run only when the session's owner calls `Server::poll()`, so commands
never race the game loop. A running game adds `play` and `pause`; while paused it keeps serving
but stops stepping the world. Tests drive the server from a helper thread while the test thread polls;
see `tests/test_server.cpp`.
