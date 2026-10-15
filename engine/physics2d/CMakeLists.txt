add_library(slash_physics2d
    src/character.cpp
    src/physics2d.cpp
    src/polygon.cpp
    src/space.cpp)
add_library(slash::physics2d ALIAS slash_physics2d)

target_include_directories(slash_physics2d PUBLIC include)
target_link_libraries(slash_physics2d
    PUBLIC slash::core slash::world
    PRIVATE box2d::box2d)
# The same results on every platform: no fused multiply-adds, which arm64 compilers would
# otherwise emit and x86-64 ones not (Box2D builds this way too).
target_compile_options(slash_physics2d PRIVATE
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-ffp-contract=off>)
slash_engine_target(slash_physics2d)
