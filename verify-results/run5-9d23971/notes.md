# Run 5: importer at 9d23971 against Unreal Editor 5.8.4 (2026-10-27)

- slash-cli rebuilt from `claude/unreal-verify` at 9d23971 (log-average auto exposure metering).
- `slash-cli import ~/SlashRunner/unreal-demo/DemoThird --project ~/SlashRunner/unreal-demo-slash-5`:
  exit 0 in 1 min 59 s, nothing on stderr. `converted` and the report's `unsupported` rows are the
  same as in run 4. In DemoLevel, only the `environment` entity differs from run 4. `export.json`'s
  `niagara_systems` and `world_aligned_materials` are unchanged.
- `project validate` and every `scene validate` pass (`validation.txt`).
- The environment's `CameraAttributesPractical` is now
  `{"auto_exposure_enabled": true, "auto_exposure_metering": "log_average",
  "auto_exposure_min_sensitivity": 0.03068, "auto_exposure_max_sensitivity": 32941986.0}`.
  `auto_exposure_speed` isn't written, so it uses the default 0.5.
- `run --headless --ticks 60 --save captures/DemoLevel.after60.scene.json`, then two
  `frame.capture`s through `slash-cli rpc` (one process, so exposure carries over) at
  `time` 0 and 10. See `calls.jsonl` and `run-and-capture.txt`.
- `settle.txt` has the same points captured at times 0, 0.5, 2, 5, 10, 20 and 30
  (`calls-settle.jsonl`), plus a single `slash-cli call frame.capture` as run 4 did it.
- The `.t3d` text exports aren't copied this time. They are the same as run 4's.

## Files

| Path | What |
| --- | --- |
| `import/` | Import stdout report, stderr (empty), `unreal.json`, DemoLevel scene, `project.json` |
| `exporter/` | `export.json`, `request.json`, `unreal.log` (0 errors, 44 warnings) |
| `captures/DemoLevel_after60_t0.png`, `captures/DemoLevel_after60_t10.png` | Captures at times 0 and 10 |
| `captures/diag/DemoLevel_after60_t30.png` | Time 30, identical to time 10 |
| `captures/diag/DemoLevel_after60_call.png` | A single `call frame.capture`, as run 4 captured |
| `compare-DemoLevel-run5.png` | Unreal (180 s), run 4 (auto exposure on), run 5 (auto exposure on, time 10) |

## Sampled pixels

Points in the 1280x720 frame, sRGB 0 to 1.

| Point | Unreal | Run 4, AE on | Run 5, time 0 | Run 5, time 10 (settled) |
| --- | --- | --- | --- | --- |
| Sky, top (640, 40) | (0.33, 0.47, 0.59) | (0.03, 0.05, 0.07) | (1, 1, 1) | (0.35, 0.43, 0.49) |
| Sky, near horizon (640, 240) | (0.27, 0.35, 0.47) | (0.01, 0.01, 0.01) | (1, 1, 1) | (0.17, 0.18, 0.18) |
| Lit floor (640, 620) | (0.90, 0.79, 0.65) | (0.79, 0.62, 0.42) | (1, 1, 1) | (1, 0.98, 0.93) |
| Cube's shadow (200, 530) | (0.43, 0.40, 0.36) | (0.04, 0.04, 0.04) | (1, 1, 1) | (0.36, 0.37, 0.40) |
| Mannequin (477, 400) | (0.76, 0.64, 0.51) | (0.04, 0.04, 0.04) | (1, 1, 1) | (0.38, 0.39, 0.38) |

Settling over time, all through one rpc process: times 0 and 0.5 are pure white at every point.
At time 2 the sky top is (0.43, 0.51, 0.58) and the shadow 0.44. From time 5 on, the values are
identical to time 10 (and so are 20 and 30).

## Findings

- **Settled, log-average metering fixes the dark frame.** The sky top is close to Unreal's
  (a little less blue). Shadows are close (0.36 to 0.40 against 0.36 to 0.43). The frame looks
  much like Unreal's, with a blue-grey sky and grey shadows.
- **Still off when settled:**
  - The lit floor is blown out to near white (1, 0.98, 0.93) where Unreal's is warm beige
    (0.90, 0.79, 0.65).
  - The mannequin is about half as bright as Unreal's and grey (0.38) instead of warm.
  - The sky near the horizon is dark grey (0.17) instead of Unreal's blue (0.27, 0.35, 0.47), with
    the dark brownish band at the horizon seen in run 4.
- **The first frame is pure white.** The capture at time 0, and the single `call frame.capture`
  that run 4 used, are (1, 1, 1) at every point. Time 0.5 is still white. The docs say exposure
  starts from the first frame's own brightness, so this looks like a bug in the first frame
  under `log_average`, or in its starting value. It reaches the settled value between time 2 and time 5.
- **Floor:** the grid is still drawn over the cube's texture coordinates (world-aligned, expected).
