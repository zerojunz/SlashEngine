# Copies the engine's notices into a game's data, so every export ships them as the licenses ask:
# the engine's own LICENSE, THIRD_PARTY.md and the license file of each dependency the build
# fetched, under licenses/slashengine/ (SDK packages' licenses go beside it, in
# licenses/<package>/). export/CMakeLists.txt runs it when it configures a game; the
# "export_notices" test runs it on the dev build's dependencies.
#
#   cmake -DROOT=<engine checkout> -DOUT=<data>/licenses/slashengine
#         -DSOURCES=<name>=<fetched source>|... -P stage_notices.cmake
#
# It fails when a fetched dependency's license file isn't where cmake/dependencies.json says.

cmake_minimum_required(VERSION 3.25)

foreach(variable ROOT OUT)
    if(NOT ${variable})
        message(FATAL_ERROR "stage_notices.cmake needs ${variable}")
    endif()
endforeach()

file(READ "${ROOT}/cmake/dependencies.json" manifest)
file(MAKE_DIRECTORY "${OUT}")
file(COPY_FILE "${ROOT}/LICENSE" "${OUT}/LICENSE.txt" ONLY_IF_DIFFERENT)
file(COPY_FILE "${ROOT}/THIRD_PARTY.md" "${OUT}/THIRD_PARTY.md" ONLY_IF_DIFFERENT)

set(problems "")
string(REPLACE "|" ";" sources "${SOURCES}")
foreach(entry IN LISTS sources)
    if(NOT entry MATCHES "^([^=]+)=(.*)$")
        continue()
    endif()
    set(name "${CMAKE_MATCH_1}")
    set(source "${CMAKE_MATCH_2}")
    string(JSON license_file ERROR_VARIABLE missing GET "${manifest}" dependencies ${name}
        license_file)
    if(missing)
        string(APPEND problems "\n- ${name} is not in cmake/dependencies.json")
        continue()
    endif()
    if(NOT EXISTS "${source}/${license_file}")
        string(APPEND problems "\n- ${name} has no ${license_file} in ${source}")
        continue()
    endif()
    cmake_path(GET license_file FILENAME file_name)
    file(MAKE_DIRECTORY "${OUT}/${name}")
    file(COPY_FILE "${source}/${license_file}" "${OUT}/${name}/${file_name}" ONLY_IF_DIFFERENT)
endforeach()

if(problems)
    message(FATAL_ERROR "Could not copy the engine's notices into the game's data:${problems}")
endif()
