# The asset library: generated and added assets kept once per machine, the generators that make
# them and the team's library they share.
add_library(slash_library
    src/adapters.cpp
    src/generator.cpp
    src/library.cpp
    src/music.cpp
    src/search.cpp
    src/team.cpp
    src/team_server.cpp)
add_library(slash::library ALIAS slash_library)

target_include_directories(slash_library PUBLIC include)
target_link_libraries(slash_library
    PUBLIC slash::ai slash::core
    PRIVATE slash::audio slash::platform slash::render slash::scene3d slash_httplib)
slash_engine_target(slash_library)
# The test generator's placeholders, and images fitted to a size, are the same on every platform.
target_compile_options(slash_library PRIVATE $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-ffp-contract=off>)

# asset.search, asset.generate and the rest.
add_library(slash_library_commands src/commands.cpp)
add_library(slash::library_commands ALIAS slash_library_commands)

target_include_directories(slash_library_commands PUBLIC include)
target_link_libraries(slash_library_commands
    PUBLIC slash::command
    PRIVATE slash::canvas slash::library slash::platform slash::render slash::storyboard
            slash::world)
slash_engine_target(slash_library_commands)
