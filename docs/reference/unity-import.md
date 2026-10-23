# Importing Unity projects

[`project.import`](commands.md#projectimport) and [`slash-cli import`](cli.md#import) convert a
Unity project's 2D or 3D game into a SlashEngine project in one pass: its scenes and prefabs,
sprites, tile maps, animation, cameras, 2D physics and joints, 2D lights and shadows, 3D meshes,
models, materials, lights, physics, terrains and skies (see [3D scenes](#3d-scenes)), sounds and audio
mixers, UI, input and settings. Scripts don't convert,
since SlashEngine games are plain C++: each becomes a storyboard task to port it, with what the
port needs, and each Animator controller's state machine a task to set its parameters.
[examples/import-unity](../../examples/import-unity/) imports a small platformer, and
[examples/import-unity-3d](../../examples/import-unity-3d/) a 3D courtyard and
[examples/import-unity-terrain](../../examples/import-unity-terrain/) a terrain.

A folder imports as a Unity project when it has `Assets/` and
`ProjectSettings/ProjectVersion.txt`. Projects saved by Unity 2018.3 or later import (prefabs
were saved another way before); open an older project in a newer Unity and save it there first.
Unity must have saved the project's assets as text, its default ("Force Text" asset
serialization). A game built with Unity imports too: see [Unity builds](#unity-builds).

## What an import writes

| File | From |
| --- | --- |
| `<scene>.scene.json` beside each `<scene>.unity` | The scene's GameObjects, as entities and components |
| `<prefab>.scene.json` beside each `<prefab>.prefab` | The prefab, as a scene with a `root` that other scenes instance |
| `<clip>.loop.audio.json` beside a sound | A sound an `AudioSource` loops |
| `Assets/tiles.tileset.json` | The sprites tile maps show, as a tile set (`tiles_2.tileset.json` and on for tile maps of other cell sizes or physics) |
| `<controller>.anim.json` beside each `.controller` and `.overrideController` | The controller's states, as an animation library |
| `<mixer>.bus_layout.json` beside the main `.mixer` | Every audio mixer's groups, as one bus layout |
| `<terrain>.height.png`, `.splat.png` and `.color.png` beside each terrain's `.asset` | Its heights, its alpha map as a splat map, and its holes (see [terrains](#terrains)) |
| `project.json` | The player, time, physics, quality and input settings, merged into the project's own if it has one |
| `storyboard/work.json` | A task per C# script and per state machine, merged into the project's own tasks |
| `src/imported/<name>.cpp` | A C++ skeleton per C# script, in the gameplay sources (see [Scripts](#scripts)) |
| `src/imported/scripts.cpp`, `src/game.cpp` | The file registering every skeleton, and a game module calling it when the project's code has none |
| `import/unity.json` | The full report: every object's entity, what each script needs, tags, layers and everything that didn't convert |

Textures (`.png`, `.jpg`, `.bmp`), models (`.glb`, `.gltf`, `.obj` with its `.mtl`, `.fbx`,
`.blend`), sounds (`.wav`, `.ogg`, `.mp3`), fonts (`.ttf`, `.otf`) and data files (`.json`,
`.txt`, `.csv`) are used as they are. When the Unity folder is inside the project, they
stay where they are and the converted files go beside the originals (`Assets/Art/a.png` in
`unity/` is `unity/Assets/Art/a.png`). When it is anywhere else, they and the scripts are copied
in at their paths in the Unity project (`Assets/Art/a.png`). Folders ending in `~` and scripts
in `Editor/` folders are skipped. Assets are found by the GUIDs in their `.meta` files, so a
reference to a moved asset still resolves.

Importing again rewrites the converted files and keeps each task's status and the project's other
settings.

## Units

Unity measures in units with y up; SlashEngine's 2D in pixels with y down. (3D scenes keep
Unity's meters: see [3D scenes](#3d-scenes).) The import takes the
project's pixels per unit from its sprites: the `Pixels Per Unit` most sprite textures use (100
when there are none), reported as `pixels_per_unit`. Positions, sizes, offsets, radii, distances
and gravity are multiplied by it, y is negated, and angles turn the other way. A 2D object turns
about z only; a rotation about x or y is reported and dropped.

## Scenes and prefabs

Each GameObject becomes an entity named after it in snake case: `Lily Pad` is `lily_pad`. A name
that is taken is prefixed with its parent's ID, then numbered, in the hierarchy's order. The
report maps every object's path (`Frog/Feet`) to its entity. Objects nest through their parent's
`Transform2D` `children`, in the hierarchy's order.

Prefabs stay prefabs. A prefab becomes a scene with a `root`, and an instance of it an entity
that extends that scene, holding only what the instance overrides; the prefab's other objects
come with it under the instance's ID (`frog/feet`). Overrides, removed components and removed
GameObjects apply as in Unity: a removed object is a `null` entry, a removed component a `null`
component. Objects a scene adds under an instance's objects are the scene's own entities.
Prefabs inside prefabs stay nested, and a prefab variant extends the prefab it varies.

An inactive GameObject (`m_IsActive: 0`, or under one) converts hidden, with its shapes disabled
and its sounds not playing, and is reported, since entities have no inactive state.

| Unity component | Components |
| --- | --- |
| `Transform` | `Transform2D` |
| `SpriteRenderer` | `Sprite2D` (see [sprites](#sprites)) |
| `Tilemap`, `TilemapRenderer`, `TilemapCollider2D` | `TileMapLayer` (see [tile maps](#tile-maps)) |
| `Grid` | Nothing: its cell size is the tile set's `tile_size` |
| `Camera` (orthographic) | `Camera2D` |
| `AudioListener` | `AudioListener2D` |
| `AudioSource` | `AudioStreamPlayer`, or `AudioStreamPlayer2D` when its spatial blend is above 0 (see [sounds](#sounds)) |
| `Rigidbody2D` and colliders | A body and shapes (see [physics](#physics)) |
| `HingeJoint2D`, `SpringJoint2D`, `SliderJoint2D` | `PinJoint2D`, `DampedSpringJoint2D`, `GrooveJoint2D` on a child entity (see [joints](#joints)) |
| `PlatformEffector2D` | `one_way_collision` on the shapes it affects |
| `MonoBehaviour` of a project script | Nothing: a storyboard task (see [scripts](#scripts)) |

## Sprites

| SpriteRenderer | Sprite2D |
| --- | --- |
| `m_Sprite` | `texture`, and `region` for a sprite cut from a sheet (Unity's rects go up from the bottom; regions go down from the top) |
| The sprite's pivot | `offset`: Unity places a sprite by its pivot, Sprite2D by its center |
| `m_FlipX`, `m_FlipY` | `flip_h`, `flip_v` (and the offset flips with them, since Unity flips about the pivot) |
| `m_Color` | `modulate` |
| Sorting layer and `m_SortingOrder` | `z_index`: the order with one sorting layer; with several, each layer a band of up to 1000 (the Default layer at 0), reported in `sorting_layers` |
| `m_Enabled` | `visible` |
| The texture's filter mode | `texture_filter`, when it differs from the project's (`rendering.texture_filter` is what most textures use) |
| The texture's `_NormalMap` secondary texture | `normal_texture`, which [2D lights](#2d-lights) shade with |

A sprite saved as an asset of its own (a `.asset` file holding a Sprite, as projects rebuilt
from builds have them) converts from its own rect, pivot, pixels per unit and physics shape on the
texture it draws from, which is an atlas's when it was packed into one: `region` is where its
pixels are, and `offset` moves them by the transparent edges packing trimmed. A sprite turned in
its atlas doesn't convert, and one packed to its outline converts with a warning, since its
neighbors may show at its edges.

Unity's sprites don't pass their order, visibility or color to the objects under them, and
Sprite2D's do, as Godot's do. So an object with children draws its sprite on a child entity of its
own (`<entity>_sprite`), and so does a sprite whose texture has another pixels per unit than the
project's, scaled to match. Sliced and tiled sprites draw as simple ones, materials and sprite
masks don't convert, and each is reported.

## Tile maps

Each `Tilemap` becomes a `TileMapLayer` on its entity, and the sprites its cells show become
tiles of a [tile set](scene-format.md#tile-sets) the import writes, `Assets/tiles.tileset.json`.
Tile maps whose `Grid`s have the same cell layout and cells of the same size in pixels share one
tile set; one whose collider has other physics gets a set of its own (`tiles_2.tileset.json`).
[examples/import-unity-isometric](../../examples/import-unity-isometric/) imports an isometric
and a hexagonal tile map.

- A texture's sprites of one size that fall on a grid, as Unity's Sprite Editor slices them, are
  one atlas source named after the texture, with its margins and separation; a sprite off the
  grid is a source of its own. A tile is named after its sprite in snake case. The texture's
  `_NormalMap` secondary texture is the source's `normal_texture`, so 2D lights shade its tiles.
- Unity's cell `(x, y)` is cell `(x, -y - 1)`, since Unity's cells go up from y 0 and rows go
  down.
- The Grid's cell layout is the tile set's [shape](scene-format.md#tile-shapes): Rectangle is
  `square`, Isometric and Isometric Z As Y are `isometric` in the `diamond_right` layout, and
  Hexagon is `hexagon` in `stacked_offset` rows. A Grid with the cell swizzle YXZ, as Unity's
  flat-top hexagons, swaps the axes: its tile size is the cell size turned, its hexagons are
  offset in columns (`tile_offset_axis` `vertical`), and its cell `(x, y)` is `(y, -x - 1)`, or
  `(x, y)` for diamonds. Other swizzles aren't converted, and gaps between cells are reported.
- Unity and Godot don't put cell `(0, 0)` in the same place on these shapes, so the layer moves
  by a fixed offset that puts each cell's center where Unity's is: an isometric map half a cell
  up, a hexagonal one half a cell left and a quarter down. The objects under the tile map move
  back by as much, so they stay where they were.
- The tile anchor and each sprite's pivot become the tile's `texture_origin`, which puts the
  pivot on the anchor as Unity does: blocks standing on their bottom middle on an isometric map
  anchored at (0, 0) draw with their feet on each cell's bottom corner. A pivot that falls
  between whole pixels is rounded, a hexagonal map's anchor is its cells' centers, and a turned
  cell turns its tile about the cell's center, not the anchor; each of these is reported.
- A cell's matrix becomes its turns: a mirrored cell is `:h` or `:v`, and a quarter turn
  counterclockwise `:vt`, as Godot's cell transform flags. A scaled or skewed matrix, or an
  offset, is reported and drawn without it.
- A cell's color and collider type pick the tile's look; each look other than the first a cell
  showed becomes an alternative tile (`ground:1`) with its own `modulate` and collision.
- With a `TilemapCollider2D`, a tile's collision polygons come from its collider type: the
  sprite's custom physics shape (pixels around the sprite's middle, y flipped), where the sprite
  draws, or the cell's square, diamond or hexagon for the Grid type. A sprite with no custom shape, whose physics shape Unity makes from its pixels, gets
  its rectangle, reported. The collider's physics material (else its `Rigidbody2D`'s, else the
  project's default) gives the tile set's physics layer its friction and bounce, the object's
  layer its collision layer and mask, and a one-way `PlatformEffector2D` makes the polygons
  one-way. A layer without a collider, or a trigger one, has `collision_enabled: false`.
- The renderer's sorting layer and order are the layer's `z_index`, the Tilemap's color its
  `modulate`, and the texture's filter mode its `texture_filter` when it differs from the
  project's. A tile map without a `TilemapRenderer` is hidden.
- An `AnimatedTile` from Unity's 2D Tilemap Extras becomes an
  [animated tile](scene-format.md#animated-and-large-tiles): its first sprite is the tile and
  its other sprites the frames, which must follow it along the sheet's grid as Godot's do (in
  one row, or wrapping to the next rows after some columns, evenly spaced). Each frame lasts a
  second over its speed times the Tilemap's animation frame rate; a random speed plays at the
  middle of its range, and start times and flags are reported. Frames out of grid order, or on
  cells other tiles show, are reported and the tile shows its first frame; a sprite shown both
  by an animated tile and a plain one plays in both, reported.
- The report's `tile_assets` names, for each tile asset (`.asset`), the tile set and tile it
  became. Other scripted tiles (rule tiles and the like) keep the sprite each cell shows and are
  reported, as is a tile map moved by a dynamic or kinematic `Rigidbody2D`. A `CompositeCollider2D` needs nothing, since a layer's
  tiles are one static body.

## 2D lights

URP's 2D renderer lights a sprite with the global light's color plus each light's color times its
strength there, and Godot does the same with a `CanvasModulate` and `PointLight2D`s in add mode
(see [2D lights](scene-format.md#2d-lights)). So the lights convert to those:

| Light2D | Converts to |
| --- | --- |
| Global | The scene's `CanvasModulate`, its `color` the light's color times its intensity, up to white (brighter is reported). A scene has one; a second global light is reported |
| Spot (Point) | A `PointLight2D` through a texture of the light's shape and falloff, `Assets/lights/spot_<inner radius %>_<falloff %>[_<inner angle>_<outer angle>].png`, 256 pixels across and scaled to the outer radius by `texture_scale`. A cone points up the texture, as Unity's points along the object's y axis, so it turns with the object |
| Freeform | A `PointLight2D` through a texture of the polygon with its falloff around it, `Assets/lights/freeform_<hash>.png`, scaled to the shape and `offset` to its middle |
| Sprite | A `PointLight2D` through the sprite's texture, or its part of a sheet cut out to `Assets/lights/<sprite>.png`, at the sprite's size with its pivot on the light |
| Parametric | Not converted and reported: Unity replaced parametric lights with freeform ones |

| Light2D field | PointLight2D |
| --- | --- |
| `m_Color`, `m_Intensity` | `color`, `energy` |
| `m_ApplyToSortingLayers` | `range_z_min` and `range_z_max`: the z indices of those sorting layers' sprites (see [Sprites](#sprites)). Layers with others between them light those too, and are reported |
| `m_ShadowsEnabled` | `shadow_enabled`. Godot's shadows block all of a light, so a shadow strength below 1 is reported |
| `m_ShadowSoftness` | `shadow_filter` `pcf13` with `shadow_filter_smooth` ten times the softness |
| `m_NormalMapQuality` and `m_NormalMapDistance` | `height`, the distance in pixels, when normal maps are on |
| `m_Enabled`, an inactive object | `enabled: false` |

The falloff becomes each texture's alpha: full inside the inner radius (or the freeform shape),
then fading to nothing at the outer radius as URP's curve does, `t^(ln(1 - b) / ln b)` with
`b = 0.25 + strength / 2`, which is linear at the default strength of 0.5. Lights saved before
URP versioned them (no `m_ComponentVersion`) take shadows from their shadow strength and normal
maps from `m_UseNormalMap`. An object with several lights keeps the first and puts each other
light on an entity under it, `<entity>_light_2`. A scene with lights and no global light gets an
`ambient` entity with a black `CanvasModulate`, since URP leaves what no light reaches dark, and
this is reported.

A `ShadowCaster2D` becomes a `LightOccluder2D` with its shape path as the `polygon`, or, when it
takes its shape from a component, that collider's shape (a circle as 24 sides, a capsule's round
ends as 8 each, an edge collider an open `closed: false` polygon) or the sprite's physics shape,
or its rectangle. A shape of several paths puts each other one on an entity under it,
`<entity>_occluder_2`. A caster set to cast no shadows converts to nothing, and a
`CompositeShadowCaster2D` isn't needed, since each occluder shadows on its own.

Differences, each reported where it applies: volumetric lights and other blend styles than the
first (Godot's lights add, as URP's default Multiply style does), alpha blending where lights
overlap, spot light cookies, a freeform light's falloff offset, a caster that shadows only itself
(Godot's occluders shadow what is behind them) and disabled casters. Godot's lights also shade
sprites with normal maps when a light has normal maps off, and an occluder shadows the sprite it
is on, as URP's casters do only with Self Shadows on.

## Animation

Each Animator controller becomes an [animation library](scene-format.md#animations)
beside it, `Frog.controller` becoming `Frog.anim.json`, with an animation per state of its base
layer, named after the state. Its base layer's state machine becomes an
[animation tree](scene-format.md#animation-trees), `Frog.animtree.json`, and an `Animator` an
`AnimationTree` playing it over the library, not `deterministic`, as Unity's states set only what
their clips animate. A controller whose states have no transitions becomes an `AnimationPlayer`
playing its default state instead.

| Unity | Becomes |
| --- | --- |
| A state | A state of the same name, an `Animation` node playing its animation, at its place in the graph |
| A sub-state machine | Its states, in the same machine: a transition to it goes through its entry, one to its exit through the parent's transitions from it, then the parent's entry. The base layer's exit starts over at its entry |
| The entry's transitions and default state | Auto transitions from `Start`, the conditional ones first |
| Any State's transitions | A transition from every state, tried before the state's own, but not back to the state itself: that is reported when `Can Transition To Self` is on |
| A transition's conditions | Its `advance_expression`, all of them joined with `and`: `Speed > 0.1 and not Grounded`, `Jump` for a bool or trigger |
| Float, int and bool parameters, triggers | The tree's `variables` with their defaults, and `triggers`, which go back to false once a transition that reads them is taken, as Unity's do. A name expressions can't read is changed (`Is Grounded` becomes `Is_Grounded`), as the report says |
| Exit time | `switch_mode` `at_end`, with `break_loop_at_end` for a looping state; Unity's default exit time (the cross-fade ending as the state does) matches exactly, others are reported |
| Duration | `xfade_time`, a normalized one in seconds of the state's animation |
| The order Unity tries a state's transitions in | `priority`, from 0 |

- A clip's curves become value tracks, its events event tracks. Position and scale curves are
  `Transform2D` `position` and `scale` (in pixels, y down), a rotation about z `rotation`
  (clockwise radians; a quaternion curve's turn is taken at each key and sample), a
  `SpriteRenderer`'s color `Sprite2D` `modulate`, its enabled and flip flags `visible`, `flip_h`
  and `flip_v`, and a sprite curve three discrete tracks, `texture`, `region` and `offset`.
  `GameObject.m_IsActive` shows and hides the object's sprites.
- Unity's curves are Hermite curves (Bezier with weighted tangents); SlashEngine's interpolate
  linearly between keys. A stretch of a curve that isn't a straight line is keyed every 60th of
  a second, a stepped one (infinite tangents) holds its key (`transition: 0`), and a straight one
  keeps its two keys.
- Only the part between the clip's start and stop times plays. A clip that loops time loops
  linearly; a state's speed scales the animation's times, so a state at speed 2 plays a clip of
  one second as an animation of half a second.
- Tracks name the objects they animate by their paths from the Animator's object, as Unity's
  curves do: `target` is `"Body/Arm"`, or none for the Animator's own object, and
  `"Body#sprite"` for the entity drawing that object's sprite. The player's `targets` bind each
  to an entity; a path with no object under the Animator is null and reported.
- An event track is named after the function the event calls, with its `float`, `int`,
  `string` and `object` parameters as the key's `data`; gameplay code reads the event
  (`World::emit`'s events, as Godot's method tracks).
- An override controller is its controller's states with its clips swapped, and its tree the
  controller's.
- A blend tree is a blend space over its motions, as Godot's `BlendSpace1D` and `BlendSpace2D`,
  with `sync` on so every motion keeps playing, as Unity's do. A 1D tree's thresholds and a 2D
  tree's positions are its points' positions (beyond -1 to 1, `min_space` and `max_space` widen
  to hold them), and a tree inside a tree is a blend space at its point. Each motion plays its
  clip's animation at the state's speed times the motion's time scale; when another state
  already plays the clip at another speed, the animation is named after the state and the clip
  (`Walk Jump`). A tree with one motion is that motion's `Animation` node.
- Godot's 2D blend spaces blend the three points around the position, in triangles, where
  Unity's 2D types weigh every motion by gradient bands, so 2D trees are reported. Motions all in
  one row or column blend in 1D along it, and ones on one slanted line, which make no triangles,
  in a `discrete` blend space where the closest plays. A direct tree plays its first motion.
- Unity's blend trees read their parameters themselves; a blend space's position is a tree
  parameter of its own: `Walk/blend_position`, or `Walk/1/blend_position` for a blend space at
  the second point of `Walk`'s. The controller's task says which parameter to set each from, and
  the `AnimationTree`'s `parameters` start them at those parameters' defaults. Motions of
  different lengths are reported: Unity stretches them to one cycle as it blends, and each plays
  at its own length here.
- Each controller is a storyboard task (`task/port-frog-anim`): with a tree, its parameters,
  which gameplay code sets as Unity's `SetFloat`, `SetInteger`, `SetBool` and `SetTrigger` did,
  with their types and defaults; with no tree, its states, parameters and transitions for
  gameplay code to play. Either way it lists the events its clips raise. The report's
  `animation_libraries` names, for each library, its controller, its first state, its tree, the
  entities using it and its task. Controllers no scene uses convert too, since gameplay code may
  set them, and clips no controller plays are reported.
- Two transitions between the same states become one taking either's conditions, with the
  first's timing. Muted transitions don't convert, and a solo one mutes the state's others.
- Layers other than the base layer, transitions to a state's own self, transition offsets and
  interruption sources, state speed parameters, cycle offsets (a state's or a blend tree
  motion's), root motion, animated script values and fields SlashEngine has no counterpart for
  are reported. The legacy `Animation`
  component doesn't convert.

## Cameras

An orthographic camera becomes a `Camera2D` whose `zoom` shows what Unity's orthographic size
does at the project's window height: `window height / (2 * size * pixels per unit)`. The clear
color of the main scene's `MainCamera` becomes `rendering.clear_color`. Perspective cameras,
viewport rects and render textures are reported.

## Physics

| Unity | SlashEngine |
| --- | --- |
| `Rigidbody2D`, Dynamic | `RigidBody2D`: mass, gravity scale, linear and angular drag as damping, frozen rotation as `lock_rotation`, continuous detection, sleeping |
| `Rigidbody2D`, Kinematic | `AnimatableBody2D` |
| `Rigidbody2D`, Static | `StaticBody2D` |
| Colliders with no `Rigidbody2D` above | `StaticBody2D` |
| Colliders whose parent has the `Rigidbody2D` | Shapes on the object, which the parent's body takes |
| Trigger colliders | `Area2D`: on the object when it has no body, else a `trigger` child entity |
| `BoxCollider2D`, `CircleCollider2D`, `CapsuleCollider2D` | `CollisionShape2D`: rectangle, circle, capsule |
| `PolygonCollider2D` | `CollisionPolygon2D`, one per path |
| `EdgeCollider2D` | A segment `CollisionShape2D` per edge |
| `PhysicsMaterial2D` | `friction` and `bounce` (at most 1, as Box2D clamps it) |
| The GameObject's layer | `collision_layer`: the layer's bit |
| The layer collision matrix | `collision_mask`: the layers the layer collides with |

An object holds one `CollisionShape2D` and one `CollisionPolygon2D`; more shapes go on `shape_2`,
`shape_3` and so on, child entities the body takes. A collider two or more levels under its
`Rigidbody2D` becomes an `AnimatableBody2D` that follows its parent, since bodies take shapes from
their own children only, and is reported. `CompositeCollider2D`s, frozen positions and auto mass
are reported. `physics_2d` gets the project's gravity and no damping, as Unity has no damping but
each body's own.

## Joints

Each joint becomes a child entity of its object, `hinge`, `spring` or `slider` (`hinge_2` and on
for more of a kind), holding Godot's joint between the connected body as `node_a` (the world when
there is none) and the object's own body as `node_b`. A joint is made where its entity is when the
scene starts, and stays there when the body carries the entity off, as Godot's.

| Unity | SlashEngine |
| --- | --- |
| `HingeJoint2D` | `PinJoint2D` at the anchor. Angle limits are `angular_limit_lower` and `angular_limit_upper` in radians, and the motor's speed is `motor_target_velocity` in radians per second; past a limit the motor stops pushing, as Godot's |
| `SpringJoint2D` | `DampedSpringJoint2D` from the connected anchor to the anchor, its `length` the distance between them; a set distance is `rest_length`. Unity's frequency and damping ratio become the `stiffness` and `damping` that give them for the two bodies' masses |
| `SliderJoint2D` | `GrooveJoint2D` along the slider's angle through the connected anchor, from its lower translation limit to its upper; `initial_offset` is where the body starts on it |
| `m_EnableCollision` | `disable_collision: false` |

A connected anchor Unity configures itself is the anchor, where the joint starts; one set by hand
is in the connected body's space, or the world's. A hinge whose two anchors are apart pins the
bodies where its own anchor is, and is reported. Limits on one side of where a hinge starts are
widened to include it (Godot's limits go from at most 0 to at least 0), and a spring with
frequency 0 (rigid in Unity) is a stiff one at 15 Hz; both are reported. A slider without limits
gets a groove 20000 pixels long, and the body on a groove turns freely unless its `Rigidbody2D`
freezes rotation; both are reported, as is a slider's motor (a groove has none).

Disabled joints, joints on objects without a `Rigidbody2D`, break forces and torques (joints don't
break; gameplay code can destroy one) and the other joints (`DistanceJoint2D`, `FixedJoint2D`,
`FrictionJoint2D`, `RelativeJoint2D`, `TargetJoint2D`, `WheelJoint2D`) are reported.

## Sounds

An `AudioSource` plays its clip with `volume_db` from its volume, `pitch_scale` from its pitch and
`autoplay` from Play On Awake. A looping source plays a `<clip>.loop.audio.json` file that loops
the sound. A spatial source becomes an `AudioStreamPlayer2D` with `max_distance` from its max
distance; rolloff other than linear is reported. A source that outputs to an audio mixer group
plays on that group's bus.

## Audio mixers

The project's `.mixer` files become one [bus layout](scene-format.md#bus-layouts), named after the
main mixer (`Main.mixer` becomes `Main.bus_layout.json`) and set as `audio.bus_layout`. The main
mixer is the one that outputs to no other mixer's group, the one with the most groups when
several don't; its Master group is the Master bus. Each group becomes a bus named after it
(`Music 2` and on when a name is taken), sending to its parent's bus. Another mixer's Master group
becomes a bus named after the mixer, sending to the bus of the group the mixer outputs to (Master
when it outputs nowhere). The report's `bus_layout` names, for each mixer, the bus each group
(`Master/Music`) became.

A group's volume, mute, solo and bypass come from the mixer's start snapshot, and so do its
effects' settings:

| Unity effect | Bus effect |
| --- | --- |
| Lowpass, Highpass | `low_pass_filter`, `high_pass_filter`: cutoff and resonance (at most 1, as Godot's; more is reported) |
| Lowpass Simple, Highpass Simple | `low_pass_filter`, `high_pass_filter` |
| Echo | `delay`: its feedback repeats each echo, quieter by the decay; with decay 0, one tap at the wet level. Delays go up to 1500 ms |
| Distortion | `distortion` in `waveshape` mode, the waveshaper Unity's is |
| Chorus | `chorus` with three voices, one per tap |
| Compressor | `compressor`: threshold, attack, release and make-up gain, at a ratio of 2.5 |
| Duck Volume | `compressor` with the bus that sends to it as its `sidechain`; with no send it does nothing and isn't converted |
| SFX Reverb | `reverb`: the decay time as `room_size`, the high-frequency ratio as `damping`, the dry and room levels, and the reflection and reverb delays as `predelay_msec` |
| Pitch Shifter | `pitch_shift`: the pitch, the FFT size (rounded up to a power of two from 256 to 4096) and the overlap as `oversampling`, at least 4 as Godot's (less is reported) |
| Attenuation | The bus's `volume_db` |
| Send to a Duck Volume | The Duck Volume's `sidechain` |
| A bypassed effect | `enabled: false` |

Godot's compressor attacks within 2 ms, so a slower attack is reported. Each bus sends to one bus,
so a Send to a Receive is reported, as are effects after a group's Attenuation (a bus runs its
effects before its volume), effects' wet levels (they run fully wet), a group's pitch, Flange,
Normalize, ParamEQ and audio plugins' effects. Snapshots other than the start one
and exposed parameters are reported too: gameplay code (or `audio.set_bus`) sets buses instead.

## UI

Objects with a `RectTransform` under a `Canvas` (uGUI) become [controls](scene-format.md#ui-and-text):
each a `Control`, with no `Transform2D`, listing the controls under it in its `children`. A UI
prefab, whose top object has a `RectTransform` with UI under it, becomes a scene of controls that
its instances extend.

A screen space canvas at the top of the hierarchy is a control over the window. Its
`CanvasScaler` gives it a scale: Constant Pixel Size its scale factor, Scale With Screen Size what
its reference resolution and match give the project's window. A scaled canvas is a control of the
window's size divided by the scale, with that `scale`, and is reported, since it keeps that scale
at other window sizes; Constant Physical Size draws at 1:1 and is reported. The canvas's sort
order, and that of a nested canvas that overrides sorting, is `z_index`. A world space canvas is
drawn over the screen at its own size, and reported; a canvas under a world object is a control of
its own. A disabled canvas or inactive object is `visible: false`, and a `CanvasGroup`'s alpha is
`modulate`.

A rect's anchors become `anchor_*` (Unity's go up from the bottom, Control's down from the top),
its anchored position, size delta and pivot `offset_*`, and the pivot the grow directions: a
control smaller than its content grows away from a pivot at an edge, and both ways from one in
the middle. A turned or scaled rect turns and scales about its pivot (`pivot_offset`).

| Unity | SlashEngine |
| --- | --- |
| `Image` without a sprite | `ColorRect` |
| `Image` of Unity's built-in UI sprites (UISprite, Background, InputFieldBackground, UIMask, Knob) | `Panel` with a flat style box with rounded corners, since Unity's sprites aren't open source |
| `Image`, simple | `TextureRect` (`keep_aspect_centered` with Preserve Aspect), or a `NinePatchRect` showing the region of a sprite cut from a sheet |
| `Image`, sliced or tiled | `NinePatchRect` with the sprite's borders as its patch margins, tiled with `tile` |
| `RawImage` | `TextureRect` |
| `Text`, `TextMeshProUGUI` | `Label`: text, alignment, wrapping, overflow and uppercase, with the font, size, color and line spacing as theme overrides. A TextMesh Pro font asset draws with its source font file, Unity's built-in Arial with the default font |
| `Outline`, `Shadow` on text | `font_outline_color` and `outline_size` (Godot strokes a quarter of it each side, so four times the distance), `font_shadow_color` and `shadow_offset_x`, `shadow_offset_y` |
| `Button` | `Button`, its target graphic's color tint or sprite swap as its `normal`, `hover`, `pressed`, `hover_pressed`, `disabled` and `focus` style boxes; a button whose graphic is another object's is `flat` |
| `Toggle` | `CheckBox` when it is Unity's default toggle (the built-in background and checkmark), else a `Button` with `toggle_mode`; `button_pressed` from Is On, `button_group` from its `ToggleGroup` |
| `Slider` | `Slider`: range, value, whole numbers as `step`, and `vertical`. Its background is the `slider` style and its fill the `grabber_area` style, as thick as the background, and a handle sprite the `grabber` icon |
| `Scrollbar` | `ScrollBar`, its size as `page` and its handle the `grabber` styles |
| `InputField`, `TMP_InputField` | `LineEdit` (`TextEdit` for Multi Line Newline): text, placeholder, character limit, password, keyboard type and caret blink, the text's font and colors, and the text area's insets as the style boxes' content margins |
| `Dropdown`, `TMP_Dropdown` | `OptionButton`: its options' text and whole-sprite images as `items`, its value as `selected` (kept in range), Interactable as `disabled`, its caption's font, size and color, and its graphic's styles as a button's. Its caption, caption image, placeholder, arrow and template are part of it; an arrow of the project's own sprite is its `arrow` icon, and the list draws with the `PopupMenu` theme, so the template's look is reported |
| `ScrollRect` | `ScrollContainer`, its content the child: a direction that doesn't scroll is `disabled`, one without a scroll bar `never`, and Auto Hide `auto` |
| `Mask`, `RectMask2D` | `clip_contents` |
| `HorizontalLayoutGroup`, `VerticalLayoutGroup` | `BoxContainer` (`vertical`): child alignment as `alignment`, spacing as `separation`; Control Child Size and Child Force Expand as the children's size flags, a child's own size its minimum where the group doesn't control it |
| `GridLayoutGroup` | `GridContainer`: columns from its constraint, spacing as `h_separation` and `v_separation`, each child at least the cell size |
| A layout group's padding | A `MarginContainer`, its child entity `<id>_layout` holding the container and the children |
| `LayoutElement` | Min and preferred sizes as `custom_minimum_size`, flexible sizes as `expand_fill` and `size_flags_stretch_ratio` |
| `ContentSizeFitter` | A rect of no size at the pivot, which grows to its content, as controls are at least their minimum size |
| Explicit navigation, none | `focus_neighbor_*`; `focus_mode: "click"` |

A widget's parts become part of its control: a default toggle's background and checkmark, a
slider's background, fill and handle, a scroll view's viewport and scroll bars, an input field's
text area, text and placeholder, and a dropdown's caption, arrow and template. The report maps
each to the control's entity, and what they hold that isn't a part is lifted into the control's
children. A graphic that takes clicks has `mouse_filter: "pass"`, so a click reaches the button
above it, as in Unity.

A UI event's persistent calls (On Click, On Value Changed, On End Edit; a dropdown's On Value
Changed is `item_selected`) to a project script's methods are listed in the script's task with
the control's event that makes them: `play in Menu.scene.json calls StartGame() on pressed`.
Calls to Unity's own components are reported.

Also reported: a dropdown option's image cut from a sprite sheet, filled images (drawn whole), a
sliced sprite whose corners Unity draws at another size, UI materials, Unity's checkmark and
arrow sprites outside a default toggle, a toggle with its own sprites (its checkmark shows as it
starts: gameplay code shows and hides it on `toggled`), `AspectRatioFitter`, reversed layout groups, a grid's start corner, axis and
alignment, input validation, best fit and auto-sized text, bold and italic styles, animation
transitions, objects that aren't UI under UI objects and other components on UI objects. The
`EventSystem` isn't needed: the gui system sends input to controls itself.

## 3D scenes

A scene or prefab converts in 3D when it has more of Unity's 3D components (mesh renderers, model
instances, lights, `Rigidbody`, 3D colliders, `CharacterController`, terrains, perspective cameras)
than 2D ones (sprites, tile maps, `Rigidbody2D`, 2D colliders, orthographic cameras). One with as
many of each (a prefab of only Transforms, say) converts as most of the project does. A 2D
component in a 3D scene, or a 3D one in a 2D scene, is reported. Its objects nest through
`Transform3D` `children`.

Unity's axes are left-handed and Godot's right-handed, so a 3D scene converts mirrored in x, which
keeps every picture as Unity draws it: a position (x, y, z) becomes (-x, y, z), and rotations turn
the other way about y and z (a quaternion (x, y, z, w) becomes (x, -y, -z, w)). Sizes and
distances stay in meters. Unity's model importers mirror right-handed glTF, OBJ and FBX files into
its axes, so the mirror puts them back as SlashEngine reads them. Cameras, lights and quads face
along +z in Unity and along -z in Godot, so they turn half a turn about y: on the object itself
when nothing else on it faces a way (its children turn back), else on a `camera`, `light`,
`listener` or `mesh` child entity turned `[0, 3.141593, 0]`.

| Unity | SlashEngine |
| --- | --- |
| `Transform` | `Transform3D`: position, rotation (Godot's YXZ Euler angles, in radians) and scale |
| `MeshFilter` and `MeshRenderer` of Unity's cube, sphere, capsule, cylinder, plane and quad | `MeshInstance3D` with `BoxMesh`, `SphereMesh`, `CapsuleMesh`, `CylinderMesh`, `PlaneMesh` (10 by 10 meters) or `QuadMesh`; cast shadows as `cast_shadow`, the layer as `layers` |
| An instance of a model file | `MeshInstance3D` whose `mesh` is the file, scaled by its import settings' scale factor on a `model` child entity; a material put in place of all of the model's is its `StandardMaterial3D` |
| `MeshFilter` and `MeshRenderer` of one mesh of a model file | `MeshInstance3D` whose `mesh` is a glTF file of that mesh alone (see below) |
| A material of Unity's Standard shader or URP's Lit, Simple Lit or Unlit | `StandardMaterial3D`, converted once per material (see below) |
| `Camera` (perspective or orthographic) | `Camera3D`: fov, `size` (twice Unity's orthographic size), near, far, the culling mask's first 20 layers; the `MainCamera` is `current` |
| `Light`: directional, point, spot | `DirectionalLight3D`, `OmniLight3D` (range), `SpotLight3D` (range, half the spot angle): color, intensity as energy, shadows on with their strength as opacity, culling mask |
| `AudioListener` | `AudioListener3D` |
| `AudioSource` with spatial blend above 0 | `AudioStreamPlayer3D`: `unit_size` its min distance, `max_db` 0, `max_distance` its max distance |
| `Rigidbody` | `RigidBody3D`: mass, gravity, drag and angular drag as damping, kinematic as `freeze` (`kinematic`), frozen axes as axis locks, continuous detection |
| `CharacterController` | `CharacterBody3D` with its capsule, `floor_max_angle` from the slope limit, `safe_margin` from the skin width |
| Colliders with no `Rigidbody` above | `StaticBody3D`; a collider one level under its `Rigidbody` is a shape its parent's body takes |
| Trigger colliders | `Area3D`: on the object when it has no body, else a `trigger` child entity |
| `BoxCollider`, `SphereCollider`, `CapsuleCollider` | `CollisionShape3D`: box, sphere, capsule (turned to its axis), at its center |
| `MeshCollider` of a built-in mesh | The same shape; a plane's or quad's is its two triangles, solid from both sides |
| `MeshCollider` of a model's mesh | `convex_polygon` of its corners when it is convex, else `concave_polygon` of its triangles, scaled as the mesh |
| `PhysicMaterial` | `friction` (its dynamic friction) and `bounce`; Unity's default friction is 0.6 |
| `LODGroup` | Only its first level draws; renderers in the others are reported |
| `Terrain` and `TerrainCollider` | `HTerrain` on a `terrain` child entity (see [terrains](#terrains)) |
| The scene's `RenderSettings` and its main camera's clear flags | An `environment` entity: `WorldEnvironment` (see below) and the sky |

A material's color, main texture (with its tiling and offset), metallic, smoothness (as 1 minus
roughness), metallic map, normal map, HDR emission (its brightest channel past 1 as
`emission_energy_multiplier`), transparency (URP's surface type and alpha clip, the Standard
shader's rendering mode), culling and URP Unlit's unshaded mode convert; the main texture's import
settings give `texture_filter` and `texture_repeat`. The specular workflow converts as metallic 0,
and occlusion, height and detail maps are reported. A material of any other shader converts from
the properties it shares with Standard's, and is reported.

A mesh picked out of a model file (in a prefab made from a model's parts, say) is written to a
glTF file of its own beside the converted model, named after both
(`Models/Props_Barrel.gltf`, with `_2` and on for the same mesh with other materials). Its
positions are scaled by the model's import settings' scale factor, and it sits at the object's
transform, as Unity draws it. The mesh is found by its ID as Unity's model importer makes it: from
the `.meta` file's tables, or the hash of its name. The object's materials pick its parts: only as
many parts as it has materials draw. One `.mat` material for all of them is the entity's
`StandardMaterial3D`, and the file keeps the model's own materials; several are written into the
file. A mesh of a `.blend` file is reported, since Blender would have to run for each object.

The environment's background is the sky when the main camera clears to the skybox, else its solid
color. Unity's procedural skybox (Default-Skybox among them) becomes a `ProceduralSkyMaterial`
gradient in the colors Unity's draws with its defaults, scaled by its tint, with its ground color
and its exposure as energy (Godot's physical sky is dim without HDR). A panoramic skybox of a
360-degree image becomes a `PanoramaSkyMaterial`, turned as Unity turns it (`sky_rotation`).
Ambient light from the skybox, a flat color or a gradient (its equator's color, reported) and fog
(exponential as is, linear as depth fog, exponential squared reported) convert, and fog leaves the
sky alone, as Unity's does.

### 3D animation

An `Animator` in a 3D scene converts as one in a 2D scene does (see [animation](#animation)): its
controller becomes an animation library and, with transitions, an animation tree, and its clips'
curves move `Transform3D`, mirrored in x as the scene is. A controller converts once for 2D
objects, once for 3D objects and once for models' instances; one played by two kinds of object is
reported, since its tracks are written for the first.

| Unity curve | Track |
| --- | --- |
| Position | `Transform3D/position`, its x negated |
| Euler angles | `Transform3D/rotation`, in radians: `[x, -y, -z]` |
| Quaternion rotation | `Transform3D/rotation` with `linear_angle` interpolation, its parts interpolated as Unity does, mirrored, and keyed wherever the turn between keys passes about 6 degrees |
| Scale | `Transform3D/scale` |
| One axis of a position, Euler angle or scale | That axis's track, as the 2D import's |
| `GameObject.m_IsActive` | `Transform3D/visible` (reported: the object's other components still run) |

- On an instance of a model file, a curve on a path below the Animator's object moves a node of
  the model: it becomes a node track (`"node"` and `"property"`) naming the path's last object.
  Node tracks interpolate linearly and turn the shorter way, so a stepped key is held by keying
  its value again just before the next key.
- A clip of a model file (a take of an FBX file, say, as the model's import settings cut it)
  becomes an animation `"from"` the converted model's take, with the clip's loop and events (their
  times scaled to its length). A clip ending before its take does is cut there; one starting past
  its take's start plays from the take's start and is reported. The clip is found by its ID as
  Unity's model importer makes it: from the `.meta` file's tables, or the hash of its name. It
  plays at the take's own speed, so a state's speed on it is reported, and a clip of a model on an
  object that isn't the model's instance, or of a `.blend` file, is reported.
- A model's instance whose prefab modifications give its root an `Animator` (as Unity's model
  importer adds one) plays it there. On a model imported at a scale it plays on the `model` child
  entity that draws it, which is reported, since the object's own curves then move that entity.
- Cameras and lights on an animated object, or on an object one of its tracks moves, face -z
  through a child entity, so the animation's turns don't undo the half turn.
- Humanoid muscle curves in `.anim` files, Animator parameter curves, animated script values and
  curves of fields with no counterpart are reported.

#### Humanoid models and root motion

A model whose import settings make it humanoid (`animationType: 3`) has an Avatar: the model's
node for each of Unity's human bones (`Hips`, `Spine`, `LeftUpperLeg` and the rest), from
`humanDescription.human`, and those bones' rotations in the T-pose it keeps, from
`humanDescription.skeleton`. An Avatar copied from another model (`avatarSetup: 2`) is that
model's.

- A humanoid model's clip played by an instance of another humanoid model is retargeted through
  both Avatars, as Unity's Mecanim plays it: sampled at the take's frame rate, each human bone of
  the instance's model turns in the model as the clip's model's bone of the same name turns from
  its T-pose, and the hips move as far, scaled by the height of the instance's hips over that of
  the clip's model's. The clip becomes node tracks of the instance's model, rotations as Euler
  angles keyed where they bend, and bones that don't move have none. Human bones the clip's model
  hasn't stay in their T-pose.
- Models with the same rig (the same human bones on nodes of the same names, at the same rest
  and T-pose) play each other's clips `"from"` the clip's model, unchanged.
- A controller converts for the first model that plays it. A controller of retargeted clips played
  by another humanoid model is reported, since its tracks move the first model's nodes.
- With `m_ApplyRootMotion`, a model's instance gets an `AnimationTree` whose `root_motion_track`
  is the humanoid model's hips, or the node a generic model's `motionNodeName` names. The tree
  takes that node's motion out of the pose into `root_motion_position` and
  `root_motion_rotation`, as Godot's does, and gameplay code moves the object by them as Unity's
  `Animator` did. A controller with no transitions gets a tree playing its first state,
  `<controller>.animtree.json`. Root motion is reported either way: on a model's instance to say
  that gameplay code applies it, and on any other object because nothing takes it out.

### Terrains

A `Terrain` becomes an `HTerrain`, the heightmap terrain that follows the HTerrain plugin for
Godot, on a child entity named `<object>_terrain`. Unity's terrains reach toward +x and +z from
their object; mirrored in x, the terrain would reach toward -x, so the child moves back the
terrain's width and the ground stays where Unity draws it. Its `TerrainData` asset converts once,
however many terrains use it:

| TerrainData | HTerrain |
| --- | --- |
| The heights and `m_Scale` | `heightmap`, a 16-bit `<terrain>.height.png` with its columns flipped as x mirrors, and `height_range`, the whole meters that hold every height; `map_scale` is the cell size. A heightmap under 65 heights across is refined to 65, the smallest an `HTerrain` takes, and reported |
| The first alpha map | `splatmap`, `<terrain>.splat.png`, resampled to one weight a height |
| The first four terrain layers | `textures`: the diffuse texture as `albedo_bump`, the normal map as `normal_roughness`, and the tile size in cells as `uv_scale`. Depth blending is off, since Unity's diffuse alpha is smoothness, not height. More layers, tiles that aren't square, tile offsets and a layer's smoothness, metallic, mask map and normal scale are reported |
| The holes | `colormap`, `<terrain>.color.png`, white with alpha 0 at the holes. A height is a hole where every cell around it is cut, so holes come in half a cell at their edges, which is reported |
| Trees and detail patches | Reported |

The `Terrain`'s shadow casting mode is `cast_shadow` and its object's layer `render_layers`; a
disabled one, or one that doesn't draw its heightmap, is hidden. A custom material and a terrain on
a turned or scaled object are reported. The `TerrainCollider` gives the collision layer and mask,
from the object's layer, and its physic material's `friction` and `bounce`; one of another
`TerrainData`, or on a `Rigidbody`, is reported. With no `TerrainCollider`, collision is off.

Reported, for a later import: meshes saved as assets, skinned meshes, model clips starting past
their take's start, mesh colliders of meshes saved as assets, 3D joints and wheels, navigation, reflection and light probes, line and trail
renderers, area and baked lights, cookies and color temperature, cubemap skyboxes, a camera's
viewport rect and render texture, and a `CharacterController`'s step offset.

## Project settings

| Unity | `project.json` |
| --- | --- |
| Player: Product Name | `name` |
| Player: Version | `version` |
| Player: Default Screen Width and Height | `window.width`, `window.height` |
| The first enabled scene in the build settings (else the first scene) | `main_scene` |
| Time: Fixed Timestep | `tick_hz` |
| Physics 2D: Gravity | `physics_2d.gravity` and `gravity_vector` |
| Physics: Gravity (when a scene converts in 3D) | `physics_3d.gravity` and `gravity_vector`, mirrored in x |
| Quality: the current level's VSync Count | `window.vsync_mode` |
| The main audio mixer | `audio.bus_layout` (see [audio mixers](#audio-mixers)) |
| Input Manager axes and Input System actions | `input` (see [input](#input)) |

## Input

The Input Manager's axes (when Active Input Handling includes it) become actions named after
them: an axis with negative buttons becomes `<name>/negative` and `<name>/positive`, and a
joystick axis binds a stick's motion to both. Keys, mouse buttons and joystick buttons (`space`,
`left ctrl`, `mouse 0`, `joystick 2 button 4`, `[5]`) bind to their Godot equivalents; mouse
movement axes are reported.

Each `.inputactions` file's actions become `<map>/<action>`. A 2D vector composite's parts, and a
stick or D-pad bound whole, become `<map>/<action>/up`, `down`, `left` and `right`; a 1D axis
composite's become `/negative` and `/positive`. Keyboard bindings bind physical keys. Interactions
(hold, tap), invert processors, other composites and devices other than the keyboard, mouse,
touch and gamepads are reported.

## Scripts

Each C# script that a scene or prefab uses becomes a storyboard task, `task/port-<name>`, linked
to the script, the scenes that use it and its skeleton, with what it declares: its classes,
public and `[SerializeField]` fields and methods. The report's `scripts` holds, for each object
using a script, the values its fields were given: numbers and strings as they are, references to
objects as entity IDs (`"frog"`, or `"frog#AudioSource"` for a component other than its
Transform) and references to assets as their paths. The scripts stay where they are, linked from
the tasks.

Each script also becomes a C++ skeleton for the agent to fill in, `src/imported/<name>.cpp` in
`project.json`'s gameplay sources, laid out as a Godot script's is ([the Godot
importer](godot-import.md#scripts) has the files): its constants and enums, its fields as a
component named after its class, an `emit_<event>` helper per `UnityEvent` field, a stub per
method with the C# in comments, the system calling them and `imported::register_<name>`. A
`ScriptableObject` is a reflected struct with no system.

The fields Unity serializes (public ones, and `[SerializeField]` ones, `[field: SerializeField]`
properties too) are the ones scenes set: each entity whose object has the script gets the
component with those values, references to objects as entity IDs and to assets as paths, and an
instance only what differs from its prefab. Other fields are the component's state, and static
ones are listed at the top. `[Tooltip]` is a field's doc, `[Range]` and `[Min]` its range.
Until the code is built the component is a stand-in that keeps the values, so the scenes open.

| C# | C++ |
| --- | --- |
| `int`, `long`, `float`, `double`, `bool`, `string` | `int`, `std::int64_t`, `float`, `double`, `bool`, `std::string` |
| `Vector2`, `Vector2Int`, `Vector3`, `Vector4`, `Color`, `Color32`, `Quaternion`, `Rect` | `slash::Vec2`, `Vec2i`, `Vec3`, `Vec4` (`Color`, `Color32` and `Rect` too), `Quat` |
| `GameObject`, `Transform` and other components, the game's own scripts | `entt::entity`, the entity the reference names |
| Assets (`Sprite`, `AudioClip`, `Material`, prefabs...) | `std::string`, the asset's path |
| `List<T>`, `T[]`, `Dictionary<string, T>` | `std::vector`, `std::map<std::string, T>` |
| `LayerMask`, Unity's enums | `int`; the script's own enums stay enums |

The system calls the stubs when Unity would call the messages:

| Message | When the system calls it |
| --- | --- |
| A constructor, `Awake`, `OnEnable`, `Start` | For each entity made since the last tick, in that order |
| `FixedUpdate`, `Update`, `LateUpdate` | Once a tick, in that order, with the tick's seconds as `delta` |
| `OnTriggerEnter2D`, `OnTriggerExit2D` | On both entities when a body or area enters or leaves an `Area2D` (a trigger collider), with the other entity |
| `OnCollisionEnter2D`, `OnCollisionExit2D` | On both entities when a `RigidBody2D` touches or leaves another body, with the other entity; the import sets `contact_monitor` on bodies whose objects have such scripts |
| A method a UI event's persistent call names | When the control emits the event it converts to (`OnClick` is `pressed`): with the call's own argument when it passes one, with nothing when it passes nothing, and otherwise with the event, whose data holds the value |
| `OnDisable`, `OnDestroy` and Unity's other messages | Not yet; their stubs say so |

Coroutines and `async` methods are stubs, listed at the top with how to port them. Importing again leaves a skeleton that exists as it is and rewrites `scripts.cpp`.
[examples/import-unity-scripts](../../examples/import-unity-scripts/) imports a script, fills in
its skeleton and runs it.

Components from Unity's packages convert where SlashEngine has them and are reported where it
doesn't: uGUI and TextMesh Pro components convert under a canvas (see [UI](#ui)) and are
reported elsewhere, the event system isn't needed (the gui system sends input to controls
itself), and other package scripts are named in the report.

## Unity builds

A game built with Unity imports from its folder (with `<Game>_Data/`, on Windows and Linux), its
executable or a file in its data folder, a macOS `.app`, an iOS `.app` or `Payload/`, an Android
build's `assets/bin/Data`, an `.apk` or `.ipa` file, or a web build's folder (its `Build/*.data`
file, gzip or brotli compressed or not). [AssetRipper](https://github.com/AssetRipper/AssetRipper)
rebuilds the Unity project the game was made from, the importer tidies it, then converts it as it
would a project made in Unity. With `unpack_to` (`--unpack-to`) the rebuilt project is written to
that folder and the import uses its assets there; without it, only the assets come along. Unpack
only games you made or have the rights to. [examples/unpack-unity](../../examples/unpack-unity/)
imports a shipped game.

AssetRipper is GPL 3.0, so it only ever runs as a program of its own: the importer starts its
headless web server, asks it to load the build and export its project into
`.slash/import/unity/` (with its log, `assetripper.log`), then stops it. `SLASH_ASSETRIPPER`
names the program; otherwise the first import installs AssetRipper 2.0.0 as the `assetripper`
toolchain ([`slash-cli toolchain`](cli.md#toolchain)). Starting a program needs the `build`
capability, so `project.import` refuses a build in a session that doesn't grant it, and a running
game refuses it outright, since unpacking takes minutes.

| What the tidying fixes | How |
| --- | --- |
| Scripts of Unity's packages (uGUI, TextMesh Pro, the event system, URP's 2D lights), which come back decompiled with GUIDs of AssetRipper's own | References to them take the GUIDs those scripts have in every Unity project, so their components convert, and the stand-ins are dropped |
| Other scripts from Unity's assemblies (`UnityEngine.*`, `Unity.*`) | Dropped, with their class written into each use's `m_EditorClassIdentifier` as Unity 6 writes it, so the report names them |
| Sprites saved as assets of their own | Converted from their own rects (see [Sprites](#sprites)), and the project's pixels per unit comes from them, not from the textures, which AssetRipper gives Unity's default of 100 |
| UI components whose values didn't survive (AssetRipper read them without the build's scripts) | Reported, not converted with Unity's defaults |

The report's `unpacked` names the build's platform, scripting backend (`mono` or `il2cpp`) and
Unity version, AssetRipper's version, counts of scenes, prefabs, scripts, package scripts and
files, and its own `unsupported`. A Mono build's scripts come back as decompiled C#; an IL2CPP
build keeps its code compiled, so each script comes back as a stand-in (reported as "script
without its code") whose task still lists the objects using it and their values. Sounds a web
build keeps as AAC (`.audioclip` files) are reported. A build whose asset bundles are encrypted
(UnityCN) is refused, never decrypted.

## Not yet converted

Particle systems convert in a later import; until then each is reported where it is used. TextMesh Pro text in the world (not under
a canvas) is reported too.
A game's own shaders (materials of them convert from the properties they share with the Standard
shader), the 3D parts listed under [3D scenes](#3d-scenes) and Timeline don't convert. Everything that didn't
convert is in the report's `unsupported`, once per kind with how many and the first place it was
seen.
