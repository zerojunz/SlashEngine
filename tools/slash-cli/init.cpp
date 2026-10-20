#include "init.hpp"

#include "slash/core/log.hpp"
#include "slash/core/version.hpp"
#include "slash/exporter/export.hpp"
#include "slash/platform/file.hpp"
#include "slash/platform/process.hpp"
#include "slash/runtime/project.hpp"
#include "slash/storyboard/storyboard.hpp"
#include "slash/world/component_registry.hpp"
#include "slash/world/components.hpp"
#include "slash/world/scene.hpp"
#include "slash/world/world.hpp"

#include <array>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace slash::cli {
namespace {

constexpr std::string_view kMainScene = "scenes/main.scene.json";

constexpr std::string_view kAgents = R"(# AGENTS.md

This is a SlashEngine game project. SlashEngine is built for agents: you inspect and change the
game through engine commands instead of editing files by hand.

## Connect to the engine

The `slashengine` MCP server is configured for Claude Code (`.mcp.json`), Codex
(`.codex/config.toml`) and Cursor (`.cursor/mcp.json`), and opens `scenes/main.scene.json` at
startup. Its tools check every change against the engine's schemas, support undo and return
compact results. Use them rather than editing `.scene.json` files directly: the server holds the
open scene, so a hand edit is overwritten the next time the scene is saved.

Without MCP, the same commands run from the shell:

```bash
slash-cli call engine.describe
slash-cli call world.query '{"with": ["Transform2D"]}' --scene scenes/main.scene.json
slash-cli call batch '{"commands": [...]}' --scene scenes/main.scene.json --save
```

To find code or content, call `project_map` before reading files: `find` names a component,
system, event, action or function and returns where it is declared and used, and `paths` returns
what a change to those files touches.

## Use the storyboard

`storyboard/` holds the game's facts that are not code: its style, characters, places, story,
design rules, names, decisions and open work, as typed entries such as `character/mira`. Look facts
up instead of inventing them, and record new ones so other agents see them:

- Before a task, call `storyboard_brief` with the scenes, entities or files it touches. It returns
  only the entries linked to them, the rules that apply everywhere, and open tasks and decisions.
- Record new facts with `storyboard_edit`, passing the version `storyboard_query` or
  `storyboard_brief` returned; an edit based on stale facts is refused. Say who you are in `by`
  and why in `why`; `storyboard/journal.jsonl` keeps the record.
- Put `{"StoryRef": {"ids": ["character/mira"]}}` on the entity that plays an entry.
- After a task, call `storyboard_check`. It finds references to missing entries and words the
  glossary says to avoid. If the project has a `providers.json`, `storyboard_review` asks a model
  whether new dialogue, interface text or art contradicts the storyboard; its findings are advice.
- Before starting work, add a `task/...` entry with your name as `owner`, so others see it.

## Test the game

`tests/` holds `.test.json` files (`slash-cli schema test`). Each test opens a scene and runs
engine commands in order, with what each reply must hold in `expect`:

```json
{"scene": "scenes/main.scene.json",
 "tests": {"crate_lands": {"steps": [
   {"command": "run.until",
    "params": {"where": [{"component": "RigidBody2D", "field": "sleeping", "value": true}]},
    "expect": {"matched": true, "entities": ["crate"]}}]}}}
```

After a change, save the scene (tests start from scene files) and call `test_run`, or run
`slash-cli test`. It runs every test headless, each in a session of its own, plus
`storyboard_check`, and returns only the checks that failed: a step whose command failed, a
reply that doesn't hold what the step expects, or a warning or error logged while it ran. There
are no logs or screenshots to read. Add a test for each behavior you build and each bug you fix;
a recorded run (`replay.run` to a `tick`, then `world.query`) makes a good one.

## Drive a running game

`slash-cli run --serve` plays the game and serves the same commands over HTTP on this machine,
so you can inspect and change the live world. `slash-mcp --live --project .` connects an MCP
client to it; for Claude Code: `claude mcp add slashengine-live -- slash-mcp --live --project .`.
The address and token are in `.slash/server.json`, which stays out of git. Edited prefab and
asset files reload while it runs: prefab instances update and keep their overrides.

## Work efficiently

- Call `engine_describe` once to learn the components (with default field values) and commands.
- Group related edits in one `batch` call. It is all or nothing, and one `undo` reverts it.
- Add `"dry_run": true` to any edit or `batch` to see what it would change, changing nothing.
  `world_snapshot`, then `world_diff`, lists what changed since; `world_restore` goes back.
- `world_query` returns only entity IDs unless you pass `include`. Filter with `where`.
- `component_set` returns only the fields that changed.
- To test gameplay, `run_until` steps the game headless until an entity matches `with`/`where`
  (or none does) and returns the tick and the matches: one call instead of a step and a query
  per tick. Keep the checks you want to repeat as tests in `tests/`.
- For many similar entities, `prefab_save` one to `prefabs/<name>.prefab.json` and create the
  rest with `entity_create` and `"extends"`. Each saves only the fields it overrides, and editing
  the prefab changes them all.
- Errors give the JSON Pointer of the bad value and suggest the right name; fix it and retry.
- Call `scene_save` when you are done, then `slash-cli scene validate <file>` to double-check.
- Copy from the engine's examples, which its CI runs on every change: C++ components, systems
  and commands, and each feature's command calls with their results. They match the engine
  version this project pins:
  https://github.com/zerojunz/SlashEngine/tree/@ENGINE_REF@/examples

## Gameplay code

The game's own components and systems are plain C++ in `src/` (`project.json`'s
`gameplay.sources`), as Godot's scripts are; there is no scripting language. `src/game.cpp`
registers them in its `SLASH_GAME_MODULE` function; copy its pattern. Before calling engine C++
you haven't seen, call `api_search` with its name or what it should do (`"play a sound"`): it
returns the real signature, doc comment and header, and lines of the engine's verified examples
that call it, so you don't guess names. After each edit, call
`build_check` (or run `slash-cli build --check`): it compiles only the files that changed and
returns each error and warning with its file, line and column, and the compiler's fix as edits
when it has one. When the code is right, call `gameplay_build` (or run `slash-cli build`): it
links the code and swaps it into the open world, which keeps its entities and every field the
code still has. `engine_describe` then lists the new components and
systems. Keep gameplay state in registered components, so it is saved, undone and kept when the
code is rebuilt. Exports link the code into the game.

