cmake_minimum_required(VERSION 3.25)

project(SlashEngine
    VERSION 0.1.0
    DESCRIPTION "A lightweight, AI-first, model-agnostic game engine"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(SLASH_BUILD_TOOLS "Build slash-cli" ${PROJECT_IS_TOP_LEVEL})
option(SLASH_BUILD_TESTS "Build the unit and smoke tests" ${PROJECT_IS_TOP_LEVEL})
option(SLASH_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerOptions)
include(Dependencies)

add_subdirectory(engine)

if(SLASH_BUILD_TOOLS)
    add_subdirectory(tools/slash-cli)
endif()

if(SLASH_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
