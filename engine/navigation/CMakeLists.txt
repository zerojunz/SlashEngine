add_library(slash_navigation
    src/bake_2d.cpp
    src/bake_3d.cpp
    src/core.cpp
    src/files.cpp
    src/navigation.cpp
    src/rvo2_2d.cpp
    src/rvo2_3d.cpp
    src/sources.cpp)
add_library(slash::navigation ALIAS slash_navigation)

target_include_directories(slash_navigation PUBLIC include)
target_link_libraries(slash_navigation PUBLIC slash::core slash::world slash::asset)
target_link_libraries(slash_navigation PRIVATE slash::canvas slash::scene3d slash::physics3d
    slash_recast slash_clipper2 slash_polypartition)
# The same paths on every platform: no fused multiply-adds.
target_compile_options(slash_navigation PRIVATE
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-ffp-contract=off>)
slash_engine_target(slash_navigation)
