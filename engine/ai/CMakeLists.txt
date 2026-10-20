# Calling language models. Exported games link it, but only the parts a game calls end up in it:
# main() installs it for a game with a providers.json (SLASH_GAME_AI), so one without has no
# adapters and no HTTP client.
add_library(slash_ai
    src/anthropic.cpp
    src/config.cpp
    src/gemini.cpp
    src/http_provider.cpp
    src/openai.cpp
    src/provider.cpp
    src/replay.cpp
    src/transport.cpp
    src/world.cpp)
add_library(slash::ai ALIAS slash_ai)

target_include_directories(slash_ai PUBLIC include)
target_link_libraries(slash_ai
    PUBLIC slash::core
    PRIVATE slash::platform slash::world)
if(NOT EMSCRIPTEN)
    target_link_libraries(slash_ai PRIVATE slash_httplib)
endif()
if(APPLE)
    # SecTrust decides whether a server's certificate chain is trusted.
    target_link_libraries(slash_ai PRIVATE "-framework CoreFoundation" "-framework Security")
endif()
slash_engine_target(slash_ai)

# The engine's commands as tools for a model, the ai.* commands and storyboard.review.
add_library(slash_ai_commands src/commands.cpp src/review.cpp)
add_library(slash::ai_commands ALIAS slash_ai_commands)

target_link_libraries(slash_ai_commands
    PUBLIC slash::ai slash::command
    PRIVATE slash::platform slash::storyboard)
slash_engine_target(slash_ai_commands)
