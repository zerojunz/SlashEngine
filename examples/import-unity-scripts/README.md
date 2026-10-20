# Importing Unity scripts as C++

`project.import` turns each C# script a Unity project's scenes and prefabs use into a C++ skeleton
for the agent porting it to fill in. [unity/Assets/Scripts/Frog.cs](unity/Assets/Scripts/Frog.cs)
drifts the frog to its lily pad as it falls, picks up the coin it passes through and reports its
landing; [main.cpp](main.cpp) imports the project, prints the skeleton, ports it and runs the
scene:

1. `project.import` writes `src/imported/frog.cpp`, `src/imported/scripts.cpp` (which registers
   every skeleton) and `src/game.cpp` (the game module calling it), and adds `src` to
   `project.json`'s gameplay sources. The report lists each script's `code` and `component`.
2. In the skeleton, the script's fields are the `Frog` component: the serialized ones (public, or
   `[SerializeField]`) hold the scene's values on the frog entity, `pad`, a `Transform` in Unity,
   holds the pad entity, and private ones are its state. `[Tooltip]` and `[Range]` describe the
   field. The `onLanded` UnityEvent is `emit_on_landed`, which emits a world event. Each method
   is a stub keeping its C# in comments, and the `frog` system calls them when Unity would:
   `Start` for the entities made since the last tick, `Update` once a tick with the tick's
   seconds as `delta`, `OnTriggerEnter2D` on both entities as a body enters an `Area2D` (a
   trigger collider), and `OnCollisionEnter2D` as the frog's `RigidBody2D` touches the pad: the
   import sets `contact_monitor` on bodies whose scripts take collisions.
3. It fills in the stubs, builds with `gameplay.build`, opens the converted scene and steps it:
   `start` fires once, the frog takes the coin, which it destroys, and lands on the pad.

- Unity measures in units; the import makes each `pixels_per_unit` pixels (in its report), so a
  speed in units a second is scaled in the port.
- A UI event's persistent calls connect too: a button's `OnClick` calling `StartGame()` becomes a
  call of `start_game` when it emits `pressed`, with the call's own argument when it has one.
- Godot's C# scripts import the same way, alongside its GDScript files (see
  [import-godot-scripts](../import-godot-scripts/)): `[Export]` fields and properties are what
  scenes set, and `[Signal]` delegates are signals.
- Importing again keeps skeletons that exist, so the agent's work stays; `scripts.cpp` is
  rewritten.
