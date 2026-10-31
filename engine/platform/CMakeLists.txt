add_library(slash_platform
    src/file.cpp
    src/fonts.cpp
    src/library.cpp
    src/locale.cpp
    src/memory.cpp
    src/platform.cpp
    src/process.cpp
    src/run.cpp)
add_library(slash::platform ALIAS slash_platform)

target_include_directories(slash_platform PUBLIC include)
target_link_libraries(slash_platform
    PUBLIC slash::core
    PRIVATE SDL3::SDL3-static)
slash_engine_target(slash_platform)
# Installed fonts (src/fonts.cpp): CoreText on Apple platforms, DirectWrite on Windows, and
# fontconfig on Linux, loaded at run time.
if(APPLE)
    target_link_libraries(slash_platform PRIVATE "-framework CoreText" "-framework CoreFoundation")
elseif(WIN32)
    target_link_libraries(slash_platform PRIVATE dwrite)
elseif(NOT EMSCRIPTEN AND NOT ANDROID)
    target_link_libraries(slash_platform PRIVATE ${CMAKE_DL_LIBS})
endif()
