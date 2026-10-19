# Runs one example in a copy of its folder and checks its output exactly; see CMakeLists.txt.
#
#   cmake -DEXAMPLE=<folder> -DWORKDIR=<dir> -DCLI=<slash-cli> [-DPROGRAM=<exe>]
#         [-DASSETRIPPER=<fake-assetripper>] -P run_example.cmake

set(update "$ENV{SLASH_UPDATE_EXAMPLES}")
# Unpacking a Unity build runs AssetRipper's stand-in, which exports the example's ripped/ folder.
if(ASSETRIPPER)
    set(ENV{SLASH_ASSETRIPPER} "${ASSETRIPPER}")
    set(ENV{FAKE_ASSETRIPPER_PROJECT} "${WORKDIR}/ripped")
endif()

# Compares output with an expected file in the example's folder, or rewrites that file.
function(check what output expected_file)
    string(REPLACE "\r\n" "\n" output "${output}")
    if(update)
        file(WRITE "${expected_file}" "${output}")
        message(STATUS "updated ${expected_file}")
        return()
    endif()
    if(NOT EXISTS "${expected_file}")
        message(FATAL_ERROR "${expected_file} is missing; run with SLASH_UPDATE_EXAMPLES=1 to "
                            "write it\n--- ${what} printed ---\n${output}")
    endif()
    file(READ "${expected_file}" expected)
    string(REPLACE "\r\n" "\n" expected "${expected}")
    if(NOT output STREQUAL expected)
        message(FATAL_ERROR "${what} printed something else than ${expected_file}\n"
                            "--- expected ---\n${expected}\n--- printed ---\n${output}")
    endif()
endfunction()

# Runs a command in the copy, with input on stdin if it is set, and returns its stdout in output.
function(run what)
    set(stdin "")
    if(input)
        set(stdin INPUT_FILE "${input}")
    endif()
    execute_process(
        COMMAND ${ARGN}
        WORKING_DIRECTORY "${WORKDIR}"
        ${stdin}
        OUTPUT_VARIABLE output
        ERROR_VARIABLE errors
        RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "${what} exited with ${result}\n--- stdout ---\n${output}\n"
                            "--- stderr ---\n${errors}")
    endif()
    set(output "${output}" PARENT_SCOPE)
endfunction()

file(REMOVE_RECURSE "${WORKDIR}")
file(COPY "${EXAMPLE}/" DESTINATION "${WORKDIR}")
set(input "")

if(EXISTS "${WORKDIR}/project.json")
    run("slash-cli project validate" "${CLI}" project validate .)
endif()
if(PROGRAM)
    run("the example program" "${PROGRAM}")
    check("the example program" "${output}" "${EXAMPLE}/expected.txt")
endif()
if(EXISTS "${WORKDIR}/calls.jsonl")
    set(input "${WORKDIR}/calls.jsonl")
    run("slash-cli rpc" "${CLI}" rpc --project .)
    check("slash-cli rpc" "${output}" "${EXAMPLE}/results.jsonl")
endif()
