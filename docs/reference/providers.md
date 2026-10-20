# Language model providers

A game, a tool or an agent calls a language model through `ai::Provider`, the engine's one
interface over any vendor's API. `providers.json`, beside `project.json`, names the providers a
project can call and the role each feature uses, such as `dialogue` or `assistant`, so code asks
for a role and the project decides which model answers. Nothing in the engine depends on one
vendor. [examples/ai-provider](../../examples/ai-provider/) has a shopkeeper who talks through
one, and [examples/ai-dialogue](../../examples/ai-dialogue/) a baker who talks from gameplay
code.

```json
{
  "providers": {
    "local": {"type": "openai", "model": "llama3.2", "url": "http://localhost:11434/v1"},
    "hosted": {"type": "anthropic", "model": "<model id>", "api_key_env": "ANTHROPIC_API_KEY"}
  },
  "roles": {
    "default": "local",
    "dialogue": {
      "provider": "local",
      "system": "You write lines for the characters of Hollow Vale.",
      "temperature": 0.8
    }
  },
  "replay": {"file": "tests/ai.replay.jsonl"}
}
```

`slash-cli schema providers` prints the JSON Schema, `slash-cli project validate` checks the file,
and [`ai.providers`](commands.md#aiproviders) lists what it configures.

## Providers

Each provider is an adapter `type` and a `model`, keyed by a name the roles use.

| Type | API | Default URL |
| --- | --- | --- |
| `openai` | OpenAI's chat completions, which Ollama, llama.cpp's server, vLLM, LM Studio and OpenRouter also serve | `https://api.openai.com/v1` |
| `anthropic` | Anthropic's Messages API | `https://api.anthropic.com` |
| `gemini` | Google's Gemini API | `https://generativelanguage.googleapis.com/v1beta` |

| Field | Meaning |
| --- | --- |
| `type` | `openai`, `anthropic` or `gemini` |
| `model` | The model's ID, as the provider names it |
| `url` | The API's base URL, such as `http://localhost:11434/v1` for Ollama. Default: the vendor's |
| `api_key_env` | The environment variable that holds the key. Leave it out for servers that take none |
| `headers` | Headers sent with every request. Never put a key here |
| `capabilities` | What the model can do: `tools`, `json_schema_output`, `vision` and `streaming`, each `true` unless set, and `context_tokens` |
| `max_tokens` | Sent when a request sets none. Anthropic's API needs one, so it defaults to 16000 there |
| `max_tokens_param` | `openai` only: `max_tokens` (the default) or `max_completion_tokens`, as the server takes it |
| `timeout_seconds` | How long to wait for a reply. Default: 600 |
| `max_retries` | Retries after a rate limit (429), a timeout (408), a conflict (409) or a server error (5xx), waiting as `Retry-After` says or 1, 2, 4... seconds. Default: 2 |

Keys come only from the environment, never from the file, and no command returns one. A provider
whose key is not set fails only when a request has to call its model, with `-32003`, so replaying
recorded answers needs no key.

## Connections

URLs are `http://`, for models on the machine or the local network (Ollama, llama.cpp, LM Studio
and vLLM), or `https://`, for hosted APIs. An `https://` server's certificate is checked against
the certificates the system trusts: the platform decides on macOS and iOS (as Safari would), and
the system's store or CA bundle is read on Windows, Linux and Android.

Web exports send requests with the browser's `fetch`, so the browser's certificates and proxies
apply and the variables below don't. The browser also applies its CORS rules: a page may call
another site only if that server allows it (`Access-Control-Allow-Origin`), which hosted APIs
mostly don't and local servers need turning on for (`OLLAMA_ORIGINS` for Ollama). A web game's
`url` should name the developer's own server, which adds the key and allows the game's site.

Connections read the same environment variables curl reads:

| Variable | Does |
| --- | --- |
| `SSL_CERT_FILE` | A PEM file of the certificates to trust instead of the system's, for a proxy or server that a private authority signs |
| `https_proxy`, `HTTPS_PROXY` | The proxy for `https://` URLs, as `http://[user:password@]host:port` |
| `http_proxy` | The proxy for `http://` URLs (never `HTTP_PROXY`, which a web server's scripts get from a request's header) |
| `no_proxy`, `NO_PROXY` | Comma-separated hosts reached directly: `*`, a host, a domain and everything in it (`example.com`, `.example.com` or `*.example.com`) or a network (`10.0.0.0/8`) |

This machine's own addresses (`localhost`, `127.x.x.x`, `::1`) never go through a proxy. A
connection that fails says why: a certificate the system doesn't trust or that names another
host, a proxy that refused it, or a server that could not be reached.

## Roles

A role is the provider a feature uses: a provider's name, or an object that also sets what the
role adds to every request.

| Field | Meaning |
| --- | --- |
| `provider` | The provider's name |
| `model` | In place of the provider's model |
| `system` | Instructions put before every request's messages |
| `max_tokens`, `temperature`, `stop`, `seed` | Used where a request sets none |

A request names a role or a provider directly. With neither, it uses the `default` role, or the
only provider when there is one.

## Requests and replies

A request holds messages (`system`, `user` with text and images, `assistant` with text and tool
calls, and `tool` with a call's result), the tools the model may call, an optional
`output_schema` and params. A reply holds the assistant's message, the parsed output, why the
model stopped (`end`, `tool_calls`, `max_tokens`, `stop_sequence`, `refused` or `other`), the
tokens it used and whether it came from the replay file.

- **Structured output.** `output_schema` asks for a JSON value. Providers whose capabilities
  include `json_schema_output` are held to the schema; the others get it in their instructions.
  Either way the reply is parsed (with any Markdown code fence taken off) and checked against the
  schema, and a reply that does not match fails with its JSON Pointer.
- **Tools.** Tool calls come back with their arguments as JSON; the caller runs them and sends
  each result back as a `tool` message. `ai::command_tools` makes tools of engine commands,
  named as MCP names them, and `ai::run_tool_calls` runs a reply's calls as those commands in a
  session.
- **Reasoning.** Replies keep what the provider needs back in later turns unchanged (Anthropic's
  thinking blocks and Gemini's thought signatures) in the message's `native` field, which only
  an adapter of the same type reads, so a conversation can move to another provider.
- **Streaming.** A request given a sink receives the reply's text as it arrives. `ai::Call` runs a
  request on its own thread, so a game keeps ticking while the model answers, and cancels it when
  destroyed, without waiting for its thread.

## Replay

`replay` records each answer in a JSON lines file in the project, keyed by the model and the whole
request, so tests that use a model run the same everywhere with no model at all.

| Mode | Does |
| --- | --- |
| `auto` (default) | Answers from the file what it has, and calls the model and records the rest |
| `record` | Calls the model every time and records the answer |
| `replay` | Answers only from the file; a request it has no recording of fails with `-32001` |

The `SLASH_AI_REPLAY` environment variable overrides the mode (`replay`, `record`, `auto` or
`off`). CI sets `replay`, so a changed prompt fails the test instead of calling a model; record it
again with `auto`. The file is rewritten sorted by key, one line per recording, so it diffs well.
Sessions that run tests (`test.run`) record nothing.

## Gameplay

Gameplay code asks a role's model and hears back through world events, as it hears input and
physics, so a game never waits on a model:

```cpp
#include "slash/ai/world.hpp"

const auto asked = slash::ai::ask(world, "dialogue", "The player asks Mira for bread.");
// At a later tick:
for (const slash::world::Event& event : world.events()) {
    if (event.kind == "ai_reply" && event.id == asked.value()) {
        show(event.data.find("text")->as_string());
    }
}
```

`ai::ask(world, role, request)` takes a `Request` or one user message, starts it streaming on
its own thread and returns its ID: `ai_1`, `ai_2` and so on. An unknown role or a missing
`providers.json` fails right away. The `ai` system (priority 1150, after particles) then emits
one event for it:

| Event | Data |
| --- | --- |
| `ai_reply` | `role`, `text`, `message` (the reply, to put back in the next request of the conversation with `ai::message_from_json`), `output` (with an `output_schema`), `stop`, `usage` |
| `ai_failed` | `role`, `code` (such as `permission_denied` or `invalid_argument`) and `message` |

`ai::reply_text` shows a reply as it streams, `ai::pending` says whether its event is still to
come and `ai::cancel` stops it. Agents read the events with
[`events.subscribe`](commands.md#eventssubscribe) and `events.read`.

- **Games.** A game whose project has a `providers.json` installs this when it starts, reading
  the file from its data; [exports](cli.md#export) stage it without its `replay` settings, and
  Android exports get the `INTERNET` permission. A shipped game holds no key, so a hosted
  provider's `url` should name the developer's own server, which adds the key, or players set the
  variable themselves on a computer. A project without the file leaves the AI code out of its
  exports.
- **Same ticks, same replies.** Headless games and sessions wait at each tick for the replies to
  the requests asked before it, so a reply arrives at the same tick on every run. Windowed games
  never wait; a reply comes at whichever tick it finishes by. Neither do web games, headless ones
  included: the browser answers between frames, so a game can't wait for it in one.
- **Sessions.** A session calls models only with the `network` capability. Without it, requests
  are answered from the replay file and others fail with `permission_denied`, so an agent can
  play a game that talks to a model with no model at all. A failed command or a dry run cancels
  the requests it asked and gives their IDs out again; `providers.json` is read again when it
  changes.

## In C++

Link `slash::ai`; games link it already. Gameplay code uses `ai::ask` above; tools call providers
directly, and `slash::ai_commands` adds the commands and the command tools.

```cpp
#include "slash/ai/config.hpp"

const slash::ai::Config config = slash::ai::load_config(".").value();
const slash::ai::Opened dialogue = slash::ai::open(config, "dialogue").value();
slash::ai::Request request;
request.messages.push_back({.role = slash::ai::Role::User, .text = "Greet the player."});
slash::ai::Call call(dialogue.provider, request, /*stream=*/true);
// Each tick: show call.text(); once call.done(), call.result() holds the reply or the error.
```

`ai::make_adapter(type, options)` makes an adapter with no file, and `ai::ReplayProvider` wraps
any provider, a game's own included. `ai::make_http_transport` takes `HttpOptions` (a
certificate file, a proxy and where to read the environment) for an adapter's `transport`, and
`ai::proxy_for` says which proxy a URL would go through.

`Call` runs `Provider::complete` on a thread of its own, except on the web, which has none: there
it calls `Provider::start`, which returns at once and hands over the reply later from the
browser's event loop. The adapters, roles and replay files start requests with
`Transport::post_async` and wait to retry with `Transport::later`, which the web's transport
does with `fetch` and timers. A provider or transport of a game's own can override them too;
by default they call `complete` and `post`, then hand over the outcome before returning.
