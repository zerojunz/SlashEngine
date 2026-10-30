add_library(slash_xr
    src/action_map.cpp
    src/settings.cpp
    src/test_interface.cpp
    src/xr.cpp)
add_library(slash::xr ALIAS slash_xr)

target_include_directories(slash_xr PUBLIC include)
target_link_libraries(slash_xr PUBLIC slash::core slash::render slash::world)
slash_engine_target(slash_xr)
# Poses place nodes the same everywhere.
target_compile_options(slash_xr PRIVATE "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-ffp-contract=off>")

# The OpenXR interface, over the Khronos loader where this platform builds it
# (cmake/Dependencies.cmake).
if(TARGET slash_openxr)
    target_sources(slash_xr PRIVATE src/openxr.cpp src/openxr_graphics.cpp)
    target_link_libraries(slash_xr PRIVATE slash_openxr)
    # Vulkan's headers, which SDL carries, for the session's graphics binding.
    target_include_directories(slash_xr SYSTEM PRIVATE ${SDL3_SOURCE_DIR}/src/video/khronos)
else()
    target_sources(slash_xr PRIVATE src/openxr_none.cpp)
endif()
