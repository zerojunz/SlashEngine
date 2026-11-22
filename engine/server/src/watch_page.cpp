#include "watch_page.hpp"

#include "slash/core/json.hpp"

#include <array>
#include <string>
#include <string_view>

namespace slash::server {
namespace {

// The page, in pieces: MSVC takes string literals of at most 16 KiB. It reads the bearer token
// from the URL's fragment, which browsers never send, and asks the session's watch server for
// the journal (work.feed), the summary (work.digest), the plan (work.plan, where the engine has
// it) and the frame (frame.capture), as any watcher does.
constexpr std::string_view kHead = R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>SlashEngine: watching</title>
<link rel="icon" href="data:,">
<style>
:root {
  --bg: #f6f7f9; --panel: #ffffff; --text: #1d2330; --muted: #677085; --line: #e2e5eb;
  --added: #1f9d55; --changed: #c27c0e; --removed: #c43c3c; --code: #eef0f4;
}
@media (prefers-color-scheme: dark) {
  :root {
    --bg: #12151b; --panel: #1a1e26; --text: #e6e9ef; --muted: #98a1b3; --line: #2a303b;
    --added: #48c47c; --changed: #e5a33b; --removed: #ec6a6a; --code: #232834;
  }
}
* { box-sizing: border-box; }
body { margin: 0; background: var(--bg); color: var(--text);
       font: 14px/1.45 system-ui, -apple-system, "Segoe UI", Roboto, sans-serif; }
header { display: flex; flex-wrap: wrap; gap: 8px 16px; align-items: baseline;
         padding: 12px 16px; border-bottom: 1px solid var(--line); background: var(--panel); }
header h1 { font-size: 16px; margin: 0; }
.pill { font-size: 12px; padding: 1px 8px; border-radius: 999px; border: 1px solid var(--line);
        color: var(--muted); }
.pill.live { color: var(--added); border-color: var(--added); }
.muted { color: var(--muted); }
main { display: grid; grid-template-columns: minmax(0, 3fr) minmax(0, 2fr); gap: 16px;
       padding: 16px; }
@media (max-width: 820px) { main { grid-template-columns: minmax(0, 1fr); } }
section { background: var(--panel); border: 1px solid var(--line); border-radius: 8px;
          padding: 12px; margin-bottom: 16px; min-width: 0; }
section h2 { font-size: 13px; margin: 0 0 8px; text-transform: uppercase; letter-spacing: .04em;
             color: var(--muted); }
#frame { width: 100%; height: auto; display: block; border-radius: 4px; background: var(--code);
         image-rendering: auto; min-height: 120px; }
button { font: inherit; padding: 4px 10px; border-radius: 6px; border: 1px solid var(--line);
         background: var(--code); color: var(--text); cursor: pointer; }
ol { list-style: none; margin: 0; padding: 0; }
#timeline li { padding: 6px 0; border-top: 1px solid var(--line); overflow-wrap: anywhere; }
#timeline li:first-child { border-top: 0; }
.time { color: var(--muted); font-variant-numeric: tabular-nums; margin-right: 6px; }
code { background: var(--code); border-radius: 4px; padding: 0 4px; font-size: 12.5px; }
.chip { display: inline-block; font-size: 12px; padding: 0 6px; margin: 2px 4px 0 0;
        border-radius: 4px; border: 1px solid currentColor; }
.added { color: var(--added); } .changed { color: var(--changed); }
.removed, .failing { color: var(--removed); } .ok { color: var(--added); }
.reads { color: var(--muted); }
#checks li, #plan li { padding: 3px 0; }
#summary { margin: 0; padding-left: 18px; }
#strip { display: flex; gap: 8px; overflow-x: auto; padding-bottom: 4px; }
.thumb { flex: 0 0 auto; width: 132px; padding: 4px; text-align: left; font-size: 12px; }
.thumb img, .pair img { width: 100%; display: block; border-radius: 4px; background: var(--code); }
.thumb.picked { border-color: var(--changed); }
.pair { display: grid; grid-template-columns: 1fr 1fr; gap: 8px; margin-top: 10px; }
.row { display: flex; flex-wrap: wrap; gap: 6px; margin-top: 6px; }
.row input { flex: 1 1 160px; min-width: 0; font: inherit; padding: 4px 8px; border-radius: 6px;
             border: 1px solid var(--line); background: var(--bg); color: var(--text); }
.pill.paused, #steer-state.paused { color: var(--changed); border-color: var(--changed); }
</style>
</head>
)HTML";

