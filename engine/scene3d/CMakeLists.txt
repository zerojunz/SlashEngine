# glTF models (src/model.cpp; OBJ models are src/obj.cpp, FBX models src/fbx.cpp): cgltf's parser
# (src/cgltf.c), ufbx and MikkTSpace, which makes the tangents models leave out. All three are C,
# and built with no fused multiply-adds, so a model's vertices are the same bits on every platform.
add_library(slash_cgltf STATIC src/cgltf.c ${mikktspace_SOURCE_DIR}/mikktspace.c)
target_include_directories(slash_cgltf SYSTEM PUBLIC ${cgltf_SOURCE_DIR} ${mikktspace_SOURCE_DIR})
target_compile_options(slash_cgltf PRIVATE "$<$<NOT:$<C_COMPILER_ID:MSVC>>:-w;-ffp-contract=off>")
target_compile_definitions(slash_cgltf PRIVATE $<$<C_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>)

add_library(slash_ufbx STATIC ${ufbx_SOURCE_DIR}/ufbx.c)
target_include_directories(slash_ufbx SYSTEM PUBLIC ${ufbx_SOURCE_DIR})
target_compile_options(slash_ufbx PRIVATE "$<$<NOT:$<C_COMPILER_ID:MSVC>>:-w;-ffp-contract=off>")
target_compile_definitions(slash_ufbx PRIVATE $<$<C_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>)

# Lightmap UVs (src/lightmap.cpp) are unwrapped with xatlas and lightmap atlases packed with
# stb_rect_pack (src/lightmapper.cpp), both built through src/xatlas_build.cpp so that their charts
# and atlases are the same on every platform.
add_library(slash_xatlas STATIC src/xatlas_build.cpp)
target_include_directories(slash_xatlas SYSTEM PUBLIC ${xatlas_SOURCE_DIR}/source/xatlas
    ${stb_SOURCE_DIR})
target_link_libraries(slash_xatlas PRIVATE slash::core)
target_compile_options(slash_xatlas PRIVATE "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-w;-ffp-contract=off>")

add_library(slash_scene3d
    src/blend.cpp
    src/bone_attachment.cpp
    src/csg.cpp
    src/fbx.cpp
    src/gltf_writer.cpp
    src/gridmap.cpp
    src/ik.cpp
    src/lightmap.cpp
    src/lightmap_probes.cpp
    src/lightmapper.cpp
    src/model.cpp
    src/model_tools.cpp
    src/obj.cpp
    src/physical_bones.cpp
    src/primitive.cpp
    src/scene3d.cpp
    src/skeleton.cpp
    src/terrain.cpp
    src/voxel_gi.cpp)
add_library(slash::scene3d ALIAS slash_scene3d)

target_include_directories(slash_scene3d PUBLIC include)
# CSG shapes (src/csg.cpp) combine on Manifold, built in cmake/Dependencies.cmake.
target_link_libraries(slash_scene3d PUBLIC slash::asset slash::core slash::render slash::world
    PRIVATE slash::platform slash_cgltf slash_manifold slash_ufbx slash_xatlas)
slash_engine_target(slash_scene3d)
# Meshes and cameras are the same bits on every platform: no fused multiply-adds, which arm64
# compilers would otherwise emit and x86-64 ones not.
target_compile_options(slash_scene3d PRIVATE
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-ffp-contract=off>)
