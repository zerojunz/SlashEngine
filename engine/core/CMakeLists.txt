add_library(slash_core
    src/assert.cpp
    src/fixed_step.cpp
    src/log.cpp
    src/version.cpp)
add_library(slash::core ALIAS slash_core)

target_include_directories(slash_core PUBLIC include)
target_compile_features(slash_core PUBLIC cxx_std_20)
set_source_files_properties(src/version.cpp PROPERTIES
    COMPILE_DEFINITIONS "SLASH_VERSION=\"${PROJECT_VERSION}\"")
slash_engine_target(slash_core)
