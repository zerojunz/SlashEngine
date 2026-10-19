# Gameplay code

A game's own C++ code, changed while the game runs. The project's `gameplay.sources` in
[project.json](project.json) names [src/](src/game.cpp), whose `SLASH_GAME_MODULE` function
registers a `Wallet` component and an `earn` system. [main.cpp](main.cpp) drives a session as an
agent would:

1. `gameplay.build` compiles the code and swaps it in: `"reload": {"added": ["Wallet"]}`. The
   scene that uses `Wallet` then opens, and 60 ticks pay each wallet its income.
2. It rewrites `src/game.cpp` so wallets also earn a bonus and builds again. The world keeps both
   entities and their coins; `Wallet` is listed as `changed` and gains `bonus`.
3. It misspells a field. The build fails with the error's file and line (the message is the
   compiler's own), and the code that was in keeps running.

- Gameplay code is plain C++ against the engine's headers, as Godot's scripts are; no scripting
  language. Exports link it into the game; the tools load it as a library (macOS and Linux).
- Keep gameplay state in registered components: rebuilding keeps each field the new code still
  has and reports in `dropped` any value it could not keep.
- `slash-cli build` does the same from a shell.
