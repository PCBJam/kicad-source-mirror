find_path( GLM_INCLUDE_DIR glm/glm.hpp
    PATHS ${GLM_ROOT_DIR} $ENV{GLM_ROOT_DIR}
    DOC "GLM library header path."
    )

if( NOT ${GLM_INCLUDE_DIR} STREQUAL "GLM_INCLUDE_DIR-NOTFOUND" )

    # For cross-compilation (WASM), find_file may fail, so use direct path check
    if( EXISTS "${GLM_INCLUDE_DIR}/glm/detail/setup.hpp" )
        set( GLM_SETUP "${GLM_INCLUDE_DIR}/glm/detail/setup.hpp" )
    elseif( EXISTS "${GLM_INCLUDE_DIR}/glm/core/setup.hpp" )
        set( GLM_SETUP "${GLM_INCLUDE_DIR}/glm/core/setup.hpp" )
    else()
        # Fallback to find_file for native builds
        find_file( GLM_SETUP setup.hpp
            PATHS ${GLM_INCLUDE_DIR}
            PATH_SUFFIXES glm/core glm/detail
            NO_DEFAULT_PATH )
    endif()

    if( GLM_SETUP AND NOT ${GLM_SETUP} STREQUAL "GLM_SETUP-NOTFOUND" )

        # extract the "#define GLM_VERSION*" lines
        file( STRINGS ${GLM_SETUP} _version REGEX "^#define.*GLM_VERSION.*" )

        foreach( SVAR ${_version} )
            string( REGEX MATCH GLM_VERSION_[M,A,J,O,R,I,N,P,T,C,H,E,V,I,S]* _VARNAME ${SVAR} )
            string( REGEX MATCH [0-9]+ _VALUE ${SVAR} )

            if( NOT ${_VARNAME} STREQUAL "" AND NOT ${_VALUE} STREQUAL "" )
                set( _${_VARNAME} ${_VALUE} )
            endif()

        endforeach()

        #ensure that NOT GLM_VERSION* will evaluate to '0'
        if( NOT _GLM_VERSION_MAJOR )
            set( _GLM_VERSION_MAJOR 0 )
        endif()

        if( NOT _GLM_VERSION_MINOR )
            set( _GLM_VERSION_MINOR 0 )
        endif()

        if( NOT _GLM_VERSION_PATCH )
            set( _GLM_VERSION_PATCH 0 )
        endif()

        if( NOT _GLM_VERSION_REVISION )
            set( _GLM_VERSION_REVISION 0 )
        endif()

        set( GLM_VERSION ${_GLM_VERSION_MAJOR}.${_GLM_VERSION_MINOR}.${_GLM_VERSION_PATCH}.${_GLM_VERSION_REVISION} )
        unset( GLM_SETUP CACHE )

    endif()
endif()


include( FindPackageHandleStandardArgs )
FIND_PACKAGE_HANDLE_STANDARD_ARGS( GLM
    REQUIRED_VARS
        GLM_INCLUDE_DIR
        GLM_VERSION
    VERSION_VAR GLM_VERSION )


mark_as_advanced( GLM_INCLUDE_DIR )
set( GLM_VERSION_MAJOR ${_GLM_VERSION_MAJOR} CACHE INTERNAL "" )
set( GLM_VERSION_MINOR ${_GLM_VERSION_MINOR} CACHE INTERNAL "" )
set( GLM_VERSION_PATCH ${_GLM_VERSION_PATCH} CACHE INTERNAL "" )
set( GLM_VERSION_TWEAK ${_GLM_VERSION_REVISION} CACHE INTERNAL "" )
