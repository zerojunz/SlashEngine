# Run 4: importer at 7789218 against Unreal Editor 5.8.4 (2026-10-27)

- slash-cli rebuilt from `claude/unreal-verify` at 7789218 (`cmake --build --preset dev --target
  slash-cli`). The build also picked up a0adde3, which draws Forward+'s sky from linear colors.
- `slash-cli import ~/SlashRunner/unreal-demo/DemoThird --project ~/SlashRunner/unreal-demo-slash-4`:
  exit 0 in 1 min 50 s, nothing on stderr. `converted` is the same as in run 3 (12 meshes,
  1 particle system, 59 files).
- `project validate` and `scene validate` on every converted file: all valid. DemoLevel has 25
  entities.
- `run --headless --ticks 60 --scene Content/Demo/DemoLevel.scene.json --save
  captures/DemoLevel.after60.scene.json`, then `frame.capture` of the saved scene. The capture
  was repeated on a copy of the scene with `auto_exposure_enabled: false`.

## Files

| Path | What |
| --- | --- |
| `import/import-report.json`, `import/import.stderr.txt` | `slash-cli import` stdout (the full report) and stderr (empty) |
| `import/unreal.json`, `import/DemoLevel.scene.json`, `import/project.json` | The import's report, the converted demo level and project settings |
| `import/NS_JumpPad_spores.process_material.json` | The Spores emitter's process material |
| `exporter/` | `export.json`, `request.json`, `unreal.log` from `.slash/import/unreal/` |
| `text/` | Every `.t3d` the export script wrote: NS_JumpPad, 34 Niagara enums, and 16 materials and material functions (run 3 had 11; the script now also reads the functions the grid material calls) |
| `validation.txt`, `run-and-capture.txt` | Commands and output, including the sampled pixels |
| `captures/DemoLevel.png`, `captures/DemoLevel_after60.png`, `captures/DemoLevel.after60.scene.json` | `frame.capture` at load and after 60 ticks, and the scene saved after 60 ticks |
| `captures/diag/no_autoexposure.*` | Copy of DemoLevel with auto exposure off, and its capture |
| `compare-DemoLevel-run4.png` | Left to right: Unreal (180 s), run 3 after 60 ticks, run 4 after 60 ticks, run 4 with auto exposure off |

## Sampled pixels

5x5 averages at points in the 1280x720 frame, sRGB 0 to 1. The run 4 values are `frame.capture`'s
`colors` (single pixels); the Unreal and run 3 values were read from their PNGs.

| Point | Unreal | Run 3 | Run 4 | Run 4, auto exposure off | Run 3, auto exposure off |
| --- | --- | --- | --- | --- | --- |
| Sky, top (640, 40) | (0.33, 0.47, 0.59) | (0.01, 0.01, 0.01) | (0.03, 0.05, 0.07) | (0.53, 0.60, 0.67) | (0.29, 0.31, 0.33) |
| Sky, near horizon (640, 240) | (0.27, 0.35, 0.47) | (0, 0, 0) | (0.01, 0.01, 0.01) | (0.29, 0.30, 0.31) | (0.06, 0.05, 0.03) |
| Lit floor (640, 620) | (0.90, 0.79, 0.65) | (0.79, 0.62, 0.42) | (0.79, 0.62, 0.42) | (1, 1, 0.97) | (1, 1, 0.97) |
| Cube's shadow (200, 530) | (0.43, 0.40, 0.36) | (0.03, 0.03, 0.04) | (0.04, 0.04, 0.04) | (0.53, 0.55, 0.57) | (0.50, 0.51, 0.52) |
| Mannequin (477, 400) | (0.76, 0.64, 0.51) | (0.03, 0.03, 0.03) | (0.04, 0.04, 0.04) | (0.73, 0.74, 0.73) | (0.68, 0.68, 0.66) |

## How it looks next to `unreal-reference/unreal-DemoLevel-180s.png`

- **The sky shader change works when auto exposure is off.** The sky is now pale blue at the
  top (0.53, 0.60, 0.67) and grey toward the horizon. In run 3 it was neutral grey with a near-black
  horizon. It is still less saturated than Unreal's, and a dark brownish band sits right at the
  horizon, where Unreal is darker blue.
- **With auto exposure on (what the import writes), the frame looks almost like run 3:** the sky is
  near black with a faint blue tint, and the shadows and mannequin are black. Only the lit floor
  is close to Unreal. So auto exposure is still the main problem: it adapts to the bright floor
  under the spot and point lights and darkens everything else by about 15x. Unreal's own auto
  exposure keeps the shadows at about 0.4 and the sky at about 0.3 to 0.5 in the same view.
- **Without auto exposure,** shadows (0.53) and the mannequin (0.73) are close to Unreal or a
  little brighter, but the lit floor blows out to white.
- **Floor:** the grid is still drawn over the cube's texture coordinates, as before, which is expected.

## Imported environment (unchanged from run 3)

`CameraAttributesPractical` has auto exposure on, sensitivity 0.03068 to 32941986.
`PhysicalSkyMaterial` has `energy_multiplier` 2.901642 and nothing else. `WorldEnvironment` uses
ACES with white 6, and its ambient and reflected light come from the sky.

## World-aligned materials

- `export.json` `world_aligned_materials` now lists exactly two:
  `/Engine/EngineSky/M_Sky_Panning_Clouds2` and **`/Game/LevelPrototyping/Materials/MI_PrototypeGrid_Gray`
  (the grid floor)**. **MI_Quinn_01/02 are gone**, and DefaultMaterial and WorldGridMaterial are gone
  too.
- I checked the dropped ones in their `.t3d` files. DefaultMaterial and WorldGridMaterial have only
  TextureCoordinate nodes, so they were flagged wrongly in run 3. M_Mannequin has WorldPosition
  nodes, but per the commit those feed only the camera fade, so it isn't world-aligned either.
  M_PrototypeGrid has WorldPosition nodes and no TextureCoordinate nodes.
- The report row is `world-aligned material`, count 2, `where`
  `/Engine/EngineSky/M_Sky_Panning_Clouds2`, `also` `["/Game/LevelPrototyping/Materials/MI_PrototypeGrid_Gray"]`.
- `also` appears on other rows too. `Niagara module ScaleColor` names Bands, with Spores in `also`.
  `exponential height fog` names DemoLevel, with Lvl_ThirdPerson in `also`. `simple collision shape`
  names Cube, with 7 meshes in `also`.

## Niagara

Same as run 3. The DemoLevel scene's entities are identical to run 3's, field for field, and so
are the Spores process material and `export.json`'s `niagara_systems` entry: `text` plus
34 enums, all with real display names. The enum names again came from the T3D fallback
(`text/enums/` has all 34). `problems` is empty, and the editor log has no Niagara errors.
The Niagara report rows are unchanged: ScaleColor ×2 (Bands, Spores), ScaleMeshSize ×1 (Bands)
and the sprite material ×1 (Spores).
