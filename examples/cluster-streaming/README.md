# Cluster streaming (research)

A research spike of Nanite-style streaming for virtualized geometry
([docs/reference/virtualized-geometry.md](../../docs/reference/virtualized-geometry.md#streaming)).
Godot has nothing like it. [props/moon.model.json](props/moon.model.json) is the cratered moon of
[virtualized-geometry](../virtualized-geometry/), 41,018 triangles, drawn with
`MeshInstance3D.virtualized`. [project.json](project.json) gives each virtualized surface a pool of
256 KB for its cluster pages and lets it load two pages a frame:

```json
"rendering": {"virtual_geometry": {"max_page_loads_per_frame": 2, "streaming_pool_kb": 256}}
```

The moon's hierarchy is kept as a page file: an index of every group's and cluster's bounds,
errors and links, which stays loaded, and a page per group of its clusters' triangles. When the
moon first draws, only the coarsest group's page is loaded. Each frame the camera's cut is made
from the pages that are loaded, coarser where finer ones are missing, and asks for the pages its
full cut lacks, the most visible first.

[calls.jsonl](calls.jsonl) drives it the way an agent does over MCP or JSON-RPC, and
[results.jsonl](results.jsonl) holds the replies. `frame.capture`'s `drawn` counts the pages
loaded (`pages`) and those asked for and still missing (`pending_pages`):

- 40 meters away, the first frame draws 4 coarse clusters into `shots/far.png` while two more
  pages load (`pages`: 3). The next frame draws the 12 clusters that distance needs, and nothing
  is pending.
- Moved to 2.5 meters, the moon needs much finer clusters. The first frame still draws the 12
  and asks for 7 pages, of which 2 load. Frame by frame the cut fills in (21, 32, 45 clusters
  drawn) as the pending pages load, with no cracks between the parts at different depths.
- By the fifth frame all 11 pages are loaded and the cut is the one drawn without streaming: 58
  clusters drawn and 16 culled, into `shots/close.png`.

With a smaller pool the moon holds fewer pages and stops short of the full cut: at 128 KB it
keeps 8 pages and draws 38 clusters close up, with 3 pages still asked for. Pages no cut uses are
dropped, least recently used first, when a page needs room.

This is research with no commitment to ship. The page file is written in memory when the moon
first draws, standing in for one read from disk, and pages load on the CPU between frames.
