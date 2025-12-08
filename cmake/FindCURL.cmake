# FindCURL.cmake - supports both native and WASM cross-compilation
#
# For WASM builds:
#   - Uses headers from sysroot (installed by scripts/deps/build-curl-headers.sh)
#   - No actual library linked - CURL functions will be stubbed
# For native builds:
#   - Falls back to system FindCURL module

if(EMSCRIPTEN)
    # WASM build - use headers only, networking handled by browser fetch API

    # Look for headers in sysroot
    foreach(_path ${CMAKE_PREFIX_PATH})
        if(EXISTS "${_path}/include/curl/curl.h")
            set(CURL_INCLUDE_DIR "${_path}/include")
            break()
        endif()
    endforeach()

    # Verify headers exist
    if(NOT CURL_INCLUDE_DIR OR NOT EXISTS "${CURL_INCLUDE_DIR}/curl/curl.h")
        message(FATAL_ERROR "CURL headers not found at ${CMAKE_PREFIX_PATH}. Run: ./scripts/deps/build-curl-headers.sh")
    endif()

    # Create a dummy library target
    if(NOT TARGET CURL::libcurl)
        add_library(CURL::libcurl INTERFACE IMPORTED)
        set_target_properties(CURL::libcurl PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${CURL_INCLUDE_DIR}")
    endif()

    # Set the required variables
    set(CURL_FOUND TRUE)
    set(CURL_INCLUDE_DIRS "${CURL_INCLUDE_DIR}")
    set(CURL_LIBRARY "")  # No actual library - will be stubbed
    set(CURL_LIBRARIES "")
    set(CURL_VERSION_STRING "8.5.0-wasm-stub")

    mark_as_advanced(CURL_INCLUDE_DIR CURL_LIBRARY)
    message(STATUS "Using CURL headers for WASM build: ${CURL_INCLUDE_DIR}")
else()
    # Native build - use system FindCURL
    include(${CMAKE_ROOT}/Modules/FindCURL.cmake)
endif()
