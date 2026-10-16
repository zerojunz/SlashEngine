# Importing Unity projects

[`project.import`](commands.md#projectimport) and [`slash-cli import`](cli.md#import) convert a
Unity project's 2D game into a SlashEngine project in one pass: its scenes and prefabs, sprites,
tile maps, animation, cameras, 2D physics and joints, sounds and audio mixers, input and settings. Scripts don't convert,
since SlashEngine games are plain C++: each becomes a storyboard task to port it, with what the
port needs, as does each Animator controller's state machine.
[examples/import-unity](../../examples/import-unity/) imports a small platformer.

A folder imports as a Unity project when it has `Assets/` and
`ProjectSettings/ProjectVersion.txt`. Projects saved by Unity 2018.3 or later import (prefabs
were saved another way before); open an older project in a newer Unity and save it there first.
Unity must have saved the project's assets as text, its default ("Force Text" asset
serialization).

## What an import writes

| File | From |
| --- | --- |
| `<scene>.scene.json` beside each `<scene>.unity` | The scene's GameObjects, as entities and components |
| `<prefab>.scene.json` beside each `<prefab>.prefab` | The prefab, as a scene with a `root` that other scenes instance |
| `<clip>.loop.audio.json` beside a sound | A sound an `AudioSource` loops |
| `Assets/tiles.tileset.json` | The sprites tile maps show, as a tile set (`tiles_2.tileset.json` and on for tile maps of other cell sizes or physics) |
| `<controller>.anim.json` beside each `.controller` and `.overrideController` | The controller's states, as an animation library |
| `<mixer>.bus_layout.json` beside the main `.mixer` | Every audio mixer's groups, as one bus layout |
| `project.json` | The player, time, physics, quality and input settings, merged into the project's own if it has one |
| `storyboard/work.json` | A task per C# script and per state machine, merged into the project's own tasks |
| `import/unity.json` | The full report: every object's entity, what each script needs, tags, layers and everything that didn't convert |

Textures (`.png`, `.bmp`), sounds (`.wav`, `.ogg`, `.mp3`), fonts (`.ttf`, `.otf`) and data files
(`.json`, `.txt`, `.csv`) are used as they are. When the Unity folder is inside the project, they
stay where they are and the converted files go beside the originals (`Assets/Art/a.png` in
`unity/` is `unity/Assets/Art/a.png`). When it is anywhere else, they and the scripts are copied
in at their paths in the Unity project (`Assets/Art/a.png`). Folders ending in `~` and scripts
in `Editor/` folders are skipped. Assets are found by the GUIDs in their `.meta` files, so a
reference to a moved asset still resolves.

Importing again rewrites the converted files and keeps each task's status and the project's other
settings.

## Units

Unity measures in units with y up; SlashEngine in pixels with y down. The import takes the
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

Unity's sprites don't pass their order, visibility or color to the objects under them, and
Sprite2D's do, as Godot's do. So an object with children draws its sprite on a child entity of its
own (`<entity>_sprite`), and so does a sprite whose texture has another pixels per unit than the
project's, scaled to match. Sliced and tiled sprites draw as simple ones, materials and sprite
masks don't convert, and each is reported.

## Tile maps

Each `Tilemap` becomes a `TileMapLayer` on its entity, and the sprites its cells show become
tiles of a [tile set](scene-format.md#tile-sets) the import writes, `Assets/tiles.tileset.json`.
Tile maps whose `Grid` cells are the same size in pixels share one tile set; one whose collider
has other physics gets a set of its own (`tiles_2.tileset.json`).

- A texture's sprites of one size that fall on a grid, as Unity's Sprite Editor slices them, are
  one atlas source named after the texture, with its margins and separation; a sprite off the
  grid is a source of its own. A tile is named after its sprite in snake case.
- Unity's cell `(x, y)` is cell `(x, -y - 1)`, since Unity's cells go up from y 0 and rows go
  down.
- A cell's matrix becomes its turns: a mirrored cell is `:h` or `:v`, and a quarter turn
  counterclockwise `:vt`, as Godot's cell transform flags. A scaled or skewed matrix, or an
  offset, is reported and drawn without it.
- A cell's color and collider type pick the tile's look; each look other than the first a cell
  showed becomes an alternative tile (`ground:1`) with its own `modulate` and collision.
- With a `TilemapCollider2D`, a tile's collision polygons come from its collider type: the
  sprite's custom physics shape (pixels around the sprite's middle, y flipped), or the cell's
  square for the Grid type. A sprite with no custom shape, whose physics shape Unity makes from its pixels, gets
  its rectangle, reported. The collider's physics material (else its `Rigidbody2D`'s, else the
  project's default) gives the tile set's physics layer its friction and bounce, the object's
  layer its collision layer and mask, and a one-way `PlatformEffector2D` makes the polygons
  one-way. A layer without a collider, or a trigger one, has `collision_enabled: false`.
