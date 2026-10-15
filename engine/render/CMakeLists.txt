add_library(slash_render
    src/device.cpp
    src/gpu_device.cpp
    src/hlsl_compiler.cpp)
add_library(slash::render ALIAS slash_render)

target_include_directories(slash_render PUBLIC include)
target_link_libraries(slash_render
    PUBLIC slash::core
    PRIVATE SDL3::SDL3-static)
slash_engine_target(slash_render)

# Regenerates src/*_shaders.hpp from shaders/*.hlsl. The headers are committed, so only someone
# changing a shader needs glslang and SPIRV-Cross.
find_program(SLASH_GLSLANG NAMES glslangValidator glslang)
find_program(SLASH_SPIRV_CROSS NAMES spirv-cross)
set(slash_shader_headers "")
foreach(shader IN ITEMS sprite)
    set(header ${CMAKE_CURRENT_SOURCE_DIR}/src/${shader}_shaders.hpp)
    set(stamp ${CMAKE_CURRENT_BINARY_DIR}/shaders/${shader}.stamp)
    string(SUBSTRING ${shader} 0 1 first)
    string(TOUPPER ${first} first)
    string(SUBSTRING ${shader} 1 -1 rest)
    add_custom_command(
        OUTPUT ${stamp}
        COMMAND ${CMAKE_COMMAND}
            -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader}.hlsl -DOUTPUT=${header}
            -DNAME=${first}${rest} -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}/shaders
            -DGLSLANG=${SLASH_GLSLANG} -DSPIRV_CROSS=${SLASH_SPIRV_CROSS}
            -P ${PROJECT_SOURCE_DIR}/cmake/compile_shaders.cmake
        COMMAND ${CMAKE_COMMAND} -E touch ${stamp}
        DEPENDS shaders/${shader}.hlsl ${PROJECT_SOURCE_DIR}/cmake/compile_shaders.cmake
        COMMENT "Compiling ${shader}.hlsl"
        VERBATIM)
    list(APPEND slash_shader_headers ${stamp})
endforeach()
add_custom_target(shaders DEPENDS ${slash_shader_headers})
