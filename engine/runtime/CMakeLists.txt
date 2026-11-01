add_library(slash_runtime
    src/game.cpp
    src/gameplay.cpp
    src/icon.cpp
    src/project.cpp)
add_library(slash::runtime ALIAS slash_runtime)

target_include_directories(slash_runtime PUBLIC include)
target_link_libraries(slash_runtime
    PUBLIC slash::animation slash::asset slash::audio slash::canvas slash::core slash::gameplay slash::input slash::l10n slash::navigation slash::net slash::physics2d slash::physics3d slash::platform slash::render
           slash::services slash::ui slash::world slash::xr
    PRIVATE SDL3::SDL3-static)
slash_engine_target(slash_runtime)
# The engine's icon is the same pixels on every machine.
set_source_files_properties(src/icon.cpp PROPERTIES
    COMPILE_OPTIONS "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-ffp-contract=off>")

# The main() of a game executable, through SDL's main callbacks. It is compiled into each game
# rather than into a library, because a linker would drop the callbacks from a static library.
add_library(slash_game_main INTERFACE)
add_library(slash::game_main ALIAS slash_game_main)
target_sources(slash_game_main INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp)
target_link_libraries(slash_game_main INTERFACE slash::runtime SDL3::SDL3-static)
