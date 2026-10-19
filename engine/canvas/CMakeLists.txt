add_library(slash_canvas
    src/canvas.cpp
    src/capture.cpp
    src/debug_draw.cpp
    src/gpu_particles.cpp
    src/light.cpp
    src/material.cpp
    src/particles.cpp
    src/process_material.cpp
    src/renderer.cpp
    src/sprite_frames.cpp
    src/summary.cpp
    src/tilemap.cpp)
add_library(slash::canvas ALIAS slash_canvas)

target_include_directories(slash_canvas PUBLIC include)
target_link_libraries(slash_canvas
    PUBLIC slash::asset slash::core slash::physics2d slash::render slash::ui slash::world)
slash_engine_target(slash_canvas)
# Particles move the same on every platform: no fused multiply-adds, which arm64 compilers
# would otherwise emit and x86-64 ones not.
target_compile_options(slash_canvas PRIVATE
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-ffp-contract=off>)
