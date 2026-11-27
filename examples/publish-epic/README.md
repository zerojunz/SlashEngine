# Publish to the Epic Games Store

[export_presets.json](export_presets.json)'s `stores.epic` says where a game's builds go on the
Epic Games Store: the organization and product ids, an artifact for each preset (or a platform's
presets), and the `Live` label and sandbox each upload is set live in, all from the game's Epic
Developer Portal pages. The store takes Windows and macOS builds; one artifact holds both here.

[calls.jsonl](calls.jsonl) asks `publish.run` for plans, which upload nothing:

1. Every artifact: each build's preset, platform, artifact, output folder, the program Epic's
   launcher starts, and the build version it goes up as. An artifact takes each version once,
   whatever its platform, so the version is project.json's with the platform after it
   (`1.4.0-Mac`, `1.4.0-Windows`). Each build has two BuildPatchTool commands: one uploads it,
   one labels it `Live`.
2. Only the `windows` preset, as version 1.4.1.
3. The `steam-deck` preset, which the store refuses: it takes no Linux games.

```bash
slash-cli rpc --project . < calls.jsonl
slash-cli publish epic                      # the same plan from the command line
```

An upload exports each preset and runs BuildPatchTool for it, so players get it once it is
labelled `Live`: an agent asks first. It needs BuildPatchTool (on `PATH` or in
`SLASH_BUILDPATCHTOOL_PATH`) and a BuildPatchTool client from the product's BuildPatchTool
Credentials page, its id in `EPIC_CLIENT_ID` and its secret in `EPIC_CLIENT_SECRET`.
BuildPatchTool reads the secret from its variable, so it never shows in a command line:

```bash
slash-cli publish epic --upload             # export each preset, upload it and label it Live
slash-cli publish epic --upload --no-build  # upload the last exports again
```

`publish.run` with `"upload": true` does the same; it needs the build and network capabilities.
Without `label`, uploads wait in the Portal to be labelled.
