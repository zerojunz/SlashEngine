# App icons

Exports make every platform's icons from one square image, as Godot's do from
`application/config/icon`. [main.cpp](main.cpp) makes a project in code whose `project.json`
names its icon, a gold coin on a clear background:

```json
{"format": 1, "name": "Coins", "main_scene": "main.scene.json", "icon": "art/coin.png"}
```

and an `export_presets.json` that gives Android's adaptive icon and iOS's opaque icon a
night-blue background:

```json
"android": {"platform": "android", "android": {"icon_background": "#1d2b53"}},
"ios": {"platform": "ios", "ios": {"icon_background": "#1d2b53"}}
```

Then it prints the icon files each platform's export ships:

1. **macOS**: `AppIcon.icns` in the app bundle, with the icon at every size macOS asks for, as
   Godot writes it: PNG images from 1024 to 32 pixels, then 32 and 16 pixel ones run-length
   encoded with masks. An opaque icon would be drawn into Apple's rounded square; the coin has
   its own shape, so it stays as it is.
2. **Windows**: `icon.ico`, PNG images from 16 to 256 pixels, compiled into the executable as
   its icon. Explorer shows it, and the window too.
3. **Linux**: no files; the game's window shows the icon.
4. **Web**: `favicon.png`, and `apple-touch-icon.png` for phones' home screens, which
   `index.html` links.
5. **Android**: an adaptive icon. Launchers mask and move its two layers: the coin fills the
   middle two thirds of the foreground, the part every mask shows, over the background color.
6. **iOS**: an asset catalog with one 1024-pixel image, which `actool` makes every size from.
   iOS icons are opaque, so the coin's clear corners become the background color.

Last, a wide image is refused: icons are square on every platform. Exports and
`slash-cli project validate` report it with the setting's JSON Pointer.

- With no `icon`, games get the engine's: a white slash on a dark blue square, drawn in code.
- A preset's `icon` replaces the project's for that preset, and Android presets can give the
  adaptive icon its own `icon_foreground` image.
- Give a 1024x1024 image: the App Store asks for that size, and smaller ones are scaled up.
