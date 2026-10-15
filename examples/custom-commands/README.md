# Custom commands

A game's own command, `health.damage`, registered next to the built-in ones and called through a
session: directly, over JSON-RPC, with a misspelled entity, with params the schema rejects, and
then undone. [main.cpp](main.cpp) prints each result.

- The registry checks params against the command's JSON Schema before the handler runs, and a
  failed command changes nothing.
- An `Edit` command gets undo and `dry_run` from the session for free.
- The same registry serves MCP (tool `health_damage`), JSON-RPC, REST and `slash-cli call`, so a
  game's commands reach agents the same way the engine's do.
