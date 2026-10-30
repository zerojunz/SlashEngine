# Plugins

A plugin is a library built apart from the engine, against
[`slash_plugin.h`](../../engine/plugin/include/slash/plugin/slash_plugin.h) alone, that adds
components and systems to a game, as Godot's GDExtension. The header is plain C and only ever
grows, so a plugin built once keeps loading into later engines, and it can be written in any
language that calls C. It links nothing of the engine's: its entry function is handed
`get_proc`, which gives each engine function by name.

[addons/orbit/orbit.c](addons/orbit/orbit.c) is one, in C: an `Orbit` component (a C struct
whose fields it describes with offsets, types, docs and ranges) and an `orbit` system that moves
each entity's `Transform2D` round a circle, emits a `lap` event about the entity `notify` names
and turns round when `ui_accept` is pressed. [orbit.plugin.json](addons/orbit/orbit.plugin.json)
names its entry function and its library on each platform (`slash-cli schema plugin`), and
[project.json](project.json) lists that file under `plugins`, so `slash-cli`, `slash-mcp` and
the game load it before scenes open. The example's test builds `orbit.c` into `addons/orbit/bin/`.

```bash
cc -shared -fPIC -I<engine>/engine/plugin/include addons/orbit/orbit.c -o addons/orbit/bin/orbit.so -lm
slash-cli rpc --project . < calls.jsonl
```

[calls.jsonl](calls.jsonl) opens [main.scene.json](main.scene.json), whose moon has an `Orbit`
like any component, reads the component's schema, steps half a second and finds the moon a
quarter of the way round, waits for its `lap` event about the sun, presses `ui_accept` and sees
the speed turn negative, then sets a radius below its minimum and gets the error with its path.
[results.jsonl](results.jsonl) holds the responses.

- The engine stores, saves, loads, validates, undoes and describes plugin components by their
  fields, as its own: `bool`, `int32`, `int64`, `float`, `double`, `vec2` to `vec4`, fixed-size
  UTF-8 strings and entities (saved as their IDs).
- `slash_world_each` walks the entities with a set of components; `slash_component_get_json`
  and `slash_component_set_json` read and write any component, the engine's too, as JSON.
- A plugin that registers anything wrong, such as fields that overlap or a component name that's
  taken, adds nothing, and the error says why. `engine.describe` lists what each plugin added.
