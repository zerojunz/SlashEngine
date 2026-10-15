add_library(slash_input
    src/event.cpp
    src/sdl.cpp
    src/state.cpp)
add_library(slash::input ALIAS slash_input)

target_include_directories(slash_input PUBLIC include)
target_link_libraries(slash_input
    PUBLIC slash::core slash::world
    PRIVATE SDL3::SDL3-static)
slash_engine_target(slash_input)
