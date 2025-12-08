# SPDX-FileCopyrightText: 2014 Dan Leinir Turthra Jensen <admin@leinir.dk
#
# Redistribution and use is allowed according to the terms of the BSD license.
# For details see the accompanying COPYING-CMAKE-SCRIPTS file.

# - Try to find the libgit2 library
# Once done this will define
#
#  LIBGIT2_FOUND - System has libgit2
#  LIBGIT2_INCLUDE_DIR - The libgit2 include directory
#  LIBGIT2_LIBRARIES - The libraries needed to use libgit2
#  LIBGIT2_DEFINITIONS - Compiler switches required for using libgit2

# WASM cross-compilation: use headers only, git functions will be stubbed
if(EMSCRIPTEN)
    foreach(_path ${CMAKE_PREFIX_PATH})
        if(EXISTS "${_path}/include/git2.h")
            set(LIBGIT2_INCLUDE_DIR "${_path}/include")
            break()
        endif()
    endforeach()

    if(LIBGIT2_INCLUDE_DIR)
        set(LIBGIT2_FOUND TRUE)
        set(LIBGIT2_LIBRARIES "")  # No library - will be stubbed
        set(LIBGIT2_DEFINITIONS "")

        # Extract version from git2/version.h
        if(EXISTS "${LIBGIT2_INCLUDE_DIR}/git2/version.h")
            file(STRINGS "${LIBGIT2_INCLUDE_DIR}/git2/version.h" _version_line
                 REGEX "^#define[ \t]+LIBGIT2_VERSION[ \t]+\"[^\"]+\"")
            string(REGEX REPLACE ".*\"([^\"]+)\".*" "\\1" LIBGIT2_VERSION "${_version_line}")
        else()
            set(LIBGIT2_VERSION "1.7.1")
        endif()

        include(FindPackageHandleStandardArgs)
        find_package_handle_standard_args(libgit2
            REQUIRED_VARS LIBGIT2_INCLUDE_DIR
            VERSION_VAR LIBGIT2_VERSION)
        message(STATUS "Using libgit2 stub for WASM build (git features disabled)")
        return()
    endif()
endif()

# use pkg-config to get the directories and then use these values
# in the FIND_PATH() and FIND_LIBRARY() calls
find_package(PkgConfig)
pkg_search_module(PC_LIBGIT2 libgit2)

set(LIBGIT2_DEFINITIONS ${PC_LIBGIT2_CFLAGS_OTHER})

find_path(LIBGIT2_INCLUDE_DIR NAMES git2.h
   HINTS
   ${PC_LIBGIT2_INCLUDEDIR}
   ${PC_LIBGIT2_INCLUDE_DIRS}
)

find_library(LIBGIT2_LIBRARIES NAMES git2
   HINTS
   ${PC_LIBGIT2_LIBDIR}
   ${PC_LIBGIT2_LIBRARY_DIRS}
)


include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(libgit2 DEFAULT_MSG LIBGIT2_LIBRARIES LIBGIT2_INCLUDE_DIR)

mark_as_advanced(LIBGIT2_INCLUDE_DIR LIBGIT2_LIBRARIES)