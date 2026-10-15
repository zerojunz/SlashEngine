# Prefabs

Entities that extend a `.prefab.json` file and override some of its fields, like Godot's
inherited scenes. [scenes/level.scene.json](scenes/level.scene.json) has a coin made from
[prefabs/coin.prefab.json](prefabs/coin.prefab.json); [calls.jsonl](calls.jsonl) adds another,
saves the player as a prefab and makes a second player from it. [results.jsonl](results.jsonl)
holds the responses, line for line.

```bash
slash-cli rpc --project . < calls.jsonl
```

- An instance saves as `extends` plus only the fields that differ from its prefab, so changing
  the prefab changes every instance that did not override that field.
- `entity.get` shows the instance's full components, with `extends`.
- A misspelled prefab path fails with the closest file.
