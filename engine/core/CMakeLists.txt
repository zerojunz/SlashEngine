add_library(slash_core
    src/assert.cpp
    src/fixed_step.cpp
    src/jobs.cpp
    src/json.cpp
    src/json_schema.cpp
    src/log.cpp
    src/math.cpp
    src/sha256.cpp
    src/text.cpp
    src/version.cpp)
add_library(slash::core ALIAS slash_core)

# slash/core/api_hash.hpp, a hash of every engine header that gameplay libraries record (see
# cmake/api_hash.cmake).
set(api_hash_header ${CMAKE_CURRENT_BINARY_DIR}/generated/slash/core/api_hash.hpp)
file(GLOB_RECURSE engine_headers CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/engine/*/include/*.hpp)
add_custom_command(OUTPUT ${api_hash_header}
    COMMAND ${CMAKE_COMMAND} -DENGINE=${PROJECT_SOURCE_DIR}/engine
        -DDEPENDENCIES=${PROJECT_SOURCE_DIR}/cmake/dependencies.json -DOUTPUT=${api_hash_header}
        -P ${PROJECT_SOURCE_DIR}/cmake/api_hash.cmake
    DEPENDS ${engine_headers} ${PROJECT_SOURCE_DIR}/cmake/dependencies.json
        ${PROJECT_SOURCE_DIR}/cmake/api_hash.cmake
    COMMENT "Hashing the engine headers"
    VERBATIM)
target_sources(slash_core PRIVATE ${api_hash_header})
# CI's clang-tidy job configures without building, so it builds the header alone.
add_custom_target(slash_api_hash DEPENDS ${api_hash_header})

target_include_directories(slash_core PUBLIC include ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_compile_features(slash_core PUBLIC cxx_std_20)
target_link_libraries(slash_core PRIVATE yyjson)
if(ANDROID)
    target_link_libraries(slash_core PRIVATE log)
endif()
# The job system's worker threads. The web has none without SharedArrayBuffer (see jobs.cpp).
if(NOT EMSCRIPTEN)
    find_package(Threads REQUIRED)
    target_link_libraries(slash_core PRIVATE Threads::Threads)
endif()
# Tracy's zones and frame marks (slash/core/profile.hpp).
if(SLASH_PROFILER STREQUAL "tracy")
    target_link_libraries(slash_core PUBLIC Tracy::TracyClient)
    target_compile_definitions(slash_core PUBLIC SLASH_TRACY)
endif()
# The reflection macros use __VA_OPT__, which MSVC only supports with its conforming preprocessor.
target_compile_options(slash_core PUBLIC $<$<CXX_COMPILER_ID:MSVC>:/Zc:preprocessor>)
set_source_files_properties(src/version.cpp PROPERTIES
    COMPILE_DEFINITIONS "SLASH_VERSION=\"${PROJECT_VERSION}\"")
# cos_sin gives the same bits everywhere: no fused multiply-adds, which arm64 compilers would
# otherwise emit and x86-64 ones not.
set_source_files_properties(src/math.cpp PROPERTIES
    COMPILE_OPTIONS "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-ffp-contract=off>")
slash_engine_target(slash_core)
