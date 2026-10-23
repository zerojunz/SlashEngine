# Import a Godot level

`project.import` converts the nodes a Godot 4 level is built from: CSG shapes, paths, grid maps,
joints, vehicles and navigation. [godot/](godot/) holds "Fort" as Godot saves it:
[project.godot](godot/project.godot), a mesh library of grass and stone blocks
([blocks.tres](godot/blocks.tres)) and one scene, [main.tscn](godot/main.tscn).
[calls.jsonl](calls.jsonl) imports it, opens the scene, asks about the floor, the tower and a
path across the yard, lets the cart settle and looks at the frame, as an agent would over MCP or
JSON-RPC; [results.jsonl](results.jsonl) holds the replies.

- The tower is a `CSGCombiner3D` of a `CSGCylinder3D` with a `CSGBox3D` door subtracted from it
  (`operation = 2` is `"subtraction"`). Each shape keeps its fields and its `material` becomes its
  `StandardMaterial3D`; `csg.get` reports the cylinder's red faces and the door's, which have no
  material, cut into it.
- The wall is a `CSGPolygon3D` pushed along the `WallLine` path: its `Curve3D` becomes the
  `Path3D`'s `points` and `path_node` names the path's entity. The guard is a `PathFollow3D` on
  the closed `Patrol` path, placed three meters along it, at its second corner, facing along it.
- `blocks.tres` becomes `godot/blocks.meshlib.json`, each `item/<id>/` an item named after its
  name with its mesh, material and box shape. The `GridMap`'s `data`, Godot's cells packed three
  numbers each, becomes its floor of keys: grass, with a row of stone across the middle.
- The cart is a `VehicleBody3D` with four `VehicleWheel3D`s. It weighs Godot's 40 kilograms
  and comes to rest on the grid map's blocks, its center about 0.55 meters above them.
- The yard's `NavigationMesh` becomes `godot/main_navigation_mesh_yard.navmesh.json`, and
  `navigation.path` finds the way across it, round the corner of its L shape.

`frame.capture` reads the tower, the door's cut, the stone road, the guard's top, the grass, the
wall, the cart and the background from the frame the camera sees.
