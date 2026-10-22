add_library(slash_physics3d
    src/character.cpp
    src/joints.cpp
    src/motion.cpp
    src/physics3d.cpp
    src/shapes.cpp
    src/space.cpp
    src/vehicle.cpp)
add_library(slash::physics3d ALIAS slash_physics3d)

target_include_directories(slash_physics3d PUBLIC include)
target_link_libraries(slash_physics3d
    PUBLIC slash::core slash::world
    PRIVATE Jolt::Jolt)
# The same results on every platform: no fused multiply-adds, as Jolt builds.
target_compile_options(slash_physics3d PRIVATE
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-ffp-contract=off>)
slash_engine_target(slash_physics3d)
