# Procedural generation, as Godot's noise module and texture resources: FastNoiseLite noise,
# gradients and the .texture.json textures made from them.
add_library(slash_procedural
    src/gradient.cpp
    src/noise.cpp
    src/texture.cpp)
add_library(slash::procedural ALIAS slash_procedural)

target_include_directories(slash_procedural PUBLIC include)
target_include_directories(slash_procedural SYSTEM PRIVATE ${FastNoiseLite_SOURCE_DIR}/Cpp)
target_link_libraries(slash_procedural PUBLIC slash::core slash::render)
slash_engine_target(slash_procedural)
# The same noise and pixels on every platform: no fused multiply-adds.
target_compile_options(slash_procedural PRIVATE $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-ffp-contract=off>)
