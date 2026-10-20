# SDK package

A closed vendor SDK added to a game without touching the engine, as Godot's Android and iOS
plugins are. [project.json](project.json)'s `sdks` lists [sdks/hello](sdks/hello/sdk.json), a
package whose `sdk.json` says what each platform's build links:

- `build` names a CMake project in the package, [stand-in/](sdks/hello/stand-in/CMakeLists.txt),
  which each export builds with the game's own compilers. Here it stands in for the vendor's
  binaries; a real package lists them under `downloads` (an https URL pinned by SHA-256, or a
  `path` on this machine) and uses them as `{name}/lib/...`.
- `keys` declares `HELLO_APP_ID`, an environment variable that becomes `${HELLO_APP_ID}` in the
  define, the iOS and macOS Info.plist entry and the Android manifest's `<meta-data>`. Secrets
  never go in the project.
- Each platform links `{build}/lib/libhello.so`, `.dylib`, `.a` or `hello.lib`. Exports ship the
  shared ones where the platform's loader finds them: beside the Linux game, in the Mac app's
  `Contents/Frameworks`, in the APK's `lib/<abi>`, and `hello.dll` (listed in `files`) beside
  the Windows game.

- Android's `aars` lists [android/](sdks/hello/android/AndroidManifest.xml), a library folder
  laid out like an unpacked `.aar`. Android exports merge its manifest into the game's (a
  permission and a `<meta-data>` whose value is a string resource), compile its `res/` with
  aapt2 and give `com.example.hello` an R class. A real package lists the vendor's `.aar` files,
  and Maven coordinates such as `"maven": ["androidx.core:core:1.13.1"]`, which
  `slash-cli sdk lock` resolves and pins in `sdk.lock.json` beside `sdk.json`.

Its glue code gives the game an analytics backend (`engine/services`) in each language a vendor
SDK might have:

- [glue/hello_analytics.cpp](sdks/hello/glue/hello_analytics.cpp), listed in every platform's
  `sources`, is compiled into the game with the SDK's header. It starts the SDK in `start` and
  hands it the game's events, and `"register": "hello::register_backends"` adds it when the game
  starts.
- On Android, `java` compiles
  [HelloAnalytics.java](sdks/hello/glue/android/com/example/hello/HelloAnalytics.java), which
  implements `org.slashengine.services.AnalyticsBackend`, and `backends` names its class.
- On macOS and iOS, [HelloAnalytics.mm](sdks/hello/glue/apple/HelloAnalytics.mm) conforms to
  `SlashAnalyticsBackend`, and `backends` names it.

The exported game's test run lists the backends it started with: `"services": {"analytics":
["hello", "hello_java"]}` on Android, `["hello", "hello_objc"]` on macOS and iOS, and
`["hello"]` elsewhere.

[src/game.cpp](src/game.cpp) calls the SDK under `#ifdef SLASH_SDK_HELLO`, which exports define,
so the tools' own build of the code, which leaves SDKs out, runs too.

[calls.jsonl](calls.jsonl) checks the packages, then adds a second one already in the project:

1. `sdk.check` lists `hello` and the one problem on this machine: `HELLO_APP_ID` isn't set. Its
   `{build}` files are checked when an export builds the package.
2. `sdk.add` with `dry_run` reads [vendor/greeter](vendor/greeter/sdk.json) and says what it
   would add; then `sdk.add` lists it in project.json where it is (`"copied": false`).
3. `sdk.check` now lists both.

To export it, set the key and export as usual; the reply lists the packages it linked:

```bash
HELLO_APP_ID=my-game slash-cli export linux --project examples/sdk-package --test
```

- `slash-cli sdk add <folder|git URL|https archive>` copies a package from outside the project
  into `sdks/<name>`; `slash-cli sdk check` exits 1 on a problem, for CI; `slash-cli sdk lock`
  pins packages' Maven dependencies.
- A preset's `sdks` limits which packages it links, such as an ad network only in store builds.
