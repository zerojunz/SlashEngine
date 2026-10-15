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
| `window.height` | No | `720` | Initial window height in pixels, 1 to 16384 |

Unknown keys are errors, with a "did you mean" suggestion for a misspelled one.

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
  build/           # exports land in build/export/<preset>/ (gitignored)
  .slash/          # caches, export logs, a running server's token (gitignored)
```

Only `project.json`, `.scene.json` and `.prefab.json` files ship in an export. See
[cli.md](cli.md#export) for `slash-cli export` and [scene-format.md](scene-format.md) for scenes.
