# Third-party dependencies. Each is pinned in dependencies.json, with its repository, exact tag,
# license and the hash of its license file, and listed in THIRD_PARTY.md; only permissive
# licenses are allowed (see docs/design.md). The "licenses" test checks all of that, so add a
# dependency with slash_add_dependency, never with CPMAddPackage or FetchContent directly.

include(CPM)

file(READ ${CMAKE_CURRENT_LIST_DIR}/dependencies.json SLASH_DEPENDENCY_MANIFEST)
set(SLASH_DEPENDENCIES "")

# Fetches a dependency at the repository and tag dependencies.json pins; other arguments go to
# CPMAddPackage. A macro, so CPM's <name>_SOURCE_DIR and <name>_ADDED reach the caller.
macro(slash_add_dependency name)
    string(JSON _slash_repository GET "${SLASH_DEPENDENCY_MANIFEST}" dependencies ${name}
        repository)
    string(JSON _slash_tag GET "${SLASH_DEPENDENCY_MANIFEST}" dependencies ${name} tag)
    # A project that tags no releases is pinned to a commit, which a shallow clone can't reach.
    string(LENGTH "${_slash_tag}" _slash_tag_length)
    if(_slash_tag_length EQUAL 40 AND _slash_tag MATCHES "^[0-9a-f]+$")
        set(_slash_shallow FALSE)
    else()
        set(_slash_shallow TRUE)
    endif()
    CPMAddPackage(
        NAME ${name}
        GIT_REPOSITORY ${_slash_repository}
        GIT_TAG ${_slash_tag}
        GIT_SHALLOW ${_slash_shallow}
        ${ARGN})
    list(APPEND SLASH_DEPENDENCIES ${name})
endmacro()

# Vulkan shaders see 16 KiB of each uniform push, as the other backends see all of it, and
# MetalFX gets Metal's objects (cmake/patches).
slash_add_dependency(SDL3
    SYSTEM TRUE
    PATCH_COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_LIST_DIR}/patches/sdl-vulkan-uniform-range.cmake
        COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_LIST_DIR}/patches/sdl-native-handles.cmake
    OPTIONS
        "SDL_SHARED OFF"
        "SDL_STATIC ON"
        "SDL_TEST_LIBRARY OFF"
        "SDL_TESTS OFF"
        "SDL_EXAMPLES OFF"
        "SDL_INSTALL OFF")

if(SLASH_BUILD_TESTS)
    slash_add_dependency(doctest
        SYSTEM TRUE
        OPTIONS
            "DOCTEST_WITH_TESTS OFF"
            "DOCTEST_NO_INSTALL ON")
endif()

slash_add_dependency(EnTT SYSTEM TRUE)

# JSON reader used by slash::json. The engine writes JSON itself so it controls the canonical
# layout; yyjson only parses and formats numbers.
slash_add_dependency(yyjson
    SYSTEM TRUE
    OPTIONS
        "YYJSON_INSTALL OFF"
        "YYJSON_DISABLE_INCR_READER ON"
        "YYJSON_DISABLE_FILE ON"
        "YYJSON_DISABLE_UTILS ON"
        "YYJSON_DISABLE_NON_STANDARD ON"
        "YYJSON_READER_DEPTH_LIMIT 256")

# 2D physics. Box2D builds single-threaded and deterministic: its own CMake turns off
# floating-point contraction, and SIMD stays off on the web, since browsers before 2021 (Chrome
# 91, Firefox 89, Safari 16.4) have no WebAssembly SIMD.
if(EMSCRIPTEN)
    set(_slash_box2d_simd "BOX2D_DISABLE_SIMD ON")
else()
    set(_slash_box2d_simd "BOX2D_DISABLE_SIMD OFF")
endif()
slash_add_dependency(box2d
    SYSTEM TRUE
    OPTIONS
        "BOX2D_SAMPLES OFF"
        "BOX2D_BENCHMARKS OFF"
        "BOX2D_DOCS OFF"
        "BOX2D_PROFILE OFF"
        "BOX2D_VALIDATE OFF"
        "BOX2D_UNIT_TESTS OFF"
        "BOX2D_AVX2 OFF"
        ${_slash_box2d_simd})

