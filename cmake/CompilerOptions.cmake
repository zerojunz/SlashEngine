# Shared compiler settings for SlashEngine targets.

# Warnings, and MSVC's /bigobj, for every target we own (engine, tools and tests).
function(slash_target_warnings target)
    if(MSVC)
        # /bigobj: sources that register every component, and the tests, make more sections
        # than an MSVC object file holds by default.
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /bigobj)
        if(SLASH_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
        # A double-precision engine rounds positions to floats wherever they meet the renderer,
        # fonts and float math, as Godot's precision=double does without a word.
        if(SLASH_PRECISION STREQUAL "double")
            target_compile_options(${target} PRIVATE /wd4244 /wd4305)
        endif()
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Wold-style-cast
            -Woverloaded-virtual -Wconversion -Wsign-conversion)
        if(SLASH_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
        if(SLASH_PRECISION STREQUAL "double")
            target_compile_options(${target} PRIVATE -Wno-float-conversion)
        endif()
    endif()
endfunction()

# Engine libraries build without exceptions or RTTI; errors travel as slash::Result.
function(slash_engine_target target)
    slash_target_warnings(${target})
    if(MSVC)
        target_compile_options(${target} PRIVATE /EHs-c- /GR-)
        target_compile_definitions(${target} PRIVATE _HAS_EXCEPTIONS=0)
    else()
        target_compile_options(${target} PRIVATE -fno-exceptions -fno-rtti)
    endif()
endfunction()
