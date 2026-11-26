# Publish to itch.io

[export_presets.json](export_presets.json)'s `stores.itch` says where a game's builds go on
itch.io: the game's project, `player/star-courier` from its address
`https://player.itch.io/star-courier`, and a channel for each preset (or a platform's defaults,
as `web` is here). butler, itch.io's upload tool, tags a channel's downloads by the platform in
its name, so the Steam Deck's Linux build goes on `linux`.

[calls.jsonl](calls.jsonl) asks `publish.run` for plans, which upload nothing:

1. Every channel: each build's preset, platform, output folder, itch.io target and the
   `butler push` command an upload runs, with project.json's version.
2. Only the `windows` preset, as a beta version.
3. The `phone` preset, which itch.io refuses: it takes no iOS games.

```bash
slash-cli rpc --project . < calls.jsonl
slash-cli publish itch                      # the same plan from the command line
```

An upload exports each preset and pushes its output, which players can then download, so an agent
asks first. It needs butler (on `PATH` or in `SLASH_BUTLER_PATH`) and its API key in
`BUTLER_API_KEY`:

```bash
slash-cli publish itch --upload             # export and push every channel
slash-cli publish itch --preset windows --upload --no-build   # push the last export again
```

`publish.run` with `"upload": true` does the same; it needs the build and network capabilities.
