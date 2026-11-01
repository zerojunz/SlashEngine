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

# Vulkan shaders see 16 KiB of each uniform push, as the other backends see all of it, MetalFX
# gets Metal's objects and ray tracing turns on Vulkan's ray query features (cmake/patches).
slash_add_dependency(SDL3
    SYSTEM TRUE
    PATCH_COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_LIST_DIR}/patches/sdl-vulkan-uniform-range.cmake
        COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_LIST_DIR}/patches/sdl-native-handles.cmake
        COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_LIST_DIR}/patches/sdl-ray-tracing.cmake
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

# Networking over reliable UDP, as Godot's ENetMultiplayerPeer: upstream ENet, built here from its
# C files (engine/net). Browsers have no UDP sockets, so the web has none.
if(NOT EMSCRIPTEN)
    slash_add_dependency(enet DOWNLOAD_ONLY TRUE)
    if(enet_ADDED)
        set(_slash_enet_sources callbacks.c compress.c host.c list.c packet.c peer.c protocol.c)
        if(WIN32)
            list(APPEND _slash_enet_sources win32.c)
        else()
            list(APPEND _slash_enet_sources unix.c)
        endif()
        list(TRANSFORM _slash_enet_sources PREPEND ${enet_SOURCE_DIR}/)
        add_library(slash_enet STATIC ${_slash_enet_sources})
        target_include_directories(slash_enet SYSTEM PUBLIC ${enet_SOURCE_DIR}/include)
        if(WIN32)
            target_link_libraries(slash_enet PUBLIC ws2_32 winmm)
            target_compile_definitions(slash_enet PRIVATE _WINSOCK_DEPRECATED_NO_WARNINGS
                _CRT_SECURE_NO_WARNINGS)
        elseif(NOT APPLE)
            # What ENet's own build finds on Linux and Android; unix.c sets Apple's itself.
            target_compile_definitions(slash_enet PRIVATE HAS_FCNTL=1 HAS_POLL=1
                HAS_GETADDRINFO=1 HAS_GETNAMEINFO=1 HAS_INET_PTON=1 HAS_INET_NTOP=1
                HAS_MSGHDR_FLAGS=1 HAS_SOCKLEN_T=1)
        endif()
    endif()
endif()

# WebSocket framing, as Godot's WSLPeer uses it (engine/net), over TCP sockets made with ENet's
# socket calls; not on the web, where the browser's WebSocket connects. Its handshake hashes
# with Mbed TLS's SHA-1.
if(NOT EMSCRIPTEN)
    slash_add_dependency(wslay DOWNLOAD_ONLY TRUE)
    if(wslay_ADDED)
        set(_slash_wslay_sources wslay_event.c wslay_frame.c wslay_net.c wslay_queue.c)
        if(EXISTS ${wslay_SOURCE_DIR}/lib/wslay_stack.c)
            list(APPEND _slash_wslay_sources wslay_stack.c)
        endif()
        list(TRANSFORM _slash_wslay_sources PREPEND ${wslay_SOURCE_DIR}/lib/)
        add_library(slash_wslay STATIC ${_slash_wslay_sources})
        target_include_directories(slash_wslay SYSTEM PUBLIC ${wslay_SOURCE_DIR}/lib/includes)
        # The version its configure step would write into wslayver.h.
        target_compile_definitions(slash_wslay PUBLIC WSLAY_VERSION="1.1.1")
        if(WIN32)
            target_compile_definitions(slash_wslay PRIVATE HAVE_WINSOCK2_H=1)
            target_link_libraries(slash_wslay PUBLIC ws2_32)
        else()
            target_compile_definitions(slash_wslay PRIVATE HAVE_ARPA_INET_H=1 HAVE_NETINET_IN_H=1)
        endif()
        if(MSVC)
            # MSVC has no ssize_t, which wslay's callbacks return.
            target_compile_definitions(slash_wslay PUBLIC ssize_t=ptrdiff_t)
        endif()
    endif()
