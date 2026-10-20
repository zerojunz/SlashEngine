add_library(slash_services
    src/services.cpp
    src/settings.cpp)
add_library(slash::services ALIAS slash_services)

target_include_directories(slash_services PUBLIC include)
target_link_libraries(slash_services
    PUBLIC slash::core slash::world
    PRIVATE slash::platform)
slash_engine_target(slash_services)
