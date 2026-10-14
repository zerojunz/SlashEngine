# Third-party software

SlashEngine is licensed under Apache 2.0 (see `LICENSE`). It uses the following third-party
software, each under a permissive license. Dependencies are fetched at build time at the pinned
versions in `cmake/Dependencies.cmake` unless noted as vendored.

| Name | Version | License | Used for | How it is included |
| --- | --- | --- | --- | --- |
| [SDL3](https://github.com/libsdl-org/SDL) | release-3.4.18 | zlib | Platform layer: windows, events, timers | Fetched, linked statically |
| [doctest](https://github.com/doctest/doctest) | v2.5.2 | MIT | Unit tests (not shipped in games) | Fetched, tests only |
| [CPM.cmake](https://github.com/cpm-cmake/CPM.cmake) | v0.40.2 | MIT | Dependency fetching | Vendored as `cmake/CPM.cmake` |
