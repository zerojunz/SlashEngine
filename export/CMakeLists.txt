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
#   SLASH_GAME_MICROPHONE        ON if the game records (project.json's audio.enable_input)
#   SLASH_GAME_MICROPHONE_USAGE  why, shown when macOS and iOS ask for the microphone
#   SLASH_GAME_AI       ON if the game calls language models (it has a providers.json)
#   SLASH_GAME_SOURCES  the game's own C++ code (project.json's gameplay.sources): absolute
#                       paths of folders, whose .cpp files are compiled in, and files
#   SLASH_GAME_SDKS     a CMake file of slash_game_sdk() calls for the SDK packages the game
#                       links, and slash_game_sdk_register() for the C++ file that registers
#                       their services backends, written per build by slash-cli export
#                       (tools/exporter/src/sdk.cpp)
#
# `cmake --install <build> --component game --prefix <out>` lays the game out for its platform:
# <out>/<binary>.app on macOS, <out>/<binary>[.exe] with <out>/data on Windows and Linux, and
# <out>/index.html with the script, WebAssembly and data files on the web. On Android it lays out
# the APK's contents for one ABI (lib/<abi>/libmain.so, assets/data, AndroidManifest.xml) plus the
# Java sources, which slash-cli export compiles and packages. On iOS it is <out>/<binary>.app,
# which slash-cli export signs and, for devices, zips into an .ipa.
cmake_minimum_required(VERSION 3.25)

project(SlashGame LANGUAGES C CXX)
# SDK packages' glue code can be Objective-C on macOS and iOS (.m and .mm files).
if(APPLE)
    enable_language(OBJC)
    enable_language(OBJCXX)
endif()

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
# older. MinGW builds keep the default 6.0: Windows 11 and Server 2022 refuse to start one marked
# 10.0 (0xC000007B, in the loader), and they need Windows 10's Universal CRT anyway. macOS (11.0)
# is set by slash-cli export, glibc (2.31) by the sysroot Linux builds use, and browsers below.
if(MSVC)
    set(CMAKE_C_CREATE_WIN32_EXE "/subsystem:windows,10.00")
    set(CMAKE_CXX_CREATE_WIN32_EXE "/subsystem:windows,10.00")
endif()

# Text escaped for the XML of Info.plist and AndroidManifest.xml.
function(slash_escape_xml out text)
    string(REPLACE "&" "&amp;" text "${text}")
    string(REPLACE "<" "&lt;" text "${text}")
    string(REPLACE ">" "&gt;" text "${text}")
    string(REPLACE "\"" "&quot;" text "${text}")
    string(REPLACE "'" "&apos;" text "${text}")
    set(${out} "${text}" PARENT_SCOPE)
endfunction()
slash_escape_xml(SLASH_GAME_NAME_XML "${SLASH_GAME_NAME}")

# A game that records asks for the microphone, with a reason on macOS and iOS (whose systems stop
# apps that record without one) and the RECORD_AUDIO permission on Android.
set(SLASH_MICROPHONE_PLIST "")
set(SLASH_ANDROID_PERMISSIONS "")
if(SLASH_GAME_MICROPHONE)
    if(NOT SLASH_GAME_MICROPHONE_USAGE)
        set(SLASH_GAME_MICROPHONE_USAGE "${SLASH_GAME_NAME} listens to the microphone.")
    endif()
    slash_escape_xml(usage_xml "${SLASH_GAME_MICROPHONE_USAGE}")
    set(SLASH_MICROPHONE_PLIST
        "<key>NSMicrophoneUsageDescription</key>\n    <string>${usage_xml}</string>")
    set(SLASH_ANDROID_PERMISSIONS
        "<uses-permission android:name=\"android.permission.RECORD_AUDIO\" />")
endif()

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
# Language models (engine/ai), for the game's own code and, with a providers.json, main(). Only
# what the game calls is linked in.
target_link_libraries(game PRIVATE slash::ai)
if(SLASH_GAME_AI)
    target_compile_definitions(game PRIVATE SLASH_GAME_AI)
endif()
# Built as the engine is, with no exceptions or RTTI, as the tools build the game's code.
slash_engine_target(game)

# The game's own code, with its SLASH_GAME_MODULE function, which main() installs.
set(game_sources "")
foreach(source IN LISTS SLASH_GAME_SOURCES)
    if(IS_DIRECTORY "${source}")
        file(GLOB_RECURSE found CONFIGURE_DEPENDS "${source}/*.cpp")
        list(APPEND game_sources ${found})
        target_include_directories(game PRIVATE "${source}")
    elseif(source MATCHES "\\.cpp$")
        list(APPEND game_sources "${source}")
    elseif(EXISTS "${source}")
        get_filename_component(folder "${source}" DIRECTORY)
        target_include_directories(game PRIVATE "${folder}")
    else()
        message(FATAL_ERROR "SLASH_GAME_SOURCES names ${source}, which does not exist")
    endif()
