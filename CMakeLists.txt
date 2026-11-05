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
