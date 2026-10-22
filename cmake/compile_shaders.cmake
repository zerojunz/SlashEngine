# Compiles an HLSL shader with vs_main and fs_main entry points into the forms the render devices
# take, and writes them to a C++ header: SPIR-V for Vulkan (glslang), MSL for Metal and GLSL ES
# 3.00 for OpenGL ES and WebGL 2 (SPIRV-Cross), and the HLSL source itself, which Direct3D 12
# compiles at startup with Windows' own d3dcompiler, and WGSL for WebGPU (Tint, from SPIR-V with
# separate textures and samplers, as WGSL has them). The headers are committed, so building the
# engine needs no shader tools.
#
#   cmake -DSOURCE=<shader.hlsl> -DOUTPUT=<header> -DNAME=<Sprite> -DWORKDIR=<dir>
#         -DGLSLANG=<glslangValidator> -DSPIRV_CROSS=<spirv-cross> -DTINT=<tint>
#         -P compile_shaders.cmake
#   cmake -DSOURCE=<shader.hlsl> -DOUTPUT=<header> -DCHECK=ON -P compile_shaders.cmake
#
# GLSL ES 3.00 links stages by the names of their varyings, which INTERFACE gives in location
# order, separated by commas (v_uv,v_color,v_world by default). With FIXUP_CLIPSPACE, the GLSL
# maps depth from 0 to 1 to OpenGL's -1 to 1, for shaders that draw with depth.
#
# With COMPUTE, the shader is a compute shader with a cs_main entry point instead, and there is no
# GLSL ES (OpenGL ES runs particles on the CPU). Its MSL comes from SPIR-V compiled with
# SLASH_MSL defined, since Metal numbers buffers in one list where the others use three; it and
# the WGSL are arrays of pieces to join.
#
# With PIECES, each stage's MSL, GLSL ES and WGSL are arrays of pieces to join too, for a shader
# whose code is longer than a string literal may be (the mesh shader's).
#
# WGSL keeps SDL's bind groups, which the HLSL's register spaces name: a vertex stage's uniforms in
# group 1, a fragment stage's textures in group 2 with their samplers 8 bindings after them
# (glslang's --shift-sampler-binding) and its uniforms in group 3; a compute shader's read-only
# buffers in group 0, read-write ones in 1 and uniforms in 2.
#
# With CHECK, nothing is compiled: it fails if the HLSL in OUTPUT differs from SOURCE, which means
# the shader changed and the header wasn't regenerated. It needs no shader tools, so every build
# can run it; compiled bytes differ between tool versions, so they aren't compared.
#
# A line `#include "name.hlsl"` brings in that file from the shader's folder (canvas.hlsl, which
# a game's shaders share), so the HLSL written to the header stands alone for Direct3D 12.
#
# MSVC refuses string literals over 16380 bytes, so long sources are written as several raw
# literals in a row, split after a line, which the compiler joins.

cmake_minimum_required(VERSION 3.25)

# The most bytes in one raw string literal, well under MSVC's limit.
set(max_literal 12000)

# SOURCE with each #include line replaced by the file it names.
function(read_with_includes out)
    file(READ "${SOURCE}" text)
    get_filename_component(folder "${SOURCE}" DIRECTORY)
    string(REGEX MATCHALL "#include \"[^\"]+\"\n" includes "${text}")
    foreach(line IN LISTS includes)
        string(REGEX REPLACE "#include \"([^\"]+)\"\n" "\\1" name "${line}")
        file(READ "${folder}/${name}" included)
        string(REPLACE "${line}" "${included}" text "${text}")
    endforeach()
    set(${out} "${text}" PARENT_SCOPE)
endfunction()

if(CHECK)
    read_with_includes(hlsl_text)
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
if(NOT EXISTS "${TINT}")
    message(FATAL_ERROR "compiling shaders needs Tint's tint tool for WGSL: build Dawn's "
                        "(github.com/google/dawn) with TINT_BUILD_CMD_TOOLS and pass "
                        "-DSLASH_TINT=<path to tint> to CMake")
endif()

# Raw string literals in a row as the items of an array, for the device to join, since Clang and
# MSVC take no string literal, even one joined from several, over 65536 bytes.
function(as_pieces literals tag out)
    string(REPLACE ")${tag}\"\nR\"${tag}(" ")${tag}\",\nR\"${tag}(" literals "${literals}")
    set(${out} "${literals}" PARENT_SCOPE)
endfunction()

# A declaration of code as one string, or as an array of pieces when PIECES says to.
function(declare_code name literals tag out)
    if(PIECES)
        as_pieces("${literals}" ${tag} pieces)
        set(${out} "inline constexpr const char* ${name}[] = {\n${pieces}};\n\n" PARENT_SCOPE)
    else()
        set(${out} "inline constexpr char ${name}[] = ${literals};\n\n" PARENT_SCOPE)
    endif()