endforeach()
if(SLASH_GAME_SOURCES AND NOT game_sources)
    message(FATAL_ERROR "the game's gameplay sources have no .cpp files")
endif()
if(game_sources)
    target_sources(game PRIVATE ${game_sources})
    target_compile_definitions(game PRIVATE SLASH_GAMEPLAY_LINKED)
endif()

# Links one SDK package (sdk.json) into the game: SLASH_SDK_<NAME> and its defines, its include
# folders, libraries and frameworks. Shared libraries and frameworks ship where the platform's
# loader finds them: beside a Linux game (found through $ORIGIN), in the bundle's Frameworks
# folder on macOS and iOS, in lib/<abi> on Android. Files ship beside the executable on Windows
# and Linux, in Resources on macOS, in the app on iOS, in assets on Android and beside index.html
# on the web. Java libraries go in jars/, which slash-cli export compiles against and puts in the
# APK. Glue code is compiled into the game: C and C++ files everywhere, Objective-C ones (with
# ARC) on macOS and iOS, and Java files into the APK, through java/sdks/<name>.
set(SLASH_SDK_PLIST "")
set(SLASH_SDK_ANDROID_MANIFEST "")
set(SLASH_SDK_ANDROID_APPLICATION "")
function(slash_game_sdk name)
    cmake_parse_arguments(PARSE_ARGV 1 sdk "" ""
        "DEFINES;INCLUDE;LIBRARIES;SHARED;FRAMEWORKS;EMBED;SYSTEM_LIBRARIES;SYSTEM_FRAMEWORKS;LINK_OPTIONS;FILES;JARS;SOURCES;JAVA")
    target_compile_definitions(game PRIVATE ${sdk_DEFINES})
    target_include_directories(game SYSTEM PRIVATE ${sdk_INCLUDE})
    target_sources(game PRIVATE ${sdk_SOURCES})
    foreach(source IN LISTS sdk_SOURCES)
        if(source MATCHES "\\.mm?$")
            set_source_files_properties("${source}" PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
        endif()
    endforeach()
    foreach(java IN LISTS sdk_JAVA)
        if(IS_DIRECTORY "${java}")
            install(DIRECTORY "${java}/" DESTINATION java/sdks/${name} COMPONENT game
                FILES_MATCHING PATTERN "*.java")
        else()
            install(FILES "${java}" DESTINATION java/sdks/${name} COMPONENT game)
        endif()
    endforeach()
    if(APPLE)
        if(IOS)
            set(frameworks_dir ${SLASH_GAME_BINARY}.app/Frameworks)
            set(beside_dir ${SLASH_GAME_BINARY}.app)
        else()
            set(frameworks_dir ${SLASH_GAME_BINARY}.app/Contents/Frameworks)
            # Contents/MacOS is only for code, which codesign seals differently.
            set(beside_dir ${SLASH_GAME_BINARY}.app/Contents/Resources)
        endif()
    elseif(ANDROID)
        set(beside_dir assets)
    else()
        set(beside_dir .)
    endif()
    foreach(library IN LISTS sdk_LIBRARIES)
        if(library IN_LIST sdk_SHARED AND (ANDROID OR (UNIX AND NOT APPLE)))
            # By file name, so the game asks for it by name even when it has no SONAME.
            get_filename_component(folder "${library}" DIRECTORY)
            get_filename_component(file "${library}" NAME)
            target_link_directories(game PRIVATE "${folder}")
            target_link_libraries(game PRIVATE "-l:${file}")
        else()
            target_link_libraries(game PRIVATE "${library}")
        endif()
    endforeach()
    foreach(library IN LISTS sdk_SHARED)
        if(ANDROID)
            install(FILES "${library}" DESTINATION lib/${ANDROID_ABI} COMPONENT game)
        elseif(APPLE)
            install(FILES "${library}" DESTINATION ${frameworks_dir} COMPONENT game)
        else()
            install(FILES "${library}" DESTINATION . COMPONENT game)
        endif()
    endforeach()
    foreach(framework IN LISTS sdk_FRAMEWORKS)
        get_filename_component(folder "${framework}" DIRECTORY)
        target_compile_options(game PRIVATE "-F${folder}")
        target_link_libraries(game PRIVATE "$<LINK_LIBRARY:FRAMEWORK,${framework}>")
    endforeach()
    foreach(framework IN LISTS sdk_EMBED)
        install(DIRECTORY "${framework}" DESTINATION ${frameworks_dir} USE_SOURCE_PERMISSIONS
            COMPONENT game)
    endforeach()
    target_link_libraries(game PRIVATE ${sdk_SYSTEM_LIBRARIES})
    foreach(framework IN LISTS sdk_SYSTEM_FRAMEWORKS)
        target_link_libraries(game PRIVATE "$<LINK_LIBRARY:FRAMEWORK,${framework}>")
    endforeach()
    target_link_options(game PRIVATE ${sdk_LINK_OPTIONS})
    foreach(file IN LISTS sdk_FILES)
        if(IS_DIRECTORY "${file}")
            install(DIRECTORY "${file}" DESTINATION ${beside_dir} USE_SOURCE_PERMISSIONS
                COMPONENT game)
        elseif(ANDROID AND file MATCHES "\\.so$")
            install(FILES "${file}" DESTINATION lib/${ANDROID_ABI} COMPONENT game)
        elseif(APPLE AND file MATCHES "\\.dylib$")
            install(FILES "${file}" DESTINATION ${frameworks_dir} COMPONENT game)
        else()
            install(FILES "${file}" DESTINATION ${beside_dir} COMPONENT game)
        endif()
    endforeach()
    if(sdk_JARS)
        install(FILES ${sdk_JARS} DESTINATION jars COMPONENT game)
    endif()
    set(rpath "")
    if(IOS)
        set(rpath "@executable_path/Frameworks")
    elseif(APPLE)
        set(rpath "@executable_path/../Frameworks")
    elseif(UNIX AND NOT ANDROID AND NOT EMSCRIPTEN)
        set(rpath "$ORIGIN")
    endif()
    get_target_property(rpaths game INSTALL_RPATH)
    if(rpath AND (sdk_SHARED OR sdk_EMBED OR sdk_FILES) AND NOT rpath IN_LIST rpaths)
        set_property(TARGET game APPEND PROPERTY INSTALL_RPATH "${rpath}")
    endif()
endfunction()
# The C++ file defining slash_sdk_register(), which main() calls to add the SDK packages'
# services backends (slash/services/services.hpp).
function(slash_game_sdk_register file)
    target_sources(game PRIVATE "${file}")
    target_compile_definitions(game PRIVATE SLASH_SDK_REGISTER)
endfunction()
if(SLASH_GAME_SDKS)
    include("${SLASH_GAME_SDKS}")
endif()

if(EMSCRIPTEN)
    set_target_properties(game PROPERTIES SUFFIX ".js")
    # The data folder becomes <binary>.data, mounted at /data. Node is allowed as well as
    # browsers, so CI can run the exported game headless. Games draw with WebGL 2.
    target_link_options(game PRIVATE
        "SHELL:--preload-file ${SLASH_GAME_DATA}@/data"
        "SHELL:-s ALLOW_MEMORY_GROWTH=1"
        "SHELL:-s ENVIRONMENT=web,node"
        "SHELL:-s MIN_CHROME_VERSION=85"
        "SHELL:-s MIN_FIREFOX_VERSION=79"
        "SHELL:-s MIN_SAFARI_VERSION=150000"
        "SHELL:-s MIN_WEBGL_VERSION=2"
        "SHELL:-s MAX_WEBGL_VERSION=2"
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
    # A game that calls language models reaches them over the network, which Android allows only
    # apps with the INTERNET permission. Once, if an SDK package asks for it too.
    if(SLASH_GAME_AI AND NOT SLASH_SDK_ANDROID_MANIFEST MATCHES "\"android\\.permission\\.INTERNET\"")
        string(APPEND SLASH_ANDROID_PERMISSIONS
            "\n    <uses-permission android:name=\"android.permission.INTERNET\" />")
    endif()
    configure_file(android/AndroidManifest.xml.in AndroidManifest.xml @ONLY)
    get_target_property(sdl_source_dir SDL3-static SOURCE_DIR)
    install(TARGETS game LIBRARY DESTINATION lib/${ANDROID_ABI} COMPONENT game)
    install(DIRECTORY ${SLASH_GAME_DATA}/ DESTINATION assets/data COMPONENT game)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/AndroidManifest.xml DESTINATION . COMPONENT game)
    install(DIRECTORY ${sdl_source_dir}/android-project/app/src/main/java/
        DESTINATION java COMPONENT game)
    install(FILES android/GameActivity.java DESTINATION java/org/slashengine COMPONENT game)
    # The interfaces Java services backends implement (slash/services/bridges.hpp).
    install(DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/../engine/services/android/
        DESTINATION java COMPONENT game)
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
        target_link_options(game PRIVATE -static)
    endif()
    # Linux games link the system's libstdc++ and libgcc_s, which every distribution ships; built
    # against GCC 10's, they need nothing newer than Ubuntu 20.04 and Debian 11 have.
    install(TARGETS game RUNTIME DESTINATION . COMPONENT game)
    install(DIRECTORY ${SLASH_GAME_DATA}/ DESTINATION data COMPONENT game)
endif()
