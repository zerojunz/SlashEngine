# Break effects

What damage leaves and whom it reaches, after Chaos Destruction's break events driving Niagara
(Godot has no destruction). A profile's `effects` say what each hit and each break leaves:
`debris` and `dust` (one-shot particles), a `decal` and a `sound`, made as entities named for
the body (`crate_debris`, `crate_decal`, ...) that go when their `lifetime` runs out. Every
material has its own, so [profiles/flesh.damage.json](profiles/flesh.damage.json) names only
its material, while [profiles/crate.damage.json](profiles/crate.damage.json) throws more chips
and leaves a scorch, a radial gradient from
[textures/scorch.texture.json](textures/scorch.texture.json):

```json
"effects": {"hit_scale": 30,
  "hit": {"debris": {"amount": 12, "lifetime": 0.8},
          "decal": {"texture": "textures/scorch.texture.json", "size": 0.3, "lifetime": 4}}}
```

[calls.jsonl](calls.jsonl) drives it the way an agent does over MCP or JSON-RPC, and
[results.jsonl](results.jsonl) holds the replies:

- `vehicle.place` puts a van from [vehicles/van.vehicle.json](vehicles/van.vehicle.json) in
  [main.scene.json](main.scene.json)'s yard, and `component.set` gives it a damage profile.
  `damage.check` finds nothing wrong, effects' files included.
- A shot at the crate leaves half its chips (15 of `hit_scale`'s 30) and a scorch facing back
  along the shot; `entity.get` shows the decal and its `DamageEffect`. A heavier blow breaks
  it, leaving wood's chips and dust.
- The hunter (`by`) shoots the deer, a `Creature3D`: it runs from him, as `creature.state`
  shows, and a second shot kills it. The living never break: its `killed` reply and event say
  it died, and it stops.
- A blast by the van wrecks it: it drives no more, and its own break effects leave black smoke.
- The 2D crate takes the same profile in pixels: a `CPUParticles2D`, a `Sprite2D` scorch.
- `damage.state` marks the deer `dead` and the van `wrecked`, `world.query` lists what the
  hits left, and 150 ticks later only the scorches remain.
- `project.json`'s `physics_3d.break_budget` caps the pieces breaks from impacts cut a tick;
  those past it break in the ticks after.
