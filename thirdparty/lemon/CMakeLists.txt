# When cross-compiling (e.g., for WASM), we need a native lemon executable
# Build it with the host compiler
if(CMAKE_CROSSCOMPILING)
    # Build native lemon using host compiler
    set(NATIVE_LEMON_DIR "${CMAKE_CURRENT_BINARY_DIR}/native")
    set(NATIVE_LEMON_EXE "${NATIVE_LEMON_DIR}/lemon")

    file(MAKE_DIRECTORY ${NATIVE_LEMON_DIR})

    # Compile lemon.c with native compiler (cc) at configure time
    # This ensures lemon is available before any targets need it
    if(NOT EXISTS ${NATIVE_LEMON_EXE})
        message(STATUS "Building native lemon parser generator...")
        execute_process(
            COMMAND cc -O2 -o ${NATIVE_LEMON_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/lemon.c
            RESULT_VARIABLE LEMON_BUILD_RESULT
            ERROR_VARIABLE LEMON_BUILD_ERROR
        )
        if(NOT LEMON_BUILD_RESULT EQUAL 0)
            message(FATAL_ERROR "Failed to build native lemon: ${LEMON_BUILD_ERROR}")
        endif()
    endif()

    # Create an imported target that points to the native executable
    add_executable(lemon IMPORTED GLOBAL)
    set_target_properties(lemon PROPERTIES IMPORTED_LOCATION ${NATIVE_LEMON_EXE})
else()
    add_executable( lemon
        lemon.c
        )
endif()

# The location of the template file for lemon is passed as a property on the lemon target
define_property( TARGET
    PROPERTY lemon_template
    BRIEF_DOCS "Location of the template file for the lemon parser"
    FULL_DOCS "Location of the template file for the lemon parser"
    )

set_property( TARGET lemon
    PROPERTY lemon_template ${CMAKE_CURRENT_SOURCE_DIR}/lempar.c
    )
