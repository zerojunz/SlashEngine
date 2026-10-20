add_library(slash_render
    src/device.cpp
    src/gl_device.cpp
    src/gpu_device.cpp
    src/hlsl_compiler.cpp
    src/image.cpp
    src/light.cpp
    src/material.cpp
    src/particle_layout.cpp
    src/particles.cpp
    src/shader.cpp
    src/shader_vm.cpp
    src/software_device.cpp
    src/uniforms.cpp
    src/upscaler.cpp)
add_library(slash::render ALIAS slash_render)

target_include_directories(slash_render PUBLIC include)
target_link_libraries(slash_render
    PUBLIC slash::core
    PRIVATE SDL3::SDL3-static)
slash_engine_target(slash_render)
# The software device draws the same pixels on every platform: no fused multiply-adds, which
# arm64 compilers would otherwise emit and x86-64 ones not.
set_source_files_properties(src/device.cpp src/particles.cpp src/shader_vm.cpp
    src/software_device.cpp src/upscaler.cpp PROPERTIES
    COMPILE_OPTIONS "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-ffp-contract=off>")

# Regenerates src/*_shaders.hpp from shaders/*.hlsl. The headers are committed, so only someone
# changing a shader needs glslang and SPIRV-Cross.
find_program(SLASH_GLSLANG NAMES glslangValidator glslang)
find_program(SLASH_SPIRV_CROSS NAMES spirv-cross)
set(slash_shader_headers "")
# Each shader as file:Name, with the compute shaders after the vertex and fragment pairs.
set(slash_shaders sprite:Sprite material:Material particles_process:ParticlesProcess:compute
    particles_draw:ParticlesDraw:compute)
foreach(entry IN LISTS slash_shaders)
    string(REPLACE ":" ";" parts ${entry})
    list(GET parts 0 shader)
    list(GET parts 1 name)
    list(LENGTH parts count)
    set(compute OFF)
    set(include canvas)
    if(count GREATER 2)
        set(compute ON)
        set(include particles)
    endif()
    set(header ${CMAKE_CURRENT_SOURCE_DIR}/src/${shader}_shaders.hpp)
    set(stamp ${CMAKE_CURRENT_BINARY_DIR}/shaders/${shader}.stamp)
    add_custom_command(
        OUTPUT ${stamp}
        COMMAND ${CMAKE_COMMAND}
            -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader}.hlsl -DOUTPUT=${header}
            -DNAME=${name} -DCOMPUTE=${compute} -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}/shaders
            -DGLSLANG=${SLASH_GLSLANG} -DSPIRV_CROSS=${SLASH_SPIRV_CROSS}
            -P ${PROJECT_SOURCE_DIR}/cmake/compile_shaders.cmake
        COMMAND ${CMAKE_COMMAND} -E touch ${stamp}
        DEPENDS shaders/${shader}.hlsl shaders/${include}.hlsl
            ${PROJECT_SOURCE_DIR}/cmake/compile_shaders.cmake
        COMMENT "Compiling ${shader}.hlsl"
        VERBATIM)
    list(APPEND slash_shader_headers ${stamp})
endforeach()
add_custom_target(shaders DEPENDS ${slash_shader_headers})
