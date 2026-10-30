# Third-party software

SlashEngine is licensed under Apache 2.0 (see `LICENSE`). It uses the following third-party
software, each under a permissive license. Dependencies are fetched at build time unless noted as
vendored. `cmake/dependencies.json` pins each one to an exact tag with its SPDX license and the
SHA-256 of its license file, and the `licenses` test fails a pull request that adds a dependency
or license not allowed here, or whose license text changed without review.

| Name | Version | License | Used for | How it is included |
| --- | --- | --- | --- | --- |
| [SDL3](https://github.com/libsdl-org/SDL) | release-3.4.18 | zlib | Platform layer: windows, events, timers, GPU; two patches: one lets Vulkan shaders see 16 KiB of each uniform push, and one hands the Metal, Vulkan and Direct3D 12 devices' native objects to MetalFX and games' temporal upscalers (`cmake/patches/`) | Fetched, linked statically |
| [EnTT](https://github.com/skypjack/entt) | v3.16.0 | MIT | Entity component system | Fetched, header-only |
| [yyjson](https://github.com/ibireme/yyjson) | 0.13.0 | MIT | JSON parsing and number formatting | Fetched, linked statically |
| [Box2D](https://github.com/erincatto/box2d) | v3.1.1 | MIT | 2D physics: bodies, shapes, joints, queries | Fetched, linked statically |
| [Jolt Physics](https://github.com/jrouwe/JoltPhysics) | v5.6.0 | MIT | 3D physics: bodies, shapes, queries and motion tests, as Godot 4.4's Jolt module uses it | Fetched, linked statically |
| [FreeType](https://github.com/freetype/freetype) | VER-2-14-3 | FreeType License (FTL), or GPL 2.0 at the user's choice; SlashEngine uses it under the FTL | Loading fonts and drawing glyph images | Fetched, linked statically |
| [HarfBuzz](https://github.com/harfbuzz/harfbuzz) | 14.5.0 | MIT ("Old MIT" and MIT) | Text shaping | Fetched, linked statically |
| [SheenBidi](https://github.com/Tehreer/SheenBidi) | v3.0.0 | Apache 2.0 | The Unicode Bidirectional Algorithm and script runs, for right-to-left text | Fetched, linked statically |
| [miniaudio](https://github.com/mackron/miniaudio) | 0.11.25 | MIT-0 or Unlicense (public domain), at the user's choice; its bundled stb_vorbis is MIT or public domain | Decoding .wav, .mp3 and (with stb_vorbis from its `extras`) .ogg sound files | Fetched, its decoders linked statically |
| [ENet](https://github.com/lsalzman/enet) | v1.3.18 | MIT | Reliable UDP networking for `ENetPeer` (not on the web) | Fetched, linked statically |
| [wslay](https://github.com/tatsuhiro-t/wslay) | release-1.1.1 | MIT | WebSocket framing for `WebSocketPeer`, as Godot's WSLPeer (not on the web) | Fetched, linked statically |
| [OpenXR SDK](https://github.com/KhronosGroup/OpenXR-SDK) | release-1.1.65 | Apache 2.0; its headers are Apache 2.0 or MIT and its bundled jsoncpp is MIT (or public domain) | The Khronos OpenXR loader, which finds the headset's runtime, for `xr::OpenXRInterface`, as Godot's `thirdparty/openxr` (Windows, Linux and macOS) | Fetched, the loader's sources linked statically |
| [dav1d](https://github.com/videolan/dav1d) | 1.5.3 | BSD-2-Clause | Decoding AV1 video for `VideoStreamPlayer`; the engine's own AV1 encoder (`video::encode_frame`) writes in the order its parser reads and uses the AV1 specification's default CDFs as its `src/cdf.c` holds them | Fetched, its C files linked statically (no assembly) |
| [AccessKit](https://github.com/AccessKit/accesskit-c) | 0.23.1 | MIT or Apache 2.0, at the user's choice; parts of its crates derive from Chromium's accessibility code under its BSD-3-Clause license | Handing the controls' accessibility tree to the system's screen reader (UI Automation, VoiceOver, AT-SPI), as Godot's AccessKit driver; its Rust crates are listed below | Fetched, built by cargo from its `Cargo.lock` into a static library (Windows, macOS and Linux, where cargo is installed) |
| [libebur128](https://github.com/jiixyj/libebur128) | v1.2.6 | MIT; its bundled `queue.h` is BSD-3-Clause | Measuring loudness (LUFS) and true peaks for `audio.analyze` | Fetched, linked statically |
| [astc-encoder](https://github.com/ARM-software/astc-encoder) | 5.7.0 | Apache 2.0 | Decoding ASTC 4x4 textures where a GPU can't sample them; tools and tests also encode with it | Fetched, its core linked statically (the decoder alone in games) |
| [bc7enc_rdo](https://github.com/richgel999/bc7enc_rdo) | b9438627eef73a1157e84201b6fa6eb2ffd6d9f0 | MIT or Unlicense (public domain), at the user's choice | Decoding BC7 textures (`bc7decomp.cpp`) where a GPU can't sample them; tools and tests also encode with `bc7enc.cpp` | Fetched, those two files linked statically (the decoder alone in games) |
| [cgltf](https://github.com/jkuhlmann/cgltf) | v1.15 | MIT | Reading glTF 2.0 models (`.gltf` and `.glb`) for `MeshInstance3D` | Fetched, header-only, compiled in `engine/scene3d` |
| [MikkTSpace](https://github.com/mmikk/MikkTSpace) | 3e895b49d05ea07e4c2133156cfa94369e19e409 | Zlib | Tangents for models that leave them out (glTF, OBJ and FBX), as Godot makes them | Fetched, compiled in `engine/scene3d` |
| [stb](https://github.com/nothings/stb) | a7d4c2175630e1ea5b6f8dd00ab96fed0b97b96d | MIT or Unlicense (public domain), at the user's choice | Decoding JPEG textures, which glTF models often carry, with `stb_image.h`; tests write JPEG files with `stb_image_write.h`; packing lightmap atlases with `stb_rect_pack.h`, as Godot packs them | Fetched, header-only, its JPEG decoder compiled in `engine/render` |
| [ufbx](https://github.com/ufbx/ufbx) | v0.23.1 | MIT or Unlicense (public domain), at the user's choice | Reading FBX models (`.fbx`, binary and ASCII) for `MeshInstance3D`, as Godot 4.3's importer reads them | Fetched, its one source file compiled in `engine/scene3d` |
| [xatlas](https://github.com/jpcy/xatlas) | f700c7790aaa030e794b52ba7791a05c085faf0c | MIT | Unwrapping meshes into lightmap UVs for `LightmapGI` bakes, as Godot unwraps them | Fetched, its one source file compiled in `engine/scene3d` through `src/xatlas_build.cpp`, which swaps in the engine's own trigonometry and a stable sort so charts are the same on every platform |
| [Recast Navigation](https://github.com/recastnavigation/recastnavigation) | v1.6.0 | Zlib | Baking 3D navigation meshes from scene geometry with Recast, as Godot does | Fetched, its Recast library compiled in `engine/navigation` |
| [Clipper2](https://github.com/AngusJohnson/Clipper2) | Clipper2_1.5.4 | BSL-1.0 | Merging, cutting and shrinking outlines when 2D navigation polygons bake, as Godot does | Fetched, linked statically |
| [meshoptimizer](https://github.com/zeux/meshoptimizer) | 73583c335e541c139821d0de2bf5f12960a04941 | MIT | Models' generated LODs: the simplifier of meshoptimizer 1.0, as Godot 4.6's importer uses it | Fetched, linked statically (`allocator.cpp` and `simplifier.cpp`) |
| [V-HACD](https://github.com/kmammou/v-hacd) | v4.1.0 | BSD-3-Clause | Decomposing meshes into convex collision hulls, as Godot's importer does with V-HACD, and the convex hull of a mesh | Fetched, header-only, its implementation compiled in `engine/scene3d` through `src/vhacd_build.cpp` on one thread, with a patch (`cmake/patches/`) that makes its hulls the same on every platform |
| [Manifold](https://github.com/elalish/manifold) | v3.3.2 | Apache 2.0 | CSG shapes' booleans (union, intersection, subtraction), as Godot 4.6's CSG module uses it | Fetched, linked statically |
| [PolyPartition](https://github.com/ivanfratric/polypartition) | 7bdffb428b2b19ad1c43aa44c714dcc104177e84 | MIT | Splitting baked 2D navigation outlines into convex polygons or triangles, as Godot does | Fetched, its one source file compiled in `engine/navigation` |
| [msdfgen](https://github.com/Chlumsky/msdfgen) | v1.13 | MIT | Multichannel signed distance fields for fonts | Fetched, its core linked statically |
| [Roboto](https://github.com/googlefonts/roboto) | v2.138 | Apache 2.0 | The default font | Fetched, Roboto Regular embedded in the engine |
| [cpp-httplib](https://github.com/yhirose/cpp-httplib) | v0.58.0 | MIT | Localhost HTTP server for the REST mirror and MCP over HTTP, and the client that calls language models | Fetched, header-only |
| [Mbed TLS](https://github.com/Mbed-TLS/mbedtls) | v3.6.7 | Apache 2.0, or GPL 2.0 or later at the user's choice; SlashEngine uses it under Apache 2.0 | TLS for `https://` requests to language models (not on the web) | Fetched, its library linked statically |
| [libwebp](https://github.com/webmproject/libwebp) | v1.5.0 | BSD-3-Clause, with an additional patent grant (`PATENTS`) | Decoding WebP textures when unpacking an exported Godot game (tools only, not shipped in games) | Fetched, its decoder linked into slash-cli and slash-mcp |
| [Zstandard](https://github.com/facebook/zstd) | v1.5.7 | BSD-3-Clause or GPL 2.0 at the user's choice; SlashEngine uses it under BSD-3-Clause | Decompressing the files of an exported Godot game (tools only, not shipped in games) | Fetched, its decompressor linked into slash-cli and slash-mcp |
| [Brotli](https://github.com/google/brotli) | v1.1.0 | MIT | Decompressing a Unity web build before it is unpacked (tools only, not shipped in games) | Fetched, its decoder linked into slash-cli and slash-mcp |
| [TinySoundFont](https://github.com/schellingb/TinySoundFont) | 853a0a171759f1ddba0de1442133a75912bbeffa | MIT | Playing scores through SoundFont 2 instruments for `soundtrack.render` (tools only, not shipped in games) | Fetched, linked into slash-cli and slash-mcp |
| [libogg](https://github.com/xiph/ogg) | v1.3.6 | BSD-3-Clause | The Ogg container for the stems `soundtrack.render` encodes (tools only, not shipped in games) | Fetched, linked into slash-cli and slash-mcp |
| [libvorbis](https://github.com/xiph/vorbis) | v1.3.7 | BSD-3-Clause | Encoding the stems `soundtrack.render` writes as Ogg Vorbis (tools only, not shipped in games) | Fetched, its encoder linked into slash-cli and slash-mcp |
| [Expat](https://github.com/libexpat/libexpat) | R_2_8_5 | MIT | Reading Wayland protocol files for the built-in wayland-scanner (tools only, not shipped in games) | Fetched, linked into slash-cli and slash-mcp on macOS and Linux |
| [Wayland](https://gitlab.freedesktop.org/wayland/wayland) | 1.18.0 | MIT | Its wayland-scanner, which writes SDL's Wayland protocol code when Linux games are exported (tools only, not shipped in games) | Fetched, the scanner linked into slash-cli and slash-mcp on macOS and Linux |
| [glslang](https://github.com/KhronosGroup/glslang) | 16.6.0 | BSD-3-Clause, with parts under BSD-2-Clause, MIT, Apache 2.0, NVIDIA's permissive notice (AML-glslang) and, in its generated parser, GPL 3.0 with the Bison exception, which lets it ship under any terms | Compiling a game's HLSL shaders to SPIR-V (tools only, not shipped in games) | Fetched, linked into slash-cli and slash-mcp |
| [SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross) | vulkan-sdk-1.4.363.0 | Apache 2.0 or MIT, at the user's choice; its `spirv.hpp` is MIT | Turning a game's compiled shaders into MSL and GLSL ES (tools only, not shipped in games) | Fetched, linked into slash-cli and slash-mcp |
| [Tint, in Dawn](https://github.com/google/dawn) | v20260423.175430 | BSD-3-Clause | Turning a game's compiled shaders into WGSL for WebGPU (tools only, not shipped in games); Dawn's CMake builds only Tint, with one patch for Xcode 15's clang (`cmake/patches/`) | Fetched, linked into slash-cli and slash-mcp |
| [SPIRV-Tools](https://github.com/KhronosGroup/SPIRV-Tools) | vulkan-sdk-1.4.350.0 | Apache 2.0 | Reading SPIR-V for Tint (tools only) | Fetched, linked into slash-cli and slash-mcp |
| [SPIRV-Headers](https://github.com/KhronosGroup/SPIRV-Headers) | vulkan-sdk-1.4.350.0 | Khronos's MIT text (MIT-Khronos-old); its Markdown docs are CC BY 4.0 and not used | SPIR-V's definitions, for SPIRV-Tools (tools only) | Fetched, headers only |
| [Abseil](https://github.com/abseil/abseil-cpp) | 20250512.1 | Apache 2.0 | Number parsing in Tint (tools only) | Fetched, linked into slash-cli and slash-mcp |
| [Tracy](https://github.com/wolfpld/tracy) | v0.14.1 | BSD-3-Clause; its bundled libbacktrace is BSD-3-Clause and its other bundled code permissive | The client of the Tracy profiler, in builds configured with `SLASH_PROFILER=tracy` (not shipped in games) | Fetched only for those builds, linked statically |
| [doctest](https://github.com/doctest/doctest) | v2.5.2 | MIT | Unit tests (not shipped in games) | Fetched, tests only |
| [CPM.cmake](https://github.com/cpm-cmake/CPM.cmake) | v0.40.2 | MIT | Dependency fetching | Vendored as `cmake/CPM.cmake` |

AccessKit's C library is built from Rust crates that cargo fetches from crates.io at the versions
its `Cargo.lock` pins. Those that go into games are below, by the platforms that link them; the
`crate_licenses` test (`cmake/check_licenses.cmake`) fails a build whose crates, macros and build
scripts included, have a license not allowed here, or that ships a crate missing from this table.

| Crate | Version | License | Platforms |
| --- | --- | --- | --- |
| [accesskit](https://crates.io/crates/accesskit) | 0.25.1 | MIT OR Apache-2.0 | Linux, Windows, macOS |
| [accesskit_atspi_common](https://crates.io/crates/accesskit_atspi_common) | 0.21.0 | MIT OR Apache-2.0 | Linux |
| [accesskit_consumer](https://crates.io/crates/accesskit_consumer) | 0.39.1 | MIT OR Apache-2.0 | Linux, Windows, macOS |
| [accesskit_macos](https://crates.io/crates/accesskit_macos) | 0.27.1 | MIT OR Apache-2.0 | macOS |
| [accesskit_unix](https://crates.io/crates/accesskit_unix) | 0.24.0 | MIT OR Apache-2.0 | Linux |
| [accesskit_windows](https://crates.io/crates/accesskit_windows) | 0.35.1 | MIT OR Apache-2.0 | Windows |
| [async-broadcast](https://crates.io/crates/async-broadcast) | 0.7.1 | MIT OR Apache-2.0 | Linux |
| [async-channel](https://crates.io/crates/async-channel) | 2.3.1 | Apache-2.0 OR MIT | Linux |
| [async-executor](https://crates.io/crates/async-executor) | 1.13.1 | Apache-2.0 OR MIT | Linux |
| [async-io](https://crates.io/crates/async-io) | 2.6.0 | Apache-2.0 OR MIT | Linux |
| [async-lock](https://crates.io/crates/async-lock) | 3.4.0 | Apache-2.0 OR MIT | Linux |
| [async-process](https://crates.io/crates/async-process) | 2.5.0 | Apache-2.0 OR MIT | Linux |
| [async-signal](https://crates.io/crates/async-signal) | 0.2.14 | Apache-2.0 OR MIT | Linux |
| [async-task](https://crates.io/crates/async-task) | 4.7.1 | Apache-2.0 OR MIT | Linux |
| [atomic-waker](https://crates.io/crates/atomic-waker) | 1.1.2 | Apache-2.0 OR MIT | Linux |
| [atspi](https://crates.io/crates/atspi) | 0.29.0 | Apache-2.0 OR MIT | Linux |
| [atspi-common](https://crates.io/crates/atspi-common) | 0.13.0 | Apache-2.0 OR MIT | Linux |
| [atspi-proxies](https://crates.io/crates/atspi-proxies) | 0.13.0 | Apache-2.0 OR MIT | Linux |
| [bitflags](https://crates.io/crates/bitflags) | 2.6.0 | MIT OR Apache-2.0 | Linux, macOS |
| [blocking](https://crates.io/crates/blocking) | 1.6.1 | Apache-2.0 OR MIT | Linux |
| [cfg-if](https://crates.io/crates/cfg-if) | 1.0.0 | MIT/Apache-2.0 | Linux |
| [concurrent-queue](https://crates.io/crates/concurrent-queue) | 2.5.0 | Apache-2.0 OR MIT | Linux |
| [crossbeam-utils](https://crates.io/crates/crossbeam-utils) | 0.8.15 | MIT OR Apache-2.0 | Linux |
| [endi](https://crates.io/crates/endi) | 1.1.0 | MIT | Linux |
| [enumflags2](https://crates.io/crates/enumflags2) | 0.7.10 | MIT OR Apache-2.0 | Linux |
| [event-listener](https://crates.io/crates/event-listener) | 5.3.1 | Apache-2.0 OR MIT | Linux |
| [event-listener-strategy](https://crates.io/crates/event-listener-strategy) | 0.5.2 | Apache-2.0 OR MIT | Linux |
| [fastrand](https://crates.io/crates/fastrand) | 2.1.1 | Apache-2.0 OR MIT | Linux |
| [foldhash](https://crates.io/crates/foldhash) | 0.2.0 | Zlib | Linux, Windows, macOS |
| [futures-core](https://crates.io/crates/futures-core) | 0.3.31 | MIT OR Apache-2.0 | Linux |
| [futures-io](https://crates.io/crates/futures-io) | 0.3.31 | MIT OR Apache-2.0 | Linux |
| [futures-lite](https://crates.io/crates/futures-lite) | 2.6.0 | Apache-2.0 OR MIT | Linux |
| [futures-task](https://crates.io/crates/futures-task) | 0.3.31 | MIT OR Apache-2.0 | Linux |
| [futures-util](https://crates.io/crates/futures-util) | 0.3.31 | MIT OR Apache-2.0 | Linux |
| [hashbrown](https://crates.io/crates/hashbrown) | 0.17.1 | MIT OR Apache-2.0 | Linux, Windows, macOS |
| [hex](https://crates.io/crates/hex) | 0.4.3 | MIT OR Apache-2.0 | Linux |
| [libc](https://crates.io/crates/libc) | 0.2.189 | MIT OR Apache-2.0 | Linux |
| [linux-raw-sys](https://crates.io/crates/linux-raw-sys) | 0.12.1 | Apache-2.0 WITH LLVM-exception OR Apache-2.0 OR MIT | Linux |
| [objc-sys](https://crates.io/crates/objc-sys) | 0.3.5 | MIT | macOS |
| [objc2](https://crates.io/crates/objc2) | 0.5.2 | MIT | macOS |
| [objc2-app-kit](https://crates.io/crates/objc2-app-kit) | 0.2.2 | MIT | macOS |
| [objc2-encode](https://crates.io/crates/objc2-encode) | 4.1.0 | MIT | macOS |
| [objc2-foundation](https://crates.io/crates/objc2-foundation) | 0.2.2 | MIT | macOS |
| [once_cell](https://crates.io/crates/once_cell) | 1.19.0 | MIT OR Apache-2.0 | Linux |
| [ordered-stream](https://crates.io/crates/ordered-stream) | 0.2.0 | MIT OR Apache-2.0 | Linux |
| [parking](https://crates.io/crates/parking) | 2.2.1 | Apache-2.0 OR MIT | Linux |
| [phf](https://crates.io/crates/phf) | 0.14.0 | MIT | Linux |
| [phf_shared](https://crates.io/crates/phf_shared) | 0.14.0 | MIT | Linux |
| [pin-project-lite](https://crates.io/crates/pin-project-lite) | 0.2.13 | Apache-2.0 OR MIT | Linux |
| [pin-utils](https://crates.io/crates/pin-utils) | 0.1.0 | MIT OR Apache-2.0 | Linux |
| [piper](https://crates.io/crates/piper) | 0.2.4 | MIT OR Apache-2.0 | Linux |
| [polling](https://crates.io/crates/polling) | 3.11.0 | Apache-2.0 OR MIT | Linux |
| [proc-macro2](https://crates.io/crates/proc-macro2) | 1.0.106 | MIT OR Apache-2.0 | Linux |
| [quote](https://crates.io/crates/quote) | 1.0.37 | MIT OR Apache-2.0 | Linux |
| [rustix](https://crates.io/crates/rustix) | 1.1.5 | Apache-2.0 WITH LLVM-exception OR Apache-2.0 OR MIT | Linux |
| [serde](https://crates.io/crates/serde) | 1.0.229 | MIT OR Apache-2.0 | Linux |
| [serde_core](https://crates.io/crates/serde_core) | 1.0.229 | MIT OR Apache-2.0 | Linux |
| [signal-hook-registry](https://crates.io/crates/signal-hook-registry) | 1.4.1 | Apache-2.0/MIT | Linux |
| [siphasher](https://crates.io/crates/siphasher) | 1.0.2 | MIT/Apache-2.0 | Linux |
| [slab](https://crates.io/crates/slab) | 0.4.8 | MIT | Linux |
| [static_assertions](https://crates.io/crates/static_assertions) | 1.1.0 | MIT OR Apache-2.0 | Linux, Windows |
| [syn](https://crates.io/crates/syn) | 3.0.6 | MIT OR Apache-2.0 | Linux |
| [tracing](https://crates.io/crates/tracing) | 0.1.40 | MIT | Linux |
| [tracing-core](https://crates.io/crates/tracing-core) | 0.1.32 | MIT | Linux |
| [unicode-ident](https://crates.io/crates/unicode-ident) | 1.0.8 | (MIT OR Apache-2.0) AND Unicode-DFS-2016 | Linux |
| [uuid](https://crates.io/crates/uuid) | 1.19.0 | Apache-2.0 OR MIT | Linux, Windows, macOS |
| [windows](https://crates.io/crates/windows) | 0.62.2 | MIT OR Apache-2.0 | Windows |
| [windows-collections](https://crates.io/crates/windows-collections) | 0.3.2 | MIT OR Apache-2.0 | Windows |
| [windows-core](https://crates.io/crates/windows-core) | 0.62.2 | MIT OR Apache-2.0 | Windows |
| [windows-future](https://crates.io/crates/windows-future) | 0.3.2 | MIT OR Apache-2.0 | Windows |
| [windows-link](https://crates.io/crates/windows-link) | 0.2.1 | MIT OR Apache-2.0 | Windows |
| [windows-numerics](https://crates.io/crates/windows-numerics) | 0.3.1 | MIT OR Apache-2.0 | Windows |
| [windows-result](https://crates.io/crates/windows-result) | 0.4.1 | MIT OR Apache-2.0 | Windows |
| [windows-strings](https://crates.io/crates/windows-strings) | 0.5.1 | MIT OR Apache-2.0 | Windows |
| [windows-threading](https://crates.io/crates/windows-threading) | 0.2.1 | MIT OR Apache-2.0 | Windows |
| [winnow](https://crates.io/crates/winnow) | 1.0.4 | MIT | Linux |
| [zbus](https://crates.io/crates/zbus) | 5.19.0 | MIT | Linux |
| [zbus-lockstep](https://crates.io/crates/zbus-lockstep) | 0.5.2 | MIT | Linux |
| [zbus_names](https://crates.io/crates/zbus_names) | 4.3.4 | MIT | Linux |
| [zbus_xml](https://crates.io/crates/zbus_xml) | 5.2.0 | MIT | Linux |
| [zcheapstr](https://crates.io/crates/zcheapstr) | 1.1.0 | MIT | Linux |
| [zvariant](https://crates.io/crates/zvariant) | 5.15.0 | MIT | Linux |
| [zvariant_utils](https://crates.io/crates/zvariant_utils) | 4.2.0 | MIT | Linux |

The FreeType License asks that products using FreeType credit it in their documentation, as Godot
does. Games built with SlashEngine should include this line in their credits or license notice:
"Portions of this software are copyright © 2026 The FreeType Project (https://freetype.org). All
rights reserved."

Some code is adapted into the engine's own sources rather than fetched. Its notice stays in the
file, as its license asks:

| Name | License | Used for | Where |
| --- | --- | --- | --- |
| smbPitchShift and smbFft, by Stephan M. Bernsee (1999-2015), as Godot uses them | Wide Open License: use, copy and change it if the notice stays in the source | The `pitch_shift` and `spectrum_analyzer` audio effects | `engine/audio/src/effects.cpp` |
| [RVO2](https://github.com/snape/RVO2) and [RVO2-3D](https://github.com/snape/RVO2-3D), by the University of North Carolina at Chapel Hill (2008), as Godot 4.6 changes them | Apache 2.0 | Agents' and obstacles' avoidance | `engine/navigation/src/rvo2.hpp`, `rvo2_2d.cpp` and `rvo2_3d.cpp` |
| [YoctoImageDenoiser](https://github.com/ManuelPrandini/YoctoImageDenoiser)'s joint non-local means (JNLM) denoiser, by Manuel Prandini (2020), as Godot's lightmapper changes it | MIT | Denoising `LightmapGI` bakes | `engine/scene3d/src/lightmapper.cpp` |
| [HTerrain](https://github.com/Zylann/godot_heightmap_plugin), the heightmap terrain plugin for Godot, by Marc Gilleron (2016-2020) | MIT | `HTerrain`: its data, range map, quad tree, mesher, generator, brushes and Classic4 shading | `engine/scene3d/src/terrain.cpp` (the shading's few functions in `engine/render/shaders/mesh.hlsl` name their source) |
| [webgl-noise](https://github.com/stegu/webgl-noise)'s classic Perlin noise, by Stefan Gustavson (2011-2016), as HTerrain's generator uses it | MIT | `terrain.generate`'s noise | `engine/scene3d/src/terrain.cpp` |
| [FidelityFX Super Resolution 1](https://github.com/GPUOpen-Effects/FidelityFX-FSR) (`ffx_fsr1.h`), by Advanced Micro Devices (2021), as Godot uses it | MIT | The `fsr` render scale mode: EASU and RCAS | `engine/render/shaders/fsr_easu.hlsl` and `fsr_rcas.hlsl`, compiled into `engine/render/src/fsr_shaders.hpp` |
| [FidelityFX Super Resolution 2.2](https://github.com/GPUOpen-Effects/FidelityFX-FSR2) (`ffx_fsr2_*.h`), by Advanced Micro Devices (2022-2023), as Godot 4.6 runs it | MIT | The `fsr2` render scale mode: its seven passes | `engine/render/shaders/fsr2.hlsl` and `fsr2_*.hlsl`, compiled into `engine/render/src/fsr2_*_shaders.hpp`, and their CPU twin in `engine/render/src/fsr2.cpp` |
| [FidelityFX SDK 1.1.4](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK) (`ffx_fsr3upscaler_*.h`), by Advanced Micro Devices (2024) | MIT | The `fsr3` render scale mode: FSR 3.1's upscaler passes | `engine/render/shaders/fsr3.hlsl` and `fsr3_*.hlsl`, compiled into `engine/render/src/fsr3_*_shaders.hpp`, and their CPU twin in `engine/render/src/fsr3.cpp` |

The renderer's own shaders are compiled ahead of time with three command-line tools whose output
is committed in `engine/render/src/*_shaders.hpp`, so building the engine needs none of them;
`fsr_shaders.hpp` is written by `slash-shader-header`, built on the same projects' libraries. (A
game's own shaders are compiled by slash-cli, slash-mcp and exports with those libraries, listed
above; games get the compiled code, never the compilers.)

| Name | License | Used for |
| --- | --- | --- |
| [glslang](https://github.com/KhronosGroup/glslang) | BSD-3-Clause, with parts under BSD-2-Clause, MIT and Apache 2.0 | HLSL to SPIR-V for Vulkan |
| [SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross) | Apache 2.0 | SPIR-V to MSL for Metal and GLSL ES for OpenGL ES and WebGL 2 |
| [Tint, in Dawn](https://github.com/google/dawn) | BSD-3-Clause | SPIR-V to WGSL for WebGPU |

On Windows, Direct3D 12 compiles the HLSL at startup with `d3dcompiler_47.dll`, which comes with
Windows; the engine loads it at run time and ships no copy.

Exports are built with toolchains, which ship in no game except for the runtime libraries listed
here. Windows and Linux exports from macOS and Linux use toolchains that `slash-cli toolchain
install` downloads, pinned in `tools/exporter/src/toolchains.cpp`:

| Name | Version | License | Used for | In the game |
| --- | --- | --- | --- | --- |
| [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) | 20260922 | Apache 2.0 with LLVM exception (LLVM, libc++, libunwind, compiler-rt); ZPL 2.1 and public domain (mingw-w64); MIT (winpthreads) | Clang and LLD for Windows and Linux exports | Windows: libc++, libunwind, compiler-rt, winpthreads and the mingw-w64 startup code, linked statically |
| [Chromium's Debian sysroots](https://chromium.googlesource.com/chromium/src/+/main/build/linux/sysroot_scripts/) | bullseye (Debian 11) | Debian's packages: glibc LGPL 2.1, libstdc++ GPL 3 with the GCC Runtime Library Exception, X11 MIT | Headers and libraries Linux exports build against, so they run on glibc 2.31 | glibc and libstdc++ are linked dynamically to the player's own copies, as every Linux program does |

Windows exports on Windows use MSVC, and macOS and iOS exports use Apple's command-line tools.
Web exports use [Emscripten](https://github.com/emscripten-core/emscripten) 6.0.10 in CI (MIT or
University of Illinois/NCSA), whose JavaScript runtime and C/C++ libraries are compiled into the
game, with its `emdawnwebgpu` port (Dawn's WebGPU bindings for the web: BSD 3-Clause, and
Emscripten's license) for the WebGPU device. The web drawing tests run on Dawn in Node through
the [webgpu](https://github.com/dawn-gpu/node-webgpu) npm package 0.6.2 (MIT, with Dawn's BSD
3-Clause code inside), which nothing ships.

Importing a Unity build runs one program that slash-cli and slash-mcp download on first use (or
`slash-cli toolchain install assetripper`), pinned in `tools/exporter/src/toolchains.cpp`. It runs
as a separate program, talked to over HTTP on this machine; none of its code is linked into
SlashEngine or shipped in games:

| Name | Version | License | Used for |
| --- | --- | --- | --- |
| [AssetRipper](https://github.com/AssetRipper/AssetRipper) | 2.0.0 | GPL 3.0 | Turning a Unity build back into a Unity project, which the Unity importer then converts |
