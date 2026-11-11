add_library(slash_npc
    src/brain.cpp
    src/npc.cpp
    src/story.cpp
    src/story_check.cpp
    src/story_world.cpp)
add_library(slash::npc ALIAS slash_npc)

target_include_directories(slash_npc PUBLIC include)
target_link_libraries(slash_npc PUBLIC slash::core slash::world slash::asset)
target_link_libraries(slash_npc PRIVATE slash::l10n slash::navigation)
# Agents walk the same way on every platform: no fused multiply-adds.
target_compile_options(slash_npc PRIVATE "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-ffp-contract=off>")
slash_engine_target(slash_npc)
