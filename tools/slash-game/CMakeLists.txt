# Runs a game project the way its exports do, from its folder: slash-game --data path/to/game
add_executable(slash-game)
target_link_libraries(slash-game PRIVATE slash::game_main)
slash_target_warnings(slash-game)
