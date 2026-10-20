# The asset library: generated and added assets kept once per machine, and the generators that
# make them.
add_library(slash_library src/generator.cpp src/library.cpp src/search.cpp)
add_library(slash::library ALIAS slash_library)

target_include_directories(slash_library PUBLIC include)
target_link_libraries(slash_library
    PUBLIC slash::ai slash::core
    PRIVATE slash::audio slash::platform slash::render)
slash_engine_target(slash_library)
# The test generator's placeholders are the same on every platform.
target_compile_options(slash_library PRIVATE $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-ffp-contract=off>)

# asset.search, asset.generate and the rest.
add_library(slash_library_commands src/commands.cpp)
add_library(slash::library_commands ALIAS slash_library_commands)

target_include_directories(slash_library_commands PUBLIC include)
target_link_libraries(slash_library_commands
    PUBLIC slash::command
    PRIVATE slash::library slash::platform)
slash_engine_target(slash_library_commands)
