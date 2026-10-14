add_executable(slash_tests
    test_main.cpp
    test_fixed_step.cpp
    test_log.cpp
    test_platform.cpp
    test_result.cpp)
target_link_libraries(slash_tests PRIVATE slash::core slash::platform doctest::doctest)
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

    # Exercises the real-time windowed loop on machines with no display.
    add_test(NAME cli_run_windowed_offscreen COMMAND slash-cli run --ticks 30)
    set_tests_properties(cli_run_windowed_offscreen PROPERTIES
        ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen"
        PASS_REGULAR_EXPRESSION "\"ticks\":30,\"quit_requested\":false,\"headless\":false")
endif()
