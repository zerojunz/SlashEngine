add_library(slash_video
    src/convert.cpp
    src/decoder.cpp
    src/encoder.cpp
    src/ivf.cpp
    src/video.cpp)
add_library(slash::video ALIAS slash_video)

target_include_directories(slash_video PUBLIC include)
target_link_libraries(slash_video PUBLIC slash::core slash::render slash::asset slash::world
                      PRIVATE slash_dav1d)
slash_engine_target(slash_video)

# Lossy encoding through libaom (cmake/Dependencies.cmake), for the tools and tests only: games
# play videos, never make them.
if(TARGET slash_aom)
    add_library(slash_video_encoder STATIC src/lossy_encoder.cpp)
    add_library(slash::video_encoder ALIAS slash_video_encoder)
    target_link_libraries(slash_video_encoder PUBLIC slash::video PRIVATE slash_aom)
    slash_engine_target(slash_video_encoder)

    # video.encode, registered by slash-cli and slash-mcp.
    add_library(slash_video_commands STATIC src/commands.cpp)
    add_library(slash::video_commands ALIAS slash_video_commands)
    target_link_libraries(slash_video_commands PUBLIC slash::command
                          PRIVATE slash::platform slash::video_encoder)
    slash_engine_target(slash_video_commands)
endif()
