# Runs a game project the way its exports do, from its folder: slash-game --data path/to/game
add_executable(slash-game)
target_link_libraries(slash-game PRIVATE slash::game_main slash::ai)
# Any project may have a providers.json, so main() installs engine/ai.
target_compile_definitions(slash-game PRIVATE SLASH_GAME_AI)
slash_target_warnings(slash-game)
slash_gameplay_host(slash-game)