## Run and export

`slash-game --data .` runs the game the way its exports do, once `slash-cli build` has built its
code. `slash-cli export <platform> --test`
(or the `export_run` tool) builds it for macos, windows, linux, web, android or ios into
`build/export/<platform>`, then runs the build headless where this machine can (Android games on
a connected device or emulator, iOS games in the Simulator). A failed export reports the step and
the end of its log.
`.github/workflows/export.yml` exports and tests every platform on GitHub Actions.

Presets in `export_presets.json` (`slash-cli schema presets`) name an output folder, a debug build
and how to sign; export one with `slash-cli export <preset>`. Signing secrets come from
environment variables, never from the file.

## Files

- `src/`: the game's C++ code (see Gameplay code).
- `project.json`: the game's name, version and main scene. `slash-cli project validate` checks it,
  `export_presets.json` and the storyboard.
- `scenes/`: scenes as canonical JSON, keyed by stable entity ID. Run
  `slash-cli scene format <file>` before committing a scene edited outside the engine.
- `prefabs/`: `.prefab.json` files, one entity's components each, which scene entities extend.
- `sprites/` (or any folder): `.png` textures for `Sprite2D` (and its normal maps and light
  textures), `.frames.json` animations (`slash-cli schema frames`) that `AnimatedSprite2D` plays,
  and `.material.json` materials (`slash-cli schema material`), node graphs that color a sprite's
  pixels. Exports ship them all. Check lighting with `light_sample` rather than a screenshot.
- `animations/` (or any folder): `.anim.json` keyframe animations (`slash-cli schema animation`)
  that `AnimationPlayer` plays on an entity's fields. Check one with `animation_eval`.
- `tiles/` (or any folder): `.tileset.json` tile sets (`slash-cli schema tileset`) that cut atlas
  textures into tiles and name the terrains they connect. A `TileMapLayer` draws them; paint one
  with `tilemap_paint` rather than editing its rows by hand.
- Physics bodies and areas are components on scene entities, as Godot's nodes: `RigidBody2D`,
  `CharacterBody2D`, `Area2D` and the rest, with `CollisionShape2D` shapes. Ask what is where
  with `physics_raycast`, `physics_overlap` and `physics_contacts` rather than by geometry.
- Particles are `CPUParticles2D` components, as Godot's, with a texture from `sprites/`, or
  `GPUParticles2D` ones for many thousands, moved on the GPU as a `.process_material.json` file
  (`slash-cli schema process_material`) says. Check either with `particles_eval` (how many,
  where, and each one) rather than a screenshot.
