# Procedural generation: FastNoiseLite noise,
# gradients, the .texture.json textures made from them, .scatter.json scatter rules and
# .pcg.json procedural graphs, and texture graphs (.texture.json files of "type": "graph").
# Parts of this file are ported from Godot Engine (MIT); see THIRD_PARTY.md.
add_library(slash_procedural
    src/foliage.cpp
    src/gradient.cpp
    src/noise.cpp
    src/pcg.cpp
    src/scatter.cpp
    src/texture.cpp
    src/texture_graph.cpp
    src/texture_graph_shader.cpp)
add_library(slash::procedural ALIAS slash_procedural)

target_include_directories(slash_procedural PUBLIC include)
target_include_directories(slash_procedural SYSTEM PRIVATE ${FastNoiseLite_SOURCE_DIR}/Cpp)
target_link_libraries(slash_procedural PUBLIC slash::asset slash::core slash::render slash::world)
slash_engine_target(slash_procedural)
# The same noise and pixels on every platform: no fused multiply-adds.
target_compile_options(slash_procedural PRIVATE $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-ffp-contract=off>)
