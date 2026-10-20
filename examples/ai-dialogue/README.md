# AI dialogue

A baker who talks through a language model, from gameplay code
([docs/reference/providers.md](../../docs/reference/providers.md#gameplay)).
[main.scene.json](main.scene.json) has Mira, with the questions the player asks in turn, a label
and a Talk button. [main.cpp](main.cpp) adds a `dialogue` system: when Talk is pressed it asks
the `baker` role with `ai::ask`, which returns at once with a request ID, and when that
request's `ai_reply` event comes it shows the reply and keeps the reply's message for the next
question. It plays the game the way an agent does over MCP or JSON-RPC, against a stand-in for a
local model server, so it needs no network or key. `expected.txt` holds each call and its reply.

- `ai::ask(world, role, request)` starts the request on its own thread and returns its ID
  (`ai_1`, `ai_2`, ...). The `ai` system (priority 1150) emits `ai_reply` with the reply's text,
  message, stop reason and usage, or `ai_failed` with the error's code and message, so gameplay
  code hears back as it does from input or physics: from `world.events()` at the next tick.
- The conversation is the `Dialogue` component's `messages`, kept with the world as
  `ai::to_json` writes them and read back with `ai::message_from_json`. The stand-in model knows
  what "two" means only from it.
- Sessions wait for each reply, as headless games do, so it arrives at the same tick on every
  run. Windowed games never wait: replies come at whichever tick they finish by, and
  `ai::reply_text` shows a reply as it streams.
- Agents hear replies through `events.subscribe` and `events.read`, like any gameplay event.
- With a replay file in `providers.json`, the first session records each reply. A session
  without the `network` capability (`--allow read,edit,run,filesystem`) answers the same requests from it
  and fails the rest, so tests of a game that talks to a model need no model.
- An export of a project with `providers.json` installs the same API in the game, without the
  replay settings, and Android exports get the `INTERNET` permission.