- UI is `Control`, `Label`, `Button` and the other UI components, as Godot's Control nodes,
  laid out in the window by anchors and offsets or by containers. `fonts/` (or any folder):
  `.ttf` and `.otf` files, and `.font.json` files (`slash-cli schema font`) for distance-field
  fonts that stay sharp at any size or that list `fallbacks` for the characters a font lacks
  (Arabic, Japanese and so on). `Control.layout_direction` `rtl` mirrors a menu for
  right-to-left languages. Check UI with `ui_layout` (each control's rect, each label's
  lines, each button's state and which control has focus) rather than a screenshot. Buttons
  emit `pressed` events; try one with `ui_press`, or a click or `ui_accept` with `input_inject`.
  `LineEdit` is a text field and `TextEdit` one of several lines: type into them with
  `input_inject` `{"type": "text", "text": "Mira"}` after a click or `ui_focus`, and read
  `text_changed` events (and a LineEdit's `text_submitted`).
- Controls are input actions in `project.json`'s `input`, as Godot's input map: name what the
  game does (`jump`, `move_left`) and bind keys, mouse buttons, gamepad buttons and sticks to it;
  gameplay code reads actions, never keys. Try them with `input_inject` (keys, clicks, touches,
  sticks or actions by name) and `step`, then `input_state`.
- `audio/` (or any folder): `.wav`, `.ogg` and `.mp3` sounds, `.audio.json` files
  (`slash-cli schema audio`) that loop one or pick one of several at random, and the
  `.bus_layout.json` file (`slash-cli schema bus_layout`) that `project.json`'s
  `audio.bus_layout` names: buses such as Music and Sfx with their volumes and effects.
  `AudioStreamPlayer` and `AudioStreamPlayer2D` components play them. Check sound with
  `audio_state` and `audio_capture` (levels in dB, slice by slice) rather than by ear.
- `storyboard/`: the game's facts as typed entries (`slash-cli schema storyboard`), and
  `journal.jsonl`, the record of who changed them. Commit both.
- `tests/`: `.test.json` tests that `test_run` and `slash-cli test` run, and the export workflow
  runs on every push.
)";

constexpr std::string_view kGameCode =
    R"(// The game's own C++ code, as Godot's scripts: components that scenes use and systems the world
// runs every tick. `slash-cli build` (or the gameplay_build tool) compiles it, and the tools swap
// it into the open world, keeping its entities and their fields. Exports link it in.

#include "slash/world/components.hpp"
#include "slash/world/game_module.hpp"

namespace {

// A starter component: replace it with the game's own.
struct Spin {
    float speed = 1.0F; // radians a second
};
SLASH_COMPONENT(Spin, speed)

void spin(slash::world::World& world, double tick_seconds) {
    const auto seconds = static_cast<float>(tick_seconds);
    world.ecs().view<slash::world::Transform2D, const Spin>().each(
        [seconds](slash::world::Transform2D& transform, const Spin& turn) {
            transform.rotation += turn.speed * seconds;
        });
}

} // namespace

SLASH_GAME_MODULE(game) {
    game.add_component<Spin>()
        .set_doc("Turns an entity at a steady speed.")
        .describe("speed", {.doc = "Radians a second, clockwise."});
    game.add_system("spin", spin, {.doc = "Turns entities that have Spin."});
}
)";

constexpr std::string_view kSkill = R"(---
name: slashengine
description: Build or change scenes in this SlashEngine game through the slashengine MCP tools. Use when creating scenes, or adding, finding or editing entities and their components.
---

# Editing SlashEngine scenes

1. Call `engine_describe` to see the components, their default field values and the open scene.
2. Call `storyboard_brief` with the scenes or entities you will touch, and use the names and rules
   it returns rather than inventing them.
3. Open a scene with `scene_open`, or start one with `scene_new` and a path.
4. Make related changes in one `batch` call, for example:

   ```json
   {"commands": [
     {"command": "entity.create",
      "params": {"id": "player", "components": {"Transform2D": {"position": [64, 32]}}}},
     {"command": "component.set",
      "params": {"id": "enemy_01", "components": {"Transform2D": {"rotation": 1.57}}}}
   ]}
   ```

5. Check the result with `world_query` (IDs only unless you pass `include`) or `entity_get`. To
   see what a player would, call `frame_summary`: what shows where, what is covered, offscreen
   or can't draw, and with `map` a text picture. Use `frame_capture` with `points` (pixel colors)
   or `"image": true` and a small `scale` only when colors matter.
