# Runs a stdio server with a file on stdin and checks its stdout. Every line of the EXPECT file
# must appear somewhere in the output. Keep expected lines free of ';' and '[', which CMake lists
# treat specially.
#
#   cmake -DPROGRAM=<exe> -DARGS=<a|b|c> -DINPUT=<file> -DEXPECT=<file> -DWORKDIR=<dir>
#         -P run_stdio.cmake
#
# ARGS are separated by '|'. WORKDIR is emptied first so the test starts from a clean project.

string(REPLACE "|" ";" ARGS "${ARGS}")
file(REMOVE_RECURSE "${WORKDIR}")
file(MAKE_DIRECTORY "${WORKDIR}")
execute_process(
    COMMAND "${PROGRAM}" ${ARGS}
    INPUT_FILE "${INPUT}"
    OUTPUT_VARIABLE output
    ERROR_VARIABLE errors
    RESULT_VARIABLE result
    WORKING_DIRECTORY "${WORKDIR}")
if(NOT result EQUAL 0)
    message(FATAL_ERROR "${PROGRAM} exited with ${result}\n${errors}")
endif()
file(STRINGS "${EXPECT}" expected_lines)
foreach(line IN LISTS expected_lines)
    string(FIND "${output}" "${line}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "missing from the output: ${line}\n--- stdout ---\n${output}\n"
                            "--- stderr ---\n${errors}")
    endif()
endforeach()
message(STATUS "${output}")
