# Autopilot playtests

[Autopilot](https://github.com/ramenvr/autopilot) (Apache 2.0) is an agent that plays a game to
explore it for bugs, check a claim about it or reach a goal in it, and writes a report. Its agent
service (`autopilot serve`, or the Aura app) listens on a WebSocket and engine plugins connect out
to it. `slash::autopilot::Bridge` is SlashEngine's plugin, a port of Autopilot's Godot addon, and
`slash-cli run` connects any project to it:

```bash
autopilot serve &                       # Autopilot's agent service, with your model's key
slash-cli run --project . --headless --autopilot-run verify \
    --autopilot-prompt "The HUD counts every coin the player takes"
echo $?                                 # 0 confirmed, 2 denied or bugs found, 3 inconclusive, 1 error
```

[main.cpp](main.cpp) runs the same thing with a scripted agent service on an in-process
connection, so it needs no model and plays the same every time. The game
([main.scene.json](main.scene.json)) is a Start button, then a walker who picks up coins under a
HUD that counts them out of 3, though the level has 4. [expected.txt](expected.txt) holds the
output:

```text
hello: engine godot, project Coin Walk, run {"claim":"The HUD counts every coin the player takes","kind":"verify"}
> ui_act {"id":"start"}
  {"via":"mouse"}
> key {"key":"D","mode":"hold","seconds":2}
> screen {}
  {"texts":{"hud.coins":"Coins 4/3"}}
finding: HUD text hud.coins shows 4/3: more than its stated maximum (hud_fraction)
finished yes, exit code 2, report in report
```

- The hello says `"engine": "godot"`: SlashEngine's scenes follow Godot's, so Autopilot plays it
  with what it knows of Godot games. Answers are in Autopilot's frame, as the Godot addon's: in
  2D, x stays X and y (down the screen) is -Z.
- `context` names the player (the body the camera follows, else an entity named `player`, else
  a character body), whether the game is 2D or 3D and the keys project.json binds. `ui` lists the
  controls on screen with where to click, and `ui_act` clicks them with the mouse, as a player
  would; a button that doesn't take the click is pressed as its signal would be.
- `key` holds keys by Unreal's names (`D`, `SpaceBar`, `LeftMouseButton`, `Gamepad_LeftX`) as
  input events, by lease or for `seconds` of game time; `input` presses actions by name and
  `move` walks by the actions W, A, S and D are bound to.
- The bridge's detectors report on their own: engine errors in the log, NaN transforms, bodies
  that fall out of the world, long falls, hitches and, here, HUD text such as `4/3` over its
  maximum.
- The result writes `result.json` and the report's files into the report folder
  (`--autopilot-report-dir`, else `.slash/autopilot/<time>` in the project) and becomes the exit
  code.

[docs/reference/autopilot.md](../../docs/reference/autopilot.md) lists every op and flag.