# 3D physics, as Godot 4.4's Jolt module. It builds deterministic on every platform, with
# floating-point contraction off, and with no x86 instruction sets past SSE2 (as Box2D above),
# since games run on machines from 2020 on. Its debug renderer, profiler, object streams, GPU
# compute and floating-point exceptions stay out, and it takes this project's compiler flags and
# runtime library rather than its own.
slash_add_dependency(JoltPhysics
    SYSTEM TRUE
    SOURCE_SUBDIR Build
    OPTIONS
        "CROSS_PLATFORM_DETERMINISTIC ON"
        "OVERRIDE_CXX_FLAGS OFF"
        "USE_STATIC_MSVC_RUNTIME_LIBRARY OFF"
        "INTERPROCEDURAL_OPTIMIZATION OFF"
        "GENERATE_DEBUG_SYMBOLS OFF"
        "ENABLE_ALL_WARNINGS OFF"
        "ENABLE_INSTALL OFF"
        "ENABLE_OBJECT_STREAM OFF"
        "FLOATING_POINT_EXCEPTIONS_ENABLED OFF"
        "DEBUG_RENDERER_IN_DEBUG_AND_RELEASE OFF"
        "PROFILER_IN_DEBUG_AND_RELEASE OFF"
        "CPP_EXCEPTIONS_ENABLED OFF"
        "CPP_RTTI_ENABLED OFF"
        "USE_SSE4_1 OFF"
        "USE_SSE4_2 OFF"
        "USE_AVX OFF"
        "USE_AVX2 OFF"
        "USE_AVX512 OFF"
        "USE_LZCNT OFF"
        "USE_TZCNT OFF"
        "USE_F16C OFF"
        "USE_FMADD OFF"
        "JPH_USE_DX12 OFF"
        "JPH_USE_VK OFF"
        "JPH_USE_MTL OFF"
        "JPH_USE_CPU_COMPUTE OFF"
        "TARGET_UNIT_TESTS OFF"
        "TARGET_HELLO_WORLD OFF"
        "TARGET_PERFORMANCE_TEST OFF"
        "TARGET_SAMPLES OFF"
        "TARGET_VIEWER OFF")

# The Tracy profiler's client, in builds configured with SLASH_PROFILER=tracy (see
# slash/core/profile.hpp). It collects only while a profiler is connected, listens on the loopback
# interface only and doesn't announce itself on the network.
if(SLASH_PROFILER STREQUAL "tracy")
    slash_add_dependency(tracy
        SYSTEM TRUE
        OPTIONS
            "TRACY_ENABLE ON"
            "TRACY_ON_DEMAND ON"
            "TRACY_ONLY_LOCALHOST ON"
            "TRACY_NO_BROADCAST ON"
            "TRACY_STATIC ON")
endif()

