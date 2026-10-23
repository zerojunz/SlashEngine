add_library(slash_animation
    src/animation.cpp
    src/player.cpp)
add_library(slash::animation ALIAS slash_animation)

target_include_directories(slash_animation PUBLIC include)
target_link_libraries(slash_animation PUBLIC slash::asset slash::core slash::scene3d slash::world)
slash_engine_target(slash_animation)
