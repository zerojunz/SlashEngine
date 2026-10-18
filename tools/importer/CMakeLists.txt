# Converts games made with other engines into SlashEngine projects, and the project.import
# command. Used by slash-cli and slash-mcp; games never link it.
add_library(slash_importer
    src/commands.cpp
    src/common.cpp
    src/gdscript_tokens.cpp
    src/godot_binary.cpp
    src/godot_import.cpp
    src/godot_pack.cpp
    src/godot_text.cpp
    src/godot_unpack.cpp
    src/import.cpp
    src/unity_import.cpp
    src/unity_yaml.cpp)
add_library(slash::importer ALIAS slash_importer)

target_include_directories(slash_importer PUBLIC include)
target_link_libraries(slash_importer
    PUBLIC slash::command slash::core
    PRIVATE slash::animation slash::audio slash::canvas slash::platform slash::render slash::runtime
            slash::storyboard slash::ui slash::world slash_zstd webpdecoder)
slash_target_warnings(slash_importer)