# TLS for httplib's client, so engine/ai reaches hosted models' https:// APIs. Its library
# only, built here rather than with its own CMake, which needs the test framework submodule.
# Not on the web, where browsers make requests. Mbed TLS is Apache-2.0 OR GPL-2.0-or-later;
# SlashEngine uses it under Apache-2.0.
if(NOT EMSCRIPTEN)
    slash_add_dependency(mbedtls DOWNLOAD_ONLY TRUE GIT_SUBMODULES "")
    if(mbedtls_ADDED)
        file(GLOB _slash_mbedtls_sources ${mbedtls_SOURCE_DIR}/library/*.c)
        add_library(slash_mbedtls STATIC ${_slash_mbedtls_sources})
        target_include_directories(slash_mbedtls SYSTEM PUBLIC ${mbedtls_SOURCE_DIR}/include)
        target_include_directories(slash_mbedtls PRIVATE ${mbedtls_SOURCE_DIR}/library)
        # Windows' random numbers come from BCryptGenRandom.
        target_link_libraries(slash_mbedtls PRIVATE $<$<PLATFORM_ID:Windows>:bcrypt>)
    endif()
endif()

# HTTP server and client: the localhost REST mirror and MCP over HTTP, engine/ai's requests to
# models and the importer's to AssetRipper. Header-only, without compression or exceptions.
# Every target gets it with the same TLS setting, since httplib's classes differ with and
# without it.
slash_add_dependency(httplib DOWNLOAD_ONLY TRUE)
if(httplib_ADDED)
    find_package(Threads REQUIRED)
    add_library(slash_httplib INTERFACE)
    target_include_directories(slash_httplib SYSTEM INTERFACE ${httplib_SOURCE_DIR})
    # httplib calls Windows 10's API (CreateFile2 and others), as old as games support, which
    # llvm-mingw's headers hide unless asked for.
    target_compile_definitions(slash_httplib INTERFACE CPPHTTPLIB_NO_EXCEPTIONS
        $<$<PLATFORM_ID:Windows>:_WIN32_WINNT=0x0A00>)
    target_link_libraries(slash_httplib INTERFACE
        Threads::Threads $<$<PLATFORM_ID:Windows>:ws2_32>)
    if(TARGET slash_mbedtls)
        target_compile_definitions(slash_httplib INTERFACE CPPHTTPLIB_MBEDTLS_SUPPORT)
        # On Windows, httplib reads the system's root certificates with crypt32.
        target_link_libraries(slash_httplib INTERFACE slash_mbedtls
            $<$<PLATFORM_ID:Windows>:crypt32>)
        # On macOS and iOS, engine/ai asks the platform whether it trusts a server's chain
        # (Security's SecTrust) instead of httplib copying the keychain's roots for each client.
        target_compile_definitions(slash_httplib INTERFACE
            CPPHTTPLIB_DISABLE_MACOSX_AUTOMATIC_ROOT_CERTIFICATES)
    endif()
endif()

# Fonts and text, as Godot's: FreeType rasterizes glyphs with its auto-hinter, HarfBuzz shapes
# text (with its own OpenType font functions, so it needs no FreeType inside), and msdfgen makes
# multichannel signed distance fields for fonts that scale. FreeType reads TrueType and OpenType
# outlines only: no compressed or color fonts, so no zlib, bzip2, PNG or Brotli.
slash_add_dependency(freetype
    SYSTEM TRUE
    OPTIONS
        "FT_DISABLE_ZLIB ON"
        "FT_DISABLE_BZIP2 ON"
        "FT_DISABLE_PNG ON"
        "FT_DISABLE_HARFBUZZ ON"
        "FT_DISABLE_BROTLI ON"
        "SKIP_INSTALL_ALL ON")

# HarfBuzz's single-file build, src/harfbuzz.cc, with no platform font backends.
slash_add_dependency(harfbuzz DOWNLOAD_ONLY TRUE)
if(harfbuzz_ADDED)
    add_library(slash_harfbuzz STATIC ${harfbuzz_SOURCE_DIR}/src/harfbuzz.cc)
    target_include_directories(slash_harfbuzz SYSTEM PUBLIC ${harfbuzz_SOURCE_DIR}/src)
    if(MSVC)
        target_compile_options(slash_harfbuzz PRIVATE /bigobj /utf-8)
    endif()
endif()

# The Unicode Bidirectional Algorithm, so right-to-left and left-to-right text mix in one line, and
# script runs (UAX #24) for shaping. Its single-file build, with no text editing API.
slash_add_dependency(SheenBidi DOWNLOAD_ONLY TRUE)
if(SheenBidi_ADDED)
    add_library(slash_sheenbidi STATIC ${SheenBidi_SOURCE_DIR}/Source/SheenBidi.c)
    target_include_directories(slash_sheenbidi SYSTEM PUBLIC ${SheenBidi_SOURCE_DIR}/Headers)
    target_include_directories(slash_sheenbidi PRIVATE ${SheenBidi_SOURCE_DIR}/Source)
    target_compile_definitions(slash_sheenbidi PRIVATE SB_CONFIG_UNITY)
    if(MSVC)
        target_compile_options(slash_sheenbidi PRIVATE /utf-8)
    endif()
endif()

# msdfgen's core only: the shape and distance field code, with no PNG, SVG or font import.
slash_add_dependency(msdfgen DOWNLOAD_ONLY TRUE)
if(msdfgen_ADDED)
    file(GLOB _slash_msdfgen_sources ${msdfgen_SOURCE_DIR}/core/*.cpp)
    add_library(slash_msdfgen STATIC ${_slash_msdfgen_sources})
    target_include_directories(slash_msdfgen SYSTEM PUBLIC ${msdfgen_SOURCE_DIR})
    target_compile_definitions(slash_msdfgen PUBLIC MSDFGEN_PUBLIC= MSDFGEN_USE_CPP11)
endif()

# The default font, as Godot has one: Roboto Regular, built into the engine (cmake/embed.cmake),
# so text draws before a game adds fonts of its own. Version 2 is Apache 2.0; later ones are not.
slash_add_dependency(roboto DOWNLOAD_ONLY TRUE)

# Audio decoding, as Godot imports sound: WAV and MP3 with miniaudio's decoders and Ogg Vorbis with
# stb_vorbis from its extras. engine/audio builds them (src/miniaudio.c) with no device backends,
# threads or engine, since SDL plays the sound and the engine mixes it.
slash_add_dependency(miniaudio DOWNLOAD_ONLY TRUE)

# Loudness as EBU R 128 measures it, for audio.analyze: libebur128, which engine/audio builds from
# its one source file with the BSD queue.h it carries, the same on every platform.
slash_add_dependency(libebur128 DOWNLOAD_ONLY TRUE)

# 3D models, as Godot imports glTF 2.0: cgltf parses .gltf and .glb files and MikkTSpace makes
# the tangents a model leaves out, as Godot's SurfaceTool does (engine/scene3d); stb_image decodes
# the JPEG textures models carry, which SDL can't (engine/render, beside PNG and BMP).
slash_add_dependency(cgltf DOWNLOAD_ONLY TRUE)
slash_add_dependency(mikktspace DOWNLOAD_ONLY TRUE)
slash_add_dependency(stb DOWNLOAD_ONLY TRUE)
# FBX models, read with ufbx as Godot 4.3's importer reads them (engine/scene3d builds its one
# source file).
slash_add_dependency(ufbx DOWNLOAD_ONLY TRUE)
# Lightmap UVs, unwrapped with xatlas as Godot's "Unwrap UV2 for Lightmap/AO" unwraps them
# (engine/scene3d builds it through src/xatlas_build.cpp, which makes its charts the same
# everywhere).
slash_add_dependency(xatlas DOWNLOAD_ONLY TRUE)
# Mesh LODs, simplified with meshoptimizer 1.0 as Godot 4.6's importer generates them
# (engine/scene3d builds its simplifier with no fused multiply-adds, so LODs are the same
# everywhere).
slash_add_dependency(meshoptimizer DOWNLOAD_ONLY TRUE)

# Baking navigation meshes, as Godot does (engine/navigation): Recast voxelizes 3D geometry into
# polygons, and Clipper2 and PolyPartition merge, cut and split 2D outlines. No fused multiply-adds,
# so bakes give the same polygons on every platform.
slash_add_dependency(recastnavigation DOWNLOAD_ONLY TRUE)
if(recastnavigation_ADDED)
    file(GLOB _slash_recast_sources ${recastnavigation_SOURCE_DIR}/Recast/Source/*.cpp)
    add_library(slash_recast STATIC ${_slash_recast_sources})
    target_include_directories(slash_recast SYSTEM PUBLIC
        ${recastnavigation_SOURCE_DIR}/Recast/Include)
    target_compile_options(slash_recast PRIVATE
        $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-ffp-contract=off>)
endif()
slash_add_dependency(Clipper2 DOWNLOAD_ONLY TRUE)
if(Clipper2_ADDED)
    file(GLOB _slash_clipper2_sources ${Clipper2_SOURCE_DIR}/CPP/Clipper2Lib/src/*.cpp)
    add_library(slash_clipper2 STATIC ${_slash_clipper2_sources})
    target_include_directories(slash_clipper2 SYSTEM PUBLIC
        ${Clipper2_SOURCE_DIR}/CPP/Clipper2Lib/include)
    target_compile_options(slash_clipper2 PRIVATE
        $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-ffp-contract=off>)
    # Its headers use std::back_inserter without including <iterator>, which newer libc++ (as in
    # llvm-mingw) no longer brings in through <algorithm>.
    target_compile_options(slash_clipper2 PUBLIC
        "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:SHELL:-include iterator>")
endif()
slash_add_dependency(polypartition DOWNLOAD_ONLY TRUE)
if(polypartition_ADDED)
    add_library(slash_polypartition STATIC ${polypartition_SOURCE_DIR}/src/polypartition.cpp)
    target_include_directories(slash_polypartition SYSTEM PUBLIC ${polypartition_SOURCE_DIR}/src)
    target_compile_options(slash_polypartition PRIVATE
        $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-ffp-contract=off>)
endif()

# CSG shapes, as Godot 4.6's (engine/scene3d/src/csg.cpp): Manifold's booleans, built as Godot
# builds them, without its threads (MANIFOLD_PAR -1) or cross sections, and with no fused
# multiply-adds, so a shape has the same triangles on every platform.
slash_add_dependency(manifold DOWNLOAD_ONLY TRUE)
if(manifold_ADDED)
    add_library(slash_manifold STATIC
        ${manifold_SOURCE_DIR}/src/boolean3.cpp
        ${manifold_SOURCE_DIR}/src/boolean_result.cpp
        ${manifold_SOURCE_DIR}/src/constructors.cpp
        ${manifold_SOURCE_DIR}/src/csg_tree.cpp
        ${manifold_SOURCE_DIR}/src/edge_op.cpp
        ${manifold_SOURCE_DIR}/src/face_op.cpp
        ${manifold_SOURCE_DIR}/src/impl.cpp
        ${manifold_SOURCE_DIR}/src/manifold.cpp
        ${manifold_SOURCE_DIR}/src/polygon.cpp
        ${manifold_SOURCE_DIR}/src/properties.cpp
        ${manifold_SOURCE_DIR}/src/quickhull.cpp
        ${manifold_SOURCE_DIR}/src/sdf.cpp
        ${manifold_SOURCE_DIR}/src/smoothing.cpp
        ${manifold_SOURCE_DIR}/src/sort.cpp
        ${manifold_SOURCE_DIR}/src/subdivision.cpp
        ${manifold_SOURCE_DIR}/src/tree2d.cpp)
    target_include_directories(slash_manifold SYSTEM PUBLIC ${manifold_SOURCE_DIR}/include)
    target_compile_definitions(slash_manifold PRIVATE MANIFOLD_PAR=-1)
    target_compile_options(slash_manifold PRIVATE
        "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-w;-ffp-contract=off>" $<$<CXX_COMPILER_ID:MSVC>:/bigobj>)
endif()

# Compressed textures, BC7 and ASTC 4x4, which GPUs sample as they are: engine/render builds the
# decoders, bc7enc_rdo's bc7decomp.cpp and astc-encoder without its compressor, for devices that
# can't sample a format (the software device, or a GPU without it). Tools and tests also build
# the encoders, bc7enc.cpp and astc-encoder's compressor (slash::texture_encoder).
slash_add_dependency(bc7enc_rdo DOWNLOAD_ONLY TRUE)
slash_add_dependency(astc-encoder DOWNLOAD_ONLY TRUE)

# Reading exported games back into projects (tools/importer): libwebp decodes the WebP images
# Godot stores imported textures as, zstd the compressed GDScript tokens and resources, and brotli
# Unity's compressed web builds. Tools only, decoders only: games never link them. The tests link
# the tools' libraries too, except in a web build, which has only the drawing tests.
if((SLASH_BUILD_TOOLS OR SLASH_BUILD_TESTS) AND NOT EMSCRIPTEN)
    slash_add_dependency(libwebp
        SYSTEM TRUE
        EXCLUDE_FROM_ALL TRUE
        OPTIONS
            "WEBP_BUILD_ANIM_UTILS OFF"
            "WEBP_BUILD_CWEBP OFF"
            "WEBP_BUILD_DWEBP OFF"
            "WEBP_BUILD_GIF2WEBP OFF"
            "WEBP_BUILD_IMG2WEBP OFF"
            "WEBP_BUILD_VWEBP OFF"
            "WEBP_BUILD_WEBPINFO OFF"
            "WEBP_BUILD_LIBWEBPMUX OFF"
            "WEBP_BUILD_WEBPMUX OFF"
            "WEBP_BUILD_EXTRAS OFF"
            "WEBP_BUILD_FUZZTEST OFF"
            "WEBP_USE_THREAD OFF")

    slash_add_dependency(zstd DOWNLOAD_ONLY TRUE)
    if(zstd_ADDED)
        file(GLOB _slash_zstd_sources ${zstd_SOURCE_DIR}/lib/common/*.c
            ${zstd_SOURCE_DIR}/lib/decompress/*.c)
        add_library(slash_zstd STATIC ${_slash_zstd_sources})
        target_include_directories(slash_zstd SYSTEM PUBLIC ${zstd_SOURCE_DIR}/lib)
        # The C decoder everywhere, without the x86-64 assembly one, and no multithreading.
        target_compile_definitions(slash_zstd PRIVATE ZSTD_DISABLE_ASM ZSTD_MULTITHREAD=0)
    endif()

    slash_add_dependency(brotli DOWNLOAD_ONLY TRUE)
    if(brotli_ADDED)
        file(GLOB _slash_brotli_sources ${brotli_SOURCE_DIR}/c/common/*.c
            ${brotli_SOURCE_DIR}/c/dec/*.c)
        add_library(slash_brotli STATIC ${_slash_brotli_sources})
        target_include_directories(slash_brotli SYSTEM PUBLIC ${brotli_SOURCE_DIR}/c/include)
    endif()

    # A game's own shaders, compiled by the tools (tools/shader_compiler): glslang turns HLSL into
    # SPIR-V, and SPIRV-Cross turns that into MSL and GLSL ES. Games get the compiled code, never
    # the compilers.
    slash_add_dependency(glslang
        SYSTEM TRUE
        EXCLUDE_FROM_ALL TRUE
        OPTIONS
            "BUILD_EXTERNAL OFF"
            "ENABLE_OPT OFF"
            "ENABLE_HLSL ON"
            "ENABLE_GLSLANG_BINARIES OFF"
            "ENABLE_PCH OFF"
            "GLSLANG_TESTS OFF"
            "GLSLANG_ENABLE_INSTALL OFF")
    slash_add_dependency(SPIRV-Cross
        SYSTEM TRUE
        EXCLUDE_FROM_ALL TRUE
        OPTIONS
            "SPIRV_CROSS_CLI OFF"
            "SPIRV_CROSS_ENABLE_TESTS OFF"
            "SPIRV_CROSS_ENABLE_GLSL ON"
            "SPIRV_CROSS_ENABLE_MSL ON"
            "SPIRV_CROSS_ENABLE_HLSL OFF"
            "SPIRV_CROSS_ENABLE_CPP OFF"
            "SPIRV_CROSS_ENABLE_REFLECT OFF"
            "SPIRV_CROSS_ENABLE_C_API OFF"
            "SPIRV_CROSS_ENABLE_UTIL OFF"
            "SPIRV_CROSS_STATIC ON"
            "SPIRV_CROSS_SHARED OFF"
            "SPIRV_CROSS_SKIP_INSTALL ON")

    # WGSL for WebGPU: Tint, from Dawn, reads the same SPIR-V and writes WGSL. Dawn's CMake builds
    # only Tint with every backend off, on its pinned Abseil (Tint's number parsing) and
    # SPIRV-Tools (its SPIR-V reader); its own submodules aren't fetched ("CMakeLists.txt" names
    # no submodule, and an empty list never reaches git through CPM).
    slash_add_dependency(SPIRV-Headers DOWNLOAD_ONLY TRUE)
    slash_add_dependency(SPIRV-Tools DOWNLOAD_ONLY TRUE)
    slash_add_dependency(abseil-cpp DOWNLOAD_ONLY TRUE)
    # Without debug info: Tint's would add hundreds of megabytes to each tool and example.
    foreach(lang C CXX)
        foreach(config DEBUG RELWITHDEBINFO)
            set(slash_flags_${lang}_${config} "${CMAKE_${lang}_FLAGS_${config}}")
            string(REGEX REPLACE "(^| )[-/](g|Zi|Z7)[^ ]*" "" CMAKE_${lang}_FLAGS_${config}
                "${CMAKE_${lang}_FLAGS_${config}}")
        endforeach()
    endforeach()
    # Xcode 15's clang needs a deduction guide in Tint (cmake/patches).
    slash_add_dependency(dawn
        SYSTEM TRUE
        EXCLUDE_FROM_ALL TRUE
        GIT_SUBMODULES "CMakeLists.txt"
        PATCH_COMMAND ${CMAKE_COMMAND} -P
            ${CMAKE_CURRENT_LIST_DIR}/patches/dawn-overloaded-deduction-guide.cmake
        OPTIONS
            "DAWN_ABSEIL_DIR ${abseil-cpp_SOURCE_DIR}"
            "DAWN_SPIRV_HEADERS_DIR ${SPIRV-Headers_SOURCE_DIR}"
            "DAWN_SPIRV_TOOLS_DIR ${SPIRV-Tools_SOURCE_DIR}"
            "DAWN_FETCH_DEPENDENCIES OFF"
            "DAWN_ENABLE_D3D11 OFF"
            "DAWN_ENABLE_D3D12 OFF"
            "DAWN_ENABLE_METAL OFF"
            "DAWN_ENABLE_NULL OFF"
            "DAWN_ENABLE_WEBGPU_ON_WEBGPU OFF"
            "DAWN_ENABLE_DESKTOP_GL OFF"
            "DAWN_ENABLE_OPENGLES OFF"
            "DAWN_ENABLE_VULKAN OFF"
            "DAWN_ENABLE_SPIRV_VALIDATION OFF"
            "DAWN_USE_WAYLAND OFF"
            "DAWN_USE_X11 OFF"
            "DAWN_USE_GLFW OFF"
            "DAWN_USE_WINDOWS_UI OFF"
            "DAWN_BUILD_SAMPLES OFF"
            "DAWN_BUILD_TESTS OFF"
            "DAWN_BUILD_BENCHMARKS OFF"
            "DAWN_BUILD_FUZZERS OFF"
            "DAWN_BUILD_PROTOBUF OFF"
            "DAWN_BUILD_NODE_BINDINGS OFF"
            "TINT_BUILD_CMD_TOOLS OFF"
            "TINT_BUILD_SPV_READER ON"
            "TINT_BUILD_WGSL_READER ON"
            "TINT_BUILD_WGSL_WRITER ON"
            "TINT_BUILD_GLSL_WRITER OFF"
            "TINT_BUILD_GLSL_VALIDATOR OFF"
            "TINT_BUILD_HLSL_WRITER OFF"
            "TINT_BUILD_MSL_WRITER OFF"
            "TINT_BUILD_SPV_WRITER OFF"
            "TINT_BUILD_NULL_WRITER OFF"
            "TINT_BUILD_IR_BINARY OFF"
            "TINT_BUILD_TESTS OFF"
            "TINT_BUILD_BENCHMARKS OFF"
            "TINT_BUILD_FUZZERS OFF"
            "TINT_BUILD_TINTD OFF")
    foreach(lang C CXX)
        foreach(config DEBUG RELWITHDEBINFO)
            set(CMAKE_${lang}_FLAGS_${config} "${slash_flags_${lang}_${config}}")
        endforeach()
    endforeach()

    # Playing scores into stems (soundtrack.render): TinySoundFont plays SoundFont 2 instruments
    # (engine/soundtrack builds it), and libvorbis, on libogg, encodes the stems as Ogg Vorbis,
    # built here from their sources without their own CMake, which looks for an installed libogg.
    # Tools only: games play the stems, never the scores.
    slash_add_dependency(TinySoundFont DOWNLOAD_ONLY TRUE)
    slash_add_dependency(libogg DOWNLOAD_ONLY TRUE)
    slash_add_dependency(libvorbis DOWNLOAD_ONLY TRUE)
    if(libogg_ADDED AND libvorbis_ADDED)
        # ogg/config_types.h is otherwise written by libogg's configure step.
        set(_slash_ogg_config ${CMAKE_BINARY_DIR}/_deps/ogg-config)
        file(WRITE ${_slash_ogg_config}/ogg/config_types.h.new
            "#ifndef __CONFIG_TYPES_H__\n#define __CONFIG_TYPES_H__\n#include <stdint.h>\n"
            "typedef int16_t ogg_int16_t;\ntypedef uint16_t ogg_uint16_t;\n"
            "typedef int32_t ogg_int32_t;\ntypedef uint32_t ogg_uint32_t;\n"
            "typedef int64_t ogg_int64_t;\ntypedef uint64_t ogg_uint64_t;\n#endif\n")
        file(COPY_FILE ${_slash_ogg_config}/ogg/config_types.h.new
            ${_slash_ogg_config}/ogg/config_types.h ONLY_IF_DIFFERENT)
        set(_slash_vorbis ${libvorbis_SOURCE_DIR}/lib)
        add_library(slash_vorbis STATIC
            ${libogg_SOURCE_DIR}/src/bitwise.c
            ${libogg_SOURCE_DIR}/src/framing.c
            ${_slash_vorbis}/analysis.c
            ${_slash_vorbis}/bitrate.c
            ${_slash_vorbis}/block.c
            ${_slash_vorbis}/codebook.c
            ${_slash_vorbis}/envelope.c
            ${_slash_vorbis}/floor0.c
            ${_slash_vorbis}/floor1.c
            ${_slash_vorbis}/info.c
            ${_slash_vorbis}/lookup.c
            ${_slash_vorbis}/lpc.c
            ${_slash_vorbis}/lsp.c
            ${_slash_vorbis}/mapping0.c
            ${_slash_vorbis}/mdct.c
            ${_slash_vorbis}/psy.c
            ${_slash_vorbis}/registry.c
            ${_slash_vorbis}/res0.c
            ${_slash_vorbis}/sharedbook.c
            ${_slash_vorbis}/smallft.c
            ${_slash_vorbis}/synthesis.c
            ${_slash_vorbis}/vorbisenc.c
            ${_slash_vorbis}/window.c)
        target_include_directories(slash_vorbis SYSTEM PUBLIC
            ${libogg_SOURCE_DIR}/include ${libvorbis_SOURCE_DIR}/include ${_slash_ogg_config})
        target_include_directories(slash_vorbis PRIVATE ${_slash_vorbis})
        target_compile_definitions(slash_vorbis PRIVATE
            $<$<NOT:$<PLATFORM_ID:Windows>>:HAVE_ALLOCA_H>
            $<$<C_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS;_USE_MATH_DEFINES>)
        target_compile_options(slash_vorbis PRIVATE
            "$<$<NOT:$<C_COMPILER_ID:MSVC>>:-w>" "$<$<C_COMPILER_ID:MSVC>:/W0>")
        if(UNIX AND NOT APPLE)
            target_link_libraries(slash_vorbis PRIVATE m)
        endif()
    endif()
endif()

# Linux exports build SDL's Wayland support against the glibc 2.31 sysroot's Wayland 1.18, whose
# headers only fit protocol code from a wayland-scanner as old, and macOS has none at all. So the
# tools build Wayland 1.18's scanner, on expat, into slash-cli and slash-mcp, which run it as
# "slash-cli wayland-scanner" (tools/exporter). Windows machines don't build Linux games.
if((SLASH_BUILD_TOOLS OR SLASH_BUILD_TESTS) AND NOT WIN32 AND NOT EMSCRIPTEN)
    slash_add_dependency(expat DOWNLOAD_ONLY TRUE)
    slash_add_dependency(wayland DOWNLOAD_ONLY TRUE)
    if(expat_ADDED AND wayland_ADDED)
        # What expat's and Wayland's configure steps would write.
        set(_slash_scanner_config ${CMAKE_BINARY_DIR}/_deps/wayland-scanner-config)
        if(APPLE)
            set(_slash_expat_entropy "#define HAVE_ARC4RANDOM_BUF 1\n")
            set(_slash_expat_random random_arc4random_buf.c)
        else()
            set(_slash_expat_entropy "#define XML_DEV_URANDOM 1\n")
            set(_slash_expat_random random_dev_urandom.c)
        endif()
        file(WRITE ${_slash_scanner_config}/expat_config.h.new
            "#ifndef EXPAT_CONFIG_H\n#define EXPAT_CONFIG_H 1\n"
            "#define BYTEORDER 1234\n#define HAVE_STDINT_H 1\n#define HAVE_STDLIB_H 1\n"
            "#define HAVE_STRING_H 1\n#define HAVE_UNISTD_H 1\n#define HAVE_FCNTL_H 1\n"
            "#define PACKAGE \"expat\"\n#define XML_CONTEXT_BYTES 1024\n#define XML_DTD 1\n"
            "#define XML_GE 1\n#define XML_NS 1\n${_slash_expat_entropy}#endif\n")
        file(WRITE ${_slash_scanner_config}/wayland-version.h.new
            "#ifndef WAYLAND_VERSION_H\n#define WAYLAND_VERSION_H\n"
            "#define WAYLAND_VERSION_MAJOR 1\n#define WAYLAND_VERSION_MINOR 18\n"
            "#define WAYLAND_VERSION_MICRO 0\n#define WAYLAND_VERSION \"1.18.0\"\n#endif\n")
        foreach(_slash_header IN ITEMS expat_config.h wayland-version.h)
            file(COPY_FILE ${_slash_scanner_config}/${_slash_header}.new
                ${_slash_scanner_config}/${_slash_header} ONLY_IF_DIFFERENT)
        endforeach()
        set(_slash_expat ${expat_SOURCE_DIR}/expat/lib)
        add_library(slash_wayland_scanner STATIC
            ${_slash_expat}/xmlparse.c
            ${_slash_expat}/xmlrole.c
            ${_slash_expat}/xmltok.c
            ${_slash_expat}/${_slash_expat_random}
            ${wayland_SOURCE_DIR}/src/scanner.c
            ${wayland_SOURCE_DIR}/src/wayland-util.c)
        target_include_directories(slash_wayland_scanner BEFORE PRIVATE ${_slash_scanner_config}
            ${_slash_expat} ${wayland_SOURCE_DIR}/src)
        # The scanner's main() becomes a function slash-cli calls.
        target_compile_definitions(slash_wayland_scanner PRIVATE HAVE_EXPAT_CONFIG_H XML_STATIC
            HAVE_LIBXML=0 HAVE_STRNDUP=1 main=slash_wayland_scanner_main)
        target_compile_options(slash_wayland_scanner PRIVATE -w)
    endif()
endif()
