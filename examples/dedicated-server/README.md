# Dedicated server

The same game run as a player's copy and as a dedicated server: [main.cpp](main.cpp) starts the
project with `runtime::Game` as a server (`GameOptions::server`, what `--server` and a
`dedicated_server` export set) and then as a headless player, and its code hosts a match only
when `world::is_dedicated_server` says it runs as a server.

```text
preset linux: game
preset linux-server: dedicated server
-- as a server
server: hosting the match
kept to real time: yes
{"headless":true,"renderer":"null","server":true,"sprites":0,"ticks":10}
...
```

- [export_presets.json](export_presets.json)'s `linux-server` preset sets `dedicated_server`:
  `slash-cli export linux-server` builds a program that always runs as a server and leaves
  `hero.png` out of its data, since a server draws nothing.
- A server has no window, GPU or sound and ticks at the project's `tick_hz` by the clock until
  it is stopped; a headless test run ticks as fast as it can.

See [game-projects.md](../../docs/reference/game-projects.md#dedicated-servers).
