# Importing Godot scripts as C++

`project.import` turns each GDScript file of a Godot project into a C++ skeleton for the agent
porting it to fill in. [godot/player.gd](godot/player.gd) walks the player to its goal and counts
the coins a button gives it; [main.cpp](main.cpp) imports the project, prints the skeleton, ports
it and runs the scene:

1. `project.import` writes `src/imported/player.cpp`, `src/imported/scripts.cpp` (which registers
   every skeleton) and `src/game.cpp` (the game module calling it), and adds `src` to
   `project.json`'s gameplay sources. The report lists each script's `code` and `component`.
2. In the skeleton, the script's variables are the `Player` component: the scene's values are on
   the player entity, and `goal`, a `Node2D` in Godot, holds the goal entity. `coin_collected` is
   `emit_coin_collected`, which emits a world event. Each function is a stub keeping its GDScript
   in comments, and the `player` system calls them when Godot would: `_ready` for the entities
   made since the last tick, `_physics_process` once a tick, and `_on_coin_pressed` when the coin
   button emits `pressed`, as the scene connects them.
3. It fills in the stubs, builds with `gameplay.build`, opens the converted scene and steps it:
   the player walks, `ready` fires once, and pressing the coin emits `coin_collected`.

- Systems react to signals through `World::events()`: the events of the tick before, which each
  system sees once whatever its priority.
- Importing again keeps skeletons that exist, so the agent's work stays; `scripts.cpp` is
  rewritten. A game module of the project's own is left alone (the report asks for a call to
  `imported::register_scripts` if it lacks one).
- What a skeleton can't hold, such as `@onready` variables or inner classes, is listed at its top.
