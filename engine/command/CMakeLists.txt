add_library(slash_command
    src/builtin.cpp
    src/command.cpp
    src/cpp_outline.cpp
    src/lighting.cpp
    src/lightmap.cpp
    src/live.cpp
    src/mcp.cpp
    src/model_cleanup.cpp
    src/navigation.cpp
    src/npc.cpp
    src/perf.cpp
    src/place.cpp
    src/procedural.cpp
    src/physics_debug.cpp
    src/ragdoll.cpp
    src/character.cpp
    src/scatter.cpp
    src/hair.cpp
    src/playtest.cpp
    src/project_map.cpp
    src/reference.cpp
    src/replay.cpp
    src/rpc.cpp
    src/l10n.cpp
    src/l10n_check.cpp
    src/l10n_extract.cpp
    src/net.cpp
    src/face_capture.cpp
    src/xr.cpp
    src/services.cpp
    src/session.cpp
    src/style.cpp
    src/terrain.cpp
    src/test.cpp)
add_library(slash::command ALIAS slash_command)

target_include_directories(slash_command PUBLIC include)
target_link_libraries(slash_command PUBLIC slash::asset slash::core slash::world
    PRIVATE slash::ai slash::animation slash::audio slash::canvas slash::gameplay slash::input slash::l10n slash::navigation slash::net slash::npc slash::physics2d slash::physics3d slash::platform slash::plugin slash::procedural slash::render
            slash::scene3d slash::services slash::soundtrack slash::storyboard slash::ui slash::xr)
slash_engine_target(slash_command)
# terrain.import reads EXR heightmaps where the tools' tinyexr is built.
if(TARGET slash_tinyexr)
    target_link_libraries(slash_command PRIVATE slash_tinyexr)
    target_compile_definitions(slash_command PRIVATE SLASH_HAS_TINYEXR=1)
endif()
# Placing is the same everywhere, as the navigation geometry it reads, and so are
# lighting.check's measures and generated characters.
set_source_files_properties(src/place.cpp src/lighting.cpp src/character.cpp PROPERTIES
    COMPILE_OPTIONS "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-ffp-contract=off>")
