# Fails unless examples/README.md links every example folder, as "(<folder>/)".
#
#   cmake -DEXAMPLES=<examples folder> -P check_index.cmake

file(READ "${EXAMPLES}/README.md" index)
file(GLOB entries LIST_DIRECTORIES true RELATIVE "${EXAMPLES}" "${EXAMPLES}/*")
set(missing "")
foreach(name IN LISTS entries)
    if(IS_DIRECTORY "${EXAMPLES}/${name}")
        string(FIND "${index}" "(${name}/)" position)
        if(position EQUAL -1)
            string(APPEND missing " ${name}")
        endif()
        if(NOT EXISTS "${EXAMPLES}/${name}/README.md")
            message(FATAL_ERROR "examples/${name} has no README.md")
        endif()
    endif()
endforeach()
if(missing)
    message(FATAL_ERROR "examples/README.md does not link these examples:${missing}")
endif()
