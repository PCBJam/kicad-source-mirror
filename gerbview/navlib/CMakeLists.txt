
# WASM: 3D mouse support not available, use stubs
if( EMSCRIPTEN )
    add_library(gerbview_navlib STATIC
        "${KICAD_WASM_LAYER}/stubs/nl_gerbview_plugin_stub.cpp"
    )
    target_include_directories(gerbview_navlib PRIVATE
        ${CMAKE_SOURCE_DIR}/gerbview
        ${CMAKE_SOURCE_DIR}/include
    )
    return()
endif()

add_library(gerbview_navlib STATIC
    "nl_gerbview_plugin.cpp"
    "nl_gerbview_plugin_impl.cpp"
)

# gerbview_navlib depends on make_lexer outputs in common
add_dependencies( gerbview_navlib common )

# Find the 3DxWare SDK component 3DxWare::NlClient
# find_package(TDxWare_SDK 4.0 REQUIRED COMPONENTS 3DxWare::Navlib)
target_compile_definitions(gerbview_navlib PRIVATE
    $<TARGET_PROPERTY:3DxWare::Navlib,INTERFACE_COMPILE_DEFINITIONS>
)
target_compile_options(gerbview_navlib PRIVATE
    $<TARGET_PROPERTY:3DxWare::Navlib,INTERFACE_COMPILE_OPTIONS>
)
target_include_directories(gerbview_navlib PRIVATE
    $<TARGET_PROPERTY:3DxWare::Navlib,INTERFACE_INCLUDE_DIRECTORIES>
    $<TARGET_PROPERTY:gerbview_kiface_objects,INCLUDE_DIRECTORIES>
)
target_link_libraries(gerbview_navlib
    kiplatform # Needs kiplatform to check versioning
    $<TARGET_PROPERTY:3DxWare::Navlib,INTERFACE_LINK_LIBRARIES>
    3DxWare::Navlib
)
