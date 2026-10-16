# Converts games made with other engines into SlashEngine projects, and the project.import
# command. Used by slash-cli and slash-mcp; games never link it.
add_library(slash_importer
    src/commands.cpp
    src/godot_import.cpp
    src/godot_text.cpp
    src/import.cpp)
add_library(slash::importer ALIAS slash_importer)

target_include_directories(slash_importer PUBLIC include)
target_link_libraries(slash_importer
    PUBLIC slash::command slash::core
    PRIVATE slash::platform slash::runtime slash::storyboard slash::world)
slash_target_warnings(slash_importer)
