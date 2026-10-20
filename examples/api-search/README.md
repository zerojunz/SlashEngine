# API search

Finding the C++ to call instead of guessing it. The project is a small game whose code in
`src/` has an enemy component, an enum, a function declared in a header and defined in a source
file, and a system registered in `SLASH_GAME_MODULE`. [calls.jsonl](calls.jsonl) asks
`api.search` for a function by name, a type, something by what it does, and a misspelled name.
[results.jsonl](results.jsonl) holds the responses, line for line.

```bash
slash-cli rpc --project . < calls.jsonl
```

- `chase` comes back with its signature and doc from the header, where it is defined, and the
  line in `game.cpp` that passes it to `add_system`.
- A type lists its fields with their defaults and comments; an enum lists its values.
- `what an enemy does next` finds `next_state` and `State` by the words in their docs.
- These calls search only the project (`"in": ["project"]`), so their results stay the same as
  the engine changes. Without it, the engine's public headers are searched too, and each
  result's examples are lines of the engine's [examples](../), which its tests build and run:

```json
{"query": "emit an event", "limit": 1}
```

```json
{"results":[{"at":"engine/world/include/slash/world/world.hpp:248","doc":"Emits a gameplay event, such as emit(\"player_died\", \"player\", {{\"cause\", \"lava\"}}). ...","examples":[{"at":"examples/physics-2d/main.cpp:64","code":"world.emit(\"coin_taken\", world.id_of(coin), {});"},{"at":"examples/project-map/src/coins.cpp:37","code":"world.emit(\"coin_collected\", event.id);"}],"include":"slash/world/world.hpp","kind":"function","name":"slash::world::World::emit","signature":"void emit(std::string kind, std::string_view id = {}, json::Value data = {})"}]}
```
