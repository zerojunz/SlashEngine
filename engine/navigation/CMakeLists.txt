add_library(slash_navigation
    src/core.cpp
    src/files.cpp
    src/navigation.cpp)
add_library(slash::navigation ALIAS slash_navigation)

target_include_directories(slash_navigation PUBLIC include)
target_link_libraries(slash_navigation PUBLIC slash::core slash::world slash::asset)
# The same paths on every platform: no fused multiply-adds.
target_compile_options(slash_navigation PRIVATE
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-ffp-contract=off>)
slash_engine_target(slash_navigation)
