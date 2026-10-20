# Compiles a game's own HLSL shaders for every backend, and installs the compiler that
# render::compile_shader uses. Used by slash-cli, slash-mcp and exports; games never link it.
set(_slash_canvas_hlsl ${PROJECT_SOURCE_DIR}/engine/render/shaders/canvas.hlsl)
set(_slash_canvas_hlsl_cpp ${CMAKE_CURRENT_BINARY_DIR}/canvas_hlsl.cpp)
add_custom_command(
    OUTPUT ${_slash_canvas_hlsl_cpp}
    COMMAND ${CMAKE_COMMAND} -DINPUT=${_slash_canvas_hlsl} -DOUTPUT=${_slash_canvas_hlsl_cpp}
            -DNAMESPACE=slash::shader_compiler::detail -DNAME=kCanvasHlsl
            -P ${PROJECT_SOURCE_DIR}/cmake/embed.cmake
    DEPENDS ${_slash_canvas_hlsl} ${PROJECT_SOURCE_DIR}/cmake/embed.cmake
    COMMENT "Building canvas.hlsl into the shader compiler"
    VERBATIM)

add_library(slash_shader_compiler
    src/shader_compiler.cpp
    ${_slash_canvas_hlsl_cpp})
add_library(slash::shader_compiler ALIAS slash_shader_compiler)

target_include_directories(slash_shader_compiler PUBLIC include)
target_link_libraries(slash_shader_compiler
    PUBLIC slash::core slash::render
    PRIVATE glslang::glslang glslang::glslang-default-resource-limits glslang::SPIRV
            spirv-cross-core spirv-cross-glsl spirv-cross-msl)
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
