# Calling language models. Optional: only what links slash_ai carries the adapters and an HTTP
# client, so a game that calls no model has none of it.
add_library(slash_ai
    src/anthropic.cpp
    src/config.cpp
    src/gemini.cpp
    src/http_provider.cpp
    src/openai.cpp
    src/provider.cpp
    src/replay.cpp
    src/transport.cpp)
add_library(slash::ai ALIAS slash_ai)

target_include_directories(slash_ai PUBLIC include)
target_link_libraries(slash_ai
    PUBLIC slash::core
    PRIVATE slash::platform)
if(NOT EMSCRIPTEN)
    target_link_libraries(slash_ai PRIVATE slash_httplib)
endif()
slash_engine_target(slash_ai)

# The engine's commands as tools for a model, and the ai.* commands.
add_library(slash_ai_commands src/commands.cpp)
add_library(slash::ai_commands ALIAS slash_ai_commands)

target_link_libraries(slash_ai_commands
    PUBLIC slash::ai slash::command
    PRIVATE slash::platform)
slash_engine_target(slash_ai_commands)
