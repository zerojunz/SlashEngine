add_library(slash_command
    src/builtin.cpp
    src/command.cpp
    src/cpp_outline.cpp
    src/live.cpp
    src/mcp.cpp
    src/navigation.cpp
    src/perf.cpp
    src/playtest.cpp
    src/project_map.cpp
    src/replay.cpp
    src/rpc.cpp
    src/services.cpp
    src/session.cpp
    src/test.cpp)
add_library(slash::command ALIAS slash_command)

target_include_directories(slash_command PUBLIC include)
target_link_libraries(slash_command PUBLIC slash::asset slash::core slash::world
    PRIVATE slash::ai slash::animation slash::audio slash::canvas slash::gameplay slash::input slash::navigation slash::physics2d slash::physics3d slash::platform slash::render
            slash::scene3d slash::services slash::soundtrack slash::storyboard slash::ui)
slash_engine_target(slash_command)
