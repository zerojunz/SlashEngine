add_executable(slash_tests
    test_main.cpp
    test_android_library.cpp
    test_animation.cpp
    test_asset.cpp
    test_audio.cpp
    test_camera2d.cpp
    test_canvas_layer.cpp
    test_canvas.cpp
    test_command.cpp
    test_debug.cpp
    test_exporter.cpp
    test_fixed_step.cpp
    test_frame_summary.cpp
    test_gameplay.cpp
    test_godot_pack.cpp
    test_gpu_particles.cpp
    test_gpu_particles_2d.cpp
    test_import_godot.cpp
    test_import_unity.cpp
    test_gui.cpp
    test_input.cpp
    test_json.cpp
    test_json_schema.cpp
    test_light.cpp
    test_line_edit.cpp
    test_log.cpp
    test_material.cpp
    test_mcp.cpp
    test_particles.cpp
    test_physics.cpp
    test_platform.cpp
    test_reflect.cpp
    test_render.cpp
    test_replay.cpp
    test_result.cpp
    test_rpc.cpp
    test_runtime.cpp
    test_scene.cpp
    test_sdk.cpp
    test_server.cpp
    test_shader.cpp
    test_storyboard.cpp
    test_syntax_highlighter.cpp
    test_test_run.cpp
    test_text.cpp
    test_text_edit.cpp
    test_tilemap.cpp
    test_tree2d.cpp
    test_ui.cpp
    test_ui_containers.cpp
    test_ui_controls.cpp
    test_ui_menus.cpp
    test_ui_drag.cpp
    test_ui_scroll.cpp
    test_unity_build.cpp
    test_world.cpp)
target_link_libraries(slash_tests PRIVATE
    slash::animation slash::asset slash::audio slash::canvas slash::command slash::core slash::debug slash::exporter slash::importer slash::input slash::physics2d
    slash::platform slash::render slash::runtime slash::server slash::shader_compiler slash::storyboard slash::text slash::ui slash::world doctest::doctest SDL3::SDL3-static
    webp) # WebP textures for the Godot unpacking tests
