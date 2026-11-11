include_directories(
    ${CMAKE_BINARY_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/v1
    ${CMAKE_CURRENT_SOURCE_DIR}/v2
    ${CMAKE_CURRENT_SOURCE_DIR}/x3d
)

# Developers may wish to set one of these parameters on debug
# builds to get informational messages from the various parser
# modules. Valid values are 1..4. Setting a value of 4 is
# generally not recommended since it causes the parsers to
# write a VRML2 equivalent copy of the input file by invoking the
# SceneGraph->WriteVRML() function and depending on the setup
# the object may attempt to write to a protected directory.
#
# add_definitions( -DDEBUG_VRML1=1 -DDEBUG_VRML2=1 -DDEBUG_X3D=1 )
#
# In addition to setting the verbosity of each parser module,
# an appropriate WXTRACE value must be set prior to program
# execution to enable the logging:
#
# export WXTRACE="PLUGIN_VRML"
#

set( S3D_PLUGIN_VRML_TYPE MODULE )

if( EMSCRIPTEN )
    # WASM has no dlopen: build as a static archive consumed by the registry in
    # 3d-viewer/3d_cache/pcbjam_static_3d_plugins.cpp.  vrml.cpp is the single TU
    # holding every flat C export (including the three defined by 3d_plugin.h);
    # rename them vrml3d_* so they don't collide with the oce plugin's copies or
    # with 3d_plugin_manager.cpp's own include of 3d_plugin.h.  `Load` is NOT
    # renamed: the token also appears as X3DPARSER::Load (called here, defined in
    # x3d.cpp, which itself calls wxXmlDocument::Load so it can't share the
    # rename).  The extern "C" `Load` stays unprefixed — unique in the binary
    # because the oce plugin's copy IS renamed.
    # FILE_DATA is renamed too: vrml.cpp and oce.cpp both define a namespace-
    # scope `struct FILE_DATA` with an inline ctor but DIFFERENT bodies — fine
    # as separate DSOs, an ODR trap statically: the linker dedupes the linkonce
    # ctor and one plugin's extension list initializes the other's (oce
    # registered wrl/x3d and never matched .step until this rename).
    set( S3D_PLUGIN_VRML_TYPE STATIC )
    set_source_files_properties( vrml.cpp PROPERTIES COMPILE_DEFINITIONS
        "GetKicadPluginClass=vrml3d_GetKicadPluginClass;GetClassVersion=vrml3d_GetClassVersion;CheckClassVersion=vrml3d_CheckClassVersion;GetKicadPluginName=vrml3d_GetKicadPluginName;GetPluginVersion=vrml3d_GetPluginVersion;GetNExtensions=vrml3d_GetNExtensions;GetModelExtension=vrml3d_GetModelExtension;GetNFilters=vrml3d_GetNFilters;GetFileFilter=vrml3d_GetFileFilter;CanRender=vrml3d_CanRender;FILE_DATA=VRML3D_FILE_DATA" )
endif()

add_library( s3d_plugin_vrml ${S3D_PLUGIN_VRML_TYPE}
        vrml.cpp
        x3d.cpp
        wrlproc.cpp
        wrlfacet.cpp
        v2/vrml2_node.cpp
        v2/vrml2_base.cpp
        v2/vrml2_transform.cpp
        v2/vrml2_shape.cpp
        v2/vrml2_appearance.cpp
        v2/vrml2_material.cpp
        v2/vrml2_faceset.cpp
        v2/vrml2_lineset.cpp
        v2/vrml2_pointset.cpp
        v2/vrml2_coords.cpp
        v2/vrml2_norms.cpp
        v2/vrml2_color.cpp
        v2/vrml2_box.cpp
        v2/vrml2_switch.cpp
        v2/vrml2_inline.cpp
        v1/vrml1_node.cpp
        v1/vrml1_base.cpp
        v1/vrml1_group.cpp
        v1/vrml1_separator.cpp
        v1/vrml1_material.cpp
        v1/vrml1_matbinding.cpp
        v1/vrml1_coords.cpp
        v1/vrml1_switch.cpp
        v1/vrml1_faceset.cpp
        v1/vrml1_transform.cpp
        v1/vrml1_shapehints.cpp
        x3d/x3d_appearance.cpp
        x3d/x3d_base.cpp
        x3d/x3d_coords.cpp
        x3d/x3d_ifaceset.cpp
        x3d/x3d_ops.cpp
        x3d/x3d_shape.cpp
        x3d/x3d_transform.cpp
        )

target_link_libraries( s3d_plugin_vrml kicad_3dsg kicommon core ${OPENGL_LIBRARIES} ${wxWidgets_LIBRARIES} kiplatform ZLIB::ZLIB )

target_include_directories( s3d_plugin_vrml PRIVATE
    $<TARGET_PROPERTY:gzip-hpp,INTERFACE_INCLUDE_DIRECTORIES>
    )

if( APPLE )
    # puts library into the main kicad.app bundle in build tree
    set_target_properties( s3d_plugin_vrml PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${OSX_BUNDLE_BUILD_PLUGIN_DIR}/3d"
    )
endif()

install( TARGETS
    s3d_plugin_vrml
    DESTINATION ${KICAD_USER_PLUGIN}/3d
    COMPONENT binary
    )

if( KICAD_WIN32_INSTALL_PDBS )
    # Get the PDBs to copy over for MSVC
    install(FILES $<TARGET_PDB_FILE:s3d_plugin_vrml> DESTINATION ${KICAD_USER_PLUGIN}/3d)
endif()
