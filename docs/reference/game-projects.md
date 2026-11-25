# Game projects and exports

Game projects and exports: project.json, presets and building for each platform.

Status: current · Updated: 2026-11-22

A game is a folder with a `project.json` (name, version, main scene; `$cli schema project`) and
its scenes. `slash-cli init` creates one. `engine/runtime` runs it: `Game` loads the main scene
and ticks it, and `src/main.cpp` is every game's entry point, built on SDL's main callbacks so
the same loop works on the web and on phones.

```bash
game=./build/dev/tools/slash-game/slash-game
$game --data tests/data/game --headless --ticks 60     # run a project as its export would
$cli project validate tests/data/game
$cli export linux --project path/to/game --test        # build, lay out and run headless
```

`slash-cli export <preset>` validates the project, its presets and every scene, prefab, texture,
font, sound, `.frames.json`, `.material.json`, `.particles.json`, `.audio.json` and
`.bus_layout.json` file, stages them as the game's data, configures
`export/CMakeLists.txt` with the engine sources, builds, signs and installs into
`<project>/build/export/<preset>`. The data carries the engine's notices in
`licenses/slashengine/` (its license, `THIRD_PARTY.md` and each dependency's license file,
`cmake/stage_notices.cmake`) and each SDK package's in `licenses/<package>/`. Its work folder
and full build log are in `<project>/.slash/export/<preset>/`; a failure reports the step and the
end of the log. A Mac builds every platform: macOS and iOS with Xcode's tools, Windows and Linux
with llvm-mingw and a glibc 2.31 Debian sysroot that the first such export downloads
(`$cli toolchain install` gets them ahead of time, into `SLASH_TOOLCHAINS_DIR` or
`~/.slash/toolchains`). Linux games run on X11 or Wayland (X11 first unless
`window.linux_display_driver` says `wayland`); SDL's Wayland code comes from Wayland 1.18's
scanner, built into slash-cli and slash-mcp (`slash-cli wayland-scanner`) to fit the sysroot's
headers, and pkg-config must be installed. Linux machines build Linux and Windows the same way,
and Windows machines build Windows with MSVC. Web exports use an activated emsdk (`EMSDK` set) or else download pinned
Emscripten, Node.js and (on macOS and Windows) Python toolchains. Android exports, on any host,
use the Android SDK, NDK and JDK in `ANDROID_HOME` and `JAVA_HOME` or the ones
`$cli toolchain install android --accept-android-sdk-license` installs (it accepts Google's
Android SDK license, so an agent asks its person first). CI exports and runs an empty game for macOS, Windows, Linux, web,
Android (on an emulator) and iOS (in the Simulator) on every PR, cross-exports Windows and Linux
(x86_64 and arm64) from macOS and runs them on Windows and Ubuntu 20.04 (on X11 and Wayland
compositors there), and `slash-cli init` writes
the per-platform workflow into new games.

App icons come from one square image, `project.json`'s `icon`, or the engine's own drawn in code
(`runtime::default_icon`). `tools/exporter/src/icons.cpp` resizes it with `render::resize_image`
(Lanczos) into each platform's files: an `.icns` (macOS, an opaque icon drawn into Apple's
rounded square), an `.ico` resource (Windows), an adaptive icon (Android,
`android.icon_foreground` and `icon_background` in presets), an asset catalog compiled with
`actool` (iOS, flattened onto `ios.icon_background`) and a favicon (web). A preset's `icon`
replaces the project's; `check_icons` backs exports and `$cli project validate`. Windowed games
and `slash-cli run` show it with `Platform::set_icon`. `examples/app-icons` prints every
platform's files.

