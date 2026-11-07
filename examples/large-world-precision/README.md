# Large-world precision

What an engine's precision allows, as Godot's `precision` build option. [main.cpp](main.cpp)
prints the step between neighboring positions at 1, 100 and 10,000 kilometers from the origin,
drops a crate onto a floor at home and ten thousand kilometers out, and asks where the 3D world
is drawn around for a camera out there. [expected.txt](expected.txt) holds a single-precision
build's output, which CI checks.

- A default engine keeps positions in floats (`slash::real_t` is `float`): ten thousand
  kilometers out, neighboring positions are a whole meter apart, so the crate aimed 0.3 meters
  east and 0.2 north of the floor's middle lands on its middle, and the camera's 300.25 becomes
  300.
- Configured with `-DSLASH_PRECISION=double` (or `cmake --preset double`), the engine keeps them
  in doubles and Jolt in double precision: the steps are a few nanometers even out there, the
  crate lands where it does at home, and the camera keeps its quarter meter. Built that way,
  this program prints:

  ```text
  precision: double (positions are 64-bit)

  step between neighboring positions:
           1 km: 1.13687e-10 mm
         100 km: 1.45519e-08 mm
       10000 km: 1.86265e-06 mm

  a crate dropped 0.3 m east and 0.2 m north of a floor's middle rests at:
           0 km out: x 0.300, y 0.484, z -0.200
       10000 km out: x 0.300, y 0.484, z -0.200

  a camera at (10000300.25, 1.80, -5000.00) draws around (10000384, 0, -5120)
  ```

- The 3D world is drawn around a render origin near the camera (`scene3d::render_origin_for`):
  the camera's place rounded to whole 1024 meters, so what reaches the GPU in floats stays near
  zero. In a single-precision build it is always the origin.

[docs/reference/precision.md](../../docs/reference/precision.md) has the rest.
