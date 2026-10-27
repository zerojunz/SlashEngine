# Run 6: importer at ae68f1a, auto exposure's first frame (2026-10-27)

- slash-cli rebuilt from `claude/unreal-verify` at ae68f1a ("Expose a log-average version's first
  frame by its own brightness").
- No new import. I reused `~/SlashRunner/unreal-demo-slash-5` (run 5's import) and its
  `captures/DemoLevel.after60.scene.json` (DemoLevel after 60 ticks, auto exposure on,
  `auto_exposure_metering` `"log_average"`).
- First, a single `slash-cli call frame.capture` at `time` 0 (one process, as runs 4 and 5 did it).
- Then `slash-cli rpc` with two `frame.capture`s at `time` 0 and 10 in one process (`calls.jsonl`).
- Commands and replies are in `run-and-capture.txt`. Captures are in `captures/`.
- `compare-DemoLevel-run6.png`, left to right: Unreal (180 s), run 5 at t=0 (white), run 6
  single call at t=0, run 6 rpc at t=10.

## Sampled pixels

Points in the 1280x720 frame, sRGB 0 to 1.

| Point | Unreal | Run 5, t=0 | Run 5, t=10 | Run 6, call t=0 | Run 6, rpc t=0 | Run 6, rpc t=10 |
| --- | --- | --- | --- | --- | --- | --- |
| Sky, top (640, 40) | (0.33, 0.47, 0.59) | (1, 1, 1) | (0.353, 0.427, 0.494) | (0.353, 0.427, 0.494) | (0.353, 0.427, 0.494) | (0.353, 0.427, 0.494) |
| Sky, near horizon (640, 240) | (0.27, 0.35, 0.47) | (1, 1, 1) | (0.173, 0.18, 0.184) | (0.173, 0.18, 0.184) | (0.173, 0.18, 0.184) | (0.173, 0.18, 0.184) |
| Lit floor (640, 620) | (0.90, 0.79, 0.65) | (1, 1, 1) | (1, 0.976, 0.933) | (1, 0.976, 0.933) | (1, 0.976, 0.933) | (1, 0.976, 0.933) |
| Cube's shadow (200, 530) | (0.43, 0.40, 0.36) | (1, 1, 1) | (0.357, 0.373, 0.396) | (0.357, 0.373, 0.396) | (0.357, 0.373, 0.396) | (0.357, 0.373, 0.396) |
| Mannequin (477, 400) | (0.76, 0.64, 0.51) | (1, 1, 1) | (0.38, 0.392, 0.38) | (0.38, 0.392, 0.38) | (0.38, 0.392, 0.38) | (0.38, 0.392, 0.38) |

## Findings

- **The white first frame is fixed.** At t=0 the single call and the rpc capture give exactly the
  t=10 values at every point.
- The three run 6 PNGs and run 5's t=10 capture are byte-identical (same SHA-1). So the first frame
  now starts at the settled exposure. For this still view, nothing changes between t=0 and t=10.
- The settled image is unchanged from run 5, and so are its remaining differences from Unreal:
  - The lit floor is near white instead of warm beige.
  - The mannequin is about half as bright and grey.
  - The horizon is dark grey with a brownish band instead of blue.
