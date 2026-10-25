#!/usr/bin/env python3
"""Nudge an agent (main session or subagent) that has spent too long on one task. Claude Code hook handler.

Silent (no output, no tokens) until a task passes the limit (default 30 min); then one short line is added to
the agent's context, again every further 30 min on the same task. Only inside a project folder (a parent of
the working directory, below $HOME, holds .git or CLAUDE.md).

  task_nudge.py hook                 the hook itself (reads the hook JSON on stdin)
  task_nudge.py install [--project DIR] [--dry-run]   copy this file to ~/.claude/hooks/ and add the hooks
                                     to ~/.claude/settings.json (or DIR/.claude/settings.local.json)
  task_nudge.py uninstall [--project DIR]             remove the hooks again
  task_nudge.py status               show tracked tasks of recent sessions

What counts as one task: main session, from a real user message (not a background-task notification) or
from a TaskUpdate that starts or completes a task-list item; subagent, from its start or its own TaskUpdate.
Settings: TASK_NUDGE_MIN (default 30), TASK_NUDGE_OFF=1, or a file .claude/no-task-nudge in the project.
Standard library only. Never fails the tool call: any error means silence.
"""
import fcntl, json, os, re, sys, time

LIMIT_MIN = float(os.environ.get("TASK_NUDGE_MIN", "30"))
STATE_DIR = os.environ.get("TASK_NUDGE_STATE") or os.path.join(os.path.expanduser("~"), ".cache", "task-nudge")
NOTIFICATION = re.compile(r"^\s*(<task-notification|<agent-message|\[SYSTEM NOTIFICATION)")
EVENTS = {"UserPromptSubmit": None, "SubagentStart": None, "SubagentStop": None, "PostToolUse": "*"}

MAIN = ("[task-nudge] {m} min on one task{label}. Quick check, no reply needed if all is well: "
        "same error or step twice? Change approach or ask Dan. Waiting on a long run you started? Carry on. "
        "Dan has heard nothing for 30 min? Send him one line.")
SUB = ("[task-nudge] {m} min on this task{label}. If you are stuck or going in circles, stop and hand back "
       "what you have and what blocks you. Otherwise carry on.")
ORCH = "[task-nudge] subagent {kind} {aid} has run {m} min on one task and was nudged; check on it only if you need its result now."


def now():
    return float(os.environ.get("TASK_NUDGE_NOW") or time.time())


def in_project(cwd):
    home = os.path.realpath(os.path.expanduser("~"))
    d = os.path.realpath(cwd or ".")
    while d not in (home, os.path.dirname(d)):          # stop at $HOME and at the filesystem root
        if os.path.exists(os.path.join(d, ".claude", "no-task-nudge")):
            return False
        if os.path.exists(os.path.join(d, ".git")) or os.path.exists(os.path.join(d, "CLAUDE.md")):
            return True
        d = os.path.dirname(d)
    return False


def start(rec, t, label=None):
    rec.update(start=t, nudged=0, label=label)


