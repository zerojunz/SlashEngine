# The benchmark of standard agent tasks (bench/), which counts the calls and tokens each costs
# over MCP. Used by slash-cli; games never link it.
add_library(slash_bench src/bench.cpp)
add_library(slash::bench ALIAS slash_bench)

target_include_directories(slash_bench PUBLIC include)
target_link_libraries(slash_bench
    PUBLIC slash::command slash::core
    PRIVATE slash::platform)
slash_target_warnings(slash_bench)
