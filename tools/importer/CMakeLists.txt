# Converts games made with other engines into SlashEngine projects, and the project.import
# command. Used by slash-cli and slash-mcp; games never link it.

# The script Unreal's editor runs to export a project (cmake/embed.cmake).
set(_slash_unreal_export_py ${CMAKE_CURRENT_SOURCE_DIR}/src/unreal_export.py)
set(_slash_unreal_export_cpp ${CMAKE_CURRENT_BINARY_DIR}/unreal_export_py.cpp)
add_custom_command(
    OUTPUT ${_slash_unreal_export_cpp}
    COMMAND ${CMAKE_COMMAND} -DINPUT=${_slash_unreal_export_py} -DOUTPUT=${_slash_unreal_export_cpp}
            -DNAMESPACE=slash::importer::detail -DNAME=kUnrealExportPy
            -P ${PROJECT_SOURCE_DIR}/cmake/embed.cmake
    DEPENDS ${_slash_unreal_export_py} ${PROJECT_SOURCE_DIR}/cmake/embed.cmake
    COMMENT "Building unreal_export.py into the importer"
    VERBATIM)

add_library(slash_importer
    src/commands.cpp
    src/common.cpp
    src/compression.cpp
    src/gdscript_tokens.cpp
    src/godot_binary.cpp
    src/godot_import.cpp
    src/godot_pack.cpp
    src/godot_terrain.cpp
    src/godot_text.cpp
    src/godot_unpack.cpp
    src/import.cpp
    src/scripts.cpp
    src/skeletons.cpp
    src/unity_build.cpp
    src/unity_import.cpp
    src/unity_yaml.cpp
    src/unreal_cpp.cpp
    src/unreal_editor.cpp
    src/unreal_import.cpp
    src/unreal_text.cpp
    ${_slash_unreal_export_cpp})
add_library(slash::importer ALIAS slash_importer)

target_include_directories(slash_importer PUBLIC include)
target_link_libraries(slash_importer
    PUBLIC slash::command slash::core
    PRIVATE slash::animation slash::audio slash::canvas slash::exporter slash::navigation
            slash::platform slash::render slash::runtime slash::scene3d slash::storyboard slash::ui
            slash::world slash_brotli slash_httplib slash_zstd webpdecoder)
slash_target_warnings(slash_importer)
