#!/usr/bin/env python3
"""Milestone 3's exit check: each coding agent opens, edits and saves a scene over MCP.

For each agent asked for, this makes a game with `slash-cli init`, gives the agent one prompt in
its own non-interactive mode with SlashEngine's MCP server as `init` set it up, then checks the
scene file on disk: the entity the prompt asks for must be there with the asked-for fields, and
`slash-cli scene validate` must pass. It writes each agent's raw output to `<out>/<agent>.log`
and a record of the run to `<out>/<agent>.md`, and prints a JSON summary.

    .github/scripts/agent-exit-check.py claude codex --out build/exit-check
    .github/scripts/agent-exit-check.py all

Agents run with the person's own sign-in and model; nothing here names a vendor's model.
Hermes Agent runs with HERMES_HOME set to a copy of ~/.hermes with the server added, so the
person's own config is left alone. OpenClaw keeps MCP servers in its own config only, so the
check sets `slashengine` there for the run and unsets it after.
"""

import argparse
import datetime
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

ENTITY = "exit_check"
POSITION = [12.0, 34.0]
PROMPT = (
    "Use only the slashengine MCP server's tools, not the shell or file edits. "
    "The scene scenes/main.scene.json is open. Create an entity with the ID "
    f'"{ENTITY}" that has a Transform2D at position [12, 34] and a Sprite2D whose modulate '
    "is [1, 0.5, 0.25, 1], then save the scene to scenes/main.scene.json. "
    "Reply with DONE when the scene is saved."
)
AGENTS = ["claude", "codex", "cursor", "opencode", "antigravity", "hermes", "openclaw"]
NAMES = {
    "claude": "Claude Code",
    "codex": "Codex",
    "cursor": "Cursor",
    "opencode": "opencode",
    "antigravity": "Antigravity",
    "hermes": "Hermes Agent",
    "openclaw": "OpenClaw",
}
PROGRAMS = {
    "claude": "claude",
    "codex": "codex",
    "cursor": "cursor-agent",
    "opencode": "opencode",
    "antigravity": "agy",
    "hermes": "hermes",
    "openclaw": "openclaw",
}


def run(command, cwd, env=None, timeout=None):
    return subprocess.run(command, cwd=cwd, env=env, timeout=timeout, capture_output=True,
                          text=True, check=False)


def version(program):
    try:
        out = run([program, "--version"], cwd=None, timeout=60)
    except (OSError, subprocess.TimeoutExpired):
        return "unknown"
    return (out.stdout or out.stderr).strip().splitlines()[0] if (out.stdout or out.stderr) else ""


def server_entry(project):
    return json.loads((project / ".slash/agents/openclaw.json").read_text())


def hermes_home(project, scratch):
    """A copy of ~/.hermes with SlashEngine's server added to config.yaml."""
    home = pathlib.Path(os.environ.get("HERMES_HOME", pathlib.Path.home() / ".hermes"))
    copy = scratch / "hermes-home"
    if home.is_dir():
        shutil.copytree(home, copy, ignore=shutil.ignore_patterns("sessions", "logs", "cache"))
    else:
        copy.mkdir()
    config = copy / "config.yaml"
    text = config.read_text() if config.exists() else ""
    entry = server_entry(project)
    try:
        import yaml  # pylint: disable=import-outside-toplevel
        data = yaml.safe_load(text) or {}
        data.setdefault("mcp_servers", {})["slashengine"] = entry
        config.write_text(yaml.safe_dump(data, sort_keys=False))
    except ImportError:
        if "mcp_servers:" in text:
            raise SystemExit("hermes: install PyYAML, or the check can't add its server to a "
                             "config.yaml that already has mcp_servers")
        config.write_text(text + "\n" + (project / ".slash/agents/hermes.yaml").read_text())
    return copy