def handle(ev):
    if os.environ.get("TASK_NUDGE_OFF") == "1" or not in_project(ev.get("cwd")):
        return None
    name, sid, aid, t = ev.get("hook_event_name"), ev.get("session_id") or "none", ev.get("agent_id"), now()
    os.makedirs(STATE_DIR, exist_ok=True)
    path = os.path.join(STATE_DIR, re.sub(r"[^\w.-]", "_", sid) + ".json")
    with open(path + ".lock", "w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        try:
            st = json.load(open(path))
        except (OSError, ValueError):
            st = {}
        st.setdefault("main", {}), st.setdefault("agents", {}), st.setdefault("subjects", {}), st.setdefault("notes", [])
        out = step(st, name, aid, ev, t)
        tmp = path + ".tmp"
        json.dump(st, open(tmp, "w"))
        os.replace(tmp, path)
    if name == "UserPromptSubmit":
        prune()
    return out


def step(st, name, aid, ev, t):
    rec = st["agents"].setdefault(aid, {"kind": ev.get("agent_type") or "agent"}) if aid else st["main"]
    if name == "UserPromptSubmit":
        if not NOTIFICATION.match(ev.get("prompt") or "") or "start" not in rec:
            start(rec, t)
        return None
    if name == "SubagentStart":
        start(rec, t)
        return None
    if name == "SubagentStop":
        st["agents"].pop(aid, None)
        st["notes"] = [n for n in st["notes"] if n.get("aid") != aid]
        return None
    if name != "PostToolUse":
        return None
    tool, tin = ev.get("tool_name"), ev.get("tool_input") or {}
    if tool == "TaskCreate":
        resp = json.dumps(ev.get("tool_response") or "")
        found = re.search(r"#(\w+)", resp) or re.search(r'"id": "?(\w+)', resp)
        if found and tin.get("subject"):
            st["subjects"][found.group(1)] = tin["subject"][:50]
            st["subjects"] = dict(list(st["subjects"].items())[-30:])
    if tool == "TaskUpdate" and tin.get("status") in ("in_progress", "completed"):
        tid = str(tin.get("taskId", ""))
        label = None
        if tin["status"] == "in_progress" and tid:
            label = f"task #{tid}" + (f" ({st['subjects'][tid]})" if tid in st["subjects"] else "")
        start(rec, t, label)
        return None
    if "start" not in rec:            # first sight of this agent (hook installed mid-task): start counting now
        start(rec, t)
        return None
    lines = []
    if not aid and st["notes"]:       # the orchestrator hears once about a long-running subagent it can still act on
        lines += [n["text"] for n in st["notes"]]
        st["notes"] = []
    mins = (t - rec["start"]) / 60
    if mins >= LIMIT_MIN and (not rec.get("nudged") or t - rec["nudged"] >= LIMIT_MIN * 60):
        label = f": {rec['label']}" if rec.get("label") else ""
        lines.append((SUB if aid else MAIN).format(m=int(mins), label=label))
        if aid and not rec.get("nudged"):
            st["notes"].append({"aid": aid, "text": ORCH.format(kind=rec.get("kind", "agent"), aid=aid, m=int(mins))})
        rec["nudged"] = t
    return "\n".join(lines) or None


def prune():
    t = time.time()   # file ages are real time, whatever the test clock says
    try:
        for e in os.scandir(STATE_DIR):
            if t - e.stat().st_mtime > 3 * 86400:
                os.remove(e.path)
    except OSError:
        pass


def hook():
    try:
        ev = json.load(sys.stdin)
        text = handle(ev)
        if text:
            print(json.dumps({"hookSpecificOutput": {"hookEventName": ev["hook_event_name"], "additionalContext": text}}))
    except Exception:
        pass
    return 0


def settings_path(project):
    if project:
        return os.path.join(os.path.abspath(project), ".claude", "settings.local.json")
    return os.path.join(os.path.expanduser("~"), ".claude", "settings.json")


def edit_settings(project, add, dry):
    import shutil
    target = os.path.join(os.path.expanduser("~"), ".claude", "hooks", "task_nudge.py")
    cmd = 'python3 "$HOME/.claude/hooks/task_nudge.py" hook'
    path = settings_path(project)
    data = {}
    if os.path.exists(path) and os.path.getsize(path):
        try:
            data = json.load(open(path))
        except ValueError as e:
            print(f"error: {path} is not valid JSON ({e}); not touching it", file=sys.stderr)
            return 2
    hooks = data.setdefault("hooks", {})
    for event, matcher in EVENTS.items():
        groups = [g for g in hooks.get(event, []) if not any(h.get("command") == cmd for h in g.get("hooks", []))]
        if add:
            g = {"hooks": [{"type": "command", "command": cmd, "timeout": 5}]}
            groups.append({"matcher": matcher, **g} if matcher else g)
        if groups:
            hooks[event] = groups
        else:
            hooks.pop(event, None)
    if not hooks:
        data.pop("hooks")
    text = json.dumps(data, indent=2) + "\n"
    if dry:
        print(text, end="")
        return 0
    if add and os.path.realpath(__file__) != os.path.realpath(target):
        os.makedirs(os.path.dirname(target), exist_ok=True)
        shutil.copyfile(__file__, target)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    open(path + ".tmp", "w").write(text)
    os.replace(path + ".tmp", path)
    print(f"{'installed' if add else 'removed'}: hooks {', '.join(EVENTS)} in {path}" + (f"; script at {target}" if add else ""))
    print("New sessions pick it up; a session already running needs a restart (or /hooks to reload).")
    return 0


def status():
    t = now()
    try:
        files = sorted((e for e in os.scandir(STATE_DIR) if e.name.endswith(".json")), key=lambda e: -e.stat().st_mtime)[:5]
    except OSError:
        files = []
    if not files:
        print("no sessions tracked yet")
    for e in files:
        st = json.load(open(e.path))
        print(f"session {e.name[:-5]}")
        for who, rec in [("main", st.get("main", {}))] + [(f"subagent {k}", v) for k, v in st.get("agents", {}).items()]:
            if "start" in rec:
                print(f"  {who}: {(t - rec['start']) / 60:.0f} min on {rec.get('label') or 'current task'}"
                      + (f", nudged {(t - rec['nudged']) / 60:.0f} min ago" if rec.get("nudged") else ""))
    return 0


def main(argv):
    cmd = argv[1] if len(argv) > 1 else ""
    project = argv[argv.index("--project") + 1] if "--project" in argv else None
    if cmd == "hook":
        return hook()
    if cmd in ("install", "uninstall"):
        return edit_settings(project, cmd == "install", "--dry-run" in argv)
    if cmd == "status":
        return status()
    print(__doc__)
    return 0 if cmd in ("-h", "--help") else 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
