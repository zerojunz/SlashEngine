# Agent benchmark

The benchmark measures what standard agent tasks cost: how many tool calls each takes and how
many tokens a model writes and reads doing it. Commands are meant to be cheap for agents, and
this is how a change that makes them dearer gets caught. `slash-cli bench` plays each task's
calls through the MCP server, as slash-mcp serves them, and `ctest` holds every pull request to
the numbers in `bench/baseline.json`.

```bash
slash-cli bench                                         # run every task in bench/tasks
slash-cli bench --baseline bench/baseline.json          # and compare with the baseline
slash-cli bench --baseline bench/baseline.json --update # write the new numbers
```

## Tasks

A task is a folder in `bench/tasks` with a `task.json` and the project it starts from, in
`project/`. Each run copies the project to `<work>/<task>` first, so tasks never change their
own files.

```json
{
  "format": 1,
  "prompt": "Move every rigid body in scenes/level.scene.json 32 pixels up, and save the scene.",
  "solution": [
    {"tool": "scene_open", "arguments": {"path": "scenes/level.scene.json"}},
    {"tool": "world_query", "arguments": {"with": ["RigidBody2D"], "include": ["Transform2D"]}},
    {"tool": "batch", "arguments": {"commands": ["..."]}},
    {"tool": "scene_save", "arguments": {}}
  ],
  "checks": [
    {"command": "scene.open", "params": {"path": "scenes/level.scene.json"}},
    {"command": "entity.get", "params": {"id": "crate_1"},
     "expect": {"components": {"Transform2D": {"position": [100, 168]}}}},
    {"step": 2, "expect": {"entities": [{}, {}, {}, {}]}}
  ]
}
```

| Field | Meaning |
| --- | --- |
| `format` | `1` |
| `prompt` | The task as a person would ask an agent for it |
| `solution` | The MCP tool calls an agent that knows the engine well makes for it, in order: each a `tool` name (dots become `_`) and its `arguments` |
| `checks` | What must hold afterwards, so a task that stops doing what it says fails |

Each check is either a `command` with `params`, run in the same session once the solution has
run, or a `step` (from 1) whose reply to check. `expect` works as in a
[`.test.json` step](scene-format.md#tests): objects need only the members given, arrays the same
number of items, and other values must be equal. A command check with no `expect` only needs
the command to succeed.

`slash-cli schema bench` prints the JSON Schema for `task.json`. The tasks so far:

| Task | What the agent does |
| --- | --- |
| `add-enemy` | Adds a bat with a body, a collision shape and a position, and saves |
| `raise-crates` | Finds every rigid body and moves each one up in one batch |
| `slime-prefab` | Makes a prefab of an entity and places two more instances of it |
| `paint-island` | Paints a rectangle of terrain on a tile map layer |
| `fit-label` | Lays out a HUD, sees a label overflow its panel, turns on wrapping and checks again |
| `landing-tick` | Runs physics until a ball lands and reads the tick |
| `story-decision` | Briefs on a scene's storyboard and records a decision linked to it |

## Report

```json
{
  "failed": [],
  "setup": {"instructions": 2897, "tool_definitions": 19145, "tools": 85},
  "tasks": {
    "raise-crates": {"calls": 4, "read": 378, "tokens": 571, "written": 193}
  },
  "total": {"calls": 23, "tokens": 1581}
}
```

| Field | Meaning |
| --- | --- |
| `setup` | What an agent reads once per session: the server's `instructions` and every tool definition from `tools/list`, in tokens, and how many tools there are |
| `tasks.<name>.calls` | The solution's tool calls |
| `tasks.<name>.written` | Tokens in the calls: each tool's name and arguments |
| `tasks.<name>.read` | Tokens in what came back: the text of each reply |
| `tasks.<name>.tokens` | `written` plus `read` |
| `failed` | Each failed solution call or check: its `task`, the JSON Pointer `path` in `task.json` (or in what was expected) and a `message` |
| `regressions` | With `--baseline`: each number that grew, as `what` (`setup/tool_definitions`, `raise-crates/tokens`, `raise-crates/calls`, or a task the baseline lacks), `baseline` and `now` |

Tokens are estimated the same way on every machine, because each model's tokenizer counts
differently: a run of letters is a token for every ten, a run of digits or of punctuation one
for every three, a line break with its indent one, spaces none, and each character beyond ASCII
one. On the engine's JSON and English that comes within a tenth of a real tokenizer's count;
compare runs with each other, not with a model's bill.

## Baseline

`bench/baseline.json` is a report without `failed`. With `--baseline`, a run fails (exit code 1)
when a task's tokens or the setup's grow more than `--tolerance` above it (default `0.05`, five
percent), when a task takes more calls, or when a task is new. A failed check fails it too.

A change that makes tasks dearer on purpose, such as a new command whose definition every agent
now reads, or cheaper, which the baseline should keep, rewrites the baseline with `--update` in
the same pull request. `--update` refuses to write numbers from a run with a failed task; with
`--task`, it keeps the other tasks' numbers.

## Adding a task

1. Make the folder, with `project/` holding the scenes and files the task starts from.
2. Write the prompt, then the calls you would make, as few and as small as the engine allows;
   if a task needs more calls than it should, that is a command to add, not a longer solution.
3. Add checks that fail if the solution no longer does what the prompt asks.
4. Run `slash-cli bench --task <name>` until nothing fails, then `--update` the baseline.
