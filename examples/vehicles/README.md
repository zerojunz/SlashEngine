# Vehicles

Vehicles as recipes, after Unreal's Chaos Vehicles (Godot has only `VehicleBody3D`), in 3D and
in 2D. A `.vehicle.json` file says what a vehicle looks like and how it handles:
[hatchback.vehicle.json](vehicles/hatchback.vehicle.json) is a front-wheel drive car with its
own engine, gears and the design a test drive must meet.

```json
{"format": 1, "name": "Hatchback", "kind": "car",
 "body": {"length": 4, "width": 1.8, "height": 1.45, "color": [0.1, 0.3, 0.8]},
 "physics": {"mass": 1100, "differential": "front",
             "engine": {"max_torque": 260, "max_rpm": 6500},
             "gears": {"ratios": [3.4, 2.1, 1.5, 1.1, 0.9]}},
 "design": {"top_speed": [30, 70], "acceleration": [5, 16], "braking_distance": [20, 70],
            "turning_radius": [4, 12]}}
```

[hot_hatch.vehicle.json](vehicles/hot_hatch.vehicle.json) `extends` it with more torque,
grip and a quicker design, [tank.vehicle.json](vehicles/tank.vehicle.json) is a tracked vehicle of its kind's
defaults, [speedboat.vehicle.json](vehicles/speedboat.vehicle.json) a boat and
[pickup.vehicle.json](vehicles/pickup.vehicle.json) an all-wheel drive pickup. The scene is a
long field with a driver and a walker (`CharacterBody3D`s), open water to the east, and a 2D road.

[calls.jsonl](calls.jsonl) works on the vehicles the way an agent does over MCP or JSON-RPC, and
[results.jsonl](results.jsonl) holds the replies:

- `vehicle.place` puts the car and the tank on the field and the boat on the water.
  `model.inspect` reads the car's model: its body, cabin and windows, and the `seat_` and
  `door_` points of its seats.
- `vehicle.check` test-drives both hatches apart from the scene: the hatchback's measures fit its
  design, and the hot hatch reaches 100 km/h a second and a half sooner.
- The driver gets in and floors it: at 16 meters a second the car hits the walker standing in its
  way (`vehicle_hit`, with the impulse and damage, which pushed them aside) and loses most of its
  speed doing it. Braking stops it, and the driver gets out beside it.
- `vehicle.tune` refuses gears out of order (at `/physics/gears/ratios/1`), then gives this one
  car more torque and grip, kept in its `overrides`; `vehicle.check` of the car itself counts them.
- The tank slews round on its tracks as it creeps forward, and the boat planes across the water
  in a curve.
- In 2D the pickup is seen from the side at 40 pixels a meter: its body and two wheels are
  `RigidBody2D`s, the wheels on sprung joints, and full throttle drives it along the road.
- An agent designs a vehicle of its own: `vehicle.design` gets a dune buggy built of parts (a
  tub that collides, a prism nose, a roll bar, a seat back and a lamp of its own color, the body's
  box not drawn) and the handling it should have (7 seconds to 100 km/h, 42 meters a second, a
  6 meter turning circle). It test-drives the buggy apart from the scene, fits its torque, drag
  and steering to those, and writes `vehicles/buggy.vehicle.json` with them as its design.
  `vehicle.place` then puts the buggy on the field and on the 2D road, where each part is a
  `Polygon2D` of its own (`buggy_2d_tub`, `buggy_2d_lamp` and the rest), and `model.inspect`
  shows its parts' materials and the tub's collision beside the body's.
- The events say who got in and out and what the car hit.
