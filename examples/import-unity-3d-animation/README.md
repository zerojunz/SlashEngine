# Import Unity 3D animation

`project.import` converts the Animators of a Unity 3D scene: their controllers become animation
libraries and trees, and their clips' curves move `Transform3D`, mirrored in x as the scene is.
[unity/](unity/) holds "Mill" as Unity 2022.3 saves it: its project settings, two materials, and a
scene ([Mill.unity](unity/Assets/Scenes/Mill.unity)) with the camera, the sun, the ground, a
windmill whose Animator spins its blades, and a gate with a spot lamp on it, whose Animator swings
it open when its `Open` trigger is set. [calls.jsonl](calls.jsonl) imports it, opens the scene and
plays it, as an agent would over MCP or JSON-RPC; [results.jsonl](results.jsonl) holds the
replies.

- [Windmill.controller](unity/Assets/Animations/Windmill.controller) has one state, `Spin`, with no
  transitions, so it becomes `Windmill.anim.json` and an `AnimationPlayer` on the windmill playing
  it. [Spin.anim](unity/Assets/Animations/Spin.anim) turns the `Blades` object a whole turn about z
  in four seconds, over and over; its track's `target` is `Blades`, which the player's `targets`
  bind to the `blades` entity.
- Unity's Euler angles become `Transform3D` `rotation` in radians, turning the other way about y
  and z as the scene is mirrored: 360 degrees about z is `[0, 0, -6.283185]`. `animation.eval`
  reads the turn at 0, 1, 2 and 4 seconds, and after 60 ticks the blades stand upright, turned a
  quarter turn (`-1.5707963`).
- [Gate.controller](unity/Assets/Animations/Gate.controller) goes from `Closed` to `Open` when
  `Open` is set, so it becomes `Gate.anim.json` and `Gate.animtree.json`, a state machine whose
  transition's `advance_expression` is `Open`, and an `AnimationTree` on the gate. `Open` is one of
  the tree's `triggers`, which `animation.set_parameters` sets as Unity's `SetTrigger` did; a
  little over a second later the gate has swung a quarter turn about y, `-1.570796` mirrored.
- The gate's spot lamp faces along +z in Unity and Godot's lights along -z. The gate's own
  rotation is animated, so the half turn about y that faces the lamp Godot's way goes on a
  `gate_light` entity under it, `[0, 3.141593, 0]`, and the animation leaves it alone.

`frame.capture` reads the sky, the ground and the blades across the windmill at the start, the
blades upright a second later, and the gate turned edge on to the camera once it opens.
