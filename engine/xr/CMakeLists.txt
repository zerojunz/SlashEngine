add_library(slash_xr
    src/settings.cpp
    src/test_interface.cpp
    src/xr.cpp)
add_library(slash::xr ALIAS slash_xr)

target_include_directories(slash_xr PUBLIC include)
target_link_libraries(slash_xr PUBLIC slash::core slash::world)
slash_engine_target(slash_xr)
# Poses place nodes the same everywhere.
target_compile_options(slash_xr PRIVATE "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-ffp-contract=off>")
