add_executable(slash-cli main.cpp)
target_link_libraries(slash-cli PRIVATE slash::core slash::platform slash::world)
slash_target_warnings(slash-cli)
