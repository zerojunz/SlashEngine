# Builds one game for one platform. slash-cli export configures this project with the game's
# details and a staged data folder, so it is not meant to be configured by hand:
#
#   SLASH_GAME_NAME     display name, e.g. "Space Pong"
#   SLASH_GAME_BINARY   file name, e.g. "SpacePong"
#   SLASH_GAME_ID       reverse-DNS identifier, e.g. "com.example.spacepong"
#   SLASH_GAME_VERSION  major.minor.patch
#   SLASH_GAME_DATA     absolute path of the folder to ship as the game's data
#
# `cmake --install <build> --component game --prefix <out>` lays the game out for its platform:
# <out>/<binary>.app on macOS, <out>/<binary>[.exe] with <out>/data on Windows and Linux, and
# <out>/index.html with the script, WebAssembly and data files on the web.
cmake_minimum_required(VERSION 3.25)

project(SlashGame LANGUAGES C CXX)

foreach(variable SLASH_GAME_NAME SLASH_GAME_BINARY SLASH_GAME_ID SLASH_GAME_VERSION
        SLASH_GAME_DATA)
    if(NOT ${variable})
        message(FATAL_ERROR "${variable} is not set. Use \"slash-cli export\" to build a game.")
    endif()
endforeach()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# Link the C and C++ runtimes statically where possible, so players need nothing installed.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
# Games support OS versions from 2020 on (docs/roadmap.md). Windows refuses to start an
# executable whose subsystem version is newer than itself, so 10.0 turns away Windows 8.1 and
# older. macOS (11.0) is set by slash-cli export, and browsers below.
if(MSVC)
    set(CMAKE_CREATE_WIN32_EXE "/subsystem:windows,10.00")
endif()

# Only the targets the game links are built; tools and tests stay off outside the engine repo.
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/.. slashengine EXCLUDE_FROM_ALL)

add_executable(game)
set_target_properties(game PROPERTIES OUTPUT_NAME ${SLASH_GAME_BINARY})
target_link_libraries(game PRIVATE slash::game_main)
slash_target_warnings(game)

if(EMSCRIPTEN)
    set_target_properties(game PROPERTIES SUFFIX ".js")
    # The data folder becomes <binary>.data, mounted at /data. Node is allowed as well as
    # browsers, so CI can run the exported game headless.
    target_link_options(game PRIVATE
        "SHELL:--preload-file ${SLASH_GAME_DATA}@/data"
        "SHELL:-s ALLOW_MEMORY_GROWTH=1"
        "SHELL:-s ENVIRONMENT=web,node"
        "SHELL:-s MIN_CHROME_VERSION=85"
        "SHELL:-s MIN_FIREFOX_VERSION=79"
        "SHELL:-s MIN_SAFARI_VERSION=150000"
        "SHELL:--pre-js ${CMAKE_CURRENT_LIST_DIR}/web/node.js")
    set_property(TARGET game APPEND PROPERTY LINK_DEPENDS
        ${SLASH_GAME_DATA}/project.json ${CMAKE_CURRENT_LIST_DIR}/web/node.js)
    configure_file(web/index.html.in index.html @ONLY)
    install(FILES
        $<TARGET_FILE:game>
        $<TARGET_FILE_DIR:game>/${SLASH_GAME_BINARY}.wasm
        $<TARGET_FILE_DIR:game>/${SLASH_GAME_BINARY}.data
        ${CMAKE_CURRENT_BINARY_DIR}/index.html
        DESTINATION . COMPONENT game)
elseif(APPLE)
    set_target_properties(game PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_LIST_DIR}/macos/Info.plist.in
        MACOSX_BUNDLE_BUNDLE_NAME ${SLASH_GAME_NAME}
        MACOSX_BUNDLE_GUI_IDENTIFIER ${SLASH_GAME_ID}
        MACOSX_BUNDLE_BUNDLE_VERSION ${SLASH_GAME_VERSION}
        MACOSX_BUNDLE_SHORT_VERSION_STRING ${SLASH_GAME_VERSION})
    install(TARGETS game BUNDLE DESTINATION . COMPONENT game)
    install(DIRECTORY ${SLASH_GAME_DATA}/
        DESTINATION ${SLASH_GAME_BINARY}.app/Contents/Resources/data COMPONENT game)
else()
    if(WIN32)
        set_target_properties(game PROPERTIES WIN32_EXECUTABLE TRUE)
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_link_options(game PRIVATE -static-libstdc++ -static-libgcc)
    endif()
    install(TARGETS game RUNTIME DESTINATION . COMPONENT game)
    install(DIRECTORY ${SLASH_GAME_DATA}/ DESTINATION data COMPONENT game)
endif()
