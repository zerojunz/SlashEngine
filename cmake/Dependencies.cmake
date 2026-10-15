# Third-party dependencies. Each is pinned in dependencies.json, with its repository, exact tag,
# license and the hash of its license file, and listed in THIRD_PARTY.md; only permissive
# licenses are allowed (see docs/design.md). The "licenses" test checks all of that, so add a
# dependency with slash_add_dependency, never with CPMAddPackage or FetchContent directly.

include(CPM)

file(READ ${CMAKE_CURRENT_LIST_DIR}/dependencies.json SLASH_DEPENDENCY_MANIFEST)
set(SLASH_DEPENDENCIES "")

# Fetches a dependency at the repository and tag dependencies.json pins; other arguments go to
# CPMAddPackage. A macro, so CPM's <name>_SOURCE_DIR and <name>_ADDED reach the caller.
macro(slash_add_dependency name)
    string(JSON _slash_repository GET "${SLASH_DEPENDENCY_MANIFEST}" dependencies ${name}
        repository)
    string(JSON _slash_tag GET "${SLASH_DEPENDENCY_MANIFEST}" dependencies ${name} tag)
    CPMAddPackage(
        NAME ${name}
        GIT_REPOSITORY ${_slash_repository}
        GIT_TAG ${_slash_tag}
        GIT_SHALLOW TRUE
        ${ARGN})
    list(APPEND SLASH_DEPENDENCIES ${name})
endmacro()

slash_add_dependency(SDL3
    SYSTEM TRUE
    OPTIONS
        "SDL_SHARED OFF"
        "SDL_STATIC ON"
        "SDL_TEST_LIBRARY OFF"
        "SDL_TESTS OFF"
        "SDL_EXAMPLES OFF"
        "SDL_INSTALL OFF")

if(SLASH_BUILD_TESTS)
    slash_add_dependency(doctest
        SYSTEM TRUE
        OPTIONS
            "DOCTEST_WITH_TESTS OFF"
            "DOCTEST_NO_INSTALL ON")
endif()

slash_add_dependency(EnTT SYSTEM TRUE)

# JSON reader used by slash::json. The engine writes JSON itself so it controls the canonical
# layout; yyjson only parses and formats numbers.
slash_add_dependency(yyjson
    SYSTEM TRUE
    OPTIONS
        "YYJSON_INSTALL OFF"
        "YYJSON_DISABLE_INCR_READER ON"
        "YYJSON_DISABLE_FILE ON"
        "YYJSON_DISABLE_UTILS ON"
        "YYJSON_DISABLE_NON_STANDARD ON"
        "YYJSON_READER_DEPTH_LIMIT 256")

# 2D physics. Box2D builds single-threaded and deterministic: its own CMake turns off
# floating-point contraction, and SIMD stays off on the web, since browsers before 2021 (Chrome
# 91, Firefox 89, Safari 16.4) have no WebAssembly SIMD.
if(EMSCRIPTEN)
    set(_slash_box2d_simd "BOX2D_DISABLE_SIMD ON")
else()
    set(_slash_box2d_simd "BOX2D_DISABLE_SIMD OFF")
endif()
slash_add_dependency(box2d
    SYSTEM TRUE
    OPTIONS
        "BOX2D_SAMPLES OFF"
        "BOX2D_BENCHMARKS OFF"
        "BOX2D_DOCS OFF"
        "BOX2D_PROFILE OFF"
        "BOX2D_VALIDATE OFF"
        "BOX2D_UNIT_TESTS OFF"
        "BOX2D_AVX2 OFF"
        ${_slash_box2d_simd})

# HTTP server and client for the localhost REST mirror and MCP over HTTP. Header-only, used
# without TLS, compression or exceptions, and only inside slash_server.
slash_add_dependency(httplib DOWNLOAD_ONLY TRUE)
if(httplib_ADDED)
    find_package(Threads REQUIRED)
    add_library(slash_httplib INTERFACE)
    target_include_directories(slash_httplib SYSTEM INTERFACE ${httplib_SOURCE_DIR})
    target_compile_definitions(slash_httplib INTERFACE CPPHTTPLIB_NO_EXCEPTIONS)
    target_link_libraries(slash_httplib INTERFACE
        Threads::Threads $<$<PLATFORM_ID:Windows>:ws2_32>)
endif()

# Fonts and text, as Godot's: FreeType rasterizes glyphs with its auto-hinter, HarfBuzz shapes
# text (with its own OpenType font functions, so it needs no FreeType inside), and msdfgen makes
# multichannel signed distance fields for fonts that scale. FreeType reads TrueType and OpenType
# outlines only: no compressed or color fonts, so no zlib, bzip2, PNG or Brotli.
slash_add_dependency(freetype
    SYSTEM TRUE
    OPTIONS
        "FT_DISABLE_ZLIB ON"
        "FT_DISABLE_BZIP2 ON"
        "FT_DISABLE_PNG ON"
        "FT_DISABLE_HARFBUZZ ON"
        "FT_DISABLE_BROTLI ON"
        "SKIP_INSTALL_ALL ON")

# HarfBuzz's single-file build, src/harfbuzz.cc, with no platform font backends.
slash_add_dependency(harfbuzz DOWNLOAD_ONLY TRUE)
if(harfbuzz_ADDED)
    add_library(slash_harfbuzz STATIC ${harfbuzz_SOURCE_DIR}/src/harfbuzz.cc)
    target_include_directories(slash_harfbuzz SYSTEM PUBLIC ${harfbuzz_SOURCE_DIR}/src)
    if(MSVC)
        target_compile_options(slash_harfbuzz PRIVATE /bigobj /utf-8)
    endif()
endif()

# msdfgen's core only: the shape and distance field code, with no PNG, SVG or font import.
slash_add_dependency(msdfgen DOWNLOAD_ONLY TRUE)
if(msdfgen_ADDED)
    file(GLOB _slash_msdfgen_sources ${msdfgen_SOURCE_DIR}/core/*.cpp)
    add_library(slash_msdfgen STATIC ${_slash_msdfgen_sources})
    target_include_directories(slash_msdfgen SYSTEM PUBLIC ${msdfgen_SOURCE_DIR})
    target_compile_definitions(slash_msdfgen PUBLIC MSDFGEN_PUBLIC= MSDFGEN_USE_CPP11)
endif()

# The default font, as Godot has one: Roboto Regular, built into the engine (cmake/embed.cmake),
# so text draws before a game adds fonts of its own. Version 2 is Apache 2.0; later ones are not.
slash_add_dependency(roboto DOWNLOAD_ONLY TRUE)
