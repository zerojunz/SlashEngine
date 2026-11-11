add_library(slash_world
    src/audio_tracks.cpp
    src/camera2d.cpp
    src/canvas_layer.cpp
    src/cloth2d.cpp
    src/component_registry.cpp
    src/foliage.cpp
    src/components.cpp
    src/game_module.cpp
    src/hair.cpp
    src/character.cpp
    src/creature.cpp
    src/flesh.cpp
    src/flesh2d.cpp
    src/flesh_solver.cpp
    src/legend.cpp
    src/path.cpp
    src/physical_bones.cpp
    src/physics_cache.cpp
    src/physics_debug.cpp
    src/physics_field.cpp
    src/physics_material.cpp
    src/ragdoll.cpp
    src/scene.cpp
    src/tree2d.cpp
    src/tree3d.cpp
    src/vehicle.cpp
    src/wind.cpp
    src/world.cpp)
add_library(slash::world ALIAS slash_world)

target_include_directories(slash_world PUBLIC include)
target_link_libraries(slash_world PUBLIC slash::core EnTT::EnTT PRIVATE slash::asset)
# The engine builds without exceptions but tools and tests may use them. Every user of EnTT must
# agree on its exception mode, or MSVC refuses to link ('entt.noexcept' mismatch).
target_compile_definitions(slash_world PUBLIC ENTT_NOEXCEPTION)
# Global transforms and the wind feed physics, and camera views, canvas layers and paths drawing
# and sound, so they give the same bits everywhere: no fused multiply-adds.
set_source_files_properties(src/camera2d.cpp src/canvas_layer.cpp src/path.cpp src/tree2d.cpp
    src/tree3d.cpp src/wind.cpp src/physics_field.cpp src/cloth2d.cpp src/hair.cpp src/character.cpp src/creature.cpp
    src/vehicle.cpp src/flesh.cpp src/flesh2d.cpp src/flesh_solver.cpp
    PROPERTIES
    COMPILE_OPTIONS "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-ffp-contract=off>")
slash_engine_target(slash_world)
