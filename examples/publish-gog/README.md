# Publish to GOG

[export_presets.json](export_presets.json)'s `stores.gog` says where a game's builds go on GOG:
the game's product id from the GOG Developer Portal, the languages its depot holds, and the
private `staging` branch each build is published to. GOG takes Windows and macOS builds; with no
`presets` listed, every preset it takes goes.

[calls.jsonl](calls.jsonl) asks `publish.run` for plans, which upload nothing:

1. Every Windows and macOS preset: each build's preset, platform and output folder, the Pipeline
   Builder project an upload writes for it (the product, one depot with the output and the task
   that starts the game) and the `GOGGalaxyPipelineBuilder build-game` command that builds and
   uploads it, with project.json's version.
2. Only the `windows` preset, as version 1.4.1.
3. The `steam-deck` preset, which GOG refuses here: it takes no Linux builds through Pipeline
   Builder.

```bash
slash-cli rpc --project . < calls.jsonl
slash-cli publish gog                       # the same plan from the command line
```

An upload exports each preset, writes its project to `.slash/publish/gog/` and runs Pipeline
Builder, which puts the build on the branch for its testers, so an agent asks first. It needs
Pipeline Builder (on `PATH` or in `SLASH_GOG_PIPELINE_BUILDER_PATH`) and the GOG account to sign
in as, in `GOG_USERNAME` and `GOG_PASSWORD`; a password-protected branch's password goes in
`GOG_BRANCH_PASSWORD`. Pipeline Builder's output is reported with the passwords taken out:

```bash
slash-cli publish gog --upload              # export each preset and upload it to staging
slash-cli publish gog --upload --no-build   # upload the last exports again
```

`publish.run` with `"upload": true` does the same; it needs the build and network capabilities.
Pipeline Builder publishes to private branches only: a build goes live on master from the
Developer Portal.
