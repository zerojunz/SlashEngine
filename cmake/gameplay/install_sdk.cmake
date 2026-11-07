# Installs the gameplay SDK beside the engine's sources (the top-level CMakeLists.txt runs it at
# install time): a gameplay-sdk.cmake for cmake/gameplay/CMakeLists.txt's "cmake -C", as a build
# tree's (tools/exporter/CMakeLists.txt) but relocatable. It names the compiler by its file name,
# found on PATH, so a game's code builds with the person's own Xcode or compiler, and its include
# folders relative to itself: the engine's own under engine/, and copies of the headers it takes
# from the build and the fetched dependencies under sdk/, with the import library game libraries
# link on Windows.
#
# SLASH_SDK_DATA names the file tools/exporter/CMakeLists.txt generates, SLASH_SDK_DESTINATION
# the engine's folder under the install prefix.
include("${SLASH_SDK_DATA}")
set(root "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/${SLASH_SDK_DESTINATION}")

set(includes "")
set(copies 0)
foreach(folder IN LISTS slash_sdk_includes)
    file(RELATIVE_PATH relative "${slash_sdk_source}" "${folder}")
    if(relative MATCHES "^engine/[^/]+/include$")
        list(APPEND includes "\${CMAKE_CURRENT_LIST_DIR}/${relative}")
    else()
        set(copy "sdk/include${copies}")
        math(EXPR copies "${copies} + 1")
        message(STATUS "Installing: ${root}/${copy} (headers of ${folder})")
        file(COPY "${folder}/" DESTINATION "${root}/${copy}" FILES_MATCHING
            PATTERN "*.h" PATTERN "*.hpp" PATTERN "*.hh" PATTERN "*.inl" PATTERN "*.ipp")
        list(APPEND includes "\${CMAKE_CURRENT_LIST_DIR}/${copy}")
    endif()
endforeach()

get_filename_component(compiler "${slash_sdk_compiler}" NAME)
set(content "# Written by cmake/gameplay/install_sdk.cmake for cmake/gameplay/CMakeLists.txt.\n")
string(APPEND content "set(CMAKE_CXX_COMPILER [==[${compiler}]==] CACHE STRING \"\" FORCE)\n")
foreach(pair IN ITEMS "CMAKE_BUILD_TYPE|build_type" "CMAKE_CXX_FLAGS|flags"
        "CMAKE_OSX_ARCHITECTURES|architectures" "CMAKE_OSX_DEPLOYMENT_TARGET|deployment_target")
    string(REPLACE "|" ";" pair "${pair}")
    list(GET pair 0 variable)
    list(GET pair 1 name)
    string(APPEND content
        "set(${variable} [==[${slash_sdk_${name}}]==] CACHE STRING \"\" FORCE)\n")
endforeach()
string(APPEND content
    "set(SLASH_SDK_INCLUDE_DIRECTORIES \"${includes}\" CACHE INTERNAL \"\")\n"
    "set(SLASH_SDK_COMPILE_DEFINITIONS [==[${slash_sdk_definitions}]==] CACHE INTERNAL \"\")\n")
# On Windows, the import library of what the installed tools export, which game libraries link.
if(slash_sdk_host_library)
    message(STATUS "Installing: ${root}/sdk/slash-host.lib")
    file(COPY "${slash_sdk_host_library}" DESTINATION "${root}/sdk")
    string(APPEND content "set(SLASH_SDK_HOST_LIBRARY \"\${CMAKE_CURRENT_LIST_DIR}/sdk/slash-host.lib\" "
        "CACHE INTERNAL \"\")\n")
endif()
message(STATUS "Installing: ${root}/gameplay-sdk.cmake")
file(WRITE "${root}/gameplay-sdk.cmake" "${content}")
