add_library(slash_command
    src/builtin.cpp
    src/command.cpp
    src/live.cpp
    src/mcp.cpp
    src/rpc.cpp
    src/session.cpp)
add_library(slash::command ALIAS slash_command)

target_include_directories(slash_command PUBLIC include)
target_link_libraries(slash_command PUBLIC slash::asset slash::core slash::world
    PRIVATE slash::animation slash::canvas slash::physics2d slash::platform slash::render
            slash::storyboard slash::ui)
slash_engine_target(slash_command)
