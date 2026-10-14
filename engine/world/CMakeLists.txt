add_library(slash_world
    src/component_registry.cpp
    src/components.cpp
    src/scene.cpp
    src/world.cpp)
add_library(slash::world ALIAS slash_world)

target_include_directories(slash_world PUBLIC include)
target_link_libraries(slash_world PUBLIC slash::core EnTT::EnTT)
slash_engine_target(slash_world)
