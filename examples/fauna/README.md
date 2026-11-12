# Fauna

Animals as recipes, after Unreal's Mutable and its animal AI (Godot has none), in 3D and in 2D.
A `.creature.json` file gives a body plan, its measures, a look, parts and a behavior:
[deer.creature.json](fauna/deer.creature.json) is a spotted deer with antlers that keeps with its
herd and is wary of people, and [wolf.creature.json](fauna/wolf.creature.json) a wolf that hunts
grazers once it is hungry.

```json
{"format": 1, "name": "Deer", "kind": "grazer",
 "body": {"length": 1.6, "height": 1.1, "build": 0.4, "legs": 1.2},
 "look": {"coat": [0.6, 0.4, 0.22], "pattern": "spots", "markings": [0.95, 0.92, 0.85]},
 "parts": {"ears": "long", "horns": "antlers"},
 "behavior": {"temperament": "wary", "flee": 6, "notice": 12, "herd": true, "herd_radius": 5,
              "roam": 15, "walk_speed": 1.2, "run_speed": 6}}
```

[doe.creature.json](fauna/doe.creature.json) `extends` the deer without antlers,
[dog.creature.json](fauna/dog.creature.json) is a friendly dog that two bones tame and its owner
can ride, [sparrow.creature.json](fauna/sparrow.creature.json) a timid flock bird and
[minnow.creature.json](fauna/minnow.creature.json) a school fish. The scene is a meadow with a
ranger (a `CharacterBody3D`, which creatures take for a person).

[calls.jsonl](calls.jsonl) works on the animals the way an agent does over MCP or JSON-RPC, and
[results.jsonl](results.jsonl) holds the replies:

- `creature.populate` puts a herd of five deer on the ground and four sparrows their altitude
  above it, apart from each other; `creature.place` adds the dog. `model.inspect` reads a deer's
  model: one skinned mesh with a material per thing it draws, its `idle`, `walk`, `run`, `eat`
  and `attack` animations and the `saddle` and `mouth` points.
- Two seconds on, the herd idles and wanders, but the deer nearest the ranger has noticed them
  and run: `creature.state` says it is `fleeing` the ranger at its run speed.
- A wolf comes from the west and soon grows hungry: it hunts a deer, and the deer it comes near
  notice it and flee.
- The dog won't eat a rock (`/food`, with what it eats). Fed two bones by the ranger it is tamed,
  then ridden and steered north: it carries the ranger, who sits on its saddle and faces where
  it goes. The events say who noticed, fled, was fed, tamed and mounted.
- In 2D, six minnows school in a pond at 100 pixels a meter, and the doe stands as a cut-out:
  after a tick each has a bone entity with a `Polygon2D` each.
- `ragdoll.generate` fits a ragdoll to the deer's model and to the doe's cut-out, a body a bone.
- A recipe that doesn't exist is an error at `/recipe`.
