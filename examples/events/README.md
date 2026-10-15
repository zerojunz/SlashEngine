# Events

Hearing what changed instead of querying the world again. `events.subscribe` starts a queue,
filtered by kind or entity ID; `events.read` returns and empties it. [calls.jsonl](calls.jsonl)
makes two subscriptions, changes the world, and reads each. [results.jsonl](results.jsonl) holds
the responses, line for line.

```bash
slash-cli rpc --project . < calls.jsonl
```

- `entity.created` and `entity.destroyed` events name the entity.
- A `command` event carries the command's changes, in the form `world.diff` returns.
- Every event has `seq`, counting up across the session, and the `tick` it happened on.
- Gameplay code adds its own kinds with `World::emit`.
