# Import a shipped Unity game

`project.import` also takes a game built with Unity: its folder, its executable, an `.apk` or
`.ipa` file or a web build. AssetRipper rebuilds the Unity project the game was made from, the
importer fixes what a project rebuilt from a build gets wrong, then converts it as it would a
project made in Unity. AssetRipper is a GPL program, so it only ever runs as a program of its own
(a session must grant the `build` capability to start it); `SLASH_ASSETRIPPER` names it, or the
first import installs it with `slash-cli toolchain install assetripper`.

[main.cpp](main.cpp) plays the part of Unity's build and writes `shipped/Hopper/`, "Hopper" as a
Linux IL2CPP build lays it out. The example runs AssetRipper's stand-in from
[tests/](../../tests/fake_assetripper.cpp), which exports [ripped/](ripped/) as AssetRipper
exports this build's project: the scripts of Unity's packages decompiled under
`Assets/Scripts/UnityEngine.UI/` with GUIDs of its own, the game's script
([Hopper.cs](ripped/Assets/Scripts/Assembly-CSharp/Hopper.cs)) a stand-in whose code stayed
compiled, and sprites as assets of their own ([star.asset](ripped/Assets/Sprite/star.asset)) on
the atlas texture. Then it makes the calls an agent would make with only the shipped game;
[expected.txt](expected.txt) holds the replies and the files the import wrote. From a shell,
`slash-cli import shipped/Hopper --unpack-to unity` does the same.

```text
project.import {"source":"shipped/Hopper","unpack_to":"unity"}
{"converted": {"assets": 1, "scenes": 1, ...}, "files": 17,
 "main_scene": "unity/Assets/Scenes/Main.scene.json", "pixels_per_unit": 16.0,
 "unpacked": {"assetripper": "2.0.0",
              "build": {"backend": "il2cpp", "platform": "linux", "unity": "2022.3.20f1"},
              "files": 5, "folder": "unity", "log": ".slash/import/unity/assetripper.log",
              "package_scripts": 4, "scenes": 1, "scripts": 1,
              "unsupported": [{"what": "script without its code", ...}]}, ...}
```

- The scene's references to the scripts of Unity's packages (uGUI, TextMesh Pro, the event
  system, URP's 2D lights) name the GUIDs those scripts have in every Unity project again, so the
  HUD's `Image` becomes a `ColorRect`. The stand-ins of Unity's own scripts are dropped. A package
  script the importer doesn't know gets its class name written as Unity 6 writes one, so the
  report names it.
- Sprites saved as `.asset` files keep their atlas rects, trimmed edges and pivots, and the
  project's pixels per unit comes from them: AssetRipper gives the atlas texture Unity's default
  of 100. A sprite turned in its atlas, or packed to its outline, is reported.
- An IL2CPP build keeps its code compiled, so `Hopper.cs` comes back without it; the objects
  using it keep their values and a storyboard task to port it to C++. A Mono build's scripts come
  back as decompiled C#. A UI component whose values AssetRipper couldn't read is reported, not
  converted with Unity's defaults.
- `unpack_to` keeps the whole rebuilt project in `unity/`, and the import uses its assets there.
  Without it, only the assets come along. AssetRipper's work and log stay in
  `.slash/import/unity/`.
- A build whose bundles are encrypted (UnityCN) is refused, and a running game refuses to
  unpack builds, which takes minutes. Unpack only games you made or have the rights to.