# doctest forward-declares std::ostream by default, which breaks string_view checks on recent
# MSVC standard libraries; include the real headers instead.
target_compile_definitions(slash_tests PRIVATE DOCTEST_CONFIG_USE_STD_HEADERS
    SLASH_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data" SLASH_TEST_CMAKE="${CMAKE_COMMAND}")
slash_target_warnings(slash_tests)
slash_gameplay_host(slash_tests)

# A stand-in for AssetRipper's web interface, so the Unity build tests and the unpack-unity
# example run without downloading it.
add_executable(fake-assetripper fake_assetripper.cpp)
target_link_libraries(fake-assetripper PRIVATE slash_httplib)
slash_target_warnings(fake-assetripper)
add_dependencies(slash_tests fake-assetripper)
target_compile_definitions(slash_tests PRIVATE
    SLASH_FAKE_ASSETRIPPER="$<TARGET_FILE:fake-assetripper>")

# Gameplay libraries for test_gameplay.cpp, built as cmake/gameplay builds a game's code: with the
# engine's headers, and its functions left for the program that loads them.
if(APPLE OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
    foreach(variant IN ITEMS v1 v2 v3 duplicate engine_component engine_system no_entry bad_abi)
        string(TOUPPER ${variant} upper)
        add_library(gameplay_${variant} MODULE gameplay_module.cpp)
        set_target_properties(gameplay_${variant} PROPERTIES PREFIX "" SUFFIX ".so"
            LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/gameplay_modules)
        target_include_directories(gameplay_${variant} PRIVATE
            $<TARGET_PROPERTY:slash_runtime,INTERFACE_INCLUDE_DIRECTORIES>)
        target_compile_definitions(gameplay_${variant} PRIVATE SLASH_TEST_MODULE_${upper}
            $<TARGET_PROPERTY:slash_runtime,INTERFACE_COMPILE_DEFINITIONS>)
        target_compile_options(gameplay_${variant} PRIVATE -fno-exceptions -fno-rtti
            $<$<CXX_COMPILER_ID:GNU>:-fno-gnu-unique>)
        if(APPLE)
            target_link_options(gameplay_${variant} PRIVATE -undefined dynamic_lookup)
        endif()
        add_dependencies(slash_tests gameplay_${variant})
    endforeach()
    target_compile_definitions(slash_tests PRIVATE
        SLASH_TEST_GAMEPLAY_DIR="${CMAKE_CURRENT_BINARY_DIR}/gameplay_modules")
endif()

add_test(NAME unit COMMAND slash_tests WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

# The compiled shaders committed in engine/render/src match their HLSL; see
# cmake/compile_shaders.cmake.
foreach(shader IN ITEMS sprite material particles_process particles_draw)
    add_test(NAME shaders_up_to_date_${shader}
        COMMAND ${CMAKE_COMMAND} -DSOURCE=${PROJECT_SOURCE_DIR}/engine/render/shaders/${shader}.hlsl
            -DOUTPUT=${PROJECT_SOURCE_DIR}/engine/render/src/${shader}_shaders.hpp -DCHECK=ON
            -P ${PROJECT_SOURCE_DIR}/cmake/compile_shaders.cmake)
endforeach()

# Every dependency this build fetched is pinned in cmake/dependencies.json with an allowed license
# and the license text that was reviewed; see cmake/check_licenses.cmake.
set(license_args "")
foreach(dependency IN LISTS SLASH_DEPENDENCIES)
    list(APPEND license_args "-DSOURCE_${dependency}=${${dependency}_SOURCE_DIR}")
endforeach()
list(JOIN SLASH_DEPENDENCIES "|" fetched_dependencies)
add_test(NAME licenses
    COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR} -DDEPENDENCIES=${fetched_dependencies}
        ${license_args} -P ${PROJECT_SOURCE_DIR}/cmake/check_licenses.cmake)
# A fixture that breaks every rule, to show the check catches each one.
set(license_fixture ${CMAKE_CURRENT_SOURCE_DIR}/data/licenses)
add_test(NAME licenses_fixture
    COMMAND ${CMAKE_COMMAND} -DROOT=${license_fixture}
        "-DDEPENDENCIES=good|changed|copyleft|branch|extra"
        -DSOURCE_good=${license_fixture}/sources/good
        -DSOURCE_changed=${license_fixture}/sources/changed
        -DSOURCE_copyleft=${license_fixture}/sources/none
        -DSOURCE_branch=${license_fixture}/sources/good
        -P ${PROJECT_SOURCE_DIR}/cmake/check_licenses.cmake)
set_tests_properties(licenses_fixture PROPERTIES PASS_REGULAR_EXPRESSION
    "branch is pinned to the branch .main.*license of changed v2[.]0 is not the reviewed text.*copyleft is under .GPL-3[.]0-only OR LGPL-2[.]1-only., which is not allowed.*no row linking https://example[.]com/copyleft.*copyleft v1[.]0 has no COPYING.*extra is fetched but not in.*cmake/gone[.]cmake is listed as vendored but does not exist.*engine/copied[.]c carries a copyleft notice.*tools/CMakeLists[.]txt fetches code without")

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
    add_test(NAME cli_run_frame_settings
        COMMAND slash-cli run --headless --ticks 5 --vsync mailbox --max-fps 30)
    set_tests_properties(cli_run_frame_settings PROPERTIES PASS_REGULAR_EXPRESSION "\"ticks\":5")
    add_test(NAME cli_run_bad_vsync COMMAND slash-cli run --headless --ticks 5 --vsync on)
    set_tests_properties(cli_run_bad_vsync PROPERTIES WILL_FAIL TRUE)
    add_test(NAME cli_run_bad_max_fps COMMAND slash-cli run --headless --ticks 5 --max-fps 1001)
    set_tests_properties(cli_run_bad_max_fps PROPERTIES WILL_FAIL TRUE)

    # Exercises the real-time windowed loop on machines with no display. SDL's "dummy" video
    # driver works on every platform; "offscreen" needs EGL, which macOS lacks.
    add_test(NAME cli_run_windowed_dummy_video COMMAND slash-cli run --ticks 30)
    set_tests_properties(cli_run_windowed_dummy_video PROPERTIES
        ENVIRONMENT "SDL_VIDEO_DRIVER=dummy"
        PASS_REGULAR_EXPRESSION "\"headless\":false,\"quit_requested\":false,\"renderer\":\"null\",\"ticks\":30")

    # A window drawn on the GPU, where SDL's offscreen video driver can make a Vulkan surface. CI
    # sets SLASH_EXPECT_GPU on Linux, where software Vulkan is installed; elsewhere the run may
    # fall back to the null device.
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        if(NOT "$ENV{SLASH_EXPECT_GPU}" STREQUAL "")
            set(windowed_renderer "vulkan")
        else()
            set(windowed_renderer "[a-z0-9]+")
        endif()
        add_test(NAME cli_run_windowed_gpu COMMAND slash-cli run --ticks 30)
        set_tests_properties(cli_run_windowed_gpu PROPERTIES
            ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen"
            PASS_REGULAR_EXPRESSION "\"headless\":false,\"quit_requested\":false,\"renderer\":\"${windowed_renderer}\",\"ticks\":30")
    endif()

    add_test(NAME cli_schema_scene COMMAND slash-cli schema scene)
    set_tests_properties(cli_schema_scene PROPERTIES
        PASS_REGULAR_EXPRESSION "\"title\": \"SlashEngine scene\"")

    add_test(NAME cli_scene_validate COMMAND slash-cli scene validate ${scenes}/basic.scene.json)
    set_tests_properties(cli_scene_validate PROPERTIES
        PASS_REGULAR_EXPRESSION "{\"entities\":3,\"valid\":true}")

    # Prefabs are read from the project the file is in: the nearest folder with a project.json.
    add_test(NAME cli_scene_validate_prefab
        COMMAND slash-cli scene validate ${scenes}/game/prefabs/marker.prefab.json)
    set_tests_properties(cli_scene_validate_prefab PROPERTIES
        PASS_REGULAR_EXPRESSION "{\"components\":1,\"valid\":true}")
    add_test(NAME cli_scene_validate_extends
        COMMAND slash-cli scene format --check ${scenes}/game/scenes/main.scene.json)
    set_tests_properties(cli_scene_validate_extends PROPERTIES
        PASS_REGULAR_EXPRESSION "{\"canonical\":true}")
    add_test(NAME cli_schema_prefab COMMAND slash-cli schema prefab)
    set_tests_properties(cli_schema_prefab PROPERTIES
        PASS_REGULAR_EXPRESSION "\"title\": \"SlashEngine prefab\"")

    add_test(NAME cli_schema_frames COMMAND slash-cli schema frames)
    set_tests_properties(cli_schema_frames PROPERTIES
        PASS_REGULAR_EXPRESSION "\"title\": \"SlashEngine sprite frames\"")
    # The sprites example's .frames.json file, which must stay valid and canonical.
    add_test(NAME cli_scene_validate_frames
        COMMAND slash-cli scene validate ${PROJECT_SOURCE_DIR}/examples/sprites/hero.frames.json)
    set_tests_properties(cli_scene_validate_frames PROPERTIES
        PASS_REGULAR_EXPRESSION "{\"animations\":2,\"valid\":true}")
    add_test(NAME cli_scene_format_frames COMMAND slash-cli scene format --check
        ${PROJECT_SOURCE_DIR}/examples/sprites/hero.frames.json)
    set_tests_properties(cli_scene_format_frames PROPERTIES
        PASS_REGULAR_EXPRESSION "{\"canonical\":true}")
    add_test(NAME cli_scene_validate_invalid_frames
        COMMAND slash-cli scene validate ${scenes}/invalid.frames.json)
    set_tests_properties(cli_scene_validate_invalid_frames PROPERTIES
        PASS_REGULAR_EXPRESSION "\"message\":\"the sheet has 4 frames, numbered from 0\",\"path\":\"/animations/walk/frames/1\"")

    add_test(NAME cli_schema_material COMMAND slash-cli schema material)
    set_tests_properties(cli_schema_material PROPERTIES
        PASS_REGULAR_EXPRESSION "\"title\": \"SlashEngine material\"")
    # The materials example's files, which must stay valid and canonical.
    foreach(material IN ITEMS hit_flash water glow)
        set(material_file ${PROJECT_SOURCE_DIR}/examples/materials/${material}.material.json)
        add_test(NAME cli_scene_validate_material_${material}
            COMMAND slash-cli scene validate ${material_file})
        set_tests_properties(cli_scene_validate_material_${material} PROPERTIES
            PASS_REGULAR_EXPRESSION "{\"instructions\":[0-9]+,\"valid\":true}")
        add_test(NAME cli_scene_format_material_${material}
            COMMAND slash-cli scene format --check ${material_file})
        set_tests_properties(cli_scene_format_material_${material} PROPERTIES
            PASS_REGULAR_EXPRESSION "{\"canonical\":true}")
    endforeach()
    add_test(NAME cli_scene_validate_invalid_material
        COMMAND slash-cli scene validate ${scenes}/invalid.material.json)
    set_tests_properties(cli_scene_validate_invalid_material PROPERTIES
        PASS_REGULAR_EXPRESSION "\"message\":\"no op \\\\\"mull\\\\\"; did you mean \\\\\"mul\\\\\"[?]\",\"path\":\"/color/op\"")

    add_test(NAME cli_schema_animation COMMAND slash-cli schema animation)
    set_tests_properties(cli_schema_animation PROPERTIES
        PASS_REGULAR_EXPRESSION "\"title\": \"SlashEngine animation library\"")
    # The keyframe animation example's library, which must stay valid and canonical.
    set(animation_file ${PROJECT_SOURCE_DIR}/examples/keyframe-animation/hero.anim.json)
    add_test(NAME cli_scene_validate_animation COMMAND slash-cli scene validate ${animation_file})
    set_tests_properties(cli_scene_validate_animation PROPERTIES
        PASS_REGULAR_EXPRESSION "{\"animations\":2,\"valid\":true}")
    add_test(NAME cli_scene_format_animation
        COMMAND slash-cli scene format --check ${animation_file})
    set_tests_properties(cli_scene_format_animation PROPERTIES
        PASS_REGULAR_EXPRESSION "{\"canonical\":true}")
    add_test(NAME cli_scene_validate_invalid_animation
        COMMAND slash-cli scene validate ${scenes}/invalid.anim.json)
    set_tests_properties(cli_scene_validate_invalid_animation PROPERTIES
        PASS_REGULAR_EXPRESSION "\"message\":\"Transform2D has no field \\\\\"positon\\\\\"; did you mean \\\\\"position\\\\\"[?]\",\"path\":\"/animations/bob/tracks/0/path\"")

    add_test(NAME cli_schema_tileset COMMAND slash-cli schema tileset)
    set_tests_properties(cli_schema_tileset PROPERTIES
        PASS_REGULAR_EXPRESSION "\"title\": \"SlashEngine tile set\"")
    # The tile maps example's tile set and scene, which must stay valid and canonical.
    set(tilemaps ${PROJECT_SOURCE_DIR}/examples/tilemaps)
    add_test(NAME cli_scene_validate_tileset
        COMMAND slash-cli scene validate ${tilemaps}/ground.tileset.json)
    set_tests_properties(cli_scene_validate_tileset PROPERTIES
        PASS_REGULAR_EXPRESSION "{\"tiles\":16,\"valid\":true}")
    add_test(NAME cli_scene_format_tileset
        COMMAND slash-cli scene format --check ${tilemaps}/ground.tileset.json)
    set_tests_properties(cli_scene_format_tileset PROPERTIES
        PASS_REGULAR_EXPRESSION "{\"canonical\":true}")
    add_test(NAME cli_scene_validate_invalid_tileset
        COMMAND slash-cli scene validate ${scenes}/invalid.tileset.json)
    set_tests_properties(cli_scene_validate_invalid_tileset PROPERTIES
        PASS_REGULAR_EXPRESSION "\"message\":\"no terrain set has a terrain named \\\\\"gras\\\\\"; did you mean \\\\\"grass\\\\\"[?]\",\"path\":\"/sources/ground/tiles/0,0/terrain\"")
    # Scenes: each layer's legend and rows are checked against its tile set, read from the
    # project folder (here, the working directory).
    add_test(NAME cli_scene_validate_tile_layers
        COMMAND slash-cli scene validate ${tilemaps}/main.scene.json
        WORKING_DIRECTORY ${tilemaps})
    set_tests_properties(cli_scene_validate_tile_layers PROPERTIES
        PASS_REGULAR_EXPRESSION "{\"entities\":1,\"valid\":true}")
    add_test(NAME cli_scene_validate_invalid_tile_layer
        COMMAND slash-cli scene validate ${scenes}/invalid_tiles.scene.json
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    set_tests_properties(cli_scene_validate_invalid_tile_layer PROPERTIES
        PASS_REGULAR_EXPRESSION "\"path\":\"/entities/map/components/TileMapLayer/legend/w\"")

    add_test(NAME cli_schema_font COMMAND slash-cli schema font)
    set_tests_properties(cli_schema_font PROPERTIES
        PASS_REGULAR_EXPRESSION "\"title\": \"SlashEngine font\"")
    # The UI example's font settings and scene, which must stay valid and canonical. Its scene
    # names a font file the example writes when it runs, so only its form is checked here.
    set(ui_example ${PROJECT_SOURCE_DIR}/examples/ui-and-text)
    add_test(NAME cli_scene_validate_font
        COMMAND slash-cli scene validate ${ui_example}/fonts/title.font.json)
    set_tests_properties(cli_scene_validate_font PROPERTIES
        PASS_REGULAR_EXPRESSION "{\"valid\":true}")
    add_test(NAME cli_scene_format_font
        COMMAND slash-cli scene format --check ${ui_example}/fonts/title.font.json)
    set_tests_properties(cli_scene_format_font PROPERTIES
        PASS_REGULAR_EXPRESSION "{\"canonical\":true}")
    add_test(NAME cli_scene_format_ui
        COMMAND slash-cli scene format --check ${ui_example}/main.scene.json)
    set_tests_properties(cli_scene_format_ui PROPERTIES
        PASS_REGULAR_EXPRESSION "{\"canonical\":true}")
    add_test(NAME cli_scene_validate_invalid_font
        COMMAND slash-cli scene validate ${scenes}/invalid.font.json)
    set_tests_properties(cli_scene_validate_invalid_font PROPERTIES
        PASS_REGULAR_EXPRESSION "\"message\":\"expected a .ttf or .otf file\",\"path\":\"/font\"")
    # Scenes: controls must lay out as written.
    add_test(NAME cli_scene_validate_invalid_ui
        COMMAND slash-cli scene validate ${scenes}/invalid_ui.scene.json
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
    set_tests_properties(cli_scene_validate_invalid_ui PROPERTIES
        PASS_REGULAR_EXPRESSION "\"message\":\"a control can't be its own child\",\"path\":\"/entities/panel/components/Control/children/1\"}.*\"path\":\"/entities/hint/components/Control/theme_override_colors/font_colour\"")

    add_test(NAME cli_schema_theme COMMAND slash-cli schema theme)
    set_tests_properties(cli_schema_theme PROPERTIES
        PASS_REGULAR_EXPRESSION "\"title\": \"SlashEngine theme\"")
    # The containers example's theme and scene, which must stay valid and canonical. Its scene
    # names textures the example writes when it runs, so only its form is checked here.
    set(containers_example ${PROJECT_SOURCE_DIR}/examples/ui-containers)
    add_test(NAME cli_scene_validate_theme
        COMMAND slash-cli scene validate ${containers_example}/ui/game.theme.json)
    set_tests_properties(cli_scene_validate_theme PROPERTIES
        PASS_REGULAR_EXPRESSION "{\"types\":8,\"valid\":true}")
    add_test(NAME cli_scene_format_theme
        COMMAND slash-cli scene format --check ${containers_example}/ui/game.theme.json)
    set_tests_properties(cli_scene_format_theme PROPERTIES
        PASS_REGULAR_EXPRESSION "{\"canonical\":true}")
    add_test(NAME cli_scene_format_ui_containers
        COMMAND slash-cli scene format --check ${containers_example}/main.scene.json)
    set_tests_properties(cli_scene_format_ui_containers PROPERTIES
        PASS_REGULAR_EXPRESSION "{\"canonical\":true}")
    add_test(NAME cli_scene_validate_invalid_theme
        COMMAND slash-cli scene validate ${scenes}/invalid.theme.json)
    set_tests_properties(cli_scene_validate_invalid_theme PROPERTIES
        PASS_REGULAR_EXPRESSION "\"message\":\"Label has no colors item \\\\\"font_colour\\\\\"; did you mean \\\\\"font_color\\\\\"[?]\",\"path\":\"/types/Label/colors/font_colour\"")

    add_test(NAME cli_schema_audio COMMAND slash-cli schema audio)
    set_tests_properties(cli_schema_audio PROPERTIES
        PASS_REGULAR_EXPRESSION "\"title\": \"SlashEngine audio stream\"")
    add_test(NAME cli_schema_bus_layout COMMAND slash-cli schema bus_layout)
    set_tests_properties(cli_schema_bus_layout PROPERTIES
        PASS_REGULAR_EXPRESSION "\"title\": \"SlashEngine bus layout\"")
    # The audio example's bus layout and randomizer, which must stay valid and canonical.
    set(audio_example ${PROJECT_SOURCE_DIR}/examples/audio)
    add_test(NAME cli_scene_validate_bus_layout
        COMMAND slash-cli scene validate ${audio_example}/audio/default.bus_layout.json)
    set_tests_properties(cli_scene_validate_bus_layout PROPERTIES
        PASS_REGULAR_EXPRESSION "{\"buses\":3,\"valid\":true}")
    add_test(NAME cli_scene_format_bus_layout
        COMMAND slash-cli scene format --check ${audio_example}/audio/default.bus_layout.json)
    set_tests_properties(cli_scene_format_bus_layout PROPERTIES
        PASS_REGULAR_EXPRESSION "{\"canonical\":true}")
    add_test(NAME cli_scene_validate_audio_stream
        COMMAND slash-cli scene validate ${audio_example}/audio/steps.audio.json)
    set_tests_properties(cli_scene_validate_audio_stream PROPERTIES
        PASS_REGULAR_EXPRESSION "{\"valid\":true}")
    add_test(NAME cli_scene_format_audio_stream
        COMMAND slash-cli scene format --check ${audio_example}/audio/steps.audio.json)
    set_tests_properties(cli_scene_format_audio_stream PROPERTIES
        PASS_REGULAR_EXPRESSION "{\"canonical\":true}")
    add_test(NAME cli_scene_format_audio_scene
        COMMAND slash-cli scene format --check ${audio_example}/main.scene.json)
    set_tests_properties(cli_scene_format_audio_scene PROPERTIES
        PASS_REGULAR_EXPRESSION "{\"canonical\":true}")
    add_test(NAME cli_scene_validate_invalid_bus_layout
        COMMAND slash-cli scene validate ${scenes}/invalid.bus_layout.json)
    set_tests_properties(cli_scene_validate_invalid_bus_layout PROPERTIES
        PASS_REGULAR_EXPRESSION "got \\\\\"revreb\\\\\"; did you mean \\\\\"reverb\\\\\"[?]\",\"path\":\"/buses/1/effects/0/type\"")

    add_test(NAME cli_schema_storyboard COMMAND slash-cli schema storyboard)
    set_tests_properties(cli_schema_storyboard PROPERTIES
        PASS_REGULAR_EXPRESSION "\"title\": \"SlashEngine storyboard file\"")

    add_test(NAME cli_storyboard_query COMMAND slash-cli call storyboard.query
        "{\"linked\": \"prefabs/marker.prefab.json\"}" --project ${scenes}/game)
    set_tests_properties(cli_storyboard_query PROPERTIES
        PASS_REGULAR_EXPRESSION "\"id\":\"place/landing_pad\",\"version\":\"[0-9a-f]+\"")

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

    # Record a headless run into a project, check the file, then play it back to the same end.
    set(replay_project ${CMAKE_CURRENT_BINARY_DIR}/cli_replay_project)
    add_test(NAME cli_run_record
        COMMAND slash-cli run --headless --ticks 90 --scene ${scenes}/basic.scene.json
            --project ${replay_project} --record ${replay_project}/runs/basic.replay.json)
    set_tests_properties(cli_run_record PROPERTIES
        PASS_REGULAR_EXPRESSION "\"replay\":{\"commands\":0,\"inputs\":0,\"path\":\"runs/basic.replay.json\",\"ticks\":90}"
        FIXTURES_SETUP recorded_run)
    add_test(NAME cli_scene_validate_replay
        COMMAND slash-cli scene validate ${replay_project}/runs/basic.replay.json)
    set_tests_properties(cli_scene_validate_replay PROPERTIES
        PASS_REGULAR_EXPRESSION "{\"ticks\":90,\"valid\":true}"
        FIXTURES_REQUIRED recorded_run)
    add_test(NAME cli_scene_format_replay
        COMMAND slash-cli scene format --check ${replay_project}/runs/basic.replay.json)
    set_tests_properties(cli_scene_format_replay PROPERTIES
        PASS_REGULAR_EXPRESSION "{\"canonical\":true}"
        FIXTURES_REQUIRED recorded_run)
    add_test(NAME cli_call_replay_run
        COMMAND slash-cli call replay.run "{\"path\":\"runs/basic.replay.json\"}"
            --project ${replay_project})
    set_tests_properties(cli_call_replay_run PROPERTIES
        PASS_REGULAR_EXPRESSION "{\"end\":90,\"matched\":true,\"start\":0,\"tick\":90}"
        FIXTURES_REQUIRED recorded_run)
    # A path relative to the current folder, whose folder doesn't exist yet.
    add_test(NAME cli_run_record_relative
        COMMAND slash-cli run --headless --ticks 3 --record runs/relative.replay.json
        WORKING_DIRECTORY ${replay_project})
    set_tests_properties(cli_run_record_relative PROPERTIES
        PASS_REGULAR_EXPRESSION "\"path\":\"runs/relative.replay.json\""
        FIXTURES_REQUIRED recorded_run)
    add_test(NAME cli_run_record_outside_project
        COMMAND slash-cli run --headless --ticks 1 --record ../outside.replay.json)
    set_tests_properties(cli_run_record_outside_project PROPERTIES WILL_FAIL TRUE)
    add_test(NAME cli_schema_replay COMMAND slash-cli schema replay)
    set_tests_properties(cli_schema_replay PROPERTIES
        PASS_REGULAR_EXPRESSION "\"title\": \"SlashEngine replay\"")
    add_test(NAME cli_schema_test COMMAND slash-cli schema test)
    set_tests_properties(cli_schema_test PROPERTIES
        PASS_REGULAR_EXPRESSION "\"title\": \"SlashEngine tests\"")

    # slash-cli test prints only the checks that failed, and exits 1 when one did.
    add_test(NAME cli_test_failed
        COMMAND slash-cli test failing.test.json --project ${scenes})
    set_tests_properties(cli_test_failed PROPERTIES
        PASS_REGULAR_EXPRESSION "\"message\":\"expected 2, got 1\",\"path\":\"/tests/one_tick/steps/0/expect/tick\"")
    add_test(NAME cli_test_failed_exit COMMAND slash-cli test failing.test.json --project ${scenes})
    set_tests_properties(cli_test_failed_exit PROPERTIES WILL_FAIL TRUE)
    add_test(NAME cli_test_validate COMMAND slash-cli scene validate ${scenes}/failing.test.json)
    set_tests_properties(cli_test_validate PROPERTIES
        PASS_REGULAR_EXPRESSION "{\"tests\":1,\"valid\":true}")

    # One-shot engine commands.
    add_test(NAME cli_call_describe COMMAND slash-cli call engine.describe)
    set_tests_properties(cli_call_describe PROPERTIES
        PASS_REGULAR_EXPRESSION "\"name\":\"SlashEngine\"")

    add_test(NAME cli_call_unknown_command COMMAND slash-cli call world.qurey)
    set_tests_properties(cli_call_unknown_command PROPERTIES WILL_FAIL TRUE)

    # Create a scene with one batch call, save it, then check the file.
    set(call_project ${CMAKE_CURRENT_BINARY_DIR}/call_project)
    add_test(NAME cli_call_clean COMMAND ${CMAKE_COMMAND} -E rm -rf ${call_project})
    set_tests_properties(cli_call_clean PROPERTIES FIXTURES_SETUP call_clean)
    add_test(NAME cli_call_batch_save
        COMMAND slash-cli call batch
            "{\"commands\":[{\"command\":\"entity.create\",\"params\":{\"id\":\"player\",\"components\":{\"Transform2D\":{\"position\":[64,32]}}}},{\"command\":\"entity.create\",\"params\":{\"id\":\"enemy\"}}]}"
            --project ${call_project} --scene levels/one.scene.json --save)
    set_tests_properties(cli_call_batch_save PROPERTIES
        FIXTURES_REQUIRED call_clean
        FIXTURES_SETUP call_saved
        PASS_REGULAR_EXPRESSION "{\"id\":\"enemy\"}")
    add_test(NAME cli_call_saved_scene_valid
        COMMAND slash-cli scene validate ${call_project}/levels/one.scene.json)
    set_tests_properties(cli_call_saved_scene_valid PROPERTIES
        FIXTURES_REQUIRED call_saved
        PASS_REGULAR_EXPRESSION "{\"entities\":2,\"valid\":true}")

    # JSON-RPC and MCP over stdio.
    set(run_stdio ${CMAKE_CURRENT_SOURCE_DIR}/run_stdio.cmake)
    add_test(NAME cli_rpc_stdio
        COMMAND ${CMAKE_COMMAND} -DPROGRAM=$<TARGET_FILE:slash-cli> -DARGS=rpc
            -DINPUT=${scenes}/rpc_session.jsonl -DEXPECT=${scenes}/rpc_session.expected
            -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}/rpc_project -P ${run_stdio})
    add_test(NAME mcp_stdio
        COMMAND ${CMAKE_COMMAND} -DPROGRAM=$<TARGET_FILE:slash-mcp>
            "-DARGS=--project|.|--scene|main.scene.json"
            -DINPUT=${scenes}/mcp_session.jsonl -DEXPECT=${scenes}/mcp_session.expected
            -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}/mcp_project -P ${run_stdio})
    set_tests_properties(mcp_stdio PROPERTIES FIXTURES_SETUP mcp_saved)
    add_test(NAME mcp_saved_scene_valid
        COMMAND slash-cli scene validate ${CMAKE_CURRENT_BINARY_DIR}/mcp_project/main.scene.json)
    set_tests_properties(mcp_saved_scene_valid PROPERTIES
        FIXTURES_REQUIRED mcp_saved
        PASS_REGULAR_EXPRESSION "{\"entities\":1,\"valid\":true}")

    # slash-mcp --live with no game running answers with the reason instead of hanging.
    add_test(NAME mcp_live_offline
        COMMAND ${CMAKE_COMMAND} -DPROGRAM=$<TARGET_FILE:slash-mcp> "-DARGS=--live|--project|."
            -DINPUT=${scenes}/mcp_live_offline.jsonl -DEXPECT=${scenes}/mcp_live_offline.expected
            -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}/live_project -P ${run_stdio})

    # A headless run can serve its world over HTTP while it ticks.
    add_test(NAME cli_run_serve
        COMMAND slash-cli run --headless --ticks 30 --serve
            --project ${CMAKE_CURRENT_BINARY_DIR}/serve_project)
    set_tests_properties(cli_run_serve PROPERTIES
        PASS_REGULAR_EXPRESSION "\"url\":\"http://127\\.0\\.0\\.1:[0-9]+\"")
    add_test(NAME cli_run_port_needs_serve COMMAND slash-cli run --headless --ticks 1 --port 0)
    set_tests_properties(cli_run_port_needs_serve PROPERTIES WILL_FAIL TRUE)

    # Game projects and the game runtime.
    add_test(NAME cli_schema_project COMMAND slash-cli schema project)
    set_tests_properties(cli_schema_project PROPERTIES
        PASS_REGULAR_EXPRESSION "\"title\": \"SlashEngine project\"")
    add_test(NAME cli_project_validate COMMAND slash-cli project validate ${scenes}/game)
    set_tests_properties(cli_project_validate PROPERTIES PASS_REGULAR_EXPRESSION "{\"valid\":true}")
    add_test(NAME cli_schema_presets COMMAND slash-cli schema presets)
    set_tests_properties(cli_schema_presets PROPERTIES
        PASS_REGULAR_EXPRESSION "\"title\": \"SlashEngine export presets\"")
    # project validate checks export_presets.json too, and says where secrets go.
    add_test(NAME cli_project_validate_presets
        COMMAND slash-cli project validate ${scenes}/broken_presets)
    set_tests_properties(cli_project_validate_presets PROPERTIES
        PASS_REGULAR_EXPRESSION "\"file\":\"export_presets\\.json\",\"message\":\"unknown platform.*did you mean \\\\\"web\\\\\".*\"message\":\"secrets are read from environment variables")
    add_test(NAME game_headless
        COMMAND slash-game --headless --ticks 30 --data ${scenes}/game)
    set_tests_properties(game_headless PROPERTIES
        PASS_REGULAR_EXPRESSION "\"entities\":3,\"headless\":true,.*\"ticks\":30}")
    add_test(NAME game_headless_needs_ticks COMMAND slash-game --headless --data ${scenes}/game)
    set_tests_properties(game_headless_needs_ticks PROPERTIES WILL_FAIL TRUE)

    # export checks its arguments and the project before building anything.
    add_test(NAME cli_export_unknown_platform COMMAND slash-cli export macso)
    set_tests_properties(cli_export_unknown_platform PROPERTIES
        PASS_REGULAR_EXPRESSION "did you mean \\\\\"macos\\\\\"")
    add_test(NAME cli_export_unknown_preset
        COMMAND slash-cli export web-relase --project ${scenes}/game)
    set_tests_properties(cli_export_unknown_preset PROPERTIES
        PASS_REGULAR_EXPRESSION "did you mean \\\\\"web-release\\\\\"")
    if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
        set(host_platform macos)
    elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
        set(host_platform windows)
    else()
        set(host_platform linux)
    endif()
    set(broken_game ${CMAKE_CURRENT_BINARY_DIR}/broken_game)
    add_test(NAME cli_export_copy_broken
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${scenes}/broken_game ${broken_game})
    set_tests_properties(cli_export_copy_broken PROPERTIES FIXTURES_SETUP broken_game)
    add_test(NAME cli_export_invalid_scene
        COMMAND slash-cli export ${host_platform} --project ${broken_game})
    set_tests_properties(cli_export_invalid_scene PROPERTIES
        FIXTURES_REQUIRED broken_game
        PASS_REGULAR_EXPRESSION "\"file\":\"scenes/main\\.scene\\.json\".*\"step\":\"validate\"")
    add_test(NAME cli_export_invalid_prefab
        COMMAND slash-cli export ${host_platform} --project ${broken_game})
    set_tests_properties(cli_export_invalid_prefab PROPERTIES
        FIXTURES_REQUIRED broken_game
        PASS_REGULAR_EXPRESSION "\"file\":\"prefabs/bad\\.prefab\\.json\",\"message\":\"expected array, got string\",\"path\":\"/components/Transform2D/scale\"")
    # A folder export didn't make is never replaced.
    add_test(NAME cli_export_keeps_other_folders
        COMMAND slash-cli export ${host_platform} --project ${broken_game} --out ${broken_game}/scenes)
    set_tests_properties(cli_export_keeps_other_folders PROPERTIES
        FIXTURES_REQUIRED broken_game
        PASS_REGULAR_EXPRESSION "was not made by slash-cli export")

    # init writes the agent configs and a valid empty scene, and keeps existing files.
    set(init_project ${CMAKE_CURRENT_BINARY_DIR}/init_project)
    add_test(NAME cli_init COMMAND slash-cli init ${init_project} --force)
    set_tests_properties(cli_init PROPERTIES
        FIXTURES_SETUP init_project
        PASS_REGULAR_EXPRESSION "\"\\.codex/config\\.toml\"")
    add_test(NAME cli_init_keeps_files COMMAND slash-cli init ${init_project})
    set_tests_properties(cli_init_keeps_files PROPERTIES
        FIXTURES_REQUIRED init_project
        PASS_REGULAR_EXPRESSION "\"created\":\\[\\],\"skipped\":\\[\"project\\.json\",\"AGENTS\\.md\".*\"\\.gitignore\"\\]")
    # An existing .gitignore gains the .slash/ line instead of being replaced.
    add_test(NAME cli_init_gitignore_copy
        COMMAND ${CMAKE_COMMAND} -E copy ${scenes}/existing.gitignore ${init_project}/.gitignore)
    set_tests_properties(cli_init_gitignore_copy PROPERTIES
        DEPENDS cli_init_keeps_files
        FIXTURES_REQUIRED init_project
        FIXTURES_SETUP init_gitignore)
    add_test(NAME cli_init_gitignore_updated COMMAND slash-cli init ${init_project})
    set_tests_properties(cli_init_gitignore_updated PROPERTIES
        FIXTURES_REQUIRED init_gitignore
        PASS_REGULAR_EXPRESSION "\"updated\":\\[\"\\.gitignore\"\\]")
    add_test(NAME cli_init_workflow
        COMMAND ${CMAKE_COMMAND} -E cat ${init_project}/.github/workflows/export.yml)
    set_tests_properties(cli_init_workflow PROPERTIES
        FIXTURES_REQUIRED init_project
        PASS_REGULAR_EXPRESSION "SLASHENGINE_REF: ([0-9a-f]+|v[0-9.]+)\n")
    add_test(NAME cli_init_scene_valid
        COMMAND slash-cli scene validate ${init_project}/scenes/main.scene.json)
    set_tests_properties(cli_init_scene_valid PROPERTIES
        FIXTURES_REQUIRED init_project
        PASS_REGULAR_EXPRESSION "{\"entities\":0,\"valid\":true}")
    # The starter storyboard passes its own checks.
    add_test(NAME cli_init_project_valid COMMAND slash-cli project validate ${init_project})
    set_tests_properties(cli_init_project_valid PROPERTIES
        FIXTURES_REQUIRED init_project
        PASS_REGULAR_EXPRESSION "^{\"valid\":true}")
    # Its starter test passes.
    add_test(NAME cli_init_tests_pass COMMAND slash-cli test --project ${init_project})
    set_tests_properties(cli_init_tests_pass PROPERTIES
        FIXTURES_REQUIRED init_project
        PASS_REGULAR_EXPRESSION "^{\"failed\":\\[\\],\"passed\":1,\"tests\":1}")
endif()
