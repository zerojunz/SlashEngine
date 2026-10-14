# Third-party dependencies, pinned to exact tags. Every entry must be listed in THIRD_PARTY.md
# with its license; only permissive licenses are allowed (see docs/design.md).

include(CPM)

CPMAddPackage(
    NAME SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-3.4.18
    GIT_SHALLOW TRUE
    SYSTEM TRUE
    OPTIONS
        "SDL_SHARED OFF"
        "SDL_STATIC ON"
        "SDL_TEST_LIBRARY OFF"
        "SDL_TESTS OFF"
        "SDL_EXAMPLES OFF"
        "SDL_INSTALL OFF")

if(SLASH_BUILD_TESTS)
    CPMAddPackage(
        NAME doctest
        GIT_REPOSITORY https://github.com/doctest/doctest.git
        GIT_TAG v2.5.2
        GIT_SHALLOW TRUE
        SYSTEM TRUE
        OPTIONS
            "DOCTEST_WITH_TESTS OFF"
            "DOCTEST_NO_INSTALL ON")
endif()
