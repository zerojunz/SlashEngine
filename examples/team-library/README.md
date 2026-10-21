# Team library

The asset library shared by a team, so what one teammate generates, uses or judges, the others
find instead of making it again. [main.cpp](main.cpp) has three teammates, each with a project
and a library on their own machine (`alice-library/` and the rest stand in for
`~/.slash/library`), and a team library in `team/`. `expected.txt` holds each call and its reply.

- Each project's `providers.json` names the team's library: a folder every teammate reaches, such
  as a network share or a folder a sync service keeps the same, or a library server's URL.

  ```json
  {"library": {"team": "/Volumes/Studio/slash-library"}}
  {"library": {"team": "http://studio-nas:7700", "token_env": "SLASH_LIBRARY_TOKEN"}}
  ```

  `SLASH_LIBRARY_TEAM` names one on a single machine instead. A library server is
  `slash-cli library serve --dir <folder> --host 0.0.0.0 --port 7700`, which prints its token;
  teammates put it in `SLASH_LIBRARY_TOKEN`. Across the internet, put the server behind HTTPS.
- What a teammate generates, refines or adds goes to the team's library with the request that
  made it, so Bob's exact repeat of Alice's request comes back `"reused": true, "team": true`
  with no model call.
- `asset.search` and the candidates `asset.generate` offers cover the team's assets too, and
  `asset.use`, `asset.info` and `asset.refine` bring one from the team's library when this
  machine lacks it.
- Feedback goes to the team's copy of the asset, so Bob's verdict ranks Alice's search and shows
  in `asset.info`, whose `shared` says whether teammates can find the asset.
- `asset.sync` shares what was made before the team was set up: the assets the project's
  `library.json` names, with the versions they came from (`"all": true` for every asset on the
  machine).
- When the team's library can't be reached, commands work on the machine's library alone and
  their replies say why in `team_error`.
