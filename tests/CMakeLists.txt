add_executable(slash_tests
    test_main.cpp
    test_fixed_step.cpp
    test_log.cpp
    test_platform.cpp
    test_result.cpp)
target_link_libraries(slash_tests PRIVATE slash::core slash::platform doctest::doctest)
# doctest forward-declares std::ostream by default, which breaks string_view checks on recent
# MSVC standard libraries; include the real headers instead.
target_compile_definitions(slash_tests PRIVATE DOCTEST_CONFIG_USE_STD_HEADERS)
slash_target_warnings(slash_tests)

add_test(NAME unit COMMAND slash_tests)

if(SLASH_BUILD_TOOLS)
    add_test(NAME cli_version COMMAND slash-cli version)
    set_tests_properties(cli_version PROPERTIES
        PASS_REGULAR_EXPRESSION "\"version\":\"${PROJECT_VERSION}\"")

    add_test(NAME cli_run_headless COMMAND slash-cli run --headless --ticks 120)
    set_tests_properties(cli_run_headless PROPERTIES
        PASS_REGULAR_EXPRESSION "\"ticks\":120")

    add_test(NAME cli_headless_needs_ticks COMMAND slash-cli run --headless)
    set_tests_properties(cli_headless_needs_ticks PROPERTIES WILL_FAIL TRUE)

    # Exercises the real-time windowed loop on machines with no display. SDL's "dummy" video
    # driver works on every platform; "offscreen" needs EGL, which macOS lacks.
    add_test(NAME cli_run_windowed_dummy_video COMMAND slash-cli run --ticks 30)
    set_tests_properties(cli_run_windowed_dummy_video PROPERTIES
        ENVIRONMENT "SDL_VIDEO_DRIVER=dummy"
        PASS_REGULAR_EXPRESSION "\"ticks\":30,\"quit_requested\":false,\"headless\":false")
endif()
