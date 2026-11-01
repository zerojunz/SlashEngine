# Captured cutscene

`video.encode` turns image files into a video a `VideoStreamPlayer` plays
([docs/reference/commands.md](../../docs/reference/commands.md#videoencode)), so an agent can
stage a cutscene in a scene, capture it frame by frame and ship it as a clip, with no video
tool of its own. [calls.jsonl](calls.jsonl) does it the way an agent does over MCP (the tool
`video_encode`) or JSON-RPC, and [results.jsonl](results.jsonl) holds the replies:

- [main.scene.json](main.scene.json) is a 96x48 window ([project.json](project.json)) with a
  ball over the ground. Six times, `component.set` moves the ball 16 pixels right and
  `frame.capture` writes the frame to `shots/f00.png` to `shots/f05.png`.
- `video.encode` takes the folder `shots/` (a folder ends in `/`; its images go in name order)
  and writes `cutscene.ivf` at 12 frames a second with a key frame every 4, which seeking starts
  from. It codes them lossy with libaom (`video::encode_lossy`), key frames and frames predicted
  from the ones before. It returns the frames, size, length and key frames, not the bytes,
  which can differ a little between machines.
- The scene's entities go, and a `VideoStreamPlayer` over the whole window plays the clip:
  three ticks in, it is at 0.2 seconds, and past its half second it stops on the last frame.

```text
video.encode {"frames":["shots/"],"out":"cutscene.ivf","fps":12,"kf_interval":4}
{"frames":6,"key_frames":[0,4],"path":"cutscene.ivf","seconds":0.5,"size":[96,48]}
```

`cq_level` trades size for detail (0 finest, 63 smallest; 32 by default) and `cpu_used`
time for size (0 slowest, 9 fastest; 6 by default). Like `frame.capture`'s `path`, it needs
the `filesystem` capability. [lossy-video](../lossy-video/) calls `video::encode_lossy` from C++.
