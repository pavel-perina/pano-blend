# Build-time version generator, invoked on every build by the
# generate_version custom target:
#
#   cmake -DSRC_DIR=<repo> -DOUT_HEADER=<version.h>
#         [-DGIT_EXECUTABLE=<git>] [-DRC_IN=<rc.in> -DRC_OUT=<rc>]
#         -P cmake/GenerateVersion.cmake
#
# The version comes solely from `git describe`; "unknown" is the fallback when
# git or .git is unavailable (source-archive build) or describe fails (e.g. a
# repo with no commits). Outputs are rewritten only when the version actually
# changes, so incremental builds don't recompile anything.

set(PANOBLEND_VERSION "unknown")
if(GIT_EXECUTABLE AND EXISTS "${SRC_DIR}/.git")
    execute_process(
        COMMAND ${GIT_EXECUTABLE} describe --tags --dirty --always
        WORKING_DIRECTORY ${SRC_DIR}
        RESULT_VARIABLE git_result
        OUTPUT_VARIABLE git_out
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(git_result EQUAL 0 AND NOT git_out STREQUAL "")
        set(PANOBLEND_VERSION "${git_out}")
    endif()
endif()

# Numeric MAJOR.MINOR[.PATCH] for the Windows VERSIONINFO resource, parsed
# from the same describe string (tags like v1.1 or v1.2.3); 0.0.0 when there
# is no such tag. No manually bumped version exists anywhere.
set(PANOBLEND_VERSION_MAJOR 0)
set(PANOBLEND_VERSION_MINOR 0)
set(PANOBLEND_VERSION_PATCH 0)
if(PANOBLEND_VERSION MATCHES "^v?([0-9]+)\\.([0-9]+)(\\.([0-9]+))?")
    set(PANOBLEND_VERSION_MAJOR ${CMAKE_MATCH_1})
    set(PANOBLEND_VERSION_MINOR ${CMAKE_MATCH_2})
    if(CMAKE_MATCH_4)  # optional group is UNSET for a two-component tag like v1.1
        set(PANOBLEND_VERSION_PATCH ${CMAKE_MATCH_4})
    endif()
endif()

set(header_content "// Generated at build time by cmake/GenerateVersion.cmake — do not edit.
#pragma once
#define PANOBLEND_VERSION \"${PANOBLEND_VERSION}\"
")

set(old_content "")
if(EXISTS "${OUT_HEADER}")
    file(READ "${OUT_HEADER}" old_content)
endif()
if(NOT old_content STREQUAL header_content)
    file(WRITE "${OUT_HEADER}" "${header_content}")
    message(STATUS "pano-blend version: ${PANOBLEND_VERSION}")
endif()

if(RC_IN AND RC_OUT)
    configure_file("${RC_IN}" "${RC_OUT}" @ONLY)  # no-op when content is unchanged
endif()
