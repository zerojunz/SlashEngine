# glTF instances

Blender's geometry nodes and Houdini export scattered copies of a mesh through glTF's
`EXT_mesh_gpu_instancing`: one node with the mesh, and accessors of each copy's translation,
rotation and scale. [models/orchard.gltf](models/orchard.gltf) is such a file, six trees in two
rows, each turned and sized differently.

A model with them loads each copy as a node of its own under the instancing node, named after it
and its number (`Tree_0` to `Tree_5`), drawing the mesh where the copy is; the instancing node
draws nothing itself. So the copies are parts as any other: `MeshInstance3D` draws them,
`model.inspect` lists them, collision hints, LODs and lightmaps take them, and `write_glb` writes
them out as plain nodes.

```text
model.inspect {"path":"models/orchard.gltf","brief":true}
{"parts": [{"name": "Tree_0", "center": [-3, 1, -2], "size": [1.2, 2, 1.2], ...},
           {"name": "Tree_1", "center": [0, 1.2, -2.5], "size": [1.3819, 2.4, 1.3819], ...}, ...],
 "triangles": 144, ...}
frame.capture {"points":[[46,44],[46,52],[82,32],[80,80]]}
{"colors": [<leaves>, <bark>, <leaves>, <grass>], "drawn": {"meshes": 13}, ...}
```

[results.jsonl](results.jsonl) holds the replies.
