add_library(slash_storyboard
    src/fingerprint.cpp
    src/storyboard.cpp
    src/style.cpp)
add_library(slash::storyboard ALIAS slash_storyboard)

target_include_directories(slash_storyboard PUBLIC include)
target_link_libraries(slash_storyboard PUBLIC slash::core)
slash_engine_target(slash_storyboard)
# Fingerprints measure the same everywhere.
set_source_files_properties(src/fingerprint.cpp PROPERTIES
    COMPILE_OPTIONS "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-ffp-contract=off>")
