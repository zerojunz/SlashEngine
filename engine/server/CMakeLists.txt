add_library(slash_server
    src/client.cpp
    src/openapi.cpp
    src/server.cpp)
add_library(slash::server ALIAS slash_server)

target_include_directories(slash_server PUBLIC include)
target_link_libraries(slash_server
    PUBLIC slash::command slash::core
    PRIVATE slash::platform slash_httplib)
slash_engine_target(slash_server)