def command_for(agent, project):
    """The agent's command for one prompt, run in the project, and any environment it needs."""
    if agent == "claude":
        return ["claude", "-p", PROMPT, "--mcp-config", ".mcp.json", "--strict-mcp-config",
                "--allowedTools", "mcp__slashengine", "--output-format", "stream-json",
                "--verbose"], {}
    if agent == "codex":
        entry = server_entry(project)
        return ["codex", "exec", "--skip-git-repo-check", "--json",
                "-c", "mcp_servers.slashengine.command=" + json.dumps(entry["command"]),
                "-c", "mcp_servers.slashengine.args=" + json.dumps(entry["args"]),
                PROMPT], {}
    if agent == "cursor":
        return ["cursor-agent", "-p", "--force", "--approve-mcps", "--output-format",
                "stream-json", PROMPT], {}
    if agent == "opencode":
        return ["opencode", "run", "--format", "json", PROMPT], {}
    if agent == "antigravity":
        # Antigravity's CLI reads .agents/mcp_config.json; its only way to allow tools without
        # asking is to allow them all.
        return ["agy", "-p", PROMPT, "--output-format", "json",
                "--dangerously-skip-permissions"], {}
    if agent == "hermes":
        return ["hermes", "--yolo", "chat", "--oneshot", "-q", PROMPT], {}
    if agent == "openclaw":
        return ["openclaw", "agent", "--local", "--json", "--session-id", "slash-exit-check",
                "--message", PROMPT], {}
    raise ValueError(agent)


def check_scene(project, cli):
    """What the saved scene holds of the asked-for entity, and the problems with it."""
    problems = []
    scene_path = project / "scenes/main.scene.json"
    try:
        scene = json.loads(scene_path.read_text())
    except (OSError, ValueError) as error:
        return None, [f"the scene can't be read: {error}"]
    entity = scene.get("entities", {}).get(ENTITY)
    if entity is None:
        return None, [f'the scene has no entity "{ENTITY}"']
    components = entity.get("components", {})
    position = components.get("Transform2D", {}).get("position")
    if position != POSITION:
        problems.append(f"Transform2D.position is {position}, not {POSITION}")
    modulate = components.get("Sprite2D", {}).get("modulate")
    if modulate != [1.0, 0.5, 0.25, 1.0]:
        problems.append(f"Sprite2D.modulate is {modulate}, not [1, 0.5, 0.25, 1]")
    validate = run([cli, "scene", "validate", str(scene_path)], cwd=project)
    if validate.returncode != 0:
        problems.append("scene validate failed: " + (validate.stdout + validate.stderr).strip())
    return entity, problems


def tool_calls(agent, log):
    """The MCP tools the agent called, in order, where its output says."""
    calls = []
    for line in log.splitlines():
        try:
            event = json.loads(line)
        except ValueError:
            continue
        if agent == "claude" and event.get("type") == "assistant":
            for block in event.get("message", {}).get("content", []):
                name = block.get("name", "") if block.get("type") == "tool_use" else ""
                if name.startswith("mcp__slashengine__"):
                    calls.append(name.removeprefix("mcp__slashengine__"))
        elif agent == "codex":
            item = event.get("item") or event.get("msg") or {}
            if item.get("type") in ("mcp_tool_call", "mcp_tool_call_begin"):
                calls.append(item.get("tool") or item.get("invocation", {}).get("tool", ""))
        elif agent == "cursor" and event.get("type") == "tool_call":
            if event.get("subtype") == "started":
                call = event.get("tool_call", {})
                calls.append(next(iter(call), ""))
        elif agent == "opencode" and event.get("type") == "tool_use":
            calls.append(event.get("part", {}).get("tool", ""))
    return calls