constexpr std::string_view kBody = R"HTML(<body>
<header>
  <h1>Watching <span id="session"></span></h1>
  <span id="state" class="pill">connecting</span>
  <span id="who" class="muted"></span>
  <span id="updated" class="muted"></span>
</header>
<main>
  <div>
    <section>
      <h2>The world now</h2>
      <img id="frame" alt="The agent's world as its window shows it">
      <p class="muted"><span id="frame-note">No frame yet.</span>
        <button id="refresh" type="button">Draw again</button></p>
    </section>
    <section id="checkpoints-section" hidden>
      <h2>Checkpoints</h2>
      <div id="strip"></div>
      <div id="compare"></div>
    </section>
    <section>
      <h2>So far</h2>
      <ul id="summary"><li class="muted">Reading the journal.</li></ul>
    </section>
  </div>
  <div>
    <section id="steer-section" hidden>
      <h2>Steer</h2>
      <p id="steer-state" class="muted">The agent is working.</p>
      <div class="row"><input id="why" maxlength="500" aria-label="Why pause"
        placeholder="Why, for the agent"><button id="pause" type="button">Pause</button><button
        id="resume" type="button" hidden>Resume</button></div>
      <div class="row"><input id="note" maxlength="2000" aria-label="A note for the agent"
        placeholder="A note for the agent"><button id="send" type="button">Send note</button></div>
    </section>
    <section id="plan-section" hidden>
      <h2>The agent's plan</h2>
      <p id="plan-task" class="muted"></p>
      <ol id="plan"></ol>
    </section>
    <section>
      <h2>Tests and checks</h2>
      <ol id="checks"><li class="muted">None run yet.</li></ol>
    </section>
    <section>
      <h2>What it did, newest first</h2>
      <ol id="timeline"></ol>
    </section>
  </div>
</main>
)HTML";

constexpr std::string_view kScript = R"HTML(<script>
"use strict";
const SESSION = __SESSION__;
const token = new URLSearchParams(location.hash.slice(1)).get("token") || "";
const $ = (id) => document.getElementById(id);
$("session").textContent = SESSION;
let next = 0, live = true, framed = 0, wantFrame = true;
let lastCheckpoint = 0, wantCheckpoints = true;
const checks = new Map();

async function call(name, params) {
  const response = await fetch("/v1/commands/" + name, {
    method: "POST", headers: {"Authorization": "Bearer " + token,
                              "Content-Type": "application/json"},
    body: JSON.stringify(params || {})});
  const body = await response.json();
  if (!response.ok) throw Object.assign(new Error(body.error ? body.error.message : "failed"),
                                        {status: response.status});
  return body;
}

function el(tag, cls, text) {
  const node = document.createElement(tag);
  if (cls) node.className = cls;
  if (text !== undefined) node.textContent = text;
  return node;
}

// As the journal judges a result: an error fails it, then ok, passed or valid, then failed
// counts, then lists of problems.
function passed(line) {
  if (line.error) return false;
  const r = line.result;
  if (!r || typeof r !== "object") return true;
  for (const flag of ["ok", "passed", "valid"]) if (typeof r[flag] === "boolean") return r[flag];
  if (Array.isArray(r.failed)) return r.failed.length === 0;
  if (Number.isInteger(r.failed)) return r.failed === 0;
  return !["failures", "errors", "problems", "findings", "broken", "drift"].some((k) =>
    (Array.isArray(r[k]) && r[k].length > 0) || (Number.isInteger(r[k]) && r[k] > 0));
}

function clock(time) { return time ? time.slice(11, 19) : ""; }

function chips(item, line) {
  const c = line.changes || {};
  for (const id of c.added || []) item.append(el("span", "chip added", "+" + id));
  for (const id of c.removed || []) item.append(el("span", "chip removed", "-" + id));
  for (const [id, parts] of Object.entries(c.changed || {}))
    item.append(el("span", "chip changed", "~" + id + "(" + Object.keys(parts).join(", ") + ")"));
  for (const more of ["added_more", "removed_more", "changed_more"])
    if (c[more]) item.append(el("span", "chip muted", "+" + c[more] + " more"));
  for (const file of line.files || []) item.append(el("span", "chip muted", file));
}

