add_executable(slash_tests
    test_main.cpp
    test_fixed_step.cpp
    test_json.cpp
    test_json_schema.cpp
    test_log.cpp
    test_platform.cpp
    test_reflect.cpp
    test_result.cpp
    test_scene.cpp
    test_world.cpp)
target_link_libraries(slash_tests PRIVATE slash::core slash::platform slash::world doctest::doctest)
# doctest forward-declares std::ostream by default, which breaks string_view checks on recent
# MSVC standard libraries; include the real headers instead.
target_compile_definitions(slash_tests PRIVATE DOCTEST_CONFIG_USE_STD_HEADERS)
slash_target_warnings(slash_tests)

add_test(NAME unit COMMAND slash_tests WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

if(SLASH_BUILD_TOOLS)
    set(scenes ${CMAKE_CURRENT_SOURCE_DIR}/data)

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
        PASS_REGULAR_EXPRESSION "\"headless\":false,\"quit_requested\":false,\"ticks\":30")

    add_test(NAME cli_schema_scene COMMAND slash-cli schema scene)
    set_tests_properties(cli_schema_scene PROPERTIES
        PASS_REGULAR_EXPRESSION "\"title\": \"SlashEngine scene\"")

    add_test(NAME cli_scene_validate COMMAND slash-cli scene validate ${scenes}/basic.scene.json)
    set_tests_properties(cli_scene_validate PROPERTIES
        PASS_REGULAR_EXPRESSION "{\"entities\":3,\"valid\":true}")

    add_test(NAME cli_scene_validate_invalid
        COMMAND slash-cli scene validate ${scenes}/invalid.scene.json)
    set_tests_properties(cli_scene_validate_invalid PROPERTIES
        PASS_REGULAR_EXPRESSION "did you mean \\\\\"Transform3D\\\\\"")

    add_test(NAME cli_scene_format_check
        COMMAND slash-cli scene format --check ${scenes}/basic.scene.json)
    set_tests_properties(cli_scene_format_check PROPERTIES
        PASS_REGULAR_EXPRESSION "{\"canonical\":true}")

    # Load a scene, tick it headless, save it, and check the saved file matches the original.
    add_test(NAME cli_run_scene_save
        COMMAND slash-cli run --headless --ticks 10 --scene ${scenes}/basic.scene.json
            --save ${CMAKE_CURRENT_BINARY_DIR}/roundtrip.scene.json)
    set_tests_properties(cli_run_scene_save PROPERTIES
        PASS_REGULAR_EXPRESSION "\"entities\":3"
        FIXTURES_SETUP roundtrip_scene)
    add_test(NAME cli_run_scene_roundtrip
        COMMAND ${CMAKE_COMMAND} -E compare_files
            ${scenes}/basic.scene.json ${CMAKE_CURRENT_BINARY_DIR}/roundtrip.scene.json)
    set_tests_properties(cli_run_scene_roundtrip PROPERTIES FIXTURES_REQUIRED roundtrip_scene)
endif()
