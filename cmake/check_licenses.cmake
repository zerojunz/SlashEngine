# Checks that every third-party dependency is pinned with an allowed license (docs/design.md, "No
# proprietary code"). CI runs it on every pull request as the "licenses" test.
#
#   cmake -DROOT=<repo> -DDEPENDENCIES=<names> -DSOURCE_<name>=<fetched source> ...
#         -P check_licenses.cmake
#
# DEPENDENCIES lists the dependencies this build fetched, separated by '|'. It fails when:
# - a license in cmake/dependencies.json is not on the allowed list below;
# - a dependency's license file at its pinned tag is missing or differs from the reviewed text
#   (license_sha256, over the text with LF line endings), so a license change gets a review;
# - a pin is a branch rather than an exact tag;
# - THIRD_PARTY.md has no row for a dependency or vendored file at its pinned version;
# - a CMake file fetches code without slash_add_dependency, which reads the manifest;
# - a source file carries a GPL, LGPL or AGPL notice.

cmake_minimum_required(VERSION 3.25)

# SPDX identifiers that may ship in games built with SlashEngine. Changing this list is a policy
# change: see docs/design.md. MIT-Modern-Variant is the "Old MIT" text HarfBuzz uses, as
# permissive as MIT. Two are glslang's, which the tools link to compile a game's shaders, as
# Godot's editor does: AML-glslang, NVIDIA's permissive notice on its preprocessor, and the Bison
# exception, which lets a parser Bison made ship under any terms.
set(allowed Apache-2.0 BSD-2-Clause BSD-3-Clause BSL-1.0 FTL MIT MIT-0 MIT-Modern-Variant
    Unlicense Zlib AML-glslang "GPL-3.0-or-later WITH Bison-exception-2.2")

set(manifest_path "${ROOT}/cmake/dependencies.json")
file(READ "${manifest_path}" manifest)
file(READ "${ROOT}/THIRD_PARTY.md" third_party)
set(problems "")
macro(problem)
    string(CONCAT _slash_problem ${ARGV})
    string(APPEND problems "\n- ${_slash_problem}")
endmacro()

# Whether an SPDX expression ("MIT", "MIT-0 OR Unlicense", "MIT AND Zlib") is allowed: one
# alternative of an OR, or every part of an AND, must be on the list.
function(license_allowed expression out)
    string(REPLACE " OR " ";" alternatives "${expression}")
    foreach(alternative IN LISTS alternatives)
        string(REPLACE " AND " ";" parts "${alternative}")
        set(all TRUE)
        foreach(part IN LISTS parts)
            if(NOT part IN_LIST allowed)
                set(all FALSE)
            endif()
        endforeach()
        if(all)
            set(${out} TRUE PARENT_SCOPE)
            return()
        endif()
    endforeach()
    set(${out} FALSE PARENT_SCOPE)
endfunction()

# Fetched dependencies.
string(REPLACE "|" ";" fetched "${DEPENDENCIES}")
string(JSON count LENGTH "${manifest}" dependencies)
math(EXPR last "${count} - 1")
set(pinned "")
foreach(index RANGE ${last})
    string(JSON name MEMBER "${manifest}" dependencies ${index})
    list(APPEND pinned ${name})
    foreach(key repository tag license license_file license_sha256)
        string(JSON ${key} ERROR_VARIABLE missing GET "${manifest}" dependencies ${name} ${key})
        if(missing)
            problem("${name} in cmake/dependencies.json has no \"${key}\"")
            set(${key} "")
        endif()
    endforeach()
    license_allowed("${license}" ok)
    if(NOT ok)
        list(JOIN allowed ", " allowed_text)
        problem("${name} is under \"${license}\", which is not allowed (allowed: ${allowed_text})")
    endif()
    if(tag MATCHES "^(main|master|trunk|develop|HEAD)$")
        problem("${name} is pinned to the branch \"${tag}\", not an exact tag")
    endif()
    string(REGEX REPLACE "\\.git$" "" page "${repository}")
    string(FIND "${third_party}" "(${page}) | ${tag} |" row)
    if(row EQUAL -1)
        problem("THIRD_PARTY.md has no row linking ${page} at ${tag}")
    endif()
    if(name IN_LIST fetched)
        set(file "${SOURCE_${name}}/${license_file}")
        if(NOT EXISTS "${file}")
            problem("${name} ${tag} has no ${license_file}: find its license and update "
                    "cmake/dependencies.json")
        else()
            file(READ "${file}" text)
            string(REPLACE "\r\n" "\n" text "${text}")
            string(SHA256 hash "${text}")
            if(NOT hash STREQUAL license_sha256)
                problem("the license of ${name} ${tag} is not the reviewed text: read ${file}, "
                        "and if it is still ${license}, set license_sha256 to ${hash}")
            endif()
        endif()
    endif()