6. If something is wrong, `undo` reverts the whole batch.
7. Call `scene_save`, then `test_run`: it runs the tests in `tests/` and `storyboard_check`, and
   returns only the checks that failed.

A run recorded with `slash-cli run --record bug.replay.json` (or `replay_record` and
`replay_stop`) plays back exactly with `replay_run`: pass `tick` to stop where a bug happens and
query the world there, and rerun it after a fix to see whether and where the run now differs.
`replay_seek` then moves through it: `ticks` steps forward or back, and `with`/`where` finds the
first tick something happens (say, the player's position.y > 600).

Do not edit `.scene.json` files by hand while the server runs: it holds the open scene and would
overwrite the edit on the next save.
)";

// Exports the game on every platform's own runner. @ENGINE_REF@ becomes the engine commit.
constexpr std::string_view kWorkflow =
    R"(# Exports the game for every platform on native runners and runs each export headless.
# Written by slash-cli init. SLASHENGINE_REF is the engine commit or tag the game builds
# against, and must exist on github.com/zerojunz/SlashEngine. To sign, use preset names from
# export_presets.json instead of platforms and set the signing secrets as repository secrets.
# If the game is in a subfolder of its repository, move this file to the repository's
# .github/workflows folder and set GAME_DIR.
name: Export

on:
  push:
    branches: [main]
  pull_request:
  workflow_dispatch:

permissions:
  contents: read

env:
  SLASHENGINE_REF: @ENGINE_REF@
  GAME_DIR: .
  EMSDK_VERSION: 6.0.10

jobs:
  export:
    name: Export (${{ matrix.platform }})
    runs-on: ${{ matrix.os }}
    strategy:
      fail-fast: false
      matrix:
        include:
          - platform: macos
            os: macos-14
          - platform: windows
            os: windows-2022
          - platform: linux
            os: ubuntu-24.04
          - platform: web
            os: ubuntu-24.04
          - platform: android
            os: ubuntu-24.04
          - platform: ios
            os: macos-14
    steps:
      - uses: actions/checkout@v4
        with:
          path: game

      - uses: actions/checkout@v4
        with:
          repository: zerojunz/SlashEngine
          ref: ${{ env.SLASHENGINE_REF }}
          path: slashengine

      - name: Cache the engine's dependencies
        uses: actions/cache@v4
        with:
          path: slashengine/.cpm-cache
          key: slashengine-${{ matrix.os }}-${{ env.SLASHENGINE_REF }}

      - name: Cache the toolchains Linux exports download
        if: matrix.platform == 'linux'
        uses: actions/cache@v4
        with:
          path: ~/.slash/toolchains
          key: slash-toolchains-${{ matrix.os }}-${{ env.SLASHENGINE_REF }}

      - name: Install tools (macOS)
        if: runner.os == 'macOS'
        run: brew install ninja

      - name: Install tools (Linux)
        if: runner.os == 'Linux'
        run: >-
          sudo apt-get update && sudo apt-get install -y ninja-build libx11-dev libxext-dev
          libxrandr-dev libxcursor-dev libxfixes-dev libxi-dev libxss-dev libxtst-dev
          libxkbcommon-dev libwayland-dev libegl-dev libgl-dev libdrm-dev libgbm-dev
          libasound2-dev libpulse-dev libudev-dev libdbus-1-dev

      - name: Install tools (Windows)
        if: runner.os == 'Windows'
        run: choco install ninja -y

      - name: Set up MSVC (Windows)
        if: runner.os == 'Windows'
        uses: ilammy/msvc-dev-cmd@v1

      - name: Install Emscripten
        if: matrix.platform == 'web'
        run: |
          git clone --depth 1 --branch "$EMSDK_VERSION" https://github.com/emscripten-core/emsdk.git "$RUNNER_TEMP/emsdk"
          "$RUNNER_TEMP/emsdk/emsdk" install "$EMSDK_VERSION"
          "$RUNNER_TEMP/emsdk/emsdk" activate "$EMSDK_VERSION"
          echo "EMSDK=$RUNNER_TEMP/emsdk" >> "$GITHUB_ENV"

      - name: Build slash-cli
        run: |
          cmake -S slashengine -B slashengine/build -G Ninja -DCMAKE_BUILD_TYPE=Release
          cmake --build slashengine/build --target slash-cli

      - name: Check the project and its storyboard
        shell: bash
        run: slashengine/build/tools/slash-cli/slash-cli project validate "game/$GAME_DIR"

      - name: Run the tests
        shell: bash
        run: slashengine/build/tools/slash-cli/slash-cli test --project "game/$GAME_DIR"

      - name: Export and run headless
        shell: bash
        run: >-
          slashengine/build/tools/slash-cli/slash-cli export ${{ matrix.platform }}
          --project "game/$GAME_DIR" --engine slashengine --test

      - name: Upload the game
        uses: actions/upload-artifact@v4
        with:
          name: ${{ matrix.platform }}
          path: game/${{ env.GAME_DIR }}/build/export/${{ matrix.platform }}

      - name: Upload the export log
        if: failure()
        uses: actions/upload-artifact@v4
        with:
          name: export-log-${{ matrix.platform }}
          path: game/${{ env.GAME_DIR }}/.slash/export/${{ matrix.platform }}/export.log
          if-no-files-found: ignore
)";

