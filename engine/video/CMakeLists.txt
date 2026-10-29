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
