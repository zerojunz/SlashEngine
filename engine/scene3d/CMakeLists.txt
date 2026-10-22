add_library(slash_scene3d
    src/primitive.cpp
    src/scene3d.cpp)
add_library(slash::scene3d ALIAS slash_scene3d)

target_include_directories(slash_scene3d PUBLIC include)
target_link_libraries(slash_scene3d PUBLIC slash::core slash::render slash::world)
slash_engine_target(slash_scene3d)
# Meshes and cameras are the same bits on every platform: no fused multiply-adds, which arm64
# compilers would otherwise emit and x86-64 ones not.
target_compile_options(slash_scene3d PRIVATE
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-ffp-contract=off>)
