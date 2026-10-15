add_library(slash_canvas
    src/canvas.cpp
    src/renderer.cpp
    src/sprite_frames.cpp)
add_library(slash::canvas ALIAS slash_canvas)

target_include_directories(slash_canvas PUBLIC include)
target_link_libraries(slash_canvas PUBLIC slash::asset slash::core slash::render slash::world)
slash_engine_target(slash_canvas)
