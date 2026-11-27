# Save data

Save data for game code, as Unreal's save game slots: [main.cpp](main.cpp) starts this project headless, then saves, lists, loads and deletes
slots with `slash/platform/save_data.hpp`, and reads and seeds them as an agent does with the
`save.*` commands.

```text
folder: .slash/saves/ (headless)
slots: autosave (version 1.4.0) slot-1 (version 1.4.0)
slot-1: {"health":72,"inventory":["key","lantern"],"level":3}, written by 1.4.0
...
```

- `runtime::Game::start` sets the save folder: the player's folder for the game
  (`platform::user_save_folder`, by the project's name) in a window, and the project's
  `.slash/saves/` for a headless run, a test or a session, so playtests never touch a player's
  saves.
- `save_game(slot, data)` writes a slot whole, to a file beside it first and then moved into
  place, as `{"format", "game_version", "saved", "data"}`; `load_game` returns the data with the
  version and time it was saved, so a game can upgrade an old save.
- `save_slots()` lists the slots newest first, for a load menu; `has_save` and `delete_save`
  check and empty one.
- On the web the folder is kept in the browser's IndexedDB; `save_data_ready()` says when the
  stored saves are read back after the game starts.
- `save.list`, `save.read`, `save.write` and `save.delete` work on the project's `.slash/saves/`,
  so an agent can check what a playtest saved or seed a save to load.

See [commands.md](../../docs/reference/commands.md#save-data) and
[gameplay-code.md](../../docs/reference/gameplay-code.md#save-data).
