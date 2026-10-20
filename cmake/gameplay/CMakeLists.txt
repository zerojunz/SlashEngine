# Builds a game's own C++ code (project.json's "gameplay") into the library slash-cli, slash-mcp
# and slash-game load and reload (slash/gameplay/host.hpp). gameplay.build and slash-cli build
# configure it with "-C <engine build>/gameplay-sdk.cmake", which sets the compiler, flags,
# include folders and definitions the engine was built with, and:
#
#   SLASH_PROJECT_DIR       the game's folder, absolute
#   SLASH_GAMEPLAY_SOURCES  its gameplay sources, absolute: folders (every .cpp file in them and
#                           their subfolders) and files (.cpp files are compiled; the folders of
#                           headers are searched for includes)
#
# build.check builds the game_objects target alone: each changed file compiled, nothing linked.
#
# The library is linked with the engine's functions left unresolved: the program loading it has
# them. It lands in <project>/.slash/gameplay/game.so, replaced in one step so a program polling
# the file never reads half of it.
cmake_minimum_required(VERSION 3.25)

project(SlashGameplay LANGUAGES CXX)

foreach(variable SLASH_PROJECT_DIR SLASH_GAMEPLAY_SOURCES SLASH_SDK_INCLUDE_DIRECTORIES)
    if(NOT ${variable})
        message(FATAL_ERROR "${variable} is not set. Use gameplay.build or \"slash-cli build\".")
    endif()
endforeach()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

set(sources "")
set(folders "")
foreach(source IN LISTS SLASH_GAMEPLAY_SOURCES)
    if(IS_DIRECTORY "${source}")
        file(GLOB_RECURSE found CONFIGURE_DEPENDS "${source}/*.cpp")
        list(APPEND sources ${found})
        list(APPEND folders "${source}")
    elseif(source MATCHES "\\.cpp$" AND EXISTS "${source}")
        list(APPEND sources "${source}")
    elseif(EXISTS "${source}")
        get_filename_component(folder "${source}" DIRECTORY)
        list(APPEND folders "${folder}")
    else()
        message(FATAL_ERROR "the gameplay source ${source} does not exist")
    endif()
endforeach()
if(NOT sources)
    message(FATAL_ERROR "the gameplay sources have no .cpp files")
endif()

# The objects first, so build.check can compile the files that changed without linking; the
# compile commands tell it which source each object is.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
add_library(game_objects OBJECT ${sources})
set_target_properties(game_objects PROPERTIES POSITION_INDEPENDENT_CODE ON)
# Includes are found next to the file, from each source folder, and in the engine.
target_include_directories(game_objects PRIVATE ${folders} ${SLASH_SDK_INCLUDE_DIRECTORIES})
target_compile_definitions(game_objects PRIVATE ${SLASH_SDK_COMPILE_DEFINITIONS})
# As the engine is built: no exceptions or RTTI. GCC's unique symbols would tie each rebuilt
# library to the statics of the first one loaded. Suggested fixes are printed as data
# ("fix-it:"file":{line:column-line:column}:"text""), which build.check returns.
target_compile_options(game_objects PRIVATE -fno-exceptions -fno-rtti -Wall -Wextra
    -fdiagnostics-parseable-fixits $<$<CXX_COMPILER_ID:GNU>:-fno-gnu-unique>)

add_library(game MODULE $<TARGET_OBJECTS:game_objects>)
set_target_properties(game PROPERTIES PREFIX "" OUTPUT_NAME game SUFFIX ".so")
if(APPLE)
    target_link_options(game PRIVATE -undefined dynamic_lookup)
endif()

set(library "${SLASH_PROJECT_DIR}/.slash/gameplay/game.so")
add_custom_command(TARGET game POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:game> "${library}.new"
    COMMAND ${CMAKE_COMMAND} -E rename "${library}.new" "${library}"
    VERBATIM)
