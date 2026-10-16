add_library(slash_debug
    src/overlay.cpp)
add_library(slash::debug ALIAS slash_debug)

target_include_directories(slash_debug PUBLIC include)
target_link_libraries(slash_debug
    PUBLIC slash::asset slash::canvas slash::command slash::core slash::input slash::world
    PRIVATE slash::physics2d slash::ui)
slash_engine_target(slash_debug)
