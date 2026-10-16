add_library(slash_world
    src/component_registry.cpp
    src/components.cpp
    src/scene.cpp
    src/tree2d.cpp
    src/world.cpp)
add_library(slash::world ALIAS slash_world)

target_include_directories(slash_world PUBLIC include)
target_link_libraries(slash_world PUBLIC slash::core EnTT::EnTT)
# The engine builds without exceptions but tools and tests may use them. Every user of EnTT must
# agree on its exception mode, or MSVC refuses to link ('entt.noexcept' mismatch).
target_compile_definitions(slash_world PUBLIC ENTT_NOEXCEPTION)
# Global transforms feed physics, so they give the same bits everywhere: no fused multiply-adds.
set_source_files_properties(src/tree2d.cpp PROPERTIES
    COMPILE_OPTIONS "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-ffp-contract=off>")
slash_engine_target(slash_world)
