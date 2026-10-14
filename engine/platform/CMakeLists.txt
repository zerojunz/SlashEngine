add_library(slash_platform
    src/platform.cpp
    src/run.cpp)
add_library(slash::platform ALIAS slash_platform)

target_include_directories(slash_platform PUBLIC include)
target_link_libraries(slash_platform
    PUBLIC slash::core
    PRIVATE SDL3::SDL3-static)
slash_engine_target(slash_platform)
