add_executable(slash-mcp main.cpp)
target_link_libraries(slash-mcp PRIVATE slash::command slash::platform slash::server slash::world)
slash_target_warnings(slash-mcp)
