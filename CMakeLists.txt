cmake_minimum_required(VERSION 3.25)

project(SlashEngine
    VERSION 0.1.0
    DESCRIPTION "A lightweight, AI-first, model-agnostic game engine"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(SLASH_BUILD_TOOLS "Build slash-cli and slash-mcp" ${PROJECT_IS_TOP_LEVEL})
option(SLASH_BUILD_TESTS "Build the unit and smoke tests" ${PROJECT_IS_TOP_LEVEL})
option(SLASH_BUILD_EXAMPLES "Build the examples and test them (needs tools and tests)"
    ${PROJECT_IS_TOP_LEVEL})
option(SLASH_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerOptions)
include(Dependencies)

add_subdirectory(engine)

if(SLASH_BUILD_TOOLS OR SLASH_BUILD_TESTS)
    add_subdirectory(tools/shader_compiler)
    add_subdirectory(tools/exporter)
    add_subdirectory(tools/importer)
endif()

if(SLASH_BUILD_TOOLS)
    add_subdirectory(tools/slash-cli)
    add_subdirectory(tools/slash-mcp)
    add_subdirectory(tools/slash-game)
    include(GNUInstallDirs)
    install(TARGETS slash-cli slash-mcp slash-game RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(SLASH_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
    if(SLASH_BUILD_EXAMPLES AND SLASH_BUILD_TOOLS)
        add_subdirectory(examples)
    endif()
endif()
