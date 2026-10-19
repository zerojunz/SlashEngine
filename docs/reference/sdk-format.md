# SDK packages

An SDK package adds a closed vendor SDK (an ad network, a store, analytics, Steamworks, an
upscaler) to a game without changing the engine, as Godot's Android and iOS plugins do. A
package is a folder with an `sdk.json` that says what each platform's build links. The vendor's
binaries are never committed to the engine's repository: `sdk.json` names a download pinned by
SHA-256, or a path on the machine that exports.

`project.json`'s `sdks` lists the packages' folders. `slash-cli sdk add` adds one,
`slash-cli sdk check` (or [`sdk.check`](commands.md#sdkcheck)) checks them,
`slash-cli schema sdk` prints the JSON Schema, and every [export](cli.md#export) builds and links
the packages for its platform. [examples/sdk-package](../../examples/sdk-package/) has one.

```json
{
  "format": 1,
  "name": "hello",
  "version": "2.1.0",
  "license": "Proprietary",
  "license_file": "{vendor}/LICENSE.txt",
  "downloads": {
    "vendor": {
      "url": "https://example.com/hello-sdk-2.1.0.zip",
      "sha256": "<64 lowercase hex digits>",
      "folder": "hello-sdk"
    }
  },
  "keys": {
    "HELLO_APP_ID": {"description": "The game's ID on the Hello dashboard."}
  },
  "platforms": {
    "linux": {
      "include": ["{vendor}/include"],
      "libraries": ["{vendor}/lib/linux-{arch}/libhello.so"],
      "defines": ["HELLO_APP_ID=\"${HELLO_APP_ID}\""]
    },
    "windows": {
      "include": ["{vendor}/include"],
      "libraries": ["{vendor}/lib/win64/hello.lib"],
      "files": ["{vendor}/bin/win64/hello.dll"]
    },
    "ios": {
      "frameworks": ["{vendor}/Hello.xcframework"],
      "system_frameworks": ["StoreKit"],
      "plist": {"HelloAppID": "${HELLO_APP_ID}"}
    },
    "android": {
      "libraries": ["{vendor}/lib/{arch}/libhello.so"],
      "jars": ["{vendor}/java/hello.jar"],
      "permissions": ["android.permission.INTERNET"],
      "application": ["<meta-data android:name=\"com.hello.APP_ID\" android:value=\"${HELLO_APP_ID}\" />"]
    }
  }
}
```

| Key | Required | Meaning |
| --- | --- | --- |
| `format` | Yes | Must be `1` |
| `name` | Yes | 1 to 64 lowercase letters, digits, `-`, `_` and `.`. Games are compiled with `SLASH_SDK_<NAME>` defined (`SLASH_SDK_HELLO`; other characters become `_`), so code can call the SDK only where it is linked |
| `version` | Yes | The SDK's version, as its vendor writes it |
| `license` | Yes | An SPDX identifier, or `Proprietary` |
| `license_file` | No | A file shipped with the game's data as `licenses/<name>/<file>`, as many SDK licenses require |
| `vendor`, `description` | No | Shown by `sdk check` and `sdk add` |
| `downloads` | No | The vendor's files, by name, used in paths as `{name}` |
| `build` | No | `{"cmake": "<folder>", "options": {"NAME": "value"}}`: a CMake project in the package that each export builds with the game's compilers and installs into `{build}`. For SDKs that ship source, and for glue code |
| `keys` | No | Environment variables the package reads at export time, by name (capital letters, digits and `_`), each with a `description` and `optional` (`false` by default; an optional key that isn't set is `""`) |
| `platforms` | No | What the package adds to each platform's builds: `macos`, `windows`, `linux`, `web`, `android` and `ios`. Exports for other platforms leave the package out |

## Downloads

A download is either `{"url", "sha256"}`, fetched with curl over `https://` (or `file://`), or
`{"path"}`, a file or folder on this machine (relative to the package, or absolute with
`${VARIABLE}`s from the environment, such as `"${STEAMWORKS_SDK}"` for an SDK behind a vendor
login).

| Key | Default | Meaning |
| --- | --- | --- |
| `url` | | Where the file is. Only `https://` and `file://` |
| `sha256` | | The file's SHA-256. A file that doesn't match is deleted and the export fails |
| `unpack` | `true` for `.zip`, `.7z` and `.tar` files, compressed or not, else `false` | Unpack the archive (with `cmake -E tar`), or keep the file as it is, such as a `.jar` |
| `folder` | `""` | The folder inside the archive that `{name}` means |
| `path` | | A local file or folder instead of a URL |

Downloads are fetched once per machine into `SLASH_SDKS_DIR`, or else `~/.slash/sdks`, in a
folder named by the start of their SHA-256, and shared by every project.

## Platforms

Paths are relative to the package and may start with `{sdk}` (the package's folder), `{build}`
(its CMake project's install folder) or a download's `{name}`. `{arch}` is the build's
architecture: the Linux preset's `x86_64` or `arm64`, each Android ABI (`arm64-v8a`,
`x86_64`...), `x86_64` on Windows, `universal` on macOS, `device` or `simulator` on iOS and
`wasm32` on the web. Paths can't use `..` or `${...}`; give the folder a download with a `path`
instead.

| Key | Platforms | Meaning |
| --- | --- | --- |
| `include` | All | Header folders the game's code is compiled with |
| `defines` | All | `NAME` or `NAME=value`; values may use `${KEY}` |
| `libraries` | All | Static libraries, shared libraries and Windows import libraries (`.lib`) to link. Shared libraries ship where the loader finds them: beside a Linux game (found through `$ORIGIN`), in a Mac app's `Contents/Frameworks` (given an `@rpath` install name), in an iOS app's `Frameworks` and in the APK's `lib/<abi>`. Linux and Android link them by file name, so a library without a SONAME still loads. A `.dll` here is an error: link its `.lib` and list the DLL in `files`. The web takes static libraries only, built with Emscripten |
| `frameworks` | macOS, iOS | `.framework` or `.xcframework` folders; an `.xcframework`'s slice for the build is picked from its `Info.plist`. Dynamic ones are embedded in `Frameworks` and signed before the app |
| `system_libraries` | All | Libraries of the system or compiler, by name: `ws2_32`, `dl`, `c++` |
| `system_frameworks` | macOS, iOS | Apple's frameworks by name: `StoreKit`, `GameKit` |
| `link_options` | All | Linker options, such as `-ObjC` |
| `files` | All | Files and folders shipped beside the game: next to the executable on Windows and Linux, in `Contents/Resources` on macOS (a `.dylib` goes in `Frameworks`), in the app on iOS, in `assets` on Android (a `.so` goes in `lib/<abi>`) and next to `index.html` on the web |
| `plist` | macOS, iOS | Info.plist entries; strings may use `${KEY}`. Keys the engine sets itself, such as `CFBundleIdentifier`, are errors, and two packages that set one key differently are an error |
| `entitlements` | macOS, iOS | Code signing entitlements. A Mac app is signed with them added to the preset's `macos.entitlements`; an iOS device build's provisioning profile must grant each one, or the export says which capability to turn on |
| `permissions` | Android | `<uses-permission>` names, listed once however many packages ask |
| `manifest` | Android | XML elements added to `<manifest>`, such as `<queries>`; may use `${KEY}` (escaped for XML) |
| `application` | Android | XML elements added to `<application>`, such as `<meta-data>` and a vendor's `<activity>`; may use `${KEY}` |
| `jars` | Android | Java libraries the APK's Java is compiled against and dexed with |

`sdk.json` files are checked as a whole before anything is built: unknown fields and platforms,
placeholders and keys that don't exist (with "did you mean"), Apple and Android fields on other
platforms, malformed defines and XML.

## In project.json and presets

```json
{
  "format": 1,
  "name": "Space Pong",
  "main_scene": "scenes/main.scene.json",
  "sdks": ["sdks/hello", "sdks/ads"]
}
```

`sdks` lists package folders, relative to the project; two packages can't share a name. An
export links every listed package with a section for its platform, unless its preset's `sdks`
names the ones to link:

```json
{"format": 1, "presets": {"store": {"platform": "android", "sdks": ["ads", "hello"]}}}
```

## Exports

An export with packages adds an `sdks` step after staging the data: it fetches each download not
fetched yet, then for each build (each Android ABI, each Linux architecture) builds the packages'
CMake projects with the same compilers and options as the game, fills in the paths for its
`{arch}`, checks every file exists, and writes the `slash_game_sdk()` calls that
`export/CMakeLists.txt` includes into `<build>/sdks.cmake`. A missing key, file or download fails
the export before the game is built, with the package and the JSON Pointer of the bad value. The
reply lists the packages linked as `"sdks": [{"name", "version", "license"}]`.

## sdk check

`slash-cli sdk check` and `sdk.check` load the packages, the presets' `sdks` and the keys from
the environment, and check that each file a platform links exists, for every architecture the
presets build. Downloads not fetched yet are listed as `unchecked` unless `--fetch` (or
`"fetch": true`, which needs the `build` capability) fetches them. `{build}` paths are checked
when an export builds the package. `slash-cli project validate` runs the same checks except keys
and downloads, which belong to the machine that exports.

```json
{"sdks": [{"name": "hello", "version": "2.1.0", "license": "Proprietary",
           "folder": "sdks/hello", "platforms": ["android", "ios", "linux", "windows"],
           "unchecked": ["vendor"]}],
 "problems": [{"sdk": "sdks/hello", "path": "/keys/HELLO_APP_ID",
               "message": "set the environment variable HELLO_APP_ID: The game's ID on the Hello dashboard."}],
 "ok": false}
```

## sdk add

`slash-cli sdk add <source>` and `sdk.add` add a package to `project.json`'s `sdks`. A folder in
the project is listed where it is. Anything else is copied into `sdks/<name>` (or `--name`): a
folder elsewhere, a git repository (an address ending in `.git`, or `git@` or `ssh://`, cloned
with `--depth 1` at `--ref`), or an `https://` `.zip` or `.tar` archive with `sdk.json` at its
root or in its one folder, checked against `--sha256` when given. The command takes folders in
the project, and URLs with the `build` capability. The reply names the package, its platforms and
the keys to set.
