# Run 3: importer at f9b6f9d against Unreal Editor 5.8.4 (2026-10-27)

- slash-cli rebuilt from `claude/unreal-verify` at f9b6f9d (`cmake --build --preset dev --target slash-cli`).
- `slash-cli import ~/SlashRunner/unreal-demo/DemoThird --project ~/SlashRunner/unreal-demo-slash-3`:
  exit 0, 2 min 15 s, nothing on stderr. `converted` now has `particle_systems: 1` and 12 meshes
  (run 2: 0 and 10; the two new meshes are the Jump Pad's SM_CircularBand and SM_CircularGlow).
- `project validate` and `scene validate` on every converted file: all valid. DemoLevel now
  has 25 entities (run 2: 22); the three new ones are the Jump Pad's emitters.
- `run --headless --ticks 60 --save captures/DemoLevel.after60.scene.json` created `captures/` by
  itself this time (ecd025f). After that, `frame.capture` of the saved scene.

## Files

| Path | What |
| --- | --- |
| `import/import-report.json`, `import/import.stderr.txt` | `slash-cli import` stdout (the full report) and stderr (empty) |
| `import/unreal.json`, `import/DemoLevel.scene.json`, `import/project.json` | The import's report, the converted demo level and project settings |
| `import/NS_JumpPad_spores.process_material.json` | The Spores emitter's process material |
| `exporter/` | `export.json`, `request.json`, `unreal.log` from `.slash/import/unreal/` |
| `text/` | Every `.t3d` the export script wrote: NS_JumpPad, 9 materials and 2 material functions, 34 Niagara enums (git stores them with LF; Unreal wrote CRLF) |
| `validation.txt`, `run-and-capture.txt` | Commands and output |
| `captures/DemoLevel.png`, `captures/DemoLevel_after60.png` | `frame.capture` of DemoLevel at load and after 60 ticks |
| `captures/diag/` | Diagnostic copies of DemoLevel: `no_autoexposure` (auto exposure off) and `no_camera_attributes` (CameraAttributesPractical removed), with their captures |
| `compare-DemoLevel-run3.png` | Left to right: Unreal (180 s), run 2 after 60 ticks, run 3 after 60 ticks, run 3 with auto exposure off |

## How it looks next to `unreal-reference/unreal-DemoLevel-180s.png`

- **Sky: still wrong, and now fully black** (run 2: dark grey). Sampled pixels: top of the frame
  (0.008, 0.008, 0.012), horizon (0, 0, 0). In Unreal it is blue and brightest at the horizon.
- **Overall exposure: no longer blown out**, but now too dark. The lit floor is warm beige
  (0.76, 0.59, 0.39), close to Unreal's tone. Everything outside the lights is black: the
  mannequin is a near-black silhouette and shadows are pure black (in Unreal they are soft
  blue-grey from the sky light). The sphere shows again; in run 2 it was blown out to a sliver.
- **Auto exposure is what darkens the sky.** With `auto_exposure_enabled: false`, or with the
  CameraAttributesPractical removed (both give the same pixels), the sky is (0.29, 0.31, 0.33)
  at the top and (0.165, 0.165, 0.165) at the horizon, and the floor is blown out to white again.
  The physical sky itself draws **grey, darker toward the horizon**, not blue. Auto exposure
  then adapts to the bright floor under the spot and point lights and pushes the sky to black.
  `frames: 30` gives the same pixels as one frame, so exposure doesn't build up over captures.
- **Floor:** the grid is still drawn over the cube's texture coordinates, so it shows large
  irregular dark lines instead of Unreal's even 1 m grid, as in run 2. This is expected, since
  world-aligned materials aren't baked.

## Sky, tonemap and auto exposure in the imported scene (`environment` entity)

```json
"CameraAttributesPractical": {"auto_exposure_enabled": true,
  "auto_exposure_max_sensitivity": 32941986.0, "auto_exposure_min_sensitivity": 0.03068},
"PhysicalSkyMaterial": {"energy_multiplier": 2.901642},
"WorldEnvironment": {"ambient_light_source": "sky", "background_mode": "sky",
  "reflected_light_source": "sky", "tonemap_mode": "aces", "tonemap_white": 6.0,
  "fog_enabled": true, "fog_density": 0.002, "fog_height_density": 0.2, "fog_aerial_perspective": 1.0}
```

- PhysicalSkyMaterial: only `energy_multiplier` 2.901642. It has no `rayleigh_color`, `mie_color`,
  coefficients or eccentricity. The demo's SkyAtmosphere is Unreal's default (Rayleigh
  0.1753, 0.4096, 1.0, scale 0.0331; Mie scale 0.003996, anisotropy 0.8), and the importer
  writes only values that differ from the default.
- Tonemap: ACES, white 6.
- Auto exposure: the extended luminance range was read from DefaultEngine.ini (max sensitivity
  32.9 M ISO = EV100 20, min 0.0307 = EV100 -10). `auto_exposure_scale` isn't written (bias 1
  gives 0.4, which is Godot's default).
- Sun: DirectionalLight3D with no `light_energy` (Unreal's 10 lux becomes energy 1). Point light 625,
  spot 2500.

## World-aligned materials

- `export.json` `world_aligned_materials` lists 6: DefaultMaterial, WorldGridMaterial,
  M_Sky_Panning_Clouds2, MI_Quinn_01, MI_Quinn_02 and **MI_PrototypeGrid_Gray (the floor)**.
- The import report has one `world-aligned material` row with count 6, but its `where` names only
  `/Engine/EngineMaterials/DefaultMaterial`. Report rows are grouped by `what` and keep only the first
  `where`, so **the report doesn't name the grid floor's material**.
- Flagging MI_Quinn_01/02 (the mannequin) as world-aligned looks doubtful. Worth checking what in
  M_Mannequin matched.

## Niagara

- In `export.json`, `niagara_systems["/Game/LevelPrototyping/Interactable/JumpPad/Assets/NS_JumpPad"]`
  has `text` (`text/Game/LevelPrototyping/Interactable/JumpPad/Assets/NS_JumpPad.t3d`, 6.8 MB)
  and `enums` (34 enums). Every value is a real display name ("Random", "Cylinder",
  "Direct Set", "Box / Plane", ...). None are NewEnumeratorN and none are empty. `problems` is
  empty.
- **The enum names came from the T3D fallback, not from KismetNodeHelperLibrary.** `text/enums/` holds a
  `.t3d` for all 34 enums, and the script writes one only when the KismetNodeHelperLibrary
  calls returned no names. The editor log doesn't mention it either way (the script's `call()`
  swallows exceptions), so this run can't tell whether the class is missing from Python or
  its calls threw.
- The editor log has no Niagara errors: "Success - 0 error(s), 44 warning(s)".

### NS_JumpPad in DemoLevel (`jump_pad_fx`, with three children)

| Emitter | Became | amount | lifetime | color | gravity | shape |
| --- | --- | --- | --- | --- | --- | --- |
| Glow_Base | CPUParticles3D, mesh SM_CircularGlow | 1, explosiveness 1 | 5 | (0.125, 0.4305, 1, 2) | 0 | point |
| Bands | CPUParticles3D, mesh SM_CircularBand | 4 | 0.75 | (0.125, 0.4305, 1, 2) | (0, 6, 0) | point |
| Spores | GPUParticles3D, QuadMesh billboard, unshaded, alpha | 80 | 0.8, lifetime_randomness 0.375 | (0.125, 0.4305, 1, 2) | (0, 6, 0) | ring, radius 0.621, height 0, axis Y; scale 0.02 to 0.07 |

I checked these against the rapid iteration parameters in NS_JumpPad.t3d, and all of them match:
- Glow_Base: a one-particle burst that lives 5 s.
- Bands: spawns 5/s and each particle lives 0.75 s, so 4 at a time.
- Spores: spawns 100/s, lifetime Random from 0.5 to 0.8, Cylinder radius 62.1 cm and height 0,
  sprite size 2 to 7 cm, gravity 600 cm/s² upward.

The color is the system's `User.Color`. Its alpha is 2.0 (HDR) and was copied unchanged.

Niagara rows in the report:
- `Niagara module ScaleColor`, count 2: Bands and Spores both use it, but `where` names only Bands
  (same grouping as above). `Niagara module ScaleMeshSize`, count 1, Bands. Both say "curves
  over a particle's life don't convert yet".
- `Niagara sprite material`, count 1, Spores: "particles draw in their color".