// Folders that stay on this machine: engine caches and the running server's token, and exports.
constexpr std::array<std::string_view, 2> kIgnoredFolders{".slash/", "build/"};
constexpr std::string_view kGitignore =
    "# SlashEngine caches, the running server's token, and exported builds\n.slash/\nbuild/\n";

bool has_line(std::string_view text, std::string_view line) {
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        std::string_view current = text.substr(start, end - start);
        if (!current.empty() && current.back() == '\r') {
            current.remove_suffix(1);
        }
        if (current == line) {
            return true;
        }
        start = end + 1;
    }
    return false;
}

// The game's name, from its folder: "games/space-pong" becomes "space-pong".
std::string project_name(const std::string& directory) {
    std::error_code error;
    std::filesystem::path path = std::filesystem::absolute(directory, error).lexically_normal();
    if (path.filename().empty()) {
        path = path.parent_path();
    }
    std::string name = path.filename().string();
    constexpr std::size_t kMaxName = 100;
    if (name.size() > kMaxName) {
        name.resize(kMaxName);
    }
    return name.empty() ? std::string("Game") : name;
}

json::Value server_entry(const InitOptions& options, std::string_view project) {
    json::Value entry;
    entry["command"] = options.mcp_command;
    entry["args"] = json::Array{"--project", project, "--scene", kMainScene};
    return entry;
}

std::string json_config(const InitOptions& options, std::string_view project) {
    json::Value config;
    config["mcpServers"]["slashengine"] = server_entry(options, project);
    return json::write(config) + "\n";
}

std::string toml_string(std::string_view text) {
    return "\"" + log::escape_json(text) + "\"";
}

std::string codex_config(const InitOptions& options) {
    return "[mcp_servers.slashengine]\ncommand = " + toml_string(options.mcp_command) +
           "\nargs = [\"--project\", \".\", \"--scene\", " + toml_string(kMainScene) + "]\n";
}

// The engine commit to pin CI to: the checkout exports build from, or else this version's tag.
std::string engine_ref() {
    if (const auto engine = exporter::find_engine(); engine) {
        auto head = platform::run_process({"git", "-C", *engine, "rev-parse", "HEAD"});
        if (head && head.value().exit_code == 0) {
            const std::string commit = head.value().tail.substr(0, head.value().tail.find('\n'));
            constexpr std::size_t kShaLength = 40;
            if (commit.size() == kShaLength &&
                commit.find_first_not_of("0123456789abcdef") == std::string::npos) {
                return commit;
            }
        }
    }
    return "v" + std::string(slash::version());
}

// The text with @ENGINE_REF@ replaced by the engine commit.
std::string with_engine_ref(std::string_view text) {
    std::string out(text);
    constexpr std::string_view kRef = "@ENGINE_REF@";
    const std::string ref = engine_ref();
    for (std::size_t at = out.find(kRef); at != std::string::npos;
         at = out.find(kRef, at + ref.size())) {
        out.replace(at, kRef.size(), ref);
    }
    return out;
}

// A storyboard entry name from the game's name: "Space Pong!" becomes "space-pong".
std::string entry_name(std::string_view name) {
    std::string out;
    for (const char c : name) {
        const bool letter = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (letter || (c >= 'A' && c <= 'Z')) {
            out += letter ? c : static_cast<char>(c - 'A' + 'a');
        } else if (!out.empty() && out.back() != '-') {
            out += '-';
        }
    }
    while (!out.empty() && out.back() == '-') {
        out.pop_back();
    }
    if (out.size() > storyboard::kMaxNameLength) {
        out.resize(storyboard::kMaxNameLength);
    }
    return out.empty() ? std::string("game") : out;
}