endforeach()
foreach(name IN LISTS fetched)
    if(NOT name IN_LIST pinned)
        problem("${name} is fetched but not in cmake/dependencies.json")
    endif()
endforeach()

# Vendored files.
string(JSON count ERROR_VARIABLE none LENGTH "${manifest}" vendored)
if(NOT none AND count GREATER 0)
    math(EXPR last "${count} - 1")
    foreach(index RANGE ${last})
        string(JSON path MEMBER "${manifest}" vendored ${index})
        string(JSON license GET "${manifest}" vendored ${path} license)
        string(JSON version GET "${manifest}" vendored ${path} version)
        license_allowed("${license}" ok)
        if(NOT ok)
            problem("${path} is under \"${license}\", which is not allowed")
        endif()
        if(NOT EXISTS "${ROOT}/${path}")
            problem("${path} is listed as vendored but does not exist")
        endif()
        string(FIND "${third_party}" "| ${version} |" version_row)
        string(FIND "${third_party}" "`${path}`" path_row)
        if(version_row EQUAL -1 OR path_row EQUAL -1)
            problem("THIRD_PARTY.md has no row for ${path} at ${version}")
        endif()
    endforeach()
endif()

# Code fetched around the manifest, and copyleft notices in the repo's own files.
set(folders cmake engine examples export tests tools)
set(sources "")
foreach(folder IN LISTS folders)
    file(GLOB_RECURSE found LIST_DIRECTORIES false "${ROOT}/${folder}/*")
    list(APPEND sources ${found})
endforeach()
if(EXISTS "${ROOT}/CMakeLists.txt")
    list(APPEND sources "${ROOT}/CMakeLists.txt")
endif()
set(fetchers "CPMAddPackage\\(|CPMFindPackage\\(|FetchContent_Declare\\(|ExternalProject_Add\\(")
set(copyleft "GNU (Affero |Lesser |Library )?General Public License|SPDX-License-Identifier: *(A|L)?GPL")
foreach(file IN LISTS sources)
    file(RELATIVE_PATH relative "${ROOT}" "${file}")
    # This script names what it looks for, CPM defines the fetch calls, and the fixture that
    # tests this script breaks every rule on purpose.
    if(relative STREQUAL "cmake/check_licenses.cmake" OR relative STREQUAL "cmake/CPM.cmake" OR
       relative MATCHES "^tests/data/licenses/")
        continue()
    endif()
    if(file MATCHES "(CMakeLists\\.txt|\\.cmake)$")
        file(STRINGS "${file}" calls REGEX "${fetchers}")
        if(calls AND NOT relative STREQUAL "cmake/Dependencies.cmake")
            problem("${relative} fetches code without slash_add_dependency")
        elseif(calls)
            list(LENGTH calls fetch_calls)
            if(fetch_calls GREATER 1)
                problem("cmake/Dependencies.cmake fetches code outside slash_add_dependency")
            endif()
        endif()
    endif()
    if(file MATCHES "\\.(c|cc|cpp|h|hpp|m|mm|java|js|py|sh|cmake|txt|in|json|md)$")
        file(STRINGS "${file}" notices REGEX "${copyleft}")
        if(notices)
            problem("${relative} carries a copyleft notice, which is not allowed in the repo")
        endif()
    endif()
endforeach()

if(problems)
    # NOTICE prints the list as it is; FATAL_ERROR would rewrap it.
    message(NOTICE "license check failed:${problems}")
    message(FATAL_ERROR "license check failed")
endif()
list(LENGTH pinned dependency_count)
message(STATUS "licenses ok: ${dependency_count} dependencies, all pinned with allowed licenses")
