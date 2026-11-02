# Runtime meshes

Ground and a minimap made by gameplay code while the game runs, as Godot's `SurfaceTool`,
`ArrayMesh` and `ImageTexture`. [main.cpp](main.cpp) adds a game's own `Ground` component and a
system that, each time a ground's `seed` changes:

- samples `procedural::Noise` over a 16 x 16 grid of cells,
- builds hills from it with `scene3d::SurfaceTool`: a vertex at each grid point colored by its
  height, two clockwise triangles a cell by `add_index`, then `generate_normals` and
  `generate_tangents`, committed with a material that takes the vertex colors,
- makes the hills a mesh with `scene3d::make_mesh(world, "ground", ...)`, which the entity's
  `MeshInstance3D` draws as `"made:ground"`,
- paints the same heights into an image and makes it a texture with
  `procedural::make_texture(world, "minimap", ...)`, which a `Sprite2D` shows as
  `"made:minimap"`.

The agent then looks at what the code made with the commands it uses on files:

- Before the first tick nothing has made them, so `frame.capture` sees the clear color and
  draws nothing. Renderers log no warning for a made asset not made yet.
- `asset.list` lists both made assets with their types and versions.
- `model.inspect` with `brief` gives the hills' size and triangles, and `texture.generate` the
  minimap's channels.
- `frame.capture` reads the hills and the minimap.
- A new `seed` set with `component.set` builds both again in place, and the minimap's channels and
  the frame's colors change.

Made assets live in the game's assets until the code drops them (`asset::Assets::drop`), so a
scene saved with `"made:ground"` in it draws again as soon as the code makes the mesh;
`scene validate` doesn't look for them.