// A starter storyboard: the game's name in the glossary, and the first piece of open work.
std::string starter_glossary(const std::string& name) {
    json::Value term;
    term["name"] = name;
    term["summary"] = "The game's title.";
    json::Object entries;
    entries.emplace("term/" + entry_name(name), std::move(term));
    return storyboard::format_file(entries);
}

std::string starter_work() {
    json::Value task;
    task["name"] = "Fill in the storyboard";
    task["summary"] = "Record the game's tone, characters, places and rules, so every agent "
                      "works from the same facts.";
    task["status"] = "open";
    json::Object entries;
    entries.emplace("task/fill-in-the-storyboard", std::move(task));
    return storyboard::format_file(entries);
}

// A starter test: the main scene runs for a second with no warning or error.
std::string starter_test() {
    json::Value step;
    step["command"] = "step";
    step["params"]["ticks"] = 60;
    json::Value test;
    test["steps"] = json::Array{std::move(step)};
    json::Value file;
    file["scene"] = kMainScene;
    file["tests"]["main_scene_runs"] = std::move(test);
    return json::write(file) + "\n";
}

std::string empty_scene() {
    const world::ComponentRegistry components;
    const world::World scene(components);
    return json::write(world::save_scene(scene)) + "\n";
}

} // namespace

Result<json::Value> init_project(const InitOptions& options) {
    // Cursor expands ${workspaceFolder}; Claude Code and Codex start servers in the project.
    const std::string name = project_name(options.directory);
    const std::string glossary = storyboard::file_path("glossary");
    const std::string work = storyboard::file_path("work");
    json::Value project = runtime::new_project(name, kMainScene);
    project["gameplay"]["sources"] = json::Array{"src"};
    const std::vector<std::pair<std::string_view, std::string>> files{
        {runtime::kProjectFile, json::write(project) + "\n"},
        {"AGENTS.md", with_engine_ref(kAgents)},
        {"CLAUDE.md", "@AGENTS.md\n"},
        {".mcp.json", json_config(options, ".")},
        {".cursor/mcp.json", json_config(options, "${workspaceFolder}")},
        {".codex/config.toml", codex_config(options)},
        {".claude/skills/slashengine/SKILL.md", std::string(kSkill)},
        {".github/workflows/export.yml", with_engine_ref(kWorkflow)},
        {kMainScene, empty_scene()},
        {"src/game.cpp", std::string(kGameCode)},
        {"tests/main.test.json", starter_test()},
        {glossary, starter_glossary(name)},
        {work, starter_work()},
    };

    std::string root = options.directory;
    if (!root.empty() && root.back() != '/' && root.back() != '\\') {
        root += '/';
    }
    json::Array created;
    json::Array skipped;
    for (const auto& [relative, contents] : files) {
        const std::string path = root + std::string(relative);
        if (!options.force && platform::exists(path)) {
            skipped.emplace_back(relative);
            continue;
        }
        if (const std::size_t slash = path.find_last_of('/'); slash != std::string::npos) {
            if (auto made = platform::create_directories(path.substr(0, slash)); !made) {
                return made.error();
            }
        }
        if (auto written = platform::write_file(path, contents); !written) {
            return written.error();
        }
        created.emplace_back(relative);
    }
    // .gitignore is never replaced, even with --force: the line is added if it is missing.
    const std::string gitignore = root + ".gitignore";
    json::Array updated;
    if (!platform::exists(gitignore)) {
        if (auto written = platform::write_file(gitignore, kGitignore); !written) {
            return written.error();
        }
        created.emplace_back(".gitignore");
    } else if (auto text = platform::read_file(gitignore); !text) {
        return text.error();
    } else {
        std::string contents = text.value();
        bool changed = false;
        for (const std::string_view folder : kIgnoredFolders) {
            if (has_line(contents, folder)) {
                continue;
            }
            if (!contents.empty() && contents.back() != '\n') {
                contents += '\n';
            }
            contents += std::string(folder) + "\n";
            changed = true;
        }
        if (!changed) {
            skipped.emplace_back(".gitignore");
        } else if (auto written = platform::write_file(gitignore, contents); !written) {
            return written.error();
        } else {
            updated.emplace_back(".gitignore");
        }
    }

    json::Object result;
    result.emplace("created", std::move(created));
    result.emplace("skipped", std::move(skipped));
    if (!updated.empty()) {
        result.emplace("updated", std::move(updated));
    }
    return json::Value(std::move(result));
}

} // namespace slash::cli
