# Exports games: presets, building with export/CMakeLists.txt, signing, and the export.run
# command. Used by slash-cli and slash-mcp; games never link it.
add_library(slash_exporter
    src/android.cpp
    src/commands.cpp
    src/export.cpp
    src/presets.cpp
    src/steps.cpp)
add_library(slash::exporter ALIAS slash_exporter)

target_include_directories(slash_exporter PUBLIC include)
target_link_libraries(slash_exporter
    PUBLIC slash::command slash::core
    PRIVATE slash::platform slash::runtime slash::world)
# Where exports find the engine sources when SLASH_ENGINE_DIR and an installed copy are absent.
target_compile_definitions(slash_exporter PRIVATE SLASH_ENGINE_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
slash_target_warnings(slash_exporter)
