add_library(slash_gameplay
    src/host.cpp)
add_library(slash::gameplay ALIAS slash_gameplay)

target_include_directories(slash_gameplay PUBLIC include)
target_link_libraries(slash_gameplay PUBLIC slash::core slash::platform slash::world)
slash_engine_target(slash_gameplay)

# Lets a program load gameplay libraries (slash/gameplay/host.hpp). They are linked with the
# engine's functions left unresolved, so the program links every engine library a game's code may
# call in whole and exports its symbols to them. Only macOS and Linux tools load them; exports
# link a game's code in instead.
function(slash_gameplay_host target)
    if(NOT (APPLE OR CMAKE_SYSTEM_NAME STREQUAL "Linux") OR IOS OR ANDROID)
        return()
    endif()
    set(libraries slash_ai slash_animation slash_asset slash_audio slash_canvas slash_core slash_gameplay
        slash_input slash_l10n slash_net slash_physics2d slash_platform slash_render slash_runtime slash_services slash_text
        slash_ui slash_video slash_world slash_xr)
    set_property(TARGET ${target} PROPERTY ENABLE_EXPORTS ON)
    target_link_libraries(${target} PRIVATE ${libraries})
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
