# Runs a shard of the unit tests, and runs it once more if the watchdog in test_main.cpp stopped a
# test case that was stuck (exit code 75). Each run's output passes straight through to ctest.
# A retry is noted in the GitHub Actions job summary, so flaky hangs stay visible.
#
#   cmake -DPROGRAM=<exe> -DARGS=<a|b|c> -DNAME=<test name> -P retry_stuck.cmake
#
# ARGS are separated by '|'.

string(REPLACE "|" ";" ARGS "${ARGS}")
set(kStuck 75)
execute_process(COMMAND "${PROGRAM}" ${ARGS} RESULT_VARIABLE result)
if(result EQUAL kStuck)
    message(NOTICE "${NAME}: a test case was stuck; running the shard once more")
    if(DEFINED ENV{GITHUB_STEP_SUMMARY})
        file(APPEND "$ENV{GITHUB_STEP_SUMMARY}"
            "- `${NAME}`: a test case was stuck, so the shard ran once more\n")
    endif()
    execute_process(COMMAND "${PROGRAM}" ${ARGS} RESULT_VARIABLE result)
endif()
if(NOT result EQUAL 0)
    message(FATAL_ERROR "${NAME} exited with ${result}")
endif()
