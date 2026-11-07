add_library(slash_gameplay
    src/host.cpp)
add_library(slash::gameplay ALIAS slash_gameplay)

target_include_directories(slash_gameplay PUBLIC include)
target_link_libraries(slash_gameplay PUBLIC slash::core slash::platform slash::world)
slash_engine_target(slash_gameplay)

# The engine libraries a game's code may call, which a program loading gameplay libraries links
# in whole and exports.
set(SLASH_GAMEPLAY_LIBRARIES slash_ai slash_animation slash_asset slash_audio slash_canvas slash_core
    slash_gameplay slash_input slash_l10n slash_net slash_physics2d slash_platform slash_procedural
    slash_render slash_runtime slash_services slash_text slash_ui slash_video slash_world slash_xr
    CACHE INTERNAL "")
set(SLASH_GAMEPLAY_DIR ${CMAKE_CURRENT_SOURCE_DIR} CACHE INTERNAL "")

# Whether this build's tools load gameplay libraries: macOS, Linux, and Windows with MSVC.
if(((APPLE OR CMAKE_SYSTEM_NAME STREQUAL "Linux") AND NOT IOS AND NOT ANDROID) OR
   (WIN32 AND MSVC))
    set(SLASH_GAMEPLAY_HOSTS ON CACHE INTERNAL "")
else()
    set(SLASH_GAMEPLAY_HOSTS OFF CACHE INTERNAL "")
endif()

# On Windows a program exports the engine's symbols through a module-definition file that
# exports/write_def.cpp writes from the engine libraries, and gameplay libraries link against
# slash-host.lib, an import library naming a program "slash-host.exe" that cmake/gameplay's
# delay_load.cpp answers with the program that loads them (gameplay-sdk.cmake's
# SLASH_SDK_HOST_LIBRARY). Made once, for every program.
function(slash_gameplay_exports)
    if(TARGET slash_gameplay_exports)
        return()
    endif()
    add_executable(slash_write_def ${SLASH_GAMEPLAY_DIR}/exports/write_def.cpp)
    set(folder ${CMAKE_BINARY_DIR}/gameplay)
    set(machine ${CMAKE_CXX_COMPILER_ARCHITECTURE_ID})
    if(NOT machine)
        set(machine x64)
    endif()
    set(files "")
    foreach(library IN LISTS SLASH_GAMEPLAY_LIBRARIES)
        list(APPEND files $<TARGET_FILE:${library}>)
    endforeach()
    add_custom_command(OUTPUT ${folder}/slash-host.def ${folder}/slash-host.lib
        COMMAND ${CMAKE_COMMAND} -E make_directory ${folder}
        COMMAND slash_write_def ${folder}/slash-host.def ${files}
        COMMAND ${CMAKE_AR} /nologo /def:${folder}/slash-host.def /name:slash-host.exe
            /machine:${machine} /out:${folder}/slash-host.lib
        DEPENDS slash_write_def ${SLASH_GAMEPLAY_LIBRARIES}
        COMMENT "Listing the engine's symbols for gameplay libraries"
        VERBATIM)
    add_custom_target(slash_gameplay_exports DEPENDS ${folder}/slash-host.def
        ${folder}/slash-host.lib)
    set(SLASH_GAMEPLAY_HOST_LIBRARY ${folder}/slash-host.lib CACHE INTERNAL "")
endfunction()

# Lets a program load gameplay libraries (slash/gameplay/host.hpp). They are linked with the
# engine's functions left unresolved (on Windows, to slash-host.lib), so the program links every
# engine library a game's code may call in whole and exports its symbols to them. Exports link a
# game's code in instead.
function(slash_gameplay_host target)
    if(NOT SLASH_GAMEPLAY_HOSTS)
        return()
    endif()
    set(libraries ${SLASH_GAMEPLAY_LIBRARIES})
    target_link_libraries(${target} PRIVATE ${libraries})
    if(WIN32)
        slash_gameplay_exports()
        add_dependencies(${target} slash_gameplay_exports)
        set(archives "")
        foreach(library IN LISTS libraries)
            list(APPEND archives "/WHOLEARCHIVE:$<TARGET_FILE:${library}>")
        endforeach()
        target_link_options(${target} PRIVATE ${archives}
            "/DEF:${CMAKE_BINARY_DIR}/gameplay/slash-host.def")
        set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS
            ${CMAKE_BINARY_DIR}/gameplay/slash-host.def)
        return()
    endif()
    set_property(TARGET ${target} PROPERTY ENABLE_EXPORTS ON)
    # Each archive whole, once, ahead of the program's objects. The usual link line names them
    # again later, but by then nothing in them is missing, so nothing is linked twice.
    set(archives "")
    foreach(library IN LISTS libraries)
        if(APPLE)
            list(APPEND archives "LINKER:-force_load,$<TARGET_FILE:${library}>")
        else()
            list(APPEND archives "$<TARGET_FILE:${library}>")
        endif()
    endforeach()
    if(APPLE)
        target_link_options(${target} PRIVATE ${archives})
    else()
        target_link_options(${target} PRIVATE "LINKER:--whole-archive" ${archives}
            "LINKER:--no-whole-archive")
    endif()
endfunction()

# Builds a library of gameplay code as cmake/gameplay/CMakeLists.txt builds a game's, for tests:
# the engine's headers, no exceptions or RTTI, and the engine's functions left for the program
# that loads it.
function(slash_gameplay_module target)
    set_target_properties(${target} PROPERTIES PREFIX "")
    target_include_directories(${target} PRIVATE
        $<TARGET_PROPERTY:slash_runtime,INTERFACE_INCLUDE_DIRECTORIES>)
    target_compile_definitions(${target} PRIVATE
        $<TARGET_PROPERTY:slash_runtime,INTERFACE_COMPILE_DEFINITIONS>)
    if(MSVC)
        slash_gameplay_exports()
        set_target_properties(${target} PROPERTIES SUFFIX ".dll")
        target_sources(${target} PRIVATE ${PROJECT_SOURCE_DIR}/cmake/gameplay/delay_load.cpp)
        target_compile_options(${target} PRIVATE /EHs-c- /GR- /permissive- /utf-8 /bigobj)
        target_compile_definitions(${target} PRIVATE _HAS_EXCEPTIONS=0)
        add_dependencies(${target} slash_gameplay_exports)
        target_link_libraries(${target} PRIVATE ${SLASH_GAMEPLAY_HOST_LIBRARY} delayimp)
        target_link_options(${target} PRIVATE /DELAYLOAD:slash-host.exe)
    else()
        set_target_properties(${target} PROPERTIES SUFFIX ".so")
        target_compile_options(${target} PRIVATE -fno-exceptions -fno-rtti
            $<$<CXX_COMPILER_ID:GNU>:-fno-gnu-unique>)
        if(APPLE)
            target_link_options(${target} PRIVATE -undefined dynamic_lookup)
        endif()
    endif()
endfunction()
