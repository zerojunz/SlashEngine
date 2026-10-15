# Try and roll back

Trying a change without risk. `"dry_run": true` shows what an edit would change and changes
nothing. `world.snapshot` saves the world under a name, `world.diff` lists exactly which entities
and fields changed since, by ID, and `world.restore` goes back. [calls.jsonl](calls.jsonl) holds
the requests and [results.jsonl](results.jsonl) the responses, line for line.

```bash
slash-cli rpc --project . < calls.jsonl
```

- A dry run returns the command's own result and the changes, and records no undo step.
- `world.diff` lists `added`, `removed` and `changed` entities, with only the fields that differ.
- `world.restore` is one undo step, so `undo` brings the experiment back.
