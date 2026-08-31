# Developers may wish to set DEBUG_OCE to a value from 1..3 to
# enable informational messages in Debug build. Setting a value
# of 4 or greater will instruct the parser to write a VRML2
# equivalent copy of the input file by invoking the
# SceneGraph->WriteVRML() function and depending on the setup
# the object may attempt to write to a protected directory.
#
# In addition to setting the verbosity via DEBUG_OCE, an
# appropriate WXTRACE value must be set prior to program
# execution to enable the logging:
#
# export WXTRACE="PLUGIN_OCE"
#

set( S3D_PLUGIN_OCE_TYPE MODULE )

if( EMSCRIPTEN )
    # WASM has no dlopen: build as a static archive consumed by the registry in
    # 3d-viewer/3d_cache/pcbjam_static_3d_plugins.cpp.  oce.cpp is the single TU
    # holding every flat C export (loadmodel.cpp is untouched — its OCCT includes
    # must not see the renames); prefix oce3d_* to avoid collisions.
    # FILE_DATA renamed for the same ODR reason as in vrml/CMakeLists.txt: both
    # plugin TUs define a different `struct FILE_DATA` with an inline ctor; the
    # static link dedupes the linkonce ctor and vrml's list initialized oce's
    # (so .step never entered the extension map).
    set( S3D_PLUGIN_OCE_TYPE STATIC )
    set_source_files_properties( oce.cpp PROPERTIES COMPILE_DEFINITIONS
        "GetKicadPluginClass=oce3d_GetKicadPluginClass;GetClassVersion=oce3d_GetClassVersion;CheckClassVersion=oce3d_CheckClassVersion;GetKicadPluginName=oce3d_GetKicadPluginName;GetPluginVersion=oce3d_GetPluginVersion;GetNExtensions=oce3d_GetNExtensions;GetModelExtension=oce3d_GetModelExtension;GetNFilters=oce3d_GetNFilters;GetFileFilter=oce3d_GetFileFilter;CanRender=oce3d_CanRender;Load=oce3d_Load;FILE_DATA=OCE3D_FILE_DATA" )
endif()

add_library( s3d_plugin_oce ${S3D_PLUGIN_OCE_TYPE}
        oce.cpp
        loadmodel.cpp
        )

target_link_libraries( s3d_plugin_oce kicad_3dsg common ${OCC_LIBRARIES} ${wxWidgets_LIBRARIES} ZLIB::ZLIB )

target_include_directories( s3d_plugin_oce PRIVATE
    $<TARGET_PROPERTY:gzip-hpp,INTERFACE_INCLUDE_DIRECTORIES>
    )

if( APPLE )
    # puts library into the main kicad.app bundle in build tree
    set_target_properties( s3d_plugin_oce PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${OSX_BUNDLE_BUILD_PLUGIN_DIR}/3d"
    )
endif()

install( TARGETS
    s3d_plugin_oce
    DESTINATION ${KICAD_USER_PLUGIN}/3d
    COMPONENT binary
    )

if( KICAD_WIN32_INSTALL_PDBS )
    # Get the PDBs to copy over for MSVC
    install(FILES $<TARGET_PDB_FILE:s3d_plugin_oce> DESTINATION ${KICAD_USER_PLUGIN}/3d)
endif()
