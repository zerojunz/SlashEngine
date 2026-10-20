# AI provider

A shopkeeper who talks through a language model, with the model behind `ai::Provider`.
[main.cpp](main.cpp) starts a stand-in for a local model server (as llama.cpp's or Ollama's
OpenAI-compatible one) on the loopback interface, so the example needs no network or key, and
writes the project's `providers.json` with its port:

```json
{
  "providers": {
    "local": {"type": "openai", "model": "llama3.2", "url": "http://127.0.0.1:<port>/v1"}
  },
  "roles": {
    "shopkeeper": {
      "provider": "local",
      "system": "You are Mira, who keeps the bakery in Hollow Vale. Answer in one sentence.",
      "temperature": 0.7
    }
  },
  "replay": {"file": "tests/ai.replay.jsonl"}
}
```

[expected.txt](expected.txt) holds its output.

- `ai::open(config, "shopkeeper")` gives the role's provider, which adds the role's
  instructions and temperature to every request. A provider that needs a key names the
  environment variable that holds it (`"api_key_env"`); keys never go in the file.
- `ai::Call` runs a request on its own thread, so the game keeps ticking and drawing while the
  model answers; with streaming on, `call.text()` grows as the words arrive.
- `output_schema` asks for a JSON value. Providers that support it are held to the schema;
  others are told it in the prompt. Either way the reply is checked, and `Response::output`
  holds the parsed value.
- `ai::command_tools` turns engine commands into tools, named as MCP names them, and
  `ai::run_tool_calls` runs the model's calls as those commands in a session, so a model in the
  game can do what an agent over MCP can and no more.
- The replay file records each request's answer, keyed by the model and the whole request. With
  `SLASH_AI_REPLAY=replay`, as CI sets, answers come only from it and a new request fails, so
  tests that use a model run the same everywhere with no model at all.
- Agents call the same providers with `ai.complete` and list them with `ai.providers`; see
  [the command reference](../../docs/reference/commands.md#aicomplete) and
  [providers.json](../../docs/reference/providers.md).
