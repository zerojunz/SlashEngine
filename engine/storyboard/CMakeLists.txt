add_library(slash_storyboard
    src/storyboard.cpp)
add_library(slash::storyboard ALIAS slash_storyboard)

target_include_directories(slash_storyboard PUBLIC include)
target_link_libraries(slash_storyboard PUBLIC slash::core)
slash_engine_target(slash_storyboard)
