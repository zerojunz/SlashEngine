add_library(slash_command
    src/builtin.cpp
    src/command.cpp
    src/cpp_outline.cpp
    src/lightmap.cpp
    src/live.cpp
    src/mcp.cpp
    src/model_cleanup.cpp
    src/navigation.cpp
    src/perf.cpp
    src/place.cpp
    src/playtest.cpp
    src/project_map.cpp
    src/replay.cpp
    src/rpc.cpp
    src/l10n.cpp
    src/services.cpp
    src/session.cpp
    src/terrain.cpp
    src/test.cpp)
add_library(slash::command ALIAS slash_command)

target_include_directories(slash_command PUBLIC include)
target_link_libraries(slash_command PUBLIC slash::asset slash::core slash::world
    PRIVATE slash::ai slash::animation slash::audio slash::canvas slash::gameplay slash::input slash::l10n slash::navigation slash::physics2d slash::physics3d slash::platform slash::render
            slash::scene3d slash::services slash::soundtrack slash::storyboard slash::ui)
slash_engine_target(slash_command)
# Placing is the same everywhere, as the navigation geometry it reads.
set_source_files_properties(src/place.cpp PROPERTIES
    COMPILE_OPTIONS "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-ffp-contract=off>")
