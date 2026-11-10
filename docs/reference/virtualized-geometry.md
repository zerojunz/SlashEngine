# Virtualized geometry (research)

A research feature with no commitment to ship ([roadmap](../roadmap.md#research)). Godot has no
counterpart. It follows the approach Brian Karis described for Unreal's Nanite in "Nanite: A Deep
Dive" (SIGGRAPH 2021) and builds on meshoptimizer's cluster LOD builder (`demo/clusterlod.h` in
its MIT source). Read here what it does now, and what it does not do yet.

## What it does

A model's surface is cut into **clusters** of up to 128 triangles. Neighbouring clusters are put
into **groups**, and each group is simplified to about half its triangles, keeping its border
with the other groups. The result is cut into new clusters, and so on until one group is left or
the simplifier gets stuck. That builds a directed acyclic graph of groups, from the surface's own
triangles at depth 0 to a few coarse clusters at the top.

Each group records its **error**: how far the clusters simplified from it may stray from the
surface, in the surface's units. Errors only grow towards the coarse end. Each group's sphere
also holds the spheres of the groups it was made from.

A view then draws, in each part of the surface, the coarsest clusters whose error it sees as at
most a pixel. A cluster is drawn when:

1. the error of the group it was simplified from (its *refined* group) projects to at most the
   threshold, or it is one of the surface's own triangles, and
2. the error of the group it is in projects to more than the threshold (the coarser clusters
   made from it would show).

The projected error is the error over the distance from the camera to the group's sphere (at
least the near plane), times the projection's pixels for a unit a unit away. Because errors and
spheres both grow towards the coarse end, every part of the surface is drawn exactly once. Parts
drawn at different depths meet with no cracks, since group borders were kept when simplifying.
Detail follows distance across one surface: the near end of a long wall or a terrain is fine,
and the far end coarse, where an LOD switches the whole mesh.

Some choices differ from meshoptimizer's demo defaults:

- **Open edges are locked**, as Godot's importer locks the border when it makes LODs. A surface's
  rim stays where it is at every depth, so pieces of a kit still meet.
- **Only the simplifier's regular mode is used.** Its permissive mode and its sloppy fallback
  can cross hard edges and leave holes that a cut cannot hide.
- **Folds are removed.** On very jagged surfaces the simplifier can leave a triangle folded back
  over its twin (the same three corners wound the other way). These pairs are taken out of each
  simplification's clusters.

Hierarchies are built with no fused multiply-adds, so they are the same on every platform.

## Using it

`MeshInstance3D.virtualized` (default `false`) draws a model's surfaces this way, in place of
their [LODs](scene-format.md#mesh-lods):

```json
"moon": {"components": {
  "Transform3D": {"position": [0, 0, -40]},
  "MeshInstance3D": {"mesh": "props/moon.model.json", "virtualized": true}
}}
```

- The threshold is project.json's
  [`rendering.mesh_lod.lod_change.threshold_pixels`](project-format.md#mesh-lods) (default `1`).
  The pixels are counted across the view's width, as LODs count them, and the entity's largest
  scale grows the errors.
- Each frame the renderer cuts each virtualized surface for the camera. It makes the cut's mesh
  again on the device only when the clusters chosen change. Shadow maps draw the camera's cut,
  and reflection probes draw the last one.
- The camera's own draw then leaves out the clusters of the cut it can't see (see
  [Culling](#culling)).
- An orthogonal camera draws the surface's own triangles.
- Skinned and lightmapped surfaces draw as they are.
- [`frame.capture`](commands.md#framecapture)'s `drawn` counts the clusters drawn as `clusters`,
  and those the camera culled as `culled`.

## Culling

As Nanite culls its clusters before it draws them, the camera's draw leaves out each cluster of
a cut that it can't see:

- **Out of view.** The cluster's sphere lies wholly outside one of the view's sides, or behind
  the camera. The planes are taken into the surface's space, scaled so they still measure
  meters, and the sphere grows by the entity's largest scale.
- **Seen from behind.** Each cluster keeps a cone around the way its triangles face
  (meshoptimizer's `meshopt_computeClusterBounds`). When the camera lies inside the cone seen
  from its apex, every triangle is seen from behind. This applies only when the material culls
  back faces and the entity's place doesn't mirror it. A cone wider than about 168 degrees culls
  nothing.
- **Behind occluders.** With [occlusion culling](scene-format.md#occlusion-culling) on, a
  cluster whose sphere the occlusion buffer hides is left out, with the same jitter timeouts
  meshes have, unless the `MeshInstance3D` has `ignore_occlusion_culling`.

Culling never changes what the frame shows. A cut with culled clusters draws its visible
clusters as a second mesh, made again only when they change, and its whole cut into shadow maps
only. A cut whose every cluster is culled draws only into shadow maps, or not at all when it
casts no shadow. Draws that cast from both sides (`cast_shadow` `"double_sided"`) and
shadow-only draws keep their whole cut. Clusters are not culled when the frame needs them from
elsewhere: path-traced frames, frames with [ray-traced](project-format.md#ray-tracing) effects,
and frames with SDFGI. An orthogonal camera does not cull them either.

In C++, `cluster_culled(cluster, view, culling)` tests one cluster against a `ClusterCulling`
(the planes in the surface's space, and whether back faces cull), and `Cluster::cone` holds the
cone. `CaptureOptions::cull_clusters` and `CanvasOptions::cull_clusters` (default `true`) turn
culling off to compare frames.

In C++, `slash/scene3d/virtual_geometry.hpp` has the parts:

- `build_cluster_hierarchy(surface)` builds the hierarchy.
- `select_clusters(hierarchy, view)` cuts it for a `ClusterView` (the camera in the surface's
  space, the projection, the view's height in pixels, the near plane, the threshold and the
  scale).
- `cluster_culled(cluster, view, culling)` says whether the view culls a cluster.
- `cluster_indices` gives the cut's triangles.
- `projected_error` gives a group's error in pixels.

## Not done yet

This is a spike to measure the idea. It still does these things the simple way:

- **The CPU picks the cut.** Each cut is drawn as one ordinary mesh, made again when the cut
  changes. Nanite culls and selects clusters on the GPU each frame and draws them in a
  persistent buffer.
- **Culling is per cut, on the CPU.** Nanite culls clusters on the GPU against the last frame's
  depth pyramid, in two passes. Here the camera's culled clusters become a mesh of their own,
  made again whenever the camera turns enough to change them.
- **No software rasterizer** for triangles smaller than a pixel. They go to the hardware
  rasterizer like any other.
- **No streaming.** The whole hierarchy stays in memory.
- **Hierarchies are built when a model first draws virtualized.** They are not stored on disk or
  in exports.
- **Materials stay per surface.** There is no visibility buffer with deferred materials.

These are the questions the spike is meant to answer before any of that: how many triangles a
cut saves at each distance, how quickly cuts change as the camera moves, and what building a
hierarchy costs. `examples/virtualized-geometry` and `tests/test_virtual_geometry.cpp` measure
them.
