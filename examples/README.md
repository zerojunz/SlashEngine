# Examples

Small working examples, one per feature. CI builds each one and runs it headless with its
output checked exactly, so the code and calls here are known to work with this version of the
engine. Copy from them.

| Example | Shows |
| --- | --- |
| [components-and-systems](components-and-systems/) | A game's own component and a system that moves it, in C++ |
| [custom-commands](custom-commands/) | A game's own command with a params schema and undo, served like the built-in ones |
| [edit-a-scene](edit-a-scene/) | Creating, changing, finding and saving entities, `batch` and `undo` |
| [prefabs](prefabs/) | Entities that extend a `.prefab.json` file and override fields |
| [try-and-roll-back](try-and-roll-back/) | Dry runs, and `world.snapshot`, `world.diff` and `world.restore` |
| [events](events/) | `events.subscribe` and `events.read` to hear what changed |
| [run-until](run-until/) | `run.until`: run a game headless until something happens, in one call |
| [storyboard](storyboard/) | Briefing before a task, editing facts with versions, and checking after |

## What an example holds

- `README.md`: what it shows and what to notice.
- `main.cpp`: a C++ program, built as `example-<folder>`. Its output is `expected.txt`.
- `calls.jsonl`: JSON-RPC requests for `slash-cli rpc --project .`, the same calls an agent makes
  as MCP tools (`entity.create` is the tool `entity_create`). `results.jsonl` holds the
  responses, line for line.
- `project.json`, scenes, prefabs and a storyboard when the example is a game project. It must
  pass `slash-cli project validate`.

## Running and adding examples

`ctest --preset dev -R example` runs every example in a copy of its folder under
`build/dev/examples/`, and fails if any output differs from what is checked in.

To add one, make a folder with a `README.md` and a `main.cpp` or `calls.jsonl`, link it in the
table above, then write its expected output and read it before committing:

```bash
cmake --preset dev && cmake --build --preset dev
SLASH_UPDATE_EXAMPLES=1 ctest --preset dev -R example_<folder>
```

Every new feature ships with an example here.
