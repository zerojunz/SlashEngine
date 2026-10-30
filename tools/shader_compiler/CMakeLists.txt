# Compiles a game's own HLSL shaders for every backend, with glslang, SPIRV-Cross and Tint, and
# installs the compiler that render::compile_shader uses. Used by slash-cli, slash-mcp and exports;
# games never link it.

# The engine's shaders a game's are written into: canvas.hlsl for canvas shaders, mesh.hlsl and the
# tonemap.hlsl it includes for spatial ones.
set(_slash_embedded_hlsl "")
foreach(entry IN ITEMS canvas:kCanvasHlsl mesh:kMeshHlsl tonemap:kTonemapHlsl)
    string(REPLACE ":" ";" parts ${entry})
    list(GET parts 0 shader)
    list(GET parts 1 name)
    set(input ${PROJECT_SOURCE_DIR}/engine/render/shaders/${shader}.hlsl)
    set(output ${CMAKE_CURRENT_BINARY_DIR}/${shader}_hlsl.cpp)
    add_custom_command(
        OUTPUT ${output}
        COMMAND ${CMAKE_COMMAND} -DINPUT=${input} -DOUTPUT=${output}
                -DNAMESPACE=slash::shader_compiler::detail -DNAME=${name}
                -P ${PROJECT_SOURCE_DIR}/cmake/embed.cmake
        DEPENDS ${input} ${PROJECT_SOURCE_DIR}/cmake/embed.cmake
        COMMENT "Building ${shader}.hlsl into the shader compiler"
        VERBATIM)
    list(APPEND _slash_embedded_hlsl ${output})
endforeach()

add_library(slash_shader_compiler
    src/shader_compiler.cpp
    ${_slash_embedded_hlsl})
add_library(slash::shader_compiler ALIAS slash_shader_compiler)

target_include_directories(slash_shader_compiler PUBLIC include)
target_link_libraries(slash_shader_compiler
    PUBLIC slash::core slash::render
    PRIVATE glslang::glslang glslang::glslang-default-resource-limits glslang::SPIRV
            spirv-cross-core spirv-cross-glsl spirv-cross-msl tint_api)
slash_target_warnings(slash_shader_compiler)

# Compiles the canvas shaders the engine builds in, written as a game's are, into a header
# engine/render commits (src/shader_header.cpp). The shaders target runs it, beside the HLSL
# shaders' own compiles, and tests/test_render.cpp checks the header against the shaders.
add_executable(slash-shader-header src/shader_header.cpp)
target_link_libraries(slash-shader-header PRIVATE slash::platform slash::shader_compiler)
slash_target_warnings(slash-shader-header)
add_custom_target(fsr_shaders
    COMMAND slash-shader-header ${PROJECT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/engine/render/src/fsr_shaders.hpp slash::render::shaders
        fsr_easu=engine/render/shaders/fsr_easu.hlsl
        fsr_rcas=engine/render/shaders/fsr_rcas.hlsl:sharpness/1/0
    COMMENT "Compiling the FSR shaders"
    VERBATIM)
add_dependencies(shaders fsr_shaders)

# Compiles the GLSL compute shaders that trace rays into the MSL the GPU device runs on Metal and
# the HLSL it runs on Direct3D 12 (as DXIL, cmake/compile_ray_dxil.cmake), into headers
# engine/render commits (src/ray_msl.cpp). tests/test_ray_tracing.cpp checks each
# header against its shader.
add_executable(slash-ray-msl src/ray_msl.cpp)
target_link_libraries(slash-ray-msl PRIVATE glslang::glslang
    glslang::glslang-default-resource-limits glslang::SPIRV spirv-cross-core spirv-cross-msl
    spirv-cross-hlsl)
slash_target_warnings(slash-ray-msl)
set(_slash_ray_msl_commands "")
# Each with its passes, the values of its specialization constant kPass.
foreach(entry IN ITEMS ray_shadows:RayShadows:1 ray_light:RayLight:5 lightmap_light:LightmapLight:3)
    string(REPLACE ":" ";" parts ${entry})
    list(GET parts 0 shader)
    list(GET parts 1 name)
    list(GET parts 2 passes)
    list(APPEND _slash_ray_msl_commands
        COMMAND slash-ray-msl ${PROJECT_SOURCE_DIR}/engine/render/shaders/${shader}.comp
            ${PROJECT_SOURCE_DIR}/engine/render/src/${shader}_msl.hpp ${name}
        COMMAND slash-ray-msl --hlsl ${PROJECT_SOURCE_DIR}/engine/render/shaders/${shader}.comp
            ${PROJECT_SOURCE_DIR}/engine/render/src/${shader}_hlsl.hpp ${name} ${passes})
endforeach()
add_custom_target(ray_msl_shaders ${_slash_ray_msl_commands}
    COMMENT "Compiling the ray shaders' MSL and HLSL"
    VERBATIM)
add_dependencies(shaders ray_msl_shaders)
