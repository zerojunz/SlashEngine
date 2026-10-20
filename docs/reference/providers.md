# Language model providers

A game, a tool or an agent calls a language model through `ai::Provider`, the engine's one
interface over any vendor's API. `providers.json`, beside `project.json`, names the providers a
project can call and the role each feature uses, such as `dialogue` or `assistant`, so code asks
for a role and the project decides which model answers. Nothing in the engine depends on one
vendor. [examples/ai-provider](../../examples/ai-provider/) has a shopkeeper who talks through
one.

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

This build speaks plain `http://`, which reaches models on the machine or the local network
(Ollama, llama.cpp, LM Studio and vLLM) and proxies in front of hosted ones. Requests to
`https://` URLs fail with a message saying so until the engine gains TLS, which is next on the
[roadmap](../roadmap.md).

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
  destroyed.

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

## In C++

Link `slash::ai`. Games call providers directly; `slash::ai_commands` adds the commands and the
command tools.

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
any provider, a game's own included.