function show(line) {
  const item = el("li");
  item.append(el("span", "time", clock(line.time)));
  if (line.start) {
    item.append(el("span", "muted", "session started (" + (line.start.host || "") + ")"));
    $("who").textContent = line.start.host || "";
  } else if (line.end) {
    item.append(el("span", "muted", "session ended"));
  } else if (line.reads) {
    const names = Object.entries(line.reads).map(([n, k]) => k > 1 ? n + " x" + k : n);
    item.append(el("span", "reads", "read: " + names.join(", ")));
  } else if (line.checkpoint) {
    item.append(el("span", "ok", "checkpoint " + line.checkpoint + ": " +
                                  (line.label || line.reason || "")));
    wantCheckpoints = true;
  } else if (line.paused) {
    item.append(el("span", "changed", "paused by a person watching: " + (line.paused.why || "")));
  } else if (line.resumed) {
    item.append(el("span", "ok", "resumed by a person watching"));
  } else if (typeof line.note === "string") {
    item.append(el("span", "changed", "note from a person watching: " + line.note));
  } else if (line.plan) {
    const steps = line.plan.steps || [];
    item.append(el("span", "muted", "plan: " + (line.plan.task ? line.plan.task + ", " : "") +
                                    steps.filter((s) => s.done).length + " of " + steps.length +
                                    " steps done"));
  } else if (line.event) {
    item.append(el("span", "muted", line.event));
  } else {
    if (line.client) $("who").textContent = ($("who").textContent.split(" (")[0]) +
                                            " (" + line.client + ")";
    item.append(el("code", "", line.command || ""));
    if (line.params && Object.keys(line.params).length)
      item.append(" ", el("span", "muted", JSON.stringify(line.params)));
    if (line.result) {
      const ok = passed(line);
      item.append(" ", el("span", ok ? "ok" : "failing", ok ? "ok" : "failing"));
      checks.set(line.command, {ok, time: line.time});
    }
  }
  item.append(el("br"));
  chips(item, line);
  if (line.error) item.append(el("div", "failing", "FAILED: " + line.error.message));
  $("timeline").prepend(item);
  if (line.changes || line.files) wantFrame = true;
}

function showChecks() {
  if (!checks.size) return;
  const list = $("checks");
  list.replaceChildren();
  for (const [name, check] of checks) {
    const item = el("li");
    item.append(el("span", check.ok ? "ok" : "failing", check.ok ? "ok  " : "failing  "),
                el("code", "", name), " ", el("span", "time", clock(check.time)));
    list.append(item);
  }
}

async function readFeed() {
  let more = true;
  while (more) {
    const page = await call("work.feed", {session: SESSION, after: next, limit: 200});
    for (const line of page.lines) show(line);
    next = page.next;
    more = page.more;
    live = page.live;
  }
  showChecks();
  $("state").textContent = live ? "live" : "ended";
  $("state").className = live ? "pill live" : "pill";
  $("updated").textContent = "updated " + new Date().toLocaleTimeString();
}

async function readSummary() {
  const digest = await call("work.digest", {session: SESSION, since: "30d"});
  const list = $("summary");
  list.replaceChildren(...(digest.lines || []).map((text) => el("li", "", text)));
  const me = (digest.sessions || []).find((one) => one.id === SESSION);
  showPaused(me && me.paused);
}

// Pausing and notes reach the agent through the session's own server, as any watcher's call.
function showPaused(paused) {
  $("steer-section").hidden = !live;
  $("pause").hidden = !!paused;
  $("resume").hidden = !paused;
  $("steer-state").textContent = paused ? "Paused: " + (paused.why || "") +
                                          ". Its edits fail with this until you resume."
                                        : "The agent is working.";
  $("steer-state").className = paused ? "paused" : "muted";
}

async function steer(name, params, done) {
  try {
    await call(name, params);
    if (done) done();
  } catch (error) {
    $("steer-state").textContent = "Couldn't " + name.slice(5) + ": " + error.message;
    $("steer-state").className = "failing";
  }
  tick();
}

$("pause").onclick = () => steer("work.pause", {why: $("why").value.trim() ||
                                                    "Paused from the watch page"},
                                 () => { $("why").value = ""; });
$("resume").onclick = () => steer("work.resume", {});
$("send").onclick = () => {
  const text = $("note").value.trim();
  if (text) steer("work.note", {text}, () => { $("note").value = ""; });
};

// An engine without work.plan answers 404 once, and the page stops asking.
let hasPlan = true;
async function readPlan() {
  if (!hasPlan) return;
  try {
    const plan = await call("work.plan", {session: SESSION});
    const steps = plan.steps || [];
    $("plan-section").hidden = steps.length === 0 && !plan.task;
    $("plan-task").textContent = (plan.task || "") +
        (plan.status && plan.status !== "working" ? " (" + plan.status + ")" : "");
    $("plan").replaceChildren(...steps.map((step) =>
      el("li", step.done ? "ok" : "", (step.done ? "\u2713 " : "\u25CB ") + step.text)));
  } catch (error) {
    $("plan-section").hidden = true;
    if (error.status === 404) hasPlan = false;
  }
}

