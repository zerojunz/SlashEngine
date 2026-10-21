# Runs the wayland-scanner built into slash-cli or slash-mcp on data/wayland/board.xml and checks
# that its client header and protocol code match the files beside it, as Wayland 1.18's scanner
# writes them: code that fits the Wayland headers of the sysroot Linux games build against.
#
#   cmake -DPROGRAM=<exe> -DDATA=<tests/data/wayland> -DWORKDIR=<dir> -P check_wayland_scanner.cmake

file(REMOVE_RECURSE "${WORKDIR}")
file(MAKE_DIRECTORY "${WORKDIR}")
foreach(mode IN ITEMS client-header private-code)
    if(mode STREQUAL "client-header")
        set(name board-client-protocol.h)
    else()
        set(name board-protocol.c)
    endif()
    execute_process(
        COMMAND "${PROGRAM}" wayland-scanner ${mode} "${DATA}/board.xml" "${WORKDIR}/${name}"
        ERROR_VARIABLE errors
        RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "wayland-scanner ${mode} exited with ${result}: ${errors}")
    endif()
    file(READ "${WORKDIR}/${name}" written)
    file(READ "${DATA}/${name}" expected)
    if(NOT written STREQUAL expected)
        message(FATAL_ERROR "wayland-scanner ${mode} wrote ${WORKDIR}/${name}, which differs from "
                            "${DATA}/${name}")
    endif()
endforeach()
