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

# JPEG textures and 16-bit heightmaps through stb_image and stb_image_write (src/image_codecs.c),
# with no SIMD, so a file decodes and encodes to the same bytes everywhere.
add_library(slash_image_codecs STATIC src/image_codecs.c)
target_include_directories(slash_image_codecs SYSTEM PRIVATE ${stb_SOURCE_DIR})
target_compile_options(slash_image_codecs PRIVATE
    "$<$<NOT:$<C_COMPILER_ID:MSVC>>:-w;-ffp-contract=off>")

add_library(slash_render
    src/clusters.cpp
    src/device.cpp
    src/forward_post.cpp
    src/gl_device.cpp
    src/gpu_device.cpp
    src/hlsl_compiler.cpp
    src/image.cpp
    src/light.cpp
    src/material.cpp
    src/mesh.cpp
    src/particle_layout.cpp
    src/particles.cpp
    src/post.cpp
    src/resize.cpp
    src/shader.cpp
    src/shader_vm.cpp
    src/shadow.cpp
    src/sky.cpp
    src/software_device.cpp
    src/ssao.cpp
    src/texture_codec.cpp
    src/uniforms.cpp
    src/upscaler.cpp)
add_library(slash::render ALIAS slash_render)

target_include_directories(slash_render PUBLIC include)
target_link_libraries(slash_render
    PUBLIC slash::core
    PRIVATE slash_astcenc slash_bc7 slash_image_codecs SDL3::SDL3-static)
slash_engine_target(slash_render)

# Web builds draw with WebGPU where the browser has it, through Dawn's webgpu.h for Emscripten
# (emdawnwebgpu), which Emscripten fetches as a port pinned to its version. web/webgpu.js asks the
# browser for the device before main() runs.
if(EMSCRIPTEN)
    target_sources(slash_render PRIVATE src/webgpu_device.cpp)
    target_compile_options(slash_render PRIVATE --use-port=emdawnwebgpu)
    target_link_options(slash_render INTERFACE --use-port=emdawnwebgpu
        "SHELL:--pre-js ${CMAKE_CURRENT_SOURCE_DIR}/web/webgpu.js")
endif()

if(slash_texture_encoders)
    add_library(slash_texture_encoder STATIC src/texture_encoder.cpp)
    add_library(slash::texture_encoder ALIAS slash_texture_encoder)
    target_link_libraries(slash_texture_encoder PUBLIC slash::render PRIVATE slash_astcenc slash_bc7)
    slash_engine_target(slash_texture_encoder)
    set_source_files_properties(src/texture_encoder.cpp PROPERTIES
        COMPILE_OPTIONS "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-ffp-contract=off>")
endif()
# The software device draws the same pixels, and particles and their graphs move the same, on
# every platform: no fused multiply-adds, which arm64 compilers would otherwise emit and x86-64
# ones not.
set_source_files_properties(src/clusters.cpp src/device.cpp src/forward_post.cpp src/material.cpp
    src/mesh.cpp src/particles.cpp src/post.cpp src/resize.cpp src/shader_vm.cpp src/shadow.cpp
    src/sky.cpp src/software_device.cpp src/ssao.cpp src/upscaler.cpp
    PROPERTIES
    COMPILE_OPTIONS "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-ffp-contract=off>")