endfunction()
file(MAKE_DIRECTORY "${WORKDIR}")
get_filename_component(base "${SOURCE}" NAME_WE)
read_with_includes(hlsl_text)
set(standalone "${WORKDIR}/${base}.hlsl")
file(WRITE "${standalone}" "${hlsl_text}")
set(body "")
if(COMPUTE)
    set(stages "")
    set(spirv "${WORKDIR}/${base}.comp.spv")
    set(msl_spirv "${WORKDIR}/${base}.comp.msl.spv")
    set(msl "${WORKDIR}/${base}.comp.msl")
    run(glslang "${GLSLANG}" -D -V -S comp -e cs_main "${standalone}" -o "${spirv}")
    run(glslang "${GLSLANG}" -D -V -S comp -e cs_main -DSLASH_MSL "${standalone}"
        -o "${msl_spirv}")
    run(spirv-cross "${SPIRV_CROSS}" --msl --msl-version 20100 --msl-decoration-binding
        --stage comp --entry cs_main "${msl_spirv}" --output "${msl}")
    set(wgsl "${WORKDIR}/${base}.comp.wgsl")
    run(tint "${TINT}" --input-format spirv --format wgsl "${spirv}" -o "${wgsl}")
    hex_array("${spirv}" bytes)
    file(READ "${msl}" msl_text)
    raw_literals("${msl_text}" msl msl_literals)
    # A compute shader's MSL and WGSL are always in pieces, since they are often too long for one.
    as_pieces("${msl_literals}" msl msl_literals)
    file(READ "${wgsl}" wgsl_text)
    raw_literals("${wgsl_text}" wgsl wgsl_literals)
    as_pieces("${wgsl_literals}" wgsl wgsl_literals)
    string(APPEND body
        "alignas(4) inline constexpr unsigned char k${NAME}ComputeSpirv[] = {\n${bytes}};\n\n"
        "inline constexpr const char* k${NAME}ComputeMsl[] = {\n${msl_literals}};\n\n"
        "inline constexpr const char* k${NAME}ComputeWgsl[] = {\n${wgsl_literals}};\n\n")
else()
    set(stages vert frag)
    if(NOT INTERFACE)
        set(INTERFACE "v_uv,v_color,v_world")
    endif()
    string(REPLACE "," ";" varyings "${INTERFACE}")
endif()
foreach(stage IN LISTS stages)
    if(stage STREQUAL "vert")
        set(entry vs_main)
        set(label Vertex)
    else()
        set(entry fs_main)
        set(label Fragment)
    endif()
    set(spirv "${WORKDIR}/${base}.${stage}.spv")
    set(msl "${WORKDIR}/${base}.${stage}.msl")
    set(glsl "${WORKDIR}/${base}.${stage}.glsl")
    run(glslang "${GLSLANG}" -D -V -S ${stage} -e ${entry} --auto-sampled-textures "${standalone}"
        -o "${spirv}")
    # Metal's texture, sampler and buffer indexes are the HLSL registers, as SDL binds them.
    run(spirv-cross "${SPIRV_CROSS}" --msl --msl-version 20100 --msl-decoration-binding
        --stage ${stage} --entry ${entry} "${spirv}" --output "${msl}")
    # GLSL ES 3.00 can't place varyings by location, so both stages name them alike to link,
    # and fragment shaders compute in full precision as the other backends do.
    if(stage STREQUAL "vert")
        set(io out)
    else()
        set(io in)
    endif()
    set(renames "")
    set(location 0)
    foreach(varying IN LISTS varyings)
        list(APPEND renames --rename-interface-variable ${io} ${location} ${varying})
        math(EXPR location "${location} + 1")
    endforeach()
    set(fixup "")
    if(FIXUP_CLIPSPACE AND stage STREQUAL "vert")
        set(fixup --fixup-clipspace)
    endif()
    run(spirv-cross "${SPIRV_CROSS}" --es --version 300 --stage ${stage} --entry ${entry}
        ${renames} ${fixup} "${spirv}" --output "${glsl}")
    set(wgsl_spirv "${WORKDIR}/${base}.${stage}.wgsl.spv")
    set(wgsl "${WORKDIR}/${base}.${stage}.wgsl")
    run(glslang "${GLSLANG}" -D -V -S ${stage} -e ${entry} --shift-sampler-binding 8
        "${standalone}" -o "${wgsl_spirv}")
    run(tint "${TINT}" --input-format spirv --format wgsl "${wgsl_spirv}" -o "${wgsl}")
    hex_array("${spirv}" bytes)
    file(READ "${msl}" msl_text)
    raw_literals("${msl_text}" msl msl_literals)
    file(READ "${glsl}" glsl_text)
    string(REPLACE "precision mediump float;" "precision highp float;" glsl_text "${glsl_text}")
    raw_literals("${glsl_text}" glsl glsl_literals)
    file(READ "${wgsl}" wgsl_text)
    raw_literals("${wgsl_text}" wgsl wgsl_literals)
    declare_code(k${NAME}${label}Msl "${msl_literals}" msl msl_code)
    declare_code(k${NAME}${label}Glsl "${glsl_literals}" glsl glsl_code)
    declare_code(k${NAME}${label}Wgsl "${wgsl_literals}" wgsl wgsl_code)
    string(APPEND body
        "alignas(4) inline constexpr unsigned char k${NAME}${label}Spirv[] = {\n${bytes}};\n\n"
        "${msl_code}${glsl_code}${wgsl_code}")
endforeach()
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
