# Joints and vehicles

3D joints and a car on Jolt Physics, as Godot 4.4's Jolt module. [main.scene.json](main.scene.json)
hangs a lamp from a `PinJoint3D` and a bell from a `ConeTwistJoint3D`, puts a door on a
`HingeJoint3D` and a drawer on a `SliderJoint3D`, and parks a car (`VehicleBody3D`) on four
`VehicleWheel3D` wheels. [main.cpp](main.cpp) drives them as an agent would, with
`component.set`, and prints where everything got to. [expected.txt](expected.txt) holds its
output.

- A joint is an entity with a joint component and a `Transform3D`. `node_a` and `node_b` name the
  bodies it holds; a missing one is the world, so the lamp's cord and the bell's mount hold them
  up. The joint is made where its `Transform3D` puts it, in its own axes: a hinge turns about its
  z axis, a slider moves along its x axis and a cone twist swings around its x axis. The door's
  hinge is turned so its z axis points up, and the bell's mount so its x axis points down.
- Limits, motors and springs are Godot's: the drawer stops half a meter out, the bell swings no
  more than its `swing_span` of 0.35 radians, and the door's motor, turned on by a
  `component.set`, opens it at a radian a second until its upper limit stops it.
  `Generic6DOFJoint3D` locks, limits, drives or springs each of six axes.
- Jointed bodies don't collide with each other while `exclude_nodes_from_collision` is on, as in
  Godot. Changing a joint's own transform or bodies makes it again where it now is; changing a
  limit or a motor rebuilds it in place and wakes its bodies.
- The car rolls on Godot's `VehicleBody3D` suspension, ported line for line: each wheel casts a
  ray down its y axis, its spring and dampers push the body up, and friction along and across its
  axle drives, steers and brakes it. The car's `engine_force`, `brake` and `steering` go to the
  wheels marked `use_as_traction` and `use_as_steering`; positive engine force drives toward +z
  and positive steering turns left, toward +x. Each step writes what a wheel found
  (`in_contact`, `contact_point`, `suspension_length`, `rpm`, `skid_info`) and moves its
  `Transform3D` with its suspension, steering and roll.
- As in Godot, the car is a `RigidBody3D` of 40 kilograms. Its wheels are the `VehicleWheel3D`
  entities among its `Transform3D` children.
- Jolt is built cross-platform deterministic and steps on one thread, so this output is checked
  exactly on macOS, Windows and Linux.
