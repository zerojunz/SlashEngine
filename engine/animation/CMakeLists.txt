add_library(slash_animation
    src/animation.cpp
    src/internal.cpp
    src/player.cpp
    src/tree.cpp
    src/tree_run.cpp
    src/tree_system.cpp)
add_library(slash::animation ALIAS slash_animation)

target_include_directories(slash_animation PUBLIC include)
target_link_libraries(slash_animation PUBLIC slash::asset slash::core slash::scene3d slash::world)
slash_engine_target(slash_animation)
# Blending results must be the same on every platform.
target_compile_options(slash_animation PRIVATE $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-ffp-contract=off>)