endif()

# AV1 video, decoded by dav1d (engine/video) on every platform. Its own build is Meson's, so its
# C files are built here, without assembly so frames come out the same everywhere (dav1d is
# bit exact either way), with the config.h its Meson build would write: 8- and 16-bit pixels,
# no logging, one decoding thread unless asked.
slash_add_dependency(dav1d DOWNLOAD_ONLY TRUE)
if(dav1d_ADDED)
    set(_slash_dav1d_config ${CMAKE_BINARY_DIR}/dav1d-config)
    set(_slash_dav1d_defines
        ARCH_AARCH64=0 ARCH_ARM=0 ARCH_LOONGARCH=0 ARCH_LOONGARCH32=0 ARCH_LOONGARCH64=0
        ARCH_PPC64LE=0 ARCH_RISCV=0 ARCH_RV32=0 ARCH_RV64=0 ARCH_X86=0 ARCH_X86_32=0
        ARCH_X86_64=0 CONFIG_16BPC=1 CONFIG_8BPC=1 CONFIG_LOG=0 CONFIG_MACOS_KPERF=0
        ENDIANNESS_BIG=0 HAVE_ASM=0 HAVE_C11_GENERIC=0 HAVE_DLSYM=0 HAVE_ELF_AUX_INFO=0
        HAVE_GETAUXVAL=0 HAVE_MEMALIGN=0 HAVE_PTHREAD_GETAFFINITY_NP=0 HAVE_PTHREAD_NP_H=0
        HAVE_PTHREAD_SETAFFINITY_NP=0 HAVE_PTHREAD_SETNAME_NP=0 HAVE_PTHREAD_SET_NAME_NP=0
        HAVE_SYS_TYPES_H=1 TRIM_DSP_FUNCTIONS=0)
    if(WIN32)
        list(APPEND _slash_dav1d_defines HAVE_ALIGNED_ALLOC=0 HAVE_CLOCK_GETTIME=0 HAVE_IO_H=1
            HAVE_POSIX_MEMALIGN=0 HAVE_UNISTD_H=0 _WIN32_WINNT=0x0601 UNICODE=1 _UNICODE=1
            _CRT_DECLARE_NONSTDC_NAMES=1 __USE_MINGW_ANSI_STDIO=1)
    else()
        list(APPEND _slash_dav1d_defines HAVE_ALIGNED_ALLOC=0 HAVE_CLOCK_GETTIME=1 HAVE_IO_H=0
            HAVE_POSIX_MEMALIGN=1 HAVE_UNISTD_H=1)
    endif()
    set(_slash_dav1d_config_h "/* Written by cmake/Dependencies.cmake, as dav1d's Meson build would. */\n#pragma once\n")
    foreach(define IN LISTS _slash_dav1d_defines)
        string(REPLACE "=" " " define "${define}")
        string(APPEND _slash_dav1d_config_h "#define ${define}\n")
    endforeach()
    file(WRITE ${_slash_dav1d_config}/config.h.new "${_slash_dav1d_config_h}")
    file(WRITE ${_slash_dav1d_config}/vcs_version.h.new
        "/* auto-generated, do not edit */\n#define DAV1D_VERSION \"1.5.3\"\n")
    foreach(header IN ITEMS config.h vcs_version.h)
        file(COPY_FILE ${_slash_dav1d_config}/${header}.new ${_slash_dav1d_config}/${header}
            ONLY_IF_DIFFERENT)
    endforeach()

    set(_slash_dav1d_sources cdf.c cpu.c ctx.c data.c decode.c dequant_tables.c getbits.c
        intra_edge.c itx_1d.c lf_mask.c lib.c log.c mem.c msac.c obu.c pal.c picture.c qm.c ref.c
        refmvs.c scan.c tables.c thread_task.c warpmv.c wedge.c)
    if(WIN32)
        list(APPEND _slash_dav1d_sources win32/thread.c)
    endif()
    set(_slash_dav1d_bitdepth_sources cdef_apply_tmpl.c cdef_tmpl.c fg_apply_tmpl.c
        filmgrain_tmpl.c ipred_prepare_tmpl.c ipred_tmpl.c itx_tmpl.c lf_apply_tmpl.c
        loopfilter_tmpl.c looprestoration_tmpl.c lr_apply_tmpl.c mc_tmpl.c recon_tmpl.c)
    list(TRANSFORM _slash_dav1d_sources PREPEND ${dav1d_SOURCE_DIR}/src/)
    list(TRANSFORM _slash_dav1d_bitdepth_sources PREPEND ${dav1d_SOURCE_DIR}/src/)
    set(_slash_dav1d_includes ${_slash_dav1d_config} ${dav1d_SOURCE_DIR}
        ${dav1d_SOURCE_DIR}/include ${dav1d_SOURCE_DIR}/include/dav1d)
    if(MSVC)
        # MSVC's C has no <stdatomic.h>; dav1d carries one over its intrinsics.
        list(APPEND _slash_dav1d_includes ${dav1d_SOURCE_DIR}/include/compat/msvc)
    endif()
    add_library(slash_dav1d STATIC ${_slash_dav1d_sources})
    foreach(bitdepth IN ITEMS 8 16)
        # The templates once for each pixel size, as dav1d's build compiles them.
        add_library(slash_dav1d_${bitdepth} OBJECT ${_slash_dav1d_bitdepth_sources})
        target_compile_definitions(slash_dav1d_${bitdepth} PRIVATE BITDEPTH=${bitdepth})
        target_include_directories(slash_dav1d_${bitdepth} PRIVATE ${_slash_dav1d_includes})
        target_sources(slash_dav1d PRIVATE $<TARGET_OBJECTS:slash_dav1d_${bitdepth}>)
        set_target_properties(slash_dav1d_${bitdepth} PROPERTIES C_STANDARD 99
            POSITION_INDEPENDENT_CODE ON)
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR ANDROID OR EMSCRIPTEN)
            target_compile_definitions(slash_dav1d_${bitdepth} PRIVATE _GNU_SOURCE)
        endif()
    endforeach()
    target_include_directories(slash_dav1d PRIVATE ${_slash_dav1d_includes})
    target_include_directories(slash_dav1d SYSTEM PUBLIC ${dav1d_SOURCE_DIR}/include)
    set_target_properties(slash_dav1d PROPERTIES C_STANDARD 99 POSITION_INDEPENDENT_CODE ON)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux" OR ANDROID OR EMSCRIPTEN)
        target_compile_definitions(slash_dav1d PRIVATE _GNU_SOURCE)
    endif()
    if(NOT WIN32 AND NOT EMSCRIPTEN)
        find_package(Threads REQUIRED)
        target_link_libraries(slash_dav1d PRIVATE Threads::Threads)
    endif()
    if(NOT MSVC)
        target_compile_options(slash_dav1d PRIVATE -w)
        foreach(bitdepth IN ITEMS 8 16)
            target_compile_options(slash_dav1d_${bitdepth} PRIVATE -w)
        endforeach()
    endif()
