# Debug overlay

The debug overlay over a running game.

Status: current · Updated: 2026-11-20

`engine/debug` is the panel a person watching `slash-cli run` sees: `debug::Overlay` keeps
its controls in a world of its own (an embedded scene in `src/overlay.cpp`), takes F3 and the
pointer on its panel through `SdlReader::set_filter` before the game sees them, and in
`update` runs its Pause, Step and Shapes buttons as the `pause`, `play` and `step` commands
through the game's session, then shows the frame stats, the tick, what `CanvasRenderer`
drew and the bodies under the mouse (`physics2d::overlap_point`). `CanvasOptions::overlay`
draws its controls last, laid out with the game's fonts so the glyph pages stay put. Collision
shapes are `canvas::collision_shapes` (`engine/canvas/src/debug_draw.cpp`), ports of Godot's
`CollisionShape2D` and `CollisionPolygon2D` debug drawing placed as `physics2d` places shapes;
`CanvasOptions::collisions` and `frame.capture`'s `collisions` draw them. `--debug-overlay`,
`--debug-collisions` and the `debug.overlay` command turn them on. `examples/debug-overlay`
captures the shapes and works the panel.
