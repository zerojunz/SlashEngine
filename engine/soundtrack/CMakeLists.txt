# The soundtrack: scores as text (.score.json) and Standard MIDI Files.
add_library(slash_soundtrack
    src/midi.cpp
    src/score.cpp)
add_library(slash::soundtrack ALIAS slash_soundtrack)

target_include_directories(slash_soundtrack PUBLIC include)
target_link_libraries(slash_soundtrack PUBLIC slash::core)
slash_engine_target(slash_soundtrack)

# score.import and score.export.
add_library(slash_soundtrack_commands src/commands.cpp)
add_library(slash::soundtrack_commands ALIAS slash_soundtrack_commands)

target_include_directories(slash_soundtrack_commands PUBLIC include)
target_link_libraries(slash_soundtrack_commands
    PUBLIC slash::command
    PRIVATE slash::platform slash::soundtrack)
slash_engine_target(slash_soundtrack_commands)
