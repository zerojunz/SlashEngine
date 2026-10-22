# glTF models (src/model.cpp; OBJ models are src/obj.cpp): cgltf's parser (src/cgltf.c) and MikkTSpace, which makes the
# tangents models leave out. Both are C, and built with no fused multiply-adds, so a model's
# vertices are the same bits on every platform.
add_library(slash_cgltf STATIC src/cgltf.c ${mikktspace_SOURCE_DIR}/mikktspace.c)
target_include_directories(slash_cgltf SYSTEM PUBLIC ${cgltf_SOURCE_DIR} ${mikktspace_SOURCE_DIR})
target_compile_options(slash_cgltf PRIVATE "$<$<NOT:$<C_COMPILER_ID:MSVC>>:-w;-ffp-contract=off>")
target_compile_definitions(slash_cgltf PRIVATE $<$<C_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>)

add_library(slash_scene3d
    src/model.cpp
    src/model_tools.cpp
    src/obj.cpp
    src/primitive.cpp
    src/scene3d.cpp)
add_library(slash::scene3d ALIAS slash_scene3d)

target_include_directories(slash_scene3d PUBLIC include)
target_link_libraries(slash_scene3d PUBLIC slash::asset slash::core slash::render slash::world
    PRIVATE slash_cgltf)
slash_engine_target(slash_scene3d)
# Meshes and cameras are the same bits on every platform: no fused multiply-adds, which arm64
# compilers would otherwise emit and x86-64 ones not.
target_compile_options(slash_scene3d PRIVATE
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-ffp-contract=off>)
