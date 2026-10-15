# Every folder here is one example. Each is built and run headless as a test, with its output
# checked exactly, so agents can copy code and calls that are known to work. See README.md.
#
# An example has any of:
#   main.cpp        built as example-<folder>; its stdout must equal expected.txt
#   calls.jsonl     JSON-RPC requests sent to "slash-cli rpc --project ."; the responses must
#                   equal results.jsonl, line for line
#   project.json    the folder is a game project and must pass "slash-cli project validate"
#
# Each runs in a copy of its folder. SLASH_UPDATE_EXAMPLES=1 in the environment writes the output
# to expected.txt or results.jsonl instead of comparing; review the diff before committing it.

set(run_example ${CMAKE_CURRENT_SOURCE_DIR}/run_example.cmake)
file(GLOB examples LIST_DIRECTORIES true CONFIGURE_DEPENDS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/*)
foreach(name IN LISTS examples)
    set(dir ${CMAKE_CURRENT_SOURCE_DIR}/${name})
    if(NOT IS_DIRECTORY ${dir})
        continue()
    endif()
    set(program "")
    if(EXISTS ${dir}/main.cpp)
        add_executable(example-${name} ${name}/main.cpp)
        target_link_libraries(example-${name} PRIVATE
            slash::animation slash::canvas slash::command slash::core slash::physics2d
            slash::platform slash::render slash::world)
        # The same output on every platform: no fused multiply-adds, which arm64 compilers
        # would otherwise emit and x86-64 ones not.
        target_compile_options(example-${name} PRIVATE
            $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-ffp-contract=off>)
        slash_target_warnings(example-${name})
        set(program -DPROGRAM=$<TARGET_FILE:example-${name}>)
    endif()
    add_test(NAME example_${name}
        COMMAND ${CMAKE_COMMAND} -DEXAMPLE=${dir} -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}/${name}
            -DCLI=$<TARGET_FILE:slash-cli> ${program} -P ${run_example})
endforeach()

# README.md lists every example, so agents find them.
add_test(NAME examples_index
    COMMAND ${CMAKE_COMMAND} -DEXAMPLES=${CMAKE_CURRENT_SOURCE_DIR}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/check_index.cmake)
