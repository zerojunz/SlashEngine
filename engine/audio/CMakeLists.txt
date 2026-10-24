# miniaudio's WAV and MP3 decoders and stb_vorbis, with no device backends, threads, encoders or
# engine: SDL plays the sound (src/sdl.cpp) and the engine mixes it (src/server.cpp).
add_library(slash_miniaudio STATIC src/decoders.c)
target_include_directories(slash_miniaudio SYSTEM PUBLIC ${miniaudio_SOURCE_DIR}
    ${miniaudio_SOURCE_DIR}/extras)
target_compile_definitions(slash_miniaudio PUBLIC MA_NO_DEVICE_IO MA_NO_THREADING MA_NO_ENCODING
    MA_NO_GENERATION MA_NO_ENGINE MA_NO_NODE_GRAPH MA_NO_RESOURCE_MANAGER MA_NO_FLAC)

# libebur128 measures loudness and true peaks (src/analysis.cpp). Its own queue.h stands in for
# <sys/queue.h>, which Windows lacks; no fused multiply-adds, so it measures the same everywhere.
add_library(slash_ebur128 STATIC ${libebur128_SOURCE_DIR}/ebur128/ebur128.c)
target_include_directories(slash_ebur128 SYSTEM PUBLIC ${libebur128_SOURCE_DIR}/ebur128)
target_include_directories(slash_ebur128 SYSTEM PRIVATE ${libebur128_SOURCE_DIR}/ebur128/queue)
target_compile_definitions(slash_ebur128 PRIVATE $<$<C_COMPILER_ID:MSVC>:_USE_MATH_DEFINES>)
# Optimized in debug builds too, where it would otherwise take most of audio.analyze's time.
target_compile_options(slash_ebur128 PRIVATE
    "$<$<NOT:$<C_COMPILER_ID:MSVC>>:-ffp-contract=off;-w;-O2>")
if(UNIX AND NOT APPLE AND NOT EMSCRIPTEN)
    target_link_libraries(slash_ebur128 PRIVATE m)
endif()

add_library(slash_audio
    src/analysis.cpp
    src/audio.cpp
    src/buses.cpp
    src/effects.cpp
    src/filter.cpp
    src/music.cpp
    src/sdl.cpp
    src/server.cpp
    src/sound.cpp
    src/stream.cpp)
add_library(slash::audio ALIAS slash_audio)

target_include_directories(slash_audio PUBLIC include)
target_link_libraries(slash_audio
    PUBLIC slash::asset slash::core slash::world
    PRIVATE slash::physics2d slash::physics3d slash::ui slash_ebur128 slash_miniaudio SDL3::SDL3-static)
slash_engine_target(slash_audio)
# The same sound on every platform: no fused multiply-adds, which arm64 compilers would otherwise
# emit and x86-64 ones not.
target_compile_options(slash_audio PRIVATE
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-ffp-contract=off>)
