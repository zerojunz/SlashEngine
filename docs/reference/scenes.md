# Scenes

Scene files and the commands that edit them, for engine contributors.

Status: current · Updated: 2026-11-20

Scenes are `.scene.json` files: entities keyed by stable ID, each with its components. Fields
equal to their defaults are left out. `tests/data/basic.scene.json` is a small example.

An entity can `"extends"` a `.prefab.json` file (one entity's components, path relative to the
project) and override its fields; a `null` component removes one the prefab has. Saving writes
only what differs from the prefab. `entity.create` takes `"extends"`, and `prefab.save` writes an
entity to a prefab file. `tests/data/game` has an example.

An entity can also `"extends"` a `.scene.json` file that names its `"root"` (`scene.save` takes
`root`), as Godot instances a PackedScene: it becomes the root, and the scene's other entities
come with it as `<instance>/<their id>` (`World::create_part`), their references renamed through
`ComponentType::rename_entities`. Entries keyed with `/` hold overrides or `null`, saving writes
only what differs, and `entity.destroy` takes them along. `expand` in `engine/world/src/scene.cpp`
does the loading; `examples/instanced-scenes` has a player with a gun scene inside it.

The entities in a `Transform2D`'s `children` are placed in its space, as Godot's Node2D children:
`world::Tree2D` (`engine/world/src/tree2d.cpp`, built with `-ffp-contract=off`) gives each one's
global transform, its parent and its draw order, and everything that places things in the world
(drawing, physics, lights, particles, sound) asks it. Make one per use; a root's global transform
is its own to the bit, so flat scenes behave as before. A `top_level` entity keeps its parent
(`parent`) but is placed in the world (`transform_parent` is null) and draws after the rest of its
tree, as Godot's top-level items; `canvas::inherited` stops its parents' `z_index` and tint there,
and at a `z_as_relative` that is off. `check_tree` backs `scene validate`, and a session fails
any command that would give a child two parents or make it its own ancestor.
`entity.destroy` takes children with it. A body takes the shapes of its children that have no
body of their own (`shape_key_of` in `engine/physics2d/src/space.cpp`), as Godot's
`Collider2D` nodes. `examples/parent-transforms` sails a boat.

```bash
$cli schema scene                                   # JSON Schema for scene files (also: prefab, frames, material, animation, animtree, process_material, tileset, font, theme, audio, bus_layout, replay, test, score, navmesh, navpoly, npc, quest, dialogue, meshlib, lightmap, voxel_gi, model, standard_material, texture, scatter, pcg, physics_material, ragdoll, foliage, groom, damage, character, face, creature, vehicle, vfx, fluid, water, pipeline, storyboard, project, presets, sdk, plugin, providers, bench)
$cli scene validate tests/data/basic.scene.json     # every error, with its JSON Pointer path
$cli scene format level.scene.json                  # rewrite in canonical form
$cli scene format --check level.scene.json          # exit 1 if not canonical
```

Validate after editing a scene or prefab by hand, and format before committing one. Both
commands take `.prefab.json`, `.frames.json`, `.material.json`, `.anim.json`, `.animtree.json`,
`.process_material.json`, `.tileset.json`, `.font.json`, `.theme.json`, `.audio.json`,
`.bus_layout.json`, `.replay.json`, `.test.json`, `.score.json`, `.navmesh.json`,
`.navpoly.json`, `.npc.json`, `.quest.json`, `.dialogue.json`, `.meshlib.json`,
`.lightmap.json`, `.voxelgi.json`, `.model.json`, `.standard_material.json`, `.texture.json`,
`.scatter.json`, `.pcg.json`, `.physics_material.json`, `.ragdoll.json`, `.foliage.json`,
`.groom.json`, `.damage.json`, `.character.json`, `.face.json`, `.creature.json`,
`.vehicle.json`, `.vfx.json`, `.fluid.json`, `.water.json` and `.pipeline.json` files too, and `scene validate` takes models (`.gltf`, `.glb`, `.obj`, `.fbx`, `.blend`, `.model.json`); validating a
scene also checks each tile map layer against its tile set, each `Animator` against its
library and that its controls lay out as written.