def check(agent, args, scratch):
    program = PROGRAMS[agent]
    record = {"agent": NAMES[agent], "program": program}
    if shutil.which(program) is None:
        record["result"] = "skipped"
        record["problems"] = [f"{program} isn't on PATH"]
        return record
    record["version"] = version(program)
    project = scratch / agent
    init = run([args.cli, "init", str(project), "--mcp-command", args.mcp], cwd=None)
    if init.returncode != 0:
        record["result"] = "failed"
        record["problems"] = ["slash-cli init failed: " + init.stderr.strip()]
        return record
    command, extra_env = command_for(agent, project)
    env = dict(os.environ, **extra_env)
    if agent == "hermes":
        env["HERMES_HOME"] = str(hermes_home(project, scratch))
    if agent == "openclaw":
        entry = (project / ".slash/agents/openclaw.json").read_text().strip()
        run(["openclaw", "mcp", "set", "slashengine", entry], cwd=project)
    started = datetime.datetime.now(datetime.timezone.utc)
    try:
        out = run(command, cwd=project, env=env, timeout=args.timeout)
        log = out.stdout + ("\n--- stderr ---\n" + out.stderr if out.stderr else "")
        exit_code = out.returncode
    except subprocess.TimeoutExpired as error:
        log = (error.stdout or b"").decode(errors="replace") if isinstance(error.stdout, bytes) \
            else (error.stdout or "")
        exit_code = None
    finally:
        if agent == "openclaw":
            run(["openclaw", "mcp", "unset", "slashengine"], cwd=project)
    record["seconds"] = round((datetime.datetime.now(datetime.timezone.utc) - started)
                              .total_seconds(), 1)
    record["started"] = started.strftime("%Y-%m-%dT%H:%M:%SZ")
    record["exit_code"] = exit_code
    record["tools"] = tool_calls(agent, log)
    entity, problems = check_scene(project, args.cli)
    if exit_code is None:
        problems.insert(0, f"timed out after {args.timeout} seconds")
    elif exit_code != 0:
        problems.insert(0, f"{program} exited with {exit_code}")
    record["result"] = "passed" if not problems else "failed"
    record["problems"] = problems
    record["entity"] = entity
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / f"{agent}.log").write_text(log)
    (args.out / f"{agent}.md").write_text(markdown(record, command))
    return record


def markdown(record, command):
    shown = [part if part != PROMPT else "<prompt>" for part in command]
    lines = [
        f"# Exit check: {record['agent']}",
        "",
        f"- Result: **{record['result']}**",
        f"- {record['program']} {record.get('version', '')}".rstrip(),
        f"- Started {record.get('started', '')}, took {record.get('seconds', '?')} s",
        f"- Command: `{' '.join(shown)}`",
        "",
        "Prompt:",
        "",
        "> " + PROMPT,
        "",
        "MCP tools called, in order: " + (", ".join(f"`{t}`" for t in record.get("tools", []))
                                          or "none recorded") + ".",
        "",
        "The saved scene's entity:",
        "",
        "```json",
        json.dumps({ENTITY: record.get("entity")}, sort_keys=True),
        "```",
    ]
    if record.get("problems"):
        lines += ["", "Problems:", ""] + [f"- {p}" for p in record["problems"]]
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n", maxsplit=1)[0])
    parser.add_argument("agents", nargs="*", default=["all"],
                        help=f"agents to check: {', '.join(AGENTS)} or all")
    parser.add_argument("--cli", default="build/dev/tools/slash-cli/slash-cli")
    parser.add_argument("--mcp", default="build/dev/tools/slash-mcp/slash-mcp")
    parser.add_argument("--out", type=pathlib.Path, default=pathlib.Path("build/exit-check"))
    parser.add_argument("--timeout", type=int, default=600, help="seconds per agent")
    args = parser.parse_args()
    agents = AGENTS if "all" in args.agents else args.agents
    for agent in agents:
        if agent not in AGENTS:
            parser.error(f"unknown agent {agent!r}; choose from {', '.join(AGENTS)}")
    args.cli = str(pathlib.Path(args.cli).resolve())
    args.mcp = str(pathlib.Path(args.mcp).resolve())
    args.out = args.out.resolve()
    with tempfile.TemporaryDirectory(prefix="slash-exit-check-") as folder:
        records = [check(agent, args, pathlib.Path(folder)) for agent in agents]
    json.dump([{k: v for k, v in r.items() if k != "entity"} for r in records], sys.stdout,
              indent=2)
    print()
    return 1 if any(r["result"] == "failed" for r in records) else 0


if __name__ == "__main__":
    sys.exit(main())