async function readFrame() {
  const frame = await call("frame.capture", {image: true});
  $("frame").src = "data:image/png;base64," + frame.png;
  $("frame-note").textContent = "Drawn at " + new Date().toLocaleTimeString() + ", " +
                                frame.width + " x " + frame.height + ".";
  framed = Date.now();
  wantFrame = false;
}

function caption(c) {
  return c.n + "  " + clock(c.time) + "  " + (c.label || c.reason);
}

function chipList(target, cls, prefix, items) {
  for (const item of items || []) target.append(el("span", "chip " + cls, prefix + item));
}

async function pick(n, button) {
  for (const other of document.querySelectorAll(".thumb")) other.classList.remove("picked");
  button.classList.add("picked");
  const out = $("compare");
  try {
    const c = await call("work.compare", {session: SESSION, to: n, image: true});
    out.replaceChildren();
    const files = c.files || {};
    const entities = c.entities || {};
    const what = el("div");
    what.append(el("span", "muted", c.from.n === c.to.n ? "The first checkpoint. "
                                   : "From " + c.from.n + " to " + c.to.n + ": "));
    chipList(what, "added", "+", files.added);
    chipList(what, "removed", "-", files.removed);
    chipList(what, "changed", "~", (files.changed || []).map((f) => f.path + " " + f.lines));
    chipList(what, "added", "+", entities.added);
    chipList(what, "removed", "-", entities.removed);
    chipList(what, "changed", "~", Object.keys(entities.changed || {})
                                         .filter((id) => !(entities.added || []).includes(id)));
    out.append(what);
    const pair = el("div", "pair");
    for (const side of [c.from, c.to]) {
      const figure = el("div");
      if (side.png) {
        const img = el("img");
        img.src = "data:image/png;base64," + side.png;
        img.alt = "Checkpoint " + side.n;
        figure.append(img);
      }
      figure.append(el("div", "muted", caption(side)));
      pair.append(figure);
    }
    out.append(pair);
  } catch (error) {
    out.replaceChildren(el("div", "failing", error.message));
  }
}

async function readCheckpoints() {
  wantCheckpoints = false;
  const list = await call("work.checkpoints", {session: SESSION, after: lastCheckpoint,
                                               image: true});
  for (const c of list.checkpoints || []) {
    const button = el("button", "thumb");
    button.type = "button";
    if (c.png) {
      const img = el("img");
      img.src = "data:image/png;base64," + c.png;
      img.alt = "Checkpoint " + c.n;
      button.append(img);
    }
    button.append(el("div", "", caption(c)));
    button.addEventListener("click", () => pick(c.n, button));
    $("strip").append(button);
    lastCheckpoint = Math.max(lastCheckpoint, c.n);
    $("checkpoints-section").hidden = false;
  }
}

$("refresh").addEventListener("click", () => { wantFrame = true; tick(); });

let busy = false;
async function tick() {
  if (busy) return;
  busy = true;
  try {
    await readFeed();
    if (wantFrame || (live && Date.now() - framed > 10000)) await readFrame();
    await readSummary();
    await readPlan();
    if (wantCheckpoints) await readCheckpoints();
  } catch (error) {
    $("state").textContent = token ? "ended, or out of reach" : "no token in the link";
    $("state").className = "pill failing";
  } finally {
    busy = false;
  }
}

tick();
setInterval(() => { if (live) tick(); }, 1000);
</script>
</body>
</html>
)HTML";

} // namespace

std::string watch_page(std::string_view session) {
    std::string page;
    page.reserve(kHead.size() + kBody.size() + kScript.size() + session.size() + 8);
    page += kHead;
    page += kBody;
    // The ID as a JavaScript string: a JSON string is one, and "</" can't end the script.
    std::string quoted = json::write(json::Value(std::string(session)));
    for (std::size_t at = quoted.find("</"); at != std::string::npos; at = quoted.find("</", at)) {
        quoted.replace(at, 2, "<\\/");
    }
    std::string script(kScript);
    constexpr std::string_view kMark = "__SESSION__";
    script.replace(script.find(kMark), kMark.size(), quoted);
    page += script;
    return page;
}

} // namespace slash::server
