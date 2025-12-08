# WASM: nng (nanomsg) not available - IPC not supported in browser
if(EMSCRIPTEN)
    set(nng_FOUND FALSE)
    set(NNG_FOUND FALSE)
    set(NNG_INCLUDE_DIR "")
    set(NNG_LIBRARY "")
    message(STATUS "nng not available for WASM build (IPC not supported in browser)")
    return()
endif()

find_package(PkgConfig)

if(PKG_CONFIG_FOUND)
    pkg_check_modules(_NNG nng)
endif (PKG_CONFIG_FOUND)

FIND_PATH(NNG_INCLUDE_DIR
    NAMES
        nng/nng.h
    PATH_SUFFIXES
        include
    )

FIND_LIBRARY(NNG_LIBRARY
    NAMES
        nng
    PATH_SUFFIXES
        "lib"
        "local/lib"
    )

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(nng
        REQUIRED_VARS NNG_INCLUDE_DIR NNG_LIBRARY)

MARK_AS_ADVANCED(NNG_INCLUDE_DIR NNG_LIBRARY)
