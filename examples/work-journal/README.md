# Work journal

Every session that serves an agent writes what its commands did to `.slash/work/<session>.jsonl`,
so a person, or another agent, can see what any agent is doing and has done, whichever vendor's
agent it is ([watching agents](../../docs/reference/watching-agents.md)). In [main.cpp](main.cpp)
an agent's session builds a small scene: it makes a floor and a crate, looks around, moves the
crate, misspells an entity, runs the storyboard check and saves. A reviewer's session then reads
that journal with `work.feed`, as plain lines and as JSON, and sums it up with `work.digest`. Both
journals run on a fixed clock, so [expected.txt](expected.txt) repeats.

```text
work.feed {"session": "20261122-130500-a1b2", "text": true, "limit": 50}
  13:05:01  session 20261122-130500-a1b2 started (slash-mcp)
  13:05:02  claude-code  entity.create {"components":{"Transform2D":{"position":[0,200]}},"id":"floor"}  +floor
  13:05:03  claude-code  entity.create {"components":{"Transform2D":{"position":[40,0]}},"id":"crate"}  +crate
  13:05:04  claude-code  read: entity.get, world.query
  13:05:07  claude-code  component.set {"components":{"Transform2D":{"position":[64,180]}},"id":"crate"}  ~crate(Transform2D)
  13:05:08  claude-code  entity.get {"id":"crat"}  FAILED: /id: no entity has the id "crat"; did you mean "crate"?
  13:05:09  claude-code  storyboard.check  -> ok
  13:05:10  claude-code  scene.save {"path":"main.scene.json"}  wrote main.scene.json
  13:05:11  session ended
work.digest {"since": "2026-11-22T13:00:00Z"}
  2 sessions: 20261122-130500-a1b2 (slash-mcp, claude-code); 20261122-131000-c3d4 (slash-cli rpc, codex)
  4 commands and 6 reads from 13:05:01 to 13:15:04 UTC; 1 failed.
  Scenes: main.scene.json.
  Files written (1): main.scene.json.
  Entities: 2 added, 0 removed, 1 changed.
  Checks passing: storyboard.check.
  Last failure: entity.get at 13:05:08: /id: no entity has the id "crat"; did you mean "crate"?
```

- A line per command that changed something: its short params, the entities it added (`+`),
  removed (`-`) and changed (`~`, with the components), the files it wrote and its error. Reads and
  dry runs fold into one line; tests and checks keep their results.
- Params whose names say they hold a secret (an API key, a token, a password) are written as
  `"[left out]"`.
- `slash-cli watch` follows the same lines as a session works, and `slash-cli work digest` prints
  the summary. Live sessions also serve themselves read-only for watchers, listed in
  `.slash/work/sessions.json`.
- `project.json`'s `work` block turns the journal or the watch server off and sets how much the
  journals may hold.