# Regenerates src/*_shaders.hpp from shaders/*.hlsl. The headers are committed, so only someone
# changing a shader needs glslang, SPIRV-Cross and Tint.
find_program(SLASH_GLSLANG NAMES glslangValidator glslang)
find_program(SLASH_SPIRV_CROSS NAMES spirv-cross)
find_program(SLASH_TINT NAMES tint)
set(slash_shader_headers "")
# Each shader as file:Name, then compute for compute shaders, and 3d for the mesh shader and
# depth for the shadow pass shader, whose varyings GLSL names apart from the canvas shaders' and
# whose depth it maps to OpenGL's, and post for the post passes, the sky and the radiance filter,
# which have no varyings. The mesh shader's code is long enough to come in pieces, and its fifteen
# textures put its WGSL samplers 16 bindings after them instead of 8, as the thirteen textures of
# the Forward+ post passes (post16) do. clustered is the mesh shader's Forward+ fragment stage,
# which reads storage buffers, and normal its fragment stage for Forward+'s normal-roughness
# prepass. The SSAO passes (ssao) are post passes with twelve textures whose code comes in pieces.
set(slash_shaders sprite:Sprite material:Material mesh:Mesh:3d
    mesh_clustered:MeshClustered:clustered mesh_normal:MeshNormal:normal shadow:Shadow:depth
    post:Post:post sky:Sky:post forward_post:ForwardPost:post16 ssao:Ssao:ssao
    radiance:Radiance:post
    particles_process:ParticlesProcess:compute
    particles_emit:ParticlesEmit:compute particles_draw:ParticlesDraw:compute)
foreach(entry IN LISTS slash_shaders)
    string(REPLACE ":" ";" parts ${entry})
    list(GET parts 0 shader)
    list(GET parts 1 name)
    list(LENGTH parts count)
    set(compute OFF)
    set(include canvas)
    set(interface v_uv,v_color,v_world)
    set(fixup OFF)
    set(pieces OFF)
    set(sampler_shift 8)
    set(fragment_only OFF)
    set(storage_buffers OFF)
    if(count GREATER 2)
        list(GET parts 2 kind)
        if(kind STREQUAL "compute")
            set(compute ON)
            set(include particles)
        elseif(kind STREQUAL "depth")
            set(include shadow)
            set(interface v_uv,v_color)
            set(fixup ON)
        elseif(kind STREQUAL "post" OR kind STREQUAL "post16" OR kind STREQUAL "ssao")
            set(include tonemap)
            set(interface v_uv)
            if(NOT kind STREQUAL "post")
                set(sampler_shift 16)
            endif()
            if(kind STREQUAL "ssao")
                set(pieces ON)
            endif()
        else()
            set(include mesh)
            set(interface v_world,v_normal,v_tangent,v_binormal,v_uv,v_color)
            set(fixup ON)
            set(pieces ON)
            set(sampler_shift 16)
            if(kind STREQUAL "clustered")
                set(fragment_only ON)
                set(storage_buffers ON)
            elseif(kind STREQUAL "normal")
                set(fragment_only ON)
            endif()
        endif()
    endif()
    set(header ${CMAKE_CURRENT_SOURCE_DIR}/src/${shader}_shaders.hpp)
    set(stamp ${CMAKE_CURRENT_BINARY_DIR}/shaders/${shader}.stamp)
    add_custom_command(
        OUTPUT ${stamp}
        COMMAND ${CMAKE_COMMAND}
            -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader}.hlsl -DOUTPUT=${header}
            -DNAME=${name} -DCOMPUTE=${compute} -DINTERFACE=${interface}
            -DFIXUP_CLIPSPACE=${fixup} -DPIECES=${pieces} -DSAMPLER_SHIFT=${sampler_shift}
            -DFRAGMENT_ONLY=${fragment_only} -DSTORAGE_BUFFERS=${storage_buffers}
            -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}/shaders
            -DGLSLANG=${SLASH_GLSLANG} -DSPIRV_CROSS=${SLASH_SPIRV_CROSS} -DTINT=${SLASH_TINT}
            -P ${PROJECT_SOURCE_DIR}/cmake/compile_shaders.cmake
        COMMAND ${CMAKE_COMMAND} -E touch ${stamp}
        DEPENDS shaders/${shader}.hlsl shaders/${include}.hlsl shaders/material_ops.hlsl
            shaders/tonemap.hlsl
            ${PROJECT_SOURCE_DIR}/cmake/compile_shaders.cmake
        COMMENT "Compiling ${shader}.hlsl"
        VERBATIM)
    list(APPEND slash_shader_headers ${stamp})
endforeach()
add_custom_target(shaders DEPENDS ${slash_shader_headers})
