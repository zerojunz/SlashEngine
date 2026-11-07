# Virtualized geometry (research)

A research spike of Nanite-style virtualized geometry
([docs/reference/virtualized-geometry.md](../../docs/reference/virtualized-geometry.md)). Godot has
nothing like it. [props/moon.model.json](props/moon.model.json) is a cratered moon of 41,018
triangles, built from a sphere with six spheres cut out of it. [main.scene.json](main.scene.json)
puts one moon two meters from the camera and one 120 meters away. Both draw with
`MeshInstance3D.virtualized`:

```json
"MeshInstance3D": {"mesh": "props/moon.model.json", "virtualized": true}
```

Each moon's surface is cut into clusters of up to 128 triangles. The clusters are grouped and
simplified again and again into a hierarchy. Each frame, every part of the surface draws the
coarsest clusters whose error the camera sees as at most a pixel. Parts drawn at different
depths meet with no cracks.

[calls.jsonl](calls.jsonl) drives it the way an agent does over MCP or JSON-RPC, and
[results.jsonl](results.jsonl) holds the replies:

- `model.inspect` gives the moon's 41,018 triangles, and the LODs it would draw otherwise.
- `frame.capture` draws both moons into `shots/moons.png`. The camera cuts 81 clusters in all.
  It culls the 17 on the near moon's far side, which face away (`drawn.culled`), and draws 64
  (`drawn.clusters`). The frame is the same as with all 81: culling only leaves out what can't
  be seen, and the whole cut still casts shadows.
- With `virtualized` off, the far moon draws a coarse LOD instead (`lods`: 1). The near moon
  alone is cut into 74 clusters, 57 of them drawn.
- Moved 60 meters away, the near moon needs only 12. Each is so coarse that it faces too many
  ways to cull.

This is research with no commitment to ship. The cut is picked and culled on the CPU and drawn
as a mesh, made again when it changes. There is no GPU culling, software rasterizer or
streaming yet.
