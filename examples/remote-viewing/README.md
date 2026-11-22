# Remote viewing

A person away from the machine can ask their own agent, in any chat, how another agent's work is
going: their agent looks in on the session through the engine and shows them the world as it is
now, steers it, or hands them a page of the whole run
([watching agents](../../docs/reference/watching-agents.md#remote-viewing)).

In [main.cpp](main.cpp) a working agent's session journals its commands and serves itself to
watchers, as slash-mcp's do. A second agent's session calls [`work.view`](../../docs/reference/commands.md#workview)
with `session` naming the first: the call goes through the first session's watch server, between
its own commands, and comes back with its world drawn as it is now, its plan and its last lines:

```text
helper: work.view
  live: yes, frame: 320 x 180
  plan: {"status":"working","steps":[{"done":true,"text":"Lay the floor"},{"done":false,"text":"Stack the crates"}],"task":"Build the yard"}
  14:00:11  checkpoint 2: first crate
helper: work.pause
worker: entity.create
  failed: a person watching paused this session's edits: "Crates go by the gate"; ...
helper: work.note
helper: work.resume
worker: component.set
  a note came with it: The gate is at x 520
```

Then it gets the checkpoints' frames in one picture with
[`work.timeline`](../../docs/reference/commands.md#worktimeline) and writes one HTML page of the
run with [`work.report`](../../docs/reference/commands.md#workreport), to hand on as an artifact
or a file. Once the session ends, `work.view` shows it from its files: the last checkpoint's frame
and the end of its journal.

- `work.pause`, `work.resume` and `work.note` take `session` the same way; an agent can't steer
  its own session.
- `slash-mcp --watch` serves only these tools, read-only, to an agent a person talks to elsewhere;
  `slash-cli work timeline --out <file.png>` and `slash-cli work report` do the same from a
  terminal.