endif()

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
# Convex decomposition, with V-HACD 4.1 as Godot's importer decomposes meshes into collision hulls
# (engine/scene3d builds its one header through src/vhacd_build.cpp; the patch makes its hulls the
# same everywhere).
slash_add_dependency(vhacd
    DOWNLOAD_ONLY TRUE
    PATCH_COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_LIST_DIR}/patches/vhacd-deterministic.cmake)

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

    # Lossy AV1 video (slash::video_encoder): libaom's encoder, for the tools and tests to make
    # clips the engine's own palette encoder can't. Games only decode, with dav1d. libaom's own
    # CMake writes its compiler flags into the cache, where every other target would get them,
    # so it builds as a project of its own, from a custom command: generic C (no assembly, so no NASM), one thread, 8-bit,
    # without its decoder, apps, examples or tests, and optimized even in Debug builds, with the C
    # runtime the rest of the build links. It needs Perl, which Git for Windows carries.
    slash_add_dependency(libaom DOWNLOAD_ONLY TRUE)
    if(libaom_ADDED)
        find_program(PERL_EXECUTABLE perl PATHS "$ENV{ProgramFiles}/Git/usr/bin")
        if(NOT PERL_EXECUTABLE)
            message(FATAL_ERROR "libaom's build needs Perl: install it, or Git for Windows")
        endif()
        set(_slash_aom_build ${CMAKE_BINARY_DIR}/_deps/libaom-build)
        set(_slash_aom_library
            ${_slash_aom_build}/${CMAKE_STATIC_LIBRARY_PREFIX}aom${CMAKE_STATIC_LIBRARY_SUFFIX})
        set(_slash_aom_args
            -DCMAKE_BUILD_TYPE=Release
            -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
            -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
            -DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}
            -DPERL_EXECUTABLE=${PERL_EXECUTABLE}
            -DAOM_TARGET_CPU=generic
            -DCONFIG_AV1_DECODER=0
            -DCONFIG_AV1_HIGHBITDEPTH=0
            -DCONFIG_LIBYUV=0
            -DCONFIG_MULTITHREAD=0
            -DCONFIG_PIC=1
            -DCONFIG_RUNTIME_CPU_DETECT=0
            -DCONFIG_WEBM_IO=0
            -DENABLE_APPS=0
            -DENABLE_DOCS=0
            -DENABLE_EXAMPLES=0
            -DENABLE_TESTDATA=0
            -DENABLE_TESTS=0
            -DENABLE_TOOLS=0)
        foreach(_slash_var IN ITEMS CMAKE_C_COMPILER_LAUNCHER CMAKE_CXX_COMPILER_LAUNCHER
                CMAKE_OSX_ARCHITECTURES CMAKE_OSX_DEPLOYMENT_TARGET CMAKE_OSX_SYSROOT)
            if(${_slash_var})
                list(APPEND _slash_aom_args "-D${_slash_var}=${${_slash_var}}")
            endif()
        endforeach()
        if(MSVC)
            if(CMAKE_BUILD_TYPE STREQUAL "Debug")
                list(APPEND _slash_aom_args -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDebugDLL)
            else()
                list(APPEND _slash_aom_args -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL)
            endif()
        else()
            # No fused multiply-adds, which arm64 compilers would emit and x86-64 ones not.
            list(APPEND _slash_aom_args -DCMAKE_C_FLAGS=-ffp-contract=off
                -DCMAKE_CXX_FLAGS=-ffp-contract=off)
        endif()
        # Configured and built when its library is first needed, and again when these settings
        # (and so the command) change. Not ExternalProject: nothing is downloaded here.
        add_custom_command(OUTPUT ${_slash_aom_library}
            COMMAND ${CMAKE_COMMAND} -G ${CMAKE_GENERATOR} -S ${libaom_SOURCE_DIR}
                -B ${_slash_aom_build} ${_slash_aom_args}
            COMMAND ${CMAKE_COMMAND} --build ${_slash_aom_build} --target aom
            COMMENT "Building libaom"
            VERBATIM)
        add_custom_target(slash_aom_build DEPENDS ${_slash_aom_library})
        add_library(slash_aom STATIC IMPORTED GLOBAL)
        set_target_properties(slash_aom PROPERTIES IMPORTED_LOCATION ${_slash_aom_library})
        target_include_directories(slash_aom SYSTEM INTERFACE ${libaom_SOURCE_DIR})
        if(UNIX AND NOT APPLE)
            target_link_libraries(slash_aom INTERFACE m)
        endif()
        add_dependencies(slash_aom slash_aom_build)
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
