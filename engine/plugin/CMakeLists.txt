add_library(slash_plugin src/components.cpp src/interface.cpp src/plugins.cpp)
add_library(slash::plugin ALIAS slash_plugin)

target_include_directories(slash_plugin PUBLIC include)
target_link_libraries(slash_plugin PUBLIC slash::core slash::world PRIVATE slash::platform
    slash::input)
slash_engine_target(slash_plugin)
