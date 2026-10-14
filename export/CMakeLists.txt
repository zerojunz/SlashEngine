# Builds one game for one platform. slash-cli export configures this project with the game's
# details and a staged data folder, so it is not meant to be configured by hand:
#
#   SLASH_GAME_NAME     display name, e.g. "Space Pong"
#   SLASH_GAME_BINARY   file name, e.g. "SpacePong"
#   SLASH_GAME_ID       reverse-DNS identifier, e.g. "com.example.spacepong"
#   SLASH_GAME_VERSION  major.minor.patch
#   SLASH_GAME_DATA     absolute path of the folder to ship as the game's data
#   SLASH_ANDROID_PACKAGE  Android application ID (Android only)
#   SLASH_IOS_BUILD     CFBundleVersion, if not the version (iOS only)
#
# `cmake --install <build> --component game --prefix <out>` lays the game out for its platform:
# <out>/<binary>.app on macOS, <out>/<binary>[.exe] with <out>/data on Windows and Linux, and
# <out>/index.html with the script, WebAssembly and data files on the web. On Android it lays out
# the APK's contents for one ABI (lib/<abi>/libmain.so, assets/data, AndroidManifest.xml) plus the
# Java sources, which slash-cli export compiles and packages. On iOS it is <out>/<binary>.app,
# which slash-cli export signs and, for devices, zips into an .ipa.
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
# Nothing uses C++ modules. Scanning for them would run whichever clang-scan-deps is on PATH,
# which can't read headers precompiled by another toolchain's clang, such as the Android NDK's.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
# Link the C and C++ runtimes statically where possible, so players need nothing installed.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
# Games support OS versions from 2020 on (docs/roadmap.md). Windows refuses to start an
# executable whose subsystem version is newer than itself, so 10.0 turns away Windows 8.1 and
# older; MinGW builds set it below. macOS (11.0) is set by slash-cli export, glibc (2.31) by the
# sysroot Linux builds use, and browsers below.
if(MSVC)
    set(CMAKE_C_CREATE_WIN32_EXE "/subsystem:windows,10.00")
    set(CMAKE_CXX_CREATE_WIN32_EXE "/subsystem:windows,10.00")
endif()

# The name, escaped for the XML of Info.plist and AndroidManifest.xml.
string(REPLACE "&" "&amp;" SLASH_GAME_NAME_XML "${SLASH_GAME_NAME}")
string(REPLACE "<" "&lt;" SLASH_GAME_NAME_XML "${SLASH_GAME_NAME_XML}")
string(REPLACE ">" "&gt;" SLASH_GAME_NAME_XML "${SLASH_GAME_NAME_XML}")
string(REPLACE "\"" "&quot;" SLASH_GAME_NAME_XML "${SLASH_GAME_NAME_XML}")
string(REPLACE "'" "&apos;" SLASH_GAME_NAME_XML "${SLASH_GAME_NAME_XML}")

# Linux builds use a glibc 2.31 sysroot (see tools/exporter), whose Wayland headers are older than
# the code any host's wayland-scanner writes. So Linux games use X11, which Wayland desktops run
# through XWayland, as Godot does by default.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSROOT)
    set(SDL_WAYLAND OFF CACHE BOOL "" FORCE)
endif()

# Only the targets the game links are built; tools and tests stay off outside the engine repo.
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/.. slashengine EXCLUDE_FROM_ALL)

if(ANDROID)
    # SDL's Java activity loads the game as libmain.so.
    add_library(game SHARED)
    set_target_properties(game PROPERTIES OUTPUT_NAME main)
else()
    add_executable(game)
    set_target_properties(game PROPERTIES OUTPUT_NAME ${SLASH_GAME_BINARY})
endif()
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
elseif(ANDROID)
    if(NOT SLASH_ANDROID_PACKAGE)
        message(FATAL_ERROR "SLASH_ANDROID_PACKAGE is not set. Use \"slash-cli export\" to build a game.")
    endif()
    # Google Play requires native code to support 16 KB memory pages.
    target_link_options(game PRIVATE "-Wl,-z,max-page-size=16384")
    set(SLASH_ANDROID_LABEL "${SLASH_GAME_NAME_XML}")
    configure_file(android/AndroidManifest.xml.in AndroidManifest.xml @ONLY)
    get_target_property(sdl_source_dir SDL3-static SOURCE_DIR)
    install(TARGETS game LIBRARY DESTINATION lib/${ANDROID_ABI} COMPONENT game)
    install(DIRECTORY ${SLASH_GAME_DATA}/ DESTINATION assets/data COMPONENT game)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/AndroidManifest.xml DESTINATION . COMPONENT game)
    install(DIRECTORY ${sdl_source_dir}/android-project/app/src/main/java/
        DESTINATION java COMPONENT game)
    install(FILES android/GameActivity.java DESTINATION java/org/slashengine COMPONENT game)
