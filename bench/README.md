# Agent benchmark

Standard agent tasks, each played through MCP by `slash-cli bench`, which counts the calls and
estimated tokens each one costs. `ctest` runs them against `baseline.json` on every pull
request. [docs/reference/benchmark.md](../docs/reference/benchmark.md) describes the task
format, the report and the baseline.

```bash
slash-cli bench --baseline bench/baseline.json            # check
slash-cli bench --baseline bench/baseline.json --update   # after a change that costs more or less on purpose
```

Each folder in `tasks/` is one task: `task.json` (the prompt, the calls that solve it and the
checks that prove it) and `project/`, the files it starts from.
