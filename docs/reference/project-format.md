# Project format

A game is a folder with a `project.json` at its root, plus its scenes. The file works like
Godot's `project.godot`: it names the game and says which scene starts it. `slash-cli init`
writes one, `slash-cli schema project` prints its JSON Schema, and `slash-cli project validate`
checks it.

```json
{
  "format": 1,
  "main_scene": "scenes/main.scene.json",
  "name": "Test Game",
  "tick_hz": 50,
  "version": "1.2.3",
  "window": {
    "height": 240,
    "width": 320
  }
}
```

| Key | Required | Default | Meaning |
| --- | --- | --- | --- |
| `format` | Yes | | Format version. Must be `1` |
| `name` | Yes | | The game's name, 1 to 100 characters, shown in its title bar. Exported files use it with only letters and digits kept (`Test Game` becomes `TestGame`) |
| `main_scene` | Yes | | The scene loaded at startup, relative to the project folder |
| `id` | No | `com.example.<name>` | Reverse-DNS identifier for bundles and app stores, e.g. `com.example.pong`. Android also needs each part to start with a letter and use no `-`, and iOS allows only letters, digits and `-` in each part |
| `version` | No | `0.1.0` | Version as major.minor.patch |
| `tick_hz` | No | `60` | Fixed simulation ticks per second, above 0 and at most 1000 |
| `window.width` | No | `1280` | Initial window width in pixels, 1 to 16384 |
| `window.height` | No | `720` | Initial window height in pixels, 1 to 16384. [Controls](scene-format.md#ui-and-text) lay out in the window as it is; `ui.layout` and `scene validate` use this size |
| `window.vsync_mode` | No | `enabled` | How frames wait for the display, as in Godot. `enabled` waits for each refresh, so nothing tears. `disabled` presents at once: the lowest latency, but it may tear. `mailbox` shows the newest finished frame at each refresh: low latency with no tearing. `adaptive` waits unless a frame is late. A mode the GPU or window lacks falls back to `enabled`; SDL's GPU API has no adaptive mode yet, so `adaptive` always does |
| `max_fps` | No | `0` | Frames per second at most, 0 to 1000, e.g. 30, 60, 120 or 240. `0` means no cap, so V-Sync alone paces frames. Headless runs ignore it |
| `rendering.clear_color` | No | `[0.3, 0.3, 0.3, 1]` | What the window shows behind everything, as `[r, g, b, a]` from 0 to 1. The default is Godot's gray |
| `rendering.driver` | No | `auto` | The GPU backend. `auto` takes the first that works: Metal on Apple platforms, Direct3D 12 then Vulkan on Windows, Vulkan on Linux and Android. `vulkan`, `metal` or `d3d12` asks for one, and `null` draws nothing. If none starts, the game logs a warning and runs with the null device, which is also what web games do for now |
| `rendering.texture_filter` | No | `linear` | How sprites whose `texture_filter` is `inherit` are sampled when scaled: `linear` is smooth, `nearest` keeps pixel art sharp |
| `rendering.interpolation` | No | `true` | Draw sprites part of the way between fixed ticks, from where they were before the last tick to where they are, so motion looks smooth when the display refreshes faster than the game ticks. Drawing lags the simulation by up to one tick; set `false` to draw exactly where things are |
| `physics_2d.gravity` | No | `980` | Gravity's strength in pixels per second squared, as Godot's `physics/2d/default_gravity` |
| `physics_2d.gravity_vector` | No | `[0, 1]` | Gravity's direction, as `[x, y]`; y points down |
| `physics_2d.linear_damp` | No | `0.1` | How fast rigid bodies slow down, per second. A body's own `linear_damp` adds to it unless its `linear_damp_mode` is `replace` |
| `physics_2d.angular_damp` | No | `1` | How fast rigid bodies stop turning, per second, combined the same way |
| `gui.theme` | No | | A [`.theme.json`](scene-format.md#themes) file every control uses after its own and its parents' themes, before Godot's default theme, as Godot's `gui/theme/custom`. `project validate` checks that it exists and `export` checks and ships it |
| `gui.allow_system_fallback` | No | `true` | Let text take characters its fonts and their [fallbacks](scene-format.md#fonts) lack from fonts installed on the machine. Off, text lays out the same on every machine, as on the web, which has none; a `.font.json` file's `allow_system_fallback` turns them off for one font |
| `input` | No | Godot's `ui_*` actions | [Input actions](#input-actions) by name, each with the events that press it |
| `input_devices.pointing.emulate_mouse_from_touch` | No | `true` | A touch also clicks the left mouse button and dragging it moves the mouse, so mouse code works on phones, as Godot's setting of the same name |
| `input_devices.pointing.emulate_touch_from_mouse` | No | `false` | A left click also touches the screen and dragging with it drags a finger, so touch code can be tried with a mouse |

Unknown keys are errors, with a "did you mean" suggestion for a misspelled one.

A running game can change `vsync_mode` and `max_fps` with
[`display.set`](commands.md#displayset), and its own code with `RenderDevice::set_vsync` and
`Loop::set_max_fps`, e.g. from an options menu. For variable refresh rate (G-Sync, FreeSync,
ProMotion), keep V-Sync enabled and, as Godot suggests, cap the frame rate a little below the
display's refresh rate. Exports ask phones for their fastest refresh rate: iOS apps set
`CADisableMinimumFrameDurationOnPhone`, so iPhones with ProMotion run at 120 Hz, and Android apps
pick the display's fastest mode at its current resolution. On the web the browser calls each
frame at the display's rate, and a frame cap sets how often it calls instead.

## Input actions

`input` is Godot's input map: actions by name, each with a `deadzone` and the `events` that press
it. Gameplay code asks about actions (`input::is_action_pressed(world, "jump")`), never keys, so
players can rebind them and agents can press them by name with
[`input.inject`](commands.md#inputinject). From [examples/input-actions](../../examples/input-actions/):

```json
"input": {
  "jump": {
    "events": [
      {"physical_key": "Space", "type": "key"},
      {"button": "a", "type": "joypad_button"}
    ]
  },
  "move_left": {
    "deadzone": 0.25,
    "events": [
      {"physical_key": "A", "type": "key"},
      {"key": "Left", "type": "key"},
      {"axis": "left_x", "axis_value": -1, "type": "joypad_motion"}
    ]
  }
}
```

| Key | Default | Meaning |
| --- | --- | --- |
| `deadzone` | `0.2` | How far a stick or trigger must move, from 0 to 1, to press the action. Past it, the action's strength runs from 0 to 1 |
| `events` | `[]` | Up to 32 bindings, any of which presses the action |

Each binding has a `type` and the fields that say which key, button or axis:

| `type` | Fields | Meaning |
| --- | --- | --- |
| `key` | `key` or `physical_key`, and `shift`, `ctrl`, `alt`, `meta` | A key by Godot's name (`"A"`, `"Space"`, `"Enter"`, `"Left"`, `"F5"`, `"Kp 1"`). `key` is what the layout labels it; `physical_key` is where it sits on a US QWERTY keyboard, so WASD stays in place on any layout. The modifiers named must be held; more are fine |
| `mouse_button` | `button`, and the modifiers | `left`, `right`, `middle`, `wheel_up`, `wheel_down`, `wheel_left`, `wheel_right`, `xbutton1` or `xbutton2` |
| `joypad_button` | `button` | `a`, `b`, `x`, `y` (the bottom, right, left and top face buttons), `back`, `guide`, `start`, `left_stick`, `right_stick`, `left_shoulder`, `right_shoulder`, `dpad_up`, `dpad_down`, `dpad_left`, `dpad_right`, `misc1`, `paddle1` to `paddle4` or `touchpad` |
| `joypad_motion` | `axis`, `axis_value` | A stick or trigger moved one way: `left_x`, `left_y`, `right_x`, `right_y`, `trigger_left` or `trigger_right`, and `-1` or `1`. Sticks go from -1 to 1 with y down, so up is `left_y` `-1` |

Every binding also takes `device`: only events from that keyboard, mouse or gamepad, from 0.
Left out, it is `-1` and matches every device.

Godot's built-in actions are always there unless an entry with the same name replaces one: each
has a deadzone of 0.5 and Godot's bindings.

| Action | Bindings |
| --- | --- |
| `ui_accept` | Enter, Kp Enter, Space |
| `ui_select` | Gamepad Y, Space |
| `ui_cancel` | Escape |
| `ui_focus_next` | Tab |
| `ui_focus_prev` | Shift+Tab |
| `ui_left`, `ui_right`, `ui_up`, `ui_down` | The arrow keys, the D-pad and the left stick |
| `ui_page_up`, `ui_page_down`, `ui_home`, `ui_end` | PageUp, PageDown, Home, End |

`project validate` checks every binding, naming a misspelled key, button or axis with the closest
one. Games read keys, the mouse, touches and gamepads from SDL; gamepads connect and disconnect as
they are plugged in, numbered from 0. A window losing focus releases everything held.

## Export presets

`export_presets.json`, next to `project.json`, holds named ways to export the game, like Godot's
`export_presets.cfg`. It is optional: without it, `slash-cli export <platform>` uses that
platform's defaults. `slash-cli schema presets` prints its JSON Schema and
`slash-cli project validate` checks it.

```json
{
  "format": 1,
  "presets": {
    "mac-store": {
      "macos": {
        "codesign_identity": "Developer ID Application: Example (ABCDE12345)",
        "notarize": true
      },
      "output": "dist/mac",
      "platform": "macos"
    },
    "windows": {
      "platform": "windows",
      "windows": {"sign": true}
    }
  }
}
```

Preset names are 1 to 64 letters, digits, `-` or `_`. A preset named after a platform replaces
that platform's defaults, so `slash-cli export windows` above signs.

| Key | Default | Meaning |
| --- | --- | --- |
| `platform` | required | `macos`, `windows`, `linux`, `web`, `android` or `ios` |
| `output` | `build/export/<preset>` | Output folder, inside the project |
| `debug` | `false` | Debug build instead of Release |
| `macos.codesign_identity` | `"-"` | Keychain identity to sign with; `"-"` signs ad hoc |
| `macos.entitlements` | none | Entitlements `.plist` to sign with, inside the project |
| `macos.notarize` | `false` | Notarize with Apple and staple the ticket; needs a Developer ID identity |
| `windows.sign` | `false` | Sign the executable with signtool |
| `windows.timestamp_url` | `http://timestamp.digicert.com` | RFC 3161 timestamp server, or `""` for none |
| `linux.arch` | `x86_64` | CPU architecture: `x86_64` or `arm64` |
| `android.abis` | `["arm64-v8a", "x86_64"]` | CPU architectures to build: `arm64-v8a`, `armeabi-v7a`, `x86_64`, `x86` |
| `android.package` | the project's `id` | Application ID; parts start with a letter and use letters, digits and `_` |
| `android.version_code` | from `version` | versionCode; by default major × 1000000 + minor × 1000 + patch, so `1.2.3` is `1002003` |
| `ios.team_id` | none | Apple Developer team ID, 10 capital letters and digits (developer.apple.com shows it under Membership) |
| `ios.destination` | `device` with a `team_id`, else `simulator` | `device` builds an `.ipa` for iPhones and iPads; `simulator` builds an `.app` for the iOS Simulator |
| `ios.bundle_id` | the project's `id` | Bundle ID; parts use letters, digits and `-` |
| `ios.codesign_identity` | the team's `Apple Development` identity (debug) or `Apple Distribution` (release) | Keychain identity for device builds, by name or SHA-1; `"-"` signs ad hoc with no profile, so it needs no team but the `.ipa` installs nowhere until re-signed |
| `ios.build_number` | from `version` | CFBundleVersion, which must grow with each upload of the same version to App Store Connect |

A `macos`, `windows`, `linux`, `android` or `ios` section is allowed only in a preset for that
platform.
Secrets never go in the file, so it can be committed: a key that looks like one (`password`, `secret`, `token`,
`private`) is an error that points to the environment variables instead. Notarizing reads an App
Store Connect API key from `SLASH_APPLE_API_KEY_PATH`, `SLASH_APPLE_API_KEY_ID` and
`SLASH_APPLE_API_ISSUER`; Windows signing reads a `.pfx` certificate from
`SLASH_WINDOWS_CERTIFICATE_PATH` and its password from `SLASH_WINDOWS_CERTIFICATE_PASSWORD`.
Release Android exports are signed with the keystore in `SLASH_ANDROID_KEYSTORE_PATH`, its
password in `SLASH_ANDROID_KEYSTORE_PASSWORD`, the key alias in `SLASH_ANDROID_KEY_ALIAS` and the
key's password in `SLASH_ANDROID_KEY_PASSWORD` if it differs; without them, and for debug builds,
the debug keystore signs, which Google Play rejects. iOS device builds embed the provisioning
profile in `SLASH_IOS_PROVISIONING_PROFILE`, or else the installed profile (Xcode keeps them in
`~/Library/MobileDevice/Provisioning Profiles`) that covers the team and bundle ID and lasts
longest, preferring an exact bundle ID to a wildcard: a development profile for debug builds and
a distribution one (App Store or Ad Hoc) for release.

## Layout

```text
my-game/
  project.json
  export_presets.json   # optional
  scenes/
    main.scene.json
  prefabs/              # optional: .prefab.json files that entities extend
    enemy.prefab.json
  sprites/              # optional: textures, .frames.json animations and .material.json
    hero.png            #   materials, in any folder
    hero.frames.json
    hit_flash.material.json
  animations/           # optional: .anim.json keyframe animations, in any folder
    hero.anim.json
  tiles/                # optional: .tileset.json tile sets and their atlas textures, in any
    ground.tileset.json #   folder
    ground.png
  fonts/                # optional: .ttf and .otf fonts, and .font.json files with settings,
    title.ttf           #   in any folder
    title.font.json
  build/           # exports land in build/export/<preset>/ (gitignored)
  .slash/          # caches, export logs, a running server's token (gitignored)
```

Only `project.json`, `.scene.json`, `.prefab.json`, `.frames.json`, `.material.json`,
`.anim.json`, `.tileset.json`, `.font.json`, `.ttf`, `.otf`, `.png` and `.bmp` files ship in an
export, and each is checked first: a texture SDL can't read or a font FreeType can't draw is an
error, like a scene, material, animation, tile set or font settings file that fails validation,
a tile map layer whose legend or rows don't match its tile set, or controls that don't lay out
as written. See
[cli.md](cli.md#export) for `slash-cli export` and [scene-format.md](scene-format.md) for scenes.
