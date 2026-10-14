add_library(slash_core
    src/assert.cpp
    src/fixed_step.cpp
    src/json.cpp
    src/json_schema.cpp
    src/log.cpp
    src/text.cpp
    src/version.cpp)
add_library(slash::core ALIAS slash_core)

target_include_directories(slash_core PUBLIC include)
target_compile_features(slash_core PUBLIC cxx_std_20)
target_link_libraries(slash_core PRIVATE yyjson)
# The reflection macros use __VA_OPT__, which MSVC only supports with its conforming preprocessor.
target_compile_options(slash_core PUBLIC $<$<CXX_COMPILER_ID:MSVC>:/Zc:preprocessor>)
set_source_files_properties(src/version.cpp PROPERTIES
    COMPILE_DEFINITIONS "SLASH_VERSION=\"${PROJECT_VERSION}\"")
slash_engine_target(slash_core)
