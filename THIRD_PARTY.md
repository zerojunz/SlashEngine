# Third-party software

SlashEngine is licensed under Apache 2.0 (see `LICENSE`). It uses the following third-party
software, each under a permissive license. Dependencies are fetched at build time unless noted as
vendored. `cmake/dependencies.json` pins each one to an exact tag with its SPDX license and the
SHA-256 of its license file, and the `licenses` test fails a pull request that adds a dependency
or license not allowed here, or whose license text changed without review.

| Name | Version | License | Used for | How it is included |
| --- | --- | --- | --- | --- |
| [SDL3](https://github.com/libsdl-org/SDL) | release-3.4.18 | zlib | Platform layer: windows, events, timers | Fetched, linked statically |
| [EnTT](https://github.com/skypjack/entt) | v3.16.0 | MIT | Entity component system | Fetched, header-only |
| [yyjson](https://github.com/ibireme/yyjson) | 0.13.0 | MIT | JSON parsing and number formatting | Fetched, linked statically |
| [cpp-httplib](https://github.com/yhirose/cpp-httplib) | v0.58.0 | MIT | Localhost HTTP server for the REST mirror and MCP over HTTP | Fetched, header-only |
| [doctest](https://github.com/doctest/doctest) | v2.5.2 | MIT | Unit tests (not shipped in games) | Fetched, tests only |
| [CPM.cmake](https://github.com/cpm-cmake/CPM.cmake) | v0.40.2 | MIT | Dependency fetching | Vendored as `cmake/CPM.cmake` |

The renderer's shaders are compiled ahead of time with two tools whose output is committed in
`engine/render/src/*_shaders.hpp`. Nothing of theirs is linked into the engine or a game, and
building needs neither:

| Name | License | Used for |
| --- | --- | --- |
| [glslang](https://github.com/KhronosGroup/glslang) | BSD-3-Clause, with parts under BSD-2-Clause, MIT and Apache 2.0 | HLSL to SPIR-V for Vulkan |
| [SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross) | Apache 2.0 | SPIR-V to MSL for Metal |

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
game.
