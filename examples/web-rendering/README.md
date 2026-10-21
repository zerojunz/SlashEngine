# Web rendering

How a game draws in a browser. SDL3's GPU API has no web backend, so web exports draw with
SlashEngine's OpenGL ES 3.0 device, which browsers call WebGL 2: `rendering.driver` `auto` picks
it on the web. Elsewhere the `opengl_es` driver picks the same device where the system has OpenGL
ES (Android, iOS, Linux), as Godot's `opengl3_es`; macOS has none, so there `auto` keeps Metal.

[main.cpp](main.cpp) writes the textures, opens [main.scene.json](main.scene.json) and draws it
into a texture on the OpenGL ES device, or on the software device where there is none, then
prints what each pixel shows; [expected.txt](expected.txt) holds the output. The two devices
draw the same pixels, so the output is the same on both: CI checks it on Linux with Mesa's
OpenGL ES and on macOS and Windows on the CPU.

```text
........................oooo....
..............tt........oooo....
......hhhh....tt........oooo....
......hhhh....tt................
```

- The hero's material (`flash.material.json`, half flashed to white) runs the same program on
  WebGL as on the other backends: the material shader's GLSL ES is compiled from the same HLSL
  ahead of time and committed.
- To see it in a browser, export it and serve the folder (browsers won't load WebAssembly from
  a `file://` page):

  ```bash
  slash-cli export web --project examples/web-rendering   # needs Emscripten (EMSDK set)
  python3 -m http.server -d examples/web-rendering/build/export/web
  ```

- `slash-cli run --driver opengl_es` draws a window with OpenGL ES on Linux, and
  `rendering.driver` `"opengl_es"` in `project.json` makes an exported game do the same. A window
  must be made for OpenGL (`platform::Config::opengl`), which both of those do for you. The
  `opengl` driver is the same device over desktop OpenGL 3.3, with the same shaders.
