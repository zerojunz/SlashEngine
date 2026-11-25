# Navigation

Navigation meshes, maps, paths and agents.

Status: current · Updated: 2026-11-20

`engine/navigation` is navigation, as Godot 4.6's `NavigationServer2D` and `NavigationServer3D`,
ported from Godot's `modules/navigation` (each function names the one it follows). Each world
has a 2D and a 3D map built from components: `NavArea3D` and `NavArea2D` place
`.navmesh.json` and `.navmesh2d.json` files (`$cli schema navmesh`; convex polygons over shared
vertices, as Godot's `NavigationMesh` and `NavigationPolygon`), and `NavLink3D` and
`NavLink2D` join places the regions don't. `src/core.cpp` is the map: regions' polygons
and edges keyed to the map's merge cells, edge connections across regions, links joined to the
closest polygons, and Godot's A* (`query_path`) with its corridor funnel, edge centering, path
clipping and simplification, `closest_point` and `random_point`. The 2D map is the 3D one with
points at (x, 0, y), as Godot's 2D server was before 4.5. The `navigation` system (priority
960, after physics) rebuilds a map only when its regions, links, files or settings change (its
`iteration_id` goes up), then updates each `NavAgent3D` and `NavAgent2D` as
Godot's agent nodes do and emits `path_changed`, `waypoint_reached`, `link_reached`,
`target_reached` and `navigation_finished`; agents move nothing, gameplay code moves them toward
`next_path_position_3d`. Agents' paths live beside the components by entity ID
(`navigation::save` and `restore`). `project.json`'s `navigation_2d` and `navigation_3d` set the
maps' cell sizes and margins. The commands are `navigation.path`, `navigation.closest`,
`navigation.random`, `navigation.map` and `navigation.agent` (`engine/command/src/navigation.cpp`),
and `check_scene` checks regions' files for `scene validate` and exports. Paths must be the same
everywhere: the library builds with `-ffp-contract=off`, keeps Godot's insertion order where it walks a hash map, and picks random points with a seeded PCG.
`examples/navigation` has a guard crossing a hatch and a cat in a yard.

Baking follows Godot 4.6's `NavMeshGenerator3D` and `NavMeshGenerator2D`. `src/sources.cpp`
parses the geometry under a region as Godot's parsers do (`parse_source_geometry`: meshes and
static bodies' shapes in 3D, static bodies' shapes as obstructions in 2D), `src/bake_3d.cpp`
runs Recast with Godot's settings and keeps the detail mesh's triangles, and `src/bake_2d.cpp`
merges and shrinks outlines with Clipper2 and splits them with PolyPartition. `navigation.bake`
writes the result into the region's file with `with_polygons`, keeping its settings, and records
undo with `Session::add_undo_file`, which puts back a file's text (or removes it) and refuses if
the file changed since. Recast, Clipper2 and PolyPartition build with `-ffp-contract=off`, and
the walkable slope's cosine comes from `slash::cos_sin`, so bakes are the same everywhere.
`examples/navigation-baking` bakes a courtyard and a garden.

Avoidance follows Godot 4.6's, on its changed RVO2 and RVO2-3D (`src/rvo2.hpp`,
`rvo2_2d.cpp`, `rvo2_3d.cpp`; Apache 2.0, notice kept): last in each navigation step,
`AvoidanceStep` in `src/navigation.cpp` puts every agent with `avoidance_enabled` and every
`NavObstacle2D` and 3D into a simulation as Godot's `NavMap` does (3D agents on the ground
by elevation and height unless `use_3d_avoidance`; obstacles as agents that avoid nothing, plus
outlines), steps it, and emits `velocity_computed` with each agent's safe velocity, which
`safe_velocity_2d` and `_3d` return. Agents submit their `velocity` only while they follow a
path, as Godot's agent nodes do. Every agent's new velocity is computed before any moves, so
results don't depend on order. Obstacles with `affect_navigation_mesh` are projected
obstructions in bakes (`SourceGeometry3D::projected_obstructions`), marked with
`rcMarkConvexPolyArea` in 3D and cut with Clipper2 in 2D, before the agent radius shrinks the
area or, with `carve_navigation_mesh`, after. `examples/navigation-avoidance` has walkers
weaving through a plaza.

Tile navigation follows Godot's `TileMapLayer` and `TileData`: a tile set's
`navigation_layers` and tiles' `navigation` outlines (`canvas::tile_navigation` turns them with
the cell). `gather_tile_regions` in `src/navigation.cpp` makes a region of each cell with
polygons on each navigation layer (`_navigation_update_cell`), bakes each tile's outlines once
per turn and keeps them while a layer uses the set, and keeps each layer's regions and tile set
handle until its cells, tile set or transform change, so an unchanged map isn't rebuilt. Bakes
take a layer's outlines as traversable and its collision polygons as obstructions
(`add_tiles` in `src/sources.cpp`, as `navmesh_parse_source_geometry`). `examples/navigation-tiles`
sends a mouse through a maze of tiles.
