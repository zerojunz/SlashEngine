# Agent exit check

Milestone 3's exit check: each coding agent opens, edits and saves a scene through SlashEngine's
MCP server, set up as `slash-cli init` sets it up. [`.github/scripts/agent-exit-check.py`](../../.github/scripts/agent-exit-check.py)
runs it for Claude Code, Codex, Cursor, opencode, Hermes Agent and OpenClaw, each in its own
non-interactive mode and with the person's own sign-in:

```bash
cmake --build --preset dev --target slash-cli slash-mcp
.github/scripts/agent-exit-check.py all --out build/exit-check   # or: claude codex cursor ...
```

For each agent it makes a game with `slash-cli init`, gives the agent one prompt (create an
entity named `exit_check` with a `Transform2D` and a `Sprite2D`, then save the scene, using only
the MCP tools) and checks the saved scene file: the entity and its fields must be there and
`slash-cli scene validate` must pass. An agent that isn't installed is skipped. It writes each
agent's raw output to `<out>/<agent>.log` and a record to `<out>/<agent>.md`, which lists the
MCP tools the agent called, and exits with 1 if any agent failed.

| Agent | Command the check runs | MCP setup |
| --- | --- | --- |
| Claude Code | `claude -p` | `.mcp.json`, only its tools allowed |
| Codex | `codex exec --json` | `.codex/config.toml`'s entry, passed with `-c` |
| Cursor | `cursor-agent -p --force --approve-mcps` | `.cursor/mcp.json` |
| opencode | `opencode run --format json` | `opencode.json` |
| Hermes Agent | `hermes --yolo chat --oneshot -q` | a copy of `~/.hermes` (as `HERMES_HOME`) with `.slash/agents/hermes.yaml`'s entry added |
| OpenClaw | `openclaw agent --local --json` | `openclaw mcp set slashengine` for the run, `unset` after |

Recorded runs:

- [Claude Code](claude-code.md), passed on 2026-11-07.

The others are recorded on a machine where they are signed in, and their records added here.
