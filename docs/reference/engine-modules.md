# Engine modules

What each engine module under engine/ holds.

Status: current · Updated: 2026-11-20

Moved from AGENTS.md, Conventions.

- Headers live in `engine/<module>/include/slash/<module>/`, sources in `engine/<module>/src/`.
  Modules: `core` (Result, logging, JSON, reflection, math, the job system), `platform` (SDL3 window, run loop,
  files and folder listings, child processes), `procedural` (FastNoiseLite noise, gradients, `.texture.json` procedural textures, `.scatter.json` rules, `.pcg.json` graphs and foliage painting), `render` (the render device over SDL3's GPU API and OpenGL 3.3 / OpenGL ES 3.0 / WebGL 2,
  the software and null devices, HLSL shaders, material programs, 2D light shading, particles and 3D meshes), `asset` (asset handles, loaders, hot reload),
  `world` (EnTT world, components, scenes and prefabs, 2D and 3D trees, and what both dimensions share: wind, physics materials and fields, damage profiles, ragdoll profiles, hair, character, face, flesh, creature and vehicle recipes, visual effects, particle simulation, fluids and water), `input` (input events, action maps,
  the input state and reading SDL's events), `l10n` (translation files, locales, plural rules,
  lookups and pseudo-localization), `audio` (sound files and streams, the mixer, buses
  and effects, players, videos' sound and SDL's audio output), `video` (AV1 video files in IVF, WebM and Matroska, decoding with dav1d,
  the engine's palette AV1 encoder, lossy encoding with libaom for the tools and the system that
  plays `Video`s), `services` (ads, analytics, in-app
  purchases, achievements and leaderboards, their backends and the test backend), `canvas` (sprites, `.frames.json`
  animations, `.material.json` materials, `.tileset.json` tile maps, 2D lights and shadows, CPU
  particles, GPU particles' steps and drawing them, 2D reflections and screen distortion), `text` (fonts, shaping, line breaking, carets and selections, and glyph atlases), `ui`
  (controls, containers and themes: layout, draw calls, buttons, focus and text fields),
  `accessibility` (screen readers through AccessKit's adapters), `animation` (`.anim.json` keyframe
  animations, `.animtree.json` animation trees and the systems that play them),
  `scene3d` (3D nodes: primitive meshes, glTF, OBJ, FBX and Blender models, model recipes, CSG shapes, grid maps, terrains, materials, cameras, lights and environments, skeletons, IK and ragdolls, characters, creatures and vehicles built as models, fracture, and drawing them), `physics2d` (2D bodies, areas, joints, queries, vehicles, ragdolls, cloth, hair, destruction and the fluids' and water's 2D host on Box2D), `physics3d` (3D bodies, areas, character movement, queries, vehicles, soft bodies and cloth, hair, destruction and the fluids' and water's 3D host on Jolt), `navigation` (navigation meshes, maps, paths and agents), `npc` (NPC brains, goal-oriented action planning, the system that runs agents' plans, quests, dialogue and fauna), `net` (multiplayer peers, the ENet transport, a world's multiplayer and RPCs), `xr` (the XR server, interfaces, trackers, XR nodes, the test headset and OpenXR), `library` (the asset library, its
  generators and commands), `storyboard` (storyboard kinds, files, versions,
  queries, edits, briefs and checks), `soundtrack` (`.score.json` scores, MIDI files, rendering stems, drafting cue sheets and arranging them),
  `command` (commands, sessions with undo, JSON-RPC and MCP),
  `debug` (the debug overlay over a running game), `autopilot` (Autopilot's agent service, or SlashEngine's own agent, playing a game), `server` (localhost HTTP over a session),
  `ai` (language model providers, their adapters, `providers.json`, replay and the `ai.*`
  commands),
  `gameplay` (loading and reloading a game's own code), `plugin` (plugins over the C interface
  in `slash_plugin.h`) and `runtime` (project files and the game executable).
  Tool-only libraries such as `tools/exporter`, `tools/importer` and `tools/shader_compiler` use
  the same layout and may use exceptions.