Closed vendor SDKs (ads, stores, Steamworks) come in as SDK packages: a folder with an `sdk.json`
(`$cli schema sdk`; `tools/exporter/src/sdk.cpp`) naming per platform the libraries, frameworks,
defines, files, Info.plist, entitlement and Android manifest entries and `.jar` files, with the
vendor's binaries downloaded by URL and SHA-256 into `~/.slash/sdks` or found at a local path,
and API keys as environment variables. `project.json`'s `sdks` lists them and a preset's `sdks`
picks which it links. Exports build each package's own CMake project with the game's compilers,
write `slash_game_sdk()` calls into `<build>/sdks.cmake` for `export/CMakeLists.txt`, ship shared
libraries where each platform's loader looks and define `SLASH_SDK_<NAME>` for the game's code.
Android packages also list `.aar` files and library folders, and Maven coordinates that `$cli sdk
lock` resolves without Gradle (`src/maven.cpp`, POMs read with `src/xml.cpp`) and pins by SHA-256
in `sdk.lock.json`; Android exports merge the libraries' manifests, resources, code and native
libraries into the APK (`src/android_library.cpp`). `$cli sdk add`, `sdk check` and `sdk lock`
(and the `sdk.add`, `sdk.check` and `sdk.lock` commands) add, check and lock packages;
`examples/sdk-package` has a stand-in SDK built from source, which CI links into every platform's
export. A package's glue code (`sources`, compiled into the game; `java` on Android) implements
`engine/services`' backends, and its `register` function and `backends` (Java or Objective-C
classes, made through `slash/services/bridges.hpp`: `src/android.cpp` over JNI, `src/apple.mm`
over the Objective-C runtime) go into the `<build>/sdk_register.cpp` exports write, which the
game calls at start.

Textures ship in the forms `project.json`'s `rendering.textures` asks for: as their files
(`lossless`, the default), BC7 or ASTC 4x4 blocks for each platform's GPUs (`vram_compressed`) or
RGBA8 (`vram_uncompressed`), each a KTX2 file beside where the texture was
(`render::texture_form_path`). `tools/exporter/src/forms.cpp` makes them once with
`slash::texture_encoder` (bc7enc_rdo, astc-encoder) into the asset library's `forms/` cache;
`texture.forms` makes and reports them, and games read the form their GPU samples through
`canvas::with_texture_forms`.

The export code is the `tools/exporter` library, shared by slash-cli and slash-mcp. It also
registers `export.run`, the one command that needs the `build` capability; a running game leaves
it out because an export would stall its loop.

Presets live in `export_presets.json` (`$cli schema presets`): a platform, an output folder, a
debug flag, signing settings and, for Linux, the CPU architecture. A preset named after a platform
replaces that platform's defaults. Secrets never go in the file:

| Variable | Used for |
| --- | --- |
| `SLASH_APPLE_API_KEY_PATH`, `SLASH_APPLE_API_KEY_ID`, `SLASH_APPLE_API_ISSUER` | Notarizing macOS apps (App Store Connect API key) |
| `SLASH_WINDOWS_CERTIFICATE_PATH`, `SLASH_WINDOWS_CERTIFICATE_PASSWORD` | Signing Windows executables with signtool |
| `SLASH_ANDROID_KEYSTORE_PATH`, `SLASH_ANDROID_KEYSTORE_PASSWORD`, `SLASH_ANDROID_KEY_ALIAS`, `SLASH_ANDROID_KEY_PASSWORD` | Signing release APKs (the key password defaults to the keystore's) |
| `SLASH_IOS_PROVISIONING_PROFILE` | The provisioning profile for iOS device builds (default: the newest installed one for the team and bundle ID) |

macOS exports are always signed, ad hoc unless a preset names a `codesign_identity`. Android
exports are APKs for arm64-v8a and x86_64, built without Gradle: the NDK builds `libmain.so` per
ABI, then javac, d8, aapt2, zipalign and apksigner package SDL's Java activity
(`export/android/`) with the game. Debug builds, and release builds with no keystore in the
environment, are signed with the debug keystore in `~/.android`, as Android Studio does.

iOS exports build against Xcode's SDK with CMake, with no Xcode project, then codesign signs
them (`export/ios/Info.plist.in` is the app's Info.plist). By default they are ad hoc signed
`.app` bundles for the Simulator, where `--test` runs them. A preset with a `team_id` builds an
`.ipa` for iPhones and iPads instead, signed with the team's keychain identity (Apple Development
for debug builds, Apple Distribution for release) and a provisioning profile Xcode downloaded.

Games support OS versions from 2020 on: macOS 11, iOS 14, Windows 10 (an MSVC build's subsystem
version is 10.0; llvm-mingw builds keep 6.0, since Windows 11 won't start a MinGW executable
marked 10.0), Android 11 (API level 30, targeting 36), and Chrome 85, Firefox 79 and Safari 15
(pinned in `export/CMakeLists.txt`).
