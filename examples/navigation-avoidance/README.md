# Navigation avoidance

Agents steering round each other and round obstacles, as Godot 4.6's `NavigationAgent2D` and
`NavigationObstacle2D` avoidance, on RVO2's optimal reciprocal collision avoidance.
[main.scene.json](main.scene.json) has a plaza (a `NavigationRegion2D`) with a fountain, and five
walkers, each a `NavigationAgent2D` with `avoidance_enabled`:

```json
"west_1": {"components": {
  "NavigationAgent2D": {"avoidance_enabled": true, "avoidance_priority": 0.5,
                        "target_position": [380.0, 280.0]},
  "Transform2D": {"position": [20.0, 280.0]}
}}
```

Two walkers come from the west and two from the east, meeting head on below the fountain, and a
courier crosses them from the north. The courier keeps the default `avoidance_priority` of 1,
higher than the others' 0.5, so it makes way for no one and they make way for it. Each agent
also has Godot's `radius`, `neighbor_distance`, `max_neighbors`, `time_horizon_agents`,
`time_horizon_obstacles`, `max_speed`, `avoidance_layers` and `avoidance_mask`.

The fountain is a `NavigationObstacle2D` with a `radius` and `affect_navigation_mesh`, so
`navigation.bake` cuts it out of the plaza's polygons, grown by the agents' radius, and paths go
round it; agents also avoid it while they walk. The cart is a `NavigationObstacle2D` with a
`velocity`: avoidance takes that as where it is going, and the program moves it.

[main.cpp](main.cpp) moves the walkers as a Godot script would. A system before the navigation
system (which steps at priority 960) sets each agent's `velocity` toward its next path position;
avoidance turns it into a safe velocity, emitted as a `velocity_computed` event and returned by
`navigation::safe_velocity_2d`; and a system after it moves each agent at that velocity. Over six
seconds the walkers weave past each other and the cart and reach their targets, never nearer
than their radii, and `navigation.agent` shows one's `safe_velocity` as it passes the others.

The output is in [expected.txt](expected.txt).
