cmake_minimum_required(VERSION 3.25)

project(SlashEngine
    VERSION 0.1.0
    DESCRIPTION "A lightweight, AI-first, model-agnostic game engine"
    LANGUAGES C CXX)

# Objective-C++ for the Apple side of engine/services' backend bridges.
if(APPLE)
    enable_language(OBJC)
    enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# A Debug link of a test or example executable takes nearly 4 GB with GNU ld, so Ninja links
# only as many at once as there are 6 GB of memory (at least one); otherwise a build linking
# many examples at once runs out of memory, which stops CI's runners. Set
# CMAKE_JOB_POOL_LINK yourself to use a pool of your own.
if(CMAKE_GENERATOR MATCHES "Ninja" AND NOT DEFINED CMAKE_JOB_POOL_LINK)
    cmake_host_system_information(RESULT _slash_memory_mib QUERY TOTAL_PHYSICAL_MEMORY)
    math(EXPR _slash_links "${_slash_memory_mib} / 6144")
    if(_slash_links LESS 1)
        set(_slash_links 1)
    endif()
    set_property(GLOBAL APPEND PROPERTY JOB_POOLS slash_link=${_slash_links})
    set(CMAKE_JOB_POOL_LINK slash_link)
endif()

# Debug info in Debug and RelWithDebInfo builds. Without it a compile takes about half as long,
# objects are a twentieth of the size and links need far less memory, so a compiler cache holds a
# whole build: CI's builds (the ci and double presets) and cloud sessions leave it out. Everything
# else builds the same. Set before the dependencies, which take these flags too.
option(SLASH_DEBUG_INFO "Compile Debug and RelWithDebInfo builds with debug info" ON)
if(NOT SLASH_DEBUG_INFO)
    foreach(config DEBUG RELWITHDEBINFO)
        foreach(lang C CXX OBJC OBJCXX)
            string(REGEX REPLACE "(^| )[-/](g|Zi|Z7|ZI)[^ ]*" "" CMAKE_${lang}_FLAGS_${config}
                "${CMAKE_${lang}_FLAGS_${config}}")
        endforeach()
        foreach(kind EXE SHARED MODULE)
            string(REGEX REPLACE "(^| )[-/]debug[^ ]*" "" CMAKE_${kind}_LINKER_FLAGS_${config}
                "${CMAKE_${kind}_LINKER_FLAGS_${config}}")
        endforeach()
    endforeach()
    # MSVC's /Z7 or /Zi (CMP0141): an empty format adds neither.
    set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "")
endif()

option(SLASH_BUILD_TOOLS "Build slash-cli and slash-mcp" ${PROJECT_IS_TOP_LEVEL})
option(SLASH_BUILD_TESTS "Build the unit and smoke tests" ${PROJECT_IS_TOP_LEVEL})
option(SLASH_BUILD_EXAMPLES "Build the examples and test them (needs tools and tests)"
    ${PROJECT_IS_TOP_LEVEL})
option(SLASH_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
set(SLASH_PROFILER "" CACHE STRING "The profiler the engine reports to: none (\"\") or tracy")
set_property(CACHE SLASH_PROFILER PROPERTY STRINGS "" tracy)
if(NOT SLASH_PROFILER STREQUAL "" AND NOT SLASH_PROFILER STREQUAL "tracy")
    message(FATAL_ERROR "SLASH_PROFILER is \"${SLASH_PROFILER}\"; use \"\" or tracy")
endif()

# How precise positions are, as Godot's precision build option: "single" keeps Vec2, Vec3 and
# Quat in floats, "double" makes them doubles (slash::real_t) for worlds that reach far from the
# origin (engine/core defines SLASH_REAL_T_IS_DOUBLE for it). A game's code built against one
# can't load into the other.
set(SLASH_PRECISION "single" CACHE STRING "Floating-point precision of positions: single or double")
set_property(CACHE SLASH_PRECISION PROPERTY STRINGS single double)
if(NOT SLASH_PRECISION STREQUAL "single" AND NOT SLASH_PRECISION STREQUAL "double")
    message(FATAL_ERROR "SLASH_PRECISION is \"${SLASH_PRECISION}\"; use single or double")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerOptions)
include(Dependencies)

add_subdirectory(engine)

# Web builds of the tests (tests/web) have only the drawing tests, which need no tools.
if((SLASH_BUILD_TOOLS OR SLASH_BUILD_TESTS) AND NOT EMSCRIPTEN)
    add_subdirectory(tools/shader_compiler)
    add_subdirectory(tools/exporter)
    add_subdirectory(tools/importer)
    add_subdirectory(tools/bench)
endif()

if(SLASH_BUILD_TOOLS)
    add_subdirectory(tools/slash-cli)
    add_subdirectory(tools/slash-mcp)
    add_subdirectory(tools/slash-game)
    include(GNUInstallDirs)
    install(TARGETS slash-cli slash-mcp slash-game RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    # The engine's sources go beside the tools, where exporter::find_engine looks for them
    # (<bin>/../share/slashengine): exports build games from them, fetching the dependencies the
    # first time, api.search reads their headers and examples, and the gameplay SDK
    # (tools/exporter/CMakeLists.txt) builds a game's code against their headers.
    set(SLASH_INSTALL_ENGINE_DIR ${CMAKE_INSTALL_DATADIR}/slashengine)
    install(FILES CMakeLists.txt LICENSE THIRD_PARTY.md README.md AGENTS.md
        DESTINATION ${SLASH_INSTALL_ENGINE_DIR})
    install(DIRECTORY cmake engine export examples docs DESTINATION ${SLASH_INSTALL_ENGINE_DIR}
        PATTERN ".DS_Store" EXCLUDE PATTERN ".slash" EXCLUDE PATTERN "build" EXCLUDE)
    install(CODE "set(SLASH_SDK_DATA [==[${CMAKE_BINARY_DIR}/gameplay-sdk-data.cmake]==])
        set(SLASH_SDK_DESTINATION [==[${SLASH_INSTALL_ENGINE_DIR}]==])")
    install(SCRIPT cmake/gameplay/install_sdk.cmake)
endif()

if(SLASH_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
    if(SLASH_BUILD_EXAMPLES AND SLASH_BUILD_TOOLS)
        add_subdirectory(examples)
    endif()
endif()
