# The soundtrack: scores as text (.score.json), Standard MIDI Files, and drafting and arranging
# cue sheets.
add_library(slash_soundtrack
    src/arrange.cpp
    src/midi.cpp
    src/plan.cpp
    src/score.cpp)
add_library(slash::soundtrack ALIAS slash_soundtrack)

target_include_directories(slash_soundtrack PUBLIC include)
target_link_libraries(slash_soundtrack PUBLIC slash::core PRIVATE slash::storyboard)
slash_engine_target(slash_soundtrack)

# score.import, score.export and the soundtrack.* commands, for the tools and tests: playing scores needs
# TinySoundFont and libvorbis, which only their builds fetch (cmake/Dependencies.cmake).
if(TARGET slash_vorbis)
    add_library(slash_tsf STATIC src/tsf.c)
    target_include_directories(slash_tsf SYSTEM PUBLIC ${TinySoundFont_SOURCE_DIR})
    target_compile_options(slash_tsf PRIVATE
        "$<$<NOT:$<C_COMPILER_ID:MSVC>>:-ffp-contract=off;-w;-O2>" "$<$<C_COMPILER_ID:MSVC>:/W0>")
    if(UNIX AND NOT APPLE)
        target_link_libraries(slash_tsf PRIVATE m)
    endif()

    add_library(slash_soundtrack_commands
        src/commands.cpp
        src/render.cpp
        src/sound_bank.cpp)
    add_library(slash::soundtrack_commands ALIAS slash_soundtrack_commands)

    target_include_directories(slash_soundtrack_commands PUBLIC include)
    target_link_libraries(slash_soundtrack_commands
        PUBLIC slash::command slash::soundtrack
        PRIVATE slash::platform slash::storyboard slash_tsf slash_vorbis)
    slash_engine_target(slash_soundtrack_commands)
    # The same sound bank and the same notes on every platform: no fused multiply-adds.
    target_compile_options(slash_soundtrack_commands PRIVATE
        $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-ffp-contract=off>)
endif()