elseif(IOS)
    # What Xcode records about the build in Info.plist, which App Store Connect checks.
    if(CMAKE_OSX_SYSROOT MATCHES "[Ss]imulator")
        set(SLASH_IOS_SDK iphonesimulator)
        set(SLASH_IOS_PLATFORM iPhoneSimulator)
    else()
        set(SLASH_IOS_SDK iphoneos)
        set(SLASH_IOS_PLATFORM iPhoneOS)
    endif()
    execute_process(COMMAND xcrun --sdk ${SLASH_IOS_SDK} --show-sdk-version
        OUTPUT_VARIABLE SLASH_IOS_SDK_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(COMMAND xcrun --sdk ${SLASH_IOS_SDK} --show-sdk-build-version
        OUTPUT_VARIABLE SLASH_IOS_SDK_BUILD OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(COMMAND sw_vers -buildVersion
        OUTPUT_VARIABLE SLASH_IOS_HOST_BUILD OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(COMMAND xcodebuild -version OUTPUT_VARIABLE xcode_version)
    # "Xcode 16.2\nBuild version 16C5032a" becomes 1620 and 16C5032a.
    string(REGEX MATCH "Xcode ([0-9]+)\\.?([0-9]*)\\.?([0-9]*)" xcode_match "${xcode_version}")
    set(xcode_minor ${CMAKE_MATCH_2})
    set(xcode_patch ${CMAKE_MATCH_3})
    if(xcode_minor STREQUAL "")
        set(xcode_minor 0)
    endif()
    if(xcode_patch STREQUAL "")
        set(xcode_patch 0)
    endif()
    set(SLASH_IOS_XCODE "${CMAKE_MATCH_1}${xcode_minor}${xcode_patch}")
    string(REGEX MATCH "Build version ([0-9A-Za-z]+)" xcode_build_match "${xcode_version}")
    set(SLASH_IOS_XCODE_BUILD ${CMAKE_MATCH_1})
    if(NOT SLASH_IOS_BUILD)
        set(SLASH_IOS_BUILD ${SLASH_GAME_VERSION})
    endif()
    set_target_properties(game PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_LIST_DIR}/ios/Info.plist.in
        MACOSX_BUNDLE_BUNDLE_NAME "${SLASH_GAME_NAME_XML}"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${SLASH_GAME_ID}
        MACOSX_BUNDLE_BUNDLE_VERSION ${SLASH_IOS_BUILD}
        MACOSX_BUNDLE_SHORT_VERSION_STRING ${SLASH_GAME_VERSION})
    install(TARGETS game BUNDLE DESTINATION . COMPONENT game)
    # iOS bundles are flat: resources sit next to the executable.
    install(DIRECTORY ${SLASH_GAME_DATA}/
        DESTINATION ${SLASH_GAME_BINARY}.app/data COMPONENT game)
elseif(APPLE)
    set_target_properties(game PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_LIST_DIR}/macos/Info.plist.in
        MACOSX_BUNDLE_BUNDLE_NAME "${SLASH_GAME_NAME_XML}"
        MACOSX_BUNDLE_GUI_IDENTIFIER ${SLASH_GAME_ID}
        MACOSX_BUNDLE_BUNDLE_VERSION ${SLASH_GAME_VERSION}
        MACOSX_BUNDLE_SHORT_VERSION_STRING ${SLASH_GAME_VERSION})
    install(TARGETS game BUNDLE DESTINATION . COMPONENT game)
    install(DIRECTORY ${SLASH_GAME_DATA}/
        DESTINATION ${SLASH_GAME_BINARY}.app/Contents/Resources/data COMPONENT game)
else()
    if(WIN32)
        set_target_properties(game PROPERTIES WIN32_EXECUTABLE TRUE)
    endif()
    if(WIN32 AND NOT MSVC)
        # llvm-mingw: link libc++, libunwind and winpthreads in, so there are no DLLs to ship.
        target_link_options(game PRIVATE -static
            "-Wl,--major-subsystem-version,10" "-Wl,--minor-subsystem-version,0")
    endif()
    # Linux games link the system's libstdc++ and libgcc_s, which every distribution ships; built
    # against GCC 10's, they need nothing newer than Ubuntu 20.04 and Debian 11 have.
    install(TARGETS game RUNTIME DESTINATION . COMPONENT game)
    install(DIRECTORY ${SLASH_GAME_DATA}/ DESTINATION data COMPONENT game)
endif()
