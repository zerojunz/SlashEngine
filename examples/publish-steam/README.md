# Publish to Steam

[export_presets.json](export_presets.json)'s `stores.steam` says where a game's builds go on
Steam: the game's app id and a depot for each preset (or a platform's presets), all from the
app's Steamworks pages, plus the `beta` branch each build is set live on. Steam takes Windows,
macOS and Linux builds, so the Steam Deck gets the Linux one.

[calls.jsonl](calls.jsonl) asks `publish.run` for plans, which upload nothing:

1. Every depot: each build's preset, platform, depot and output folder, the app build script an
   upload writes (one script for every depot, with project.json's version as its description)
   and the `steamcmd` command that runs it.
2. Only the `windows` preset, as a beta version.
3. The `phone` preset, which Steam refuses: it takes no iOS games.

```bash
slash-cli rpc --project . < calls.jsonl
slash-cli publish steam                     # the same plan from the command line
```

An upload exports each preset, writes the script to `.slash/publish/steam/` and runs steamcmd,
which uploads every depot in one build that players on the branch then get, so an agent asks
first. It needs steamcmd (on `PATH` or in `SLASH_STEAMCMD_PATH`) and the Steamworks account to
sign in as in `STEAM_USERNAME`. steamcmd keeps the account's login once it has been signed in by
hand (`steamcmd +login <account>`, where Steam Guard asks for its code), so no password is
passed:

```bash
slash-cli publish steam --upload            # export every depot's preset and upload them
slash-cli publish steam --upload --no-build # upload the last exports again
```

`publish.run` with `"upload": true` does the same; it needs the build and network capabilities.
