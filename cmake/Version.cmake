# Derives the project version from git describe, falling back to a hardcoded
# value in tarball builds. Generates version.h into the build tree.
set(ONVIFSIM_VERSION_FALLBACK "0.1.0")

find_package(Git QUIET)
set(ONVIFSIM_VERSION_FULL "")
if(GIT_FOUND AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git")
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" describe --tags --dirty --always
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_VARIABLE ONVIFSIM_VERSION_FULL
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
endif()

if(NOT ONVIFSIM_VERSION_FULL)
    set(ONVIFSIM_VERSION_FULL "${ONVIFSIM_VERSION_FALLBACK}")
endif()

# Extract a bare X.Y.Z for project(VERSION ...); git describe may prepend "v"
# and append "-<n>-g<sha>", neither of which CMake accepts.
if(ONVIFSIM_VERSION_FULL MATCHES "([0-9]+)\\.([0-9]+)\\.([0-9]+)")
    set(ONVIFSIM_VERSION_SHORT "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}.${CMAKE_MATCH_3}")
else()
    set(ONVIFSIM_VERSION_SHORT "${ONVIFSIM_VERSION_FALLBACK}")
endif()

function(onvifsim_generate_version_header target)
    set(_dir "${CMAKE_BINARY_DIR}/generated")
    file(MAKE_DIRECTORY "${_dir}")
    file(WRITE "${_dir}/version.h.in"
"#pragma once
#define ONVIFSIM_VERSION \"@ONVIFSIM_VERSION_FULL@\"
#define ONVIFSIM_VERSION_SHORT \"@ONVIFSIM_VERSION_SHORT@\"
")
    configure_file("${_dir}/version.h.in" "${_dir}/version.h" @ONLY)
    target_include_directories(${target} PUBLIC "${_dir}")
endfunction()
