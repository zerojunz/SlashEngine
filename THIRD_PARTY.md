# Third-party software

SlashEngine is licensed under Apache 2.0 (see `LICENSE`). It uses the following third-party
software, each under a permissive license. Dependencies are fetched at build time at the pinned
versions in `cmake/Dependencies.cmake` unless noted as vendored.

| Name | Version | License | Used for | How it is included |
| --- | --- | --- | --- | --- |
| [SDL3](https://github.com/libsdl-org/SDL) | release-3.4.18 | zlib | Platform layer: windows, events, timers | Fetched, linked statically |
| [EnTT](https://github.com/skypjack/entt) | v3.16.0 | MIT | Entity component system | Fetched, header-only |
| [yyjson](https://github.com/ibireme/yyjson) | 0.13.0 | MIT | JSON parsing and number formatting | Fetched, linked statically |
| [cpp-httplib](https://github.com/yhirose/cpp-httplib) | v0.58.0 | MIT | Localhost HTTP server for the REST mirror and MCP over HTTP | Fetched, header-only |
| [doctest](https://github.com/doctest/doctest) | v2.5.2 | MIT | Unit tests (not shipped in games) | Fetched, tests only |
| [CPM.cmake](https://github.com/cpm-cmake/CPM.cmake) | v0.40.2 | MIT | Dependency fetching | Vendored as `cmake/CPM.cmake` |

Exports are built with toolchains the developer installs, not fetched by the engine. Web exports
use [Emscripten](https://github.com/emscripten-core/emscripten) 6.0.10 in CI (MIT or University of
Illinois/NCSA), whose JavaScript runtime and C/C++ libraries are compiled into the game.
