# The stand-in vendor SDK, built by each export with the game's compilers (sdk.json's "build").
# A real SDK ships these binaries instead. Shared where the platform ships shared libraries with
# apps; static on iOS and the web.
cmake_minimum_required(VERSION 3.25)

project(Hello LANGUAGES C)

if(IOS OR EMSCRIPTEN)
    add_library(hello STATIC hello.c)
else()
    add_library(hello SHARED hello.c)
endif()
target_compile_definitions(hello PRIVATE HELLO_BUILDING)
set_target_properties(hello PROPERTIES C_VISIBILITY_PRESET hidden)
if(ANDROID)
    # Google Play requires native code to support 16 KB memory pages.
    target_link_options(hello PRIVATE "-Wl,-z,max-page-size=16384")
endif()
if(WIN32)
    # hello.dll and its import library hello.lib, with MSVC and MinGW alike.
    set_target_properties(hello PROPERTIES PREFIX "" IMPORT_PREFIX "" IMPORT_SUFFIX ".lib")
endif()

install(TARGETS hello RUNTIME DESTINATION bin LIBRARY DESTINATION lib ARCHIVE DESTINATION lib)
install(FILES hello.h DESTINATION include)
