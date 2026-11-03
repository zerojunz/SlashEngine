# Playtest agent

SlashEngine's own playtest agent plays a game the way [Autopilot](../autopilot-playtest/) does,
with any model `providers.json` names (Claude, GPT, Gemini, or a local model through Ollama or
llama.cpp) instead of Autopilot's agent service and its hosted decision model. It needs no
Autopilot install and no Aura account:

```bash
# providers.json names the model, e.g. a "playtester" role on Anthropic's API or a local server
slash-cli run --project . --headless --autopilot-run verify \
    --autopilot-prompt "The HUD counts every coin the player takes" \
    --autopilot-agent playtester
echo $?    # 0 confirmed, 2 denied or bugs found, 3 inconclusive, 1 error
cat .slash/autopilot/*/report.md
```

The agent is an agent service inside the game: it talks to `slash::autopilot::Bridge` over an
in-process connection, so every request Autopilot makes (what the player sees, keys, moves,
clicks, pictures, the project's files) is a tool the model calls, and the bridge's detectors,
report files and exit codes work the same. The game is paused while the model thinks and runs
only inside the tools that take time, so a slow model plays as well as a fast one.

[main.cpp](main.cpp) runs a verify run with a stand-in for the model, a few rules that read what
the tools answer, so it needs no key and plays the same every time. The game
([main.scene.json](main.scene.json)) is a Start button, then a walker who picks up coins under a
HUD that counts them out of 3, though the level has 4. [expected.txt](expected.txt) holds the
output:

```text
turn 1 sees the brief: 18 tools, a Start button on screen: yes
  > ui_act {"id":"start"}
  > key {"key":"D","mode":"hold","seconds":2}
  > look {}
turn 2 sees:
  {"via":"mouse"}
  {"held_seconds":2.0}
  {"pose":{"at":{"X":282.0,...},"screen":{"texts":{"hud.coins":"Coins 4/3"},"ui":[]}}
  The detectors found:
  - HUD text hud.coins shows 4/3: more than its stated maximum (hud_fraction)
  > note {"text":"D walks right; the level has 4 coins."}
  > report {"evidence":[...],"outcome":"DENIED","summary":"..."}
exit code 2
```

- The first turn's brief holds the request, the game's context (the player, its camera and the
  keys project.json binds), what is on screen, the project in brief and the notes earlier runs
  saved, so the model's first turn goes to playing.
- Several tool calls in one turn run in order. `key` with `"mode": "hold"` holds the key for
  `seconds` of game time while the game plays; `look` reads where the player is and the screen.
- The bridge's detectors add what they find to the next results: here the HUD's `4/3`.
- `note` keeps a fact in `.slash/autopilot/notes.md` for later runs, and `report` ends the run:
  the report and `result.json` go in the report folder and the outcome is the exit code.

[docs/reference/autopilot.md](../../docs/reference/autopilot.md) lists the agent's tools and
flags.
