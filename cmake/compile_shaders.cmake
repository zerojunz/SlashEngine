# Compiles an HLSL shader with vs_main and fs_main entry points into the forms SDL's GPU API
# takes, and writes them to a C++ header: SPIR-V for Vulkan (glslang), MSL for Metal
# (SPIRV-Cross), and the HLSL source itself, which Direct3D 12 compiles at startup with Windows'
# own d3dcompiler. The headers are committed, so building the engine needs no shader tools.
#
#   cmake -DSOURCE=<shader.hlsl> -DOUTPUT=<header> -DNAME=<Sprite> -DWORKDIR=<dir>
#         -DGLSLANG=<glslangValidator> -DSPIRV_CROSS=<spirv-cross> -P compile_shaders.cmake
#   cmake -DSOURCE=<shader.hlsl> -DOUTPUT=<header> -DCHECK=ON -P compile_shaders.cmake
#
# With CHECK, nothing is compiled: it fails if the HLSL in OUTPUT differs from SOURCE, which means
# the shader changed and the header wasn't regenerated. It needs no shader tools, so every build
# can run it; compiled bytes differ between tool versions, so they aren't compared.
#
# MSVC refuses string literals over 16380 bytes, so long sources are written as several raw
# literals in a row, split after a line, which the compiler joins.

cmake_minimum_required(VERSION 3.25)

# The most bytes in one raw string literal, well under MSVC's limit.
set(max_literal 12000)

if(CHECK)
    file(READ "${SOURCE}" hlsl_text)
    file(READ "${OUTPUT}" committed)
    string(FIND "${committed}" "R\"hlsl(" start)
    string(FIND "${committed}" ")hlsl\"" end REVERSE)
    if(start EQUAL -1 OR end EQUAL -1)
        message(FATAL_ERROR "${OUTPUT} has no HLSL; regenerate it")
    endif()
    math(EXPR start "${start} + 7")
    math(EXPR length "${end} - ${start}")
    string(SUBSTRING "${committed}" ${start} ${length} committed_hlsl)
    string(REPLACE ")hlsl\"\nR\"hlsl(" "" committed_hlsl "${committed_hlsl}")
    if(NOT committed_hlsl STREQUAL hlsl_text)
        message(FATAL_ERROR "${OUTPUT} is out of date with ${SOURCE}; run "
                            "\"cmake --build --preset dev --target shaders\" and commit it")
    endif()
    return()
endif()

function(run what)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE result OUTPUT_VARIABLE output
                    ERROR_VARIABLE output)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "${what} failed for ${SOURCE}:\n${output}")
    endif()
endfunction()

# text as raw string literals with this delimiter, each at most max_literal bytes and split after
# a line, for the compiler to join.
function(raw_literals text tag out)
    set(result "")
    set(rest "${text}")
    string(LENGTH "${rest}" length)
    while(length GREATER max_literal)
        string(SUBSTRING "${rest}" 0 ${max_literal} head)
        string(FIND "${head}" "\n" cut REVERSE)
        if(cut EQUAL -1)
            math(EXPR cut "${max_literal} - 1")
        endif()
        math(EXPR cut "${cut} + 1")
        string(SUBSTRING "${rest}" 0 ${cut} piece)
        string(SUBSTRING "${rest}" ${cut} -1 rest)
        string(APPEND result "R\"${tag}(${piece})${tag}\"\n")
        string(LENGTH "${rest}" length)
    endwhile()
    string(APPEND result "R\"${tag}(${rest})${tag}\"")
    set(${out} "${result}" PARENT_SCOPE)
endfunction()

# The bytes of a file as a C array body, twelve to a line. Vulkan reads SPIR-V as 32-bit words,
# so the arrays are declared 4-byte aligned.
function(hex_array file out)
    file(READ "${file}" hex HEX)
    string(REGEX REPLACE "(..)" "0x\\1, " hex "${hex}")
    # CMake's regular expressions have no {n}, so the line's pattern is spelled out.
    string(REPEAT "0x[0-9a-f][0-9a-f], " 11 line)
    string(REGEX REPLACE "(${line}0x[0-9a-f][0-9a-f],) " "\\1\n    " hex "${hex}")
    string(REGEX REPLACE ",\n    $" ",\n" hex "${hex}")
    string(REGEX REPLACE ", $" ",\n" hex "${hex}")
    set(${out} "    ${hex}" PARENT_SCOPE)
endfunction()

if(NOT EXISTS "${GLSLANG}" OR NOT EXISTS "${SPIRV_CROSS}")
    message(FATAL_ERROR "compiling shaders needs glslangValidator and spirv-cross: install "
                        "glslang-tools and spirv-cross (apt) or glslang and spirv-cross (brew)")
endif()
file(MAKE_DIRECTORY "${WORKDIR}")
get_filename_component(base "${SOURCE}" NAME_WE)
set(body "")
foreach(stage IN ITEMS vert frag)
    if(stage STREQUAL "vert")
        set(entry vs_main)
        set(label Vertex)
    else()
        set(entry fs_main)
        set(label Fragment)
    endif()
    set(spirv "${WORKDIR}/${base}.${stage}.spv")
    set(msl "${WORKDIR}/${base}.${stage}.msl")
    run(glslang "${GLSLANG}" -D -V -S ${stage} -e ${entry} --auto-sampled-textures "${SOURCE}"
        -o "${spirv}")
    # Metal's texture, sampler and buffer indexes are the HLSL registers, as SDL binds them.
    run(spirv-cross "${SPIRV_CROSS}" --msl --msl-version 20100 --msl-decoration-binding
        --stage ${stage} --entry ${entry} "${spirv}" --output "${msl}")
    hex_array("${spirv}" bytes)
    file(READ "${msl}" msl_text)
    raw_literals("${msl_text}" msl msl_literals)
    string(APPEND body
        "alignas(4) inline constexpr unsigned char k${NAME}${label}Spirv[] = {\n${bytes}};\n\n"
        "inline constexpr char k${NAME}${label}Msl[] = ${msl_literals};\n\n")
endforeach()
file(READ "${SOURCE}" hlsl_text)
raw_literals("${hlsl_text}" hlsl hlsl_literals)
get_filename_component(source_name "${SOURCE}" NAME)
set(header "// Generated by cmake/compile_shaders.cmake from engine/render/shaders/${source_name}.
// Don't edit: change the shader, then run \"cmake --build --preset dev --target shaders\".
// clang-format off
#pragma once

namespace slash::render::shaders {

${body}inline constexpr char k${NAME}Hlsl[] = ${hlsl_literals};

} // namespace slash::render::shaders
// clang-format on
")

file(WRITE "${OUTPUT}" "${header}")
message(STATUS "wrote ${OUTPUT}")
