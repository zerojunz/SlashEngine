# Instanced scenes

A scene other scenes instance, as Godot's PackedScene: an entity that `"extends"` a
`.scene.json` file becomes that scene's root, and the scene's other entities come with it, with
IDs under the instance's. [scenes/player.scene.json](scenes/player.scene.json) is a player with a
body and a gun, the gun itself an instance of [scenes/gun.scene.json](scenes/gun.scene.json);
[scenes/level.scene.json](scenes/level.scene.json) instances the player as `hero` and tints its
body. [calls.jsonl](calls.jsonl) adds a second player, changes it, removes the hero's gun, then
edits the player scene and reopens the level; [results.jsonl](results.jsonl) holds the
responses, line for line.

```bash
slash-cli rpc --project . < calls.jsonl
```

- A scene can be instanced when it names its `"root"`: the entity an instance becomes.
  `scene.save` takes `"root"` to set it.
- The level's `hero` brings `hero/body`, `hero/gun` and, from the gun's own scene,
  `hero/gun/muzzle`. References inside the scene follow: `hero`'s `Transform2D` `children` are
  `hero/body` and `hero/gun`, and `entity.get` places the muzzle in the world through both.
- An entry keyed `hero/body` holds only that entity's overrides, and `null` leaves one out:
  destroying `hero/gun` saves `"hero/gun": null` for it and the muzzle it held.
- Editing the player scene changes every instance, except the fields an instance overrides:
  `rival/body` is scaled by the scene's edit and keeps its own tint.
- Only a scene with a `"root"` can be instanced, and the error says how to give it one.
