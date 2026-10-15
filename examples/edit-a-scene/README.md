# Edit a scene

The everyday loop over JSON-RPC, the same calls an agent makes as MCP tools (`entity_create` and
so on): create entities, several at once with `batch`, change a field, find entities, recover
from a misspelled ID, undo, and save. [calls.jsonl](calls.jsonl) holds the requests and
[results.jsonl](results.jsonl) the responses, line for line.

```bash
slash-cli rpc --project . < calls.jsonl
```

- `component.set` returns only the fields that changed: `position` was already `[64, 32]`.
- `world.query` returns IDs unless asked for components.
- A misspelled ID fails with the JSON Pointer of the bad value and the closest ID.
- `batch` runs its commands all or nothing, as one undo step.
