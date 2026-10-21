# Compressed textures (src/texture_codec.cpp) through bc7enc_rdo and astc-encoder. Both decode
# exactly as the formats specify, so pixels are the same on every platform. Games only decode,
# so their builds leave the encoders out; slash-cli, slash-mcp and the tests compress too, with
# slash::texture_encoder. astc-encoder is built without SIMD, so it makes the same blocks on
# every machine.
set(slash_texture_encoders OFF)
if(SLASH_BUILD_TOOLS OR SLASH_BUILD_TESTS)
    set(slash_texture_encoders ON)
endif()
add_library(slash_bc7 STATIC ${bc7enc_rdo_SOURCE_DIR}/bc7decomp.cpp)
if(slash_texture_encoders)
    target_sources(slash_bc7 PRIVATE ${bc7enc_rdo_SOURCE_DIR}/bc7enc.cpp)
endif()
target_include_directories(slash_bc7 SYSTEM PUBLIC ${bc7enc_rdo_SOURCE_DIR})
target_compile_options(slash_bc7 PRIVATE "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-w;-ffp-contract=off>")
set(slash_astc_sources "")
foreach(file averages_and_directions block_sizes color_quantize color_unquantize
        compress_symbolic compute_variance decompress_symbolic diagnostic_trace entry
        find_best_partitioning ideal_endpoints_and_weights image integer_sequence mathlib
        mathlib_softfloat partition_tables percentile_tables pick_best_endpoint_format
        quantization symbolic_physical weight_align weight_quant_xfer_tables)
    list(APPEND slash_astc_sources ${astc-encoder_SOURCE_DIR}/Source/astcenc_${file}.cpp)
endforeach()
add_library(slash_astcenc STATIC ${slash_astc_sources})
target_include_directories(slash_astcenc SYSTEM PUBLIC ${astc-encoder_SOURCE_DIR}/Source)
target_compile_definitions(slash_astcenc PRIVATE ASTCENC_SSE=0 ASTCENC_AVX=0 ASTCENC_NEON=0
    ASTCENC_SVE=0 ASTCENC_POPCNT=0 ASTCENC_F16C=0)
if(NOT slash_texture_encoders)
    target_compile_definitions(slash_astcenc PRIVATE ASTCENC_DECOMPRESS_ONLY)
endif()
target_compile_options(slash_astcenc PRIVATE
    "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-w;-ffp-contract=off>")

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
    src/texture_codec.cpp
    src/uniforms.cpp
    src/upscaler.cpp)
add_library(slash::render ALIAS slash_render)

target_include_directories(slash_render PUBLIC include)
target_link_libraries(slash_render
    PUBLIC slash::core
    PRIVATE slash_astcenc slash_bc7 SDL3::SDL3-static)
slash_engine_target(slash_render)

if(slash_texture_encoders)
    add_library(slash_texture_encoder STATIC src/texture_encoder.cpp)
    add_library(slash::texture_encoder ALIAS slash_texture_encoder)
    target_link_libraries(slash_texture_encoder PUBLIC slash::render PRIVATE slash_astcenc slash_bc7)
    slash_engine_target(slash_texture_encoder)
    set_source_files_properties(src/texture_encoder.cpp PROPERTIES
        COMPILE_OPTIONS "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-ffp-contract=off>")
endif()
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