- The renderer's sorting layer and order are the layer's `z_index`, the Tilemap's color its
  `modulate`, and the texture's filter mode its `texture_filter` when it differs from the
  project's. A tile map without a `TilemapRenderer` is hidden.
- The report's `tile_assets` names, for each tile asset (`.asset`), the tile set and tile it
  became. Scripted tiles (rule tiles, animated tiles) keep the sprite each cell shows and are
  reported, as are isometric and hexagonal grids, which don't convert, and a tile map moved by a
  dynamic or kinematic `Rigidbody2D`. A `CompositeCollider2D` needs nothing, since a layer's
  tiles are one static body.

## Animation

Each Animator controller becomes an [animation library](scene-format.md#animations)
beside it, `Frog.controller` becoming `Frog.anim.json`, with an animation per state of its base
layer, named after the state. An `Animator` becomes an `AnimationPlayer` playing that library
from the controller's default state.

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
- An override controller is its controller's states with its clips swapped. A blend tree plays
  its first motion.
- The state machine, which gameplay code drives, becomes a storyboard task per controller
  (`task/port-frog-anim`) when it has parameters or transitions: its states, its parameters with
  their types and defaults, each transition with its conditions and exit time, and the events
  its clips raise. The report's `animation_libraries` names, for each library, its controller,
  its first state, the entities using it and its task. Controllers no scene uses convert too,
  since gameplay code may set them, and clips no controller plays are reported.
- Layers other than the base layer, transitions' blending, state speed parameters, cycle
  offsets, root motion, animated script values and fields SlashEngine has no counterpart for
  are reported. The legacy `Animation` component doesn't convert.

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
| Attenuation | The bus's `volume_db` |
| Send to a Duck Volume | The Duck Volume's `sidechain` |
| A bypassed effect | `enabled: false` |

Godot's compressor attacks within 2 ms, so a slower attack is reported. Each bus sends to one bus,
so a Send to a Receive is reported, as are effects after a group's Attenuation (a bus runs its
effects before its volume), effects' wet levels (they run fully wet), a group's pitch, Flange,
Normalize, ParamEQ, Pitch Shifter and audio plugins' effects. Snapshots other than the start one
and exposed parameters are reported too: gameplay code (or `audio.set_bus`) sets buses instead.

## Project settings

| Unity | `project.json` |
| --- | --- |
| Player: Product Name | `name` |
| Player: Version | `version` |
| Player: Default Screen Width and Height | `window.width`, `window.height` |
| The first enabled scene in the build settings (else the first scene) | `main_scene` |
| Time: Fixed Timestep | `tick_hz` |
| Physics 2D: Gravity | `physics_2d.gravity` and `gravity_vector` |
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
to the script and to the scenes that use it, with what it declares: its classes, public and
`[SerializeField]` fields and methods. The report's `scripts` holds, for each object using a
script, the values its fields were given: numbers and strings as they are, references to objects
as entity IDs (`"frog"`, or `"frog#AudioSource"` for a component other than its Transform) and
references to assets as their paths. The scripts stay where they are, linked from the tasks.

Components from Unity's packages convert where SlashEngine has them and are reported where it
doesn't: uGUI and TextMesh Pro components convert in a later import, the event system isn't
needed (the gui system sends input to controls itself), and other package scripts are named in
the report.

## Not yet converted

uGUI, 2D lights, particle systems and isometric and hexagonal tile maps convert in later imports; until then each is reported where it is used.
Materials and shaders, 3D (milestone 7) and Timeline don't convert. Everything that didn't
convert is in the report's `unsupported`, once per kind with how many and the first place it was
seen.
