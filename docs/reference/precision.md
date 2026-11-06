# Large-world precision

SlashEngine follows Godot's `precision` build option. A single-precision engine, the default,
keeps positions in 32-bit floats: past a few kilometers from the origin, neighboring positions
are millimeters apart, and at ten thousand kilometers a whole meter. An engine configured with
`SLASH_PRECISION=double` keeps them in doubles, for worlds that reach far from the origin (a
planet, a long road, space).

```bash
cmake --preset double                       # RelWithDebInfo in build/double, warnings as errors
cmake --build --preset double --target slash_tests slash-cli
ctest --preset double -R unit_
# or any preset, with the option:
cmake --preset dev -B build/dev-double -DSLASH_PRECISION=double
```

## What changes

| | Single (default) | Double |
| --- | --- | --- |
| `slash::real_t`, and `Vec2`, `Vec3` and `Quat`'s fields | `float` | `double` |
| Positions in scene files | written as the shortest decimal a float reads back as | written as the shortest decimal a double reads back as |
| 3D physics (Jolt) | floats | `JPH_DOUBLE_PRECISION`: body positions and queries in doubles |
| 3D drawing | around the world's origin | around a render origin near the camera |
| Gameplay libraries (`SLASH_COMPILER_ID`) | as built | ends with `, double precision` |

`engine/core` defines `SLASH_REAL_T_IS_DOUBLE` for double builds (public, so gameplay code and
plugins built through the gameplay SDK see it too), as Godot's `REAL_T_IS_DOUBLE`.

Only coordinates widen. Colors (`Vec4`), sizes, angles, speeds and the rest of a component's
`float` fields stay floats, as Godot's do, and so does everything the render device takes:
`Vec2f` and `Vec3f` are the float vectors of render and particle structures (the same types as
`Vec2` and `Vec3` in single builds), and `to_float` narrows a position to one.

## Drawing far from the origin

GPUs draw in floats. Godot's double builds emulate doubles in their shaders; SlashEngine keeps
the numbers small instead. `scene3d::Renderer` draws each frame around a render origin
(`Renderer::render_origin`): the camera's place rounded to the nearest 1024 meters on each axis
(`render_origin_for`, `kRenderOriginGrid`). Every transform, light, decal, probe, fog volume,
occluder and particle reaches the device relative to it, so what the device sees stays within a
kilometer or two of zero however far the camera is. In single builds the render origin is always
zero and frames are drawn exactly as before.

When the camera crosses into another 1024-meter cell the render origin moves. Meshes' transforms
from the frame before are moved with it, so TAA, FSR and frame generation see their motion as
before; the camera's own reprojection, and GPU particles in world coordinates, are off for that
one frame. Height fog's `fog_height` is in world coordinates, measured from the render origin as
the meshes are.

## Gameplay code and plugins

A game's code must be built with the engine's precision: its components' `Vec3`s are a different
size in the other. Engines refuse a gameplay library built for the other precision, by the
compiler ID it records (`gameplay::Host`), and `slash-cli build` builds against the engine's own
precision. Plugins see positions only through the C interface (`slash_plugin.h`), which takes
its own field types, so they load into either.

## Testing it

CI's `double` job configures the `double` preset, builds `slash_tests` and `slash-cli` and runs
the unit tests. `tests/test_precision.cpp` checks the types, scene files keeping a far position's
centimeters and a lit, shadowed and fogged room drawing the same pixels a billion meters out as at
the origin; `tests/test_physics3d.cpp` drops a crate ten thousand kilometers out.
`examples/large-world-precision` prints what a build's precision allows.
