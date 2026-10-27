# Unreal importer check against a real Unreal Editor (2026-10-27)

- Unreal Engine 5.8.4 (CL 58902052, `++UE5+Release-5.8`), Epic Games Launcher install.
- Editor used: `/Users/Shared/Epic Games/UE_5.8/Engine/Binaries/Mac/UnrealEditor-Cmd` (found by the
  importer itself; `SLASH_UNREAL_EDITOR` was not needed).
- slash-cli built from `claude/unreal-verify` at 4c9ab12 (`cmake --preset dev` + ccache).
- Demo project: TP_ThirdPersonBP + its LevelPrototyping, Characters and Input content packs,
  PythonScriptPlugin, EditorScriptingUtilities and Niagara enabled, and a level
  `/Game/Demo/DemoLevel` built headless by `demo-project/make_demo_level.py` and `add_niagara.py`.
  The level is the default map.

## Folders

| Path | What |
| --- | --- |
| `import/import-report.json`, `import/import-dry-run-report.json` | `slash-cli import` results (real and `--dry-run`) |
| `import/unreal.json` | The import's full report, including `unsupported` |
| `import/DemoLevel.scene.json`, `import/project.json` | The converted demo level and project settings |
| `exporter/` | `unreal.log`, `export.json` and `request.json` from `<project>/.slash/import/unreal/` |
| `validation.txt` | `project validate` and `scene validate` on every converted scene, `.anim.json` and `.animtree.json` |
| `run-and-capture.txt` | `run --headless --ticks 60` and `call frame.capture` output |
| `captures/` | SlashEngine CPU captures: DemoLevel at load and after 60 ticks, Lvl_ThirdPerson |
| `demo-project/` | The scripts that built the level, their results, the `.uproject` and `DefaultEngine.ini` |
| `probes/` | Read-only probes of Unreal 5.8.4's Python API, and the log tail of the failed Unreal screenshot run |

No Unreal screenshot: the full editor exits at startup with "cannot execute tool 'metal' due to
missing Metal Toolchain; use: xcodebuild -downloadComponent MetalToolchain".
