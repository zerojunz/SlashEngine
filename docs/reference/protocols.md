# Protocols

The engine's [commands](commands.md) are served four ways: MCP, JSON-RPC 2.0, a REST mirror over
HTTP and the command line. All four are generated from one command registry and run through the
same session, so params, results and errors are identical; only the envelope differs. This page
covers the envelopes, the HTTP server and the error codes.

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

**Tools.** Every command is a tool. Tool names replace dots with underscores (`entity_create` for
`entity.create`), because some clients only accept letters, digits, `_` and `-`. The tool's
`inputSchema` is the command's params schema, and its annotations say whether it is read-only and,
if not, whether it is destructive (opening a scene or writing files cannot be undone; other edits
can).

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
model: call `engine_describe` first, use `batch` for many edits, query IDs before components, and
use the tools rather than editing `.scene.json` files by hand.

Also supported: `ping`, `tools/list`, `resources/list`, `resources/templates/list` (empty) and
notifications. The tool and resource lists never change while the server runs.

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

### Security

- The server listens on the loopback interface only.
- Every request needs `Authorization: Bearer <token>`. A missing or wrong token gets 401.
- A request whose `Host` or `Origin` header names another site gets 403, which blocks DNS
  rebinding attacks from web pages.
- `.slash/server.json` is owner-only on macOS and Linux, should stay out of git (`slash-cli init`
  adds `.slash/` to `.gitignore`) and is removed when the server stops.
- `--allow` limits what callers can do, as on every front end.

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
| `-32004` | Platform failure | The operating system refused, such as a failed file write |

`data` carries structured details where they help:

| Key | When |
| --- | --- |
| `path` | The JSON Pointer of the bad value inside the params |
| `errors` | Several schema errors, each `{"path", "message"}` |
| `capability` | The capability a session lacked |
| `command`, `index` | The command inside a `batch` that failed, and its position |
