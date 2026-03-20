

set( FONT_SRCS
    ../font/font.cpp
    ../font/glyph.cpp
    ../font/stroke_font.cpp
	../font/outline_font.cpp
	../font/outline_decomposer.cpp
    ../font/text_attributes.cpp
	)

set( GAL_SRCS
    # Common part
    ../callback_gal.cpp
    painter.cpp
    cursors.cpp
    gal_display_options.cpp
    graphics_abstraction_layer.cpp

    ../view/view.cpp
    ../view/view_controls.cpp
    ../view/view_group.cpp
    ../view/view_overlay.cpp
    ../view/zoom_controller.cpp
    ../view/view_item.cpp

    ${FONT_SRCS}

    3d/camera.cpp

    # Cairo GAL
    cairo/cairo_gal.cpp
    cairo/cairo_compositor.cpp
    cairo/cairo_print.cpp
    )

# OpenGL GAL - enabled for native platforms
if( NOT EMSCRIPTEN )
    list( APPEND GAL_SRCS
        hidpi_gl_canvas.cpp
        hidpi_gl_3D_canvas.cpp

        # OpenGL GAL
        opengl/opengl_gal.cpp
        opengl/gl_resources.cpp
        opengl/shader.cpp
        opengl/vertex_item.cpp
        opengl/vertex_container.cpp
        opengl/cached_container.cpp
        opengl/cached_container_gpu.cpp
        opengl/cached_container_ram.cpp
        opengl/noncached_container.cpp
        opengl/vertex_manager.cpp
        opengl/gpu_manager.cpp
        opengl/antialiasing.cpp
        opengl/opengl_compositor.cpp
        opengl/utils.cpp
    )
endif()

# WebGL GAL - Emscripten only (pure WebGL 2.0, no LEGACY_GL_EMULATION)
if( EMSCRIPTEN )
    list( APPEND GAL_SRCS
        hidpi_gl_canvas.cpp
        hidpi_gl_3D_canvas.cpp

        # WebGL GAL (ported from OPENGL_GAL)
        webgl/webgl_gal.cpp
        webgl/webgl_compositor.cpp
        webgl/shader.cpp
        webgl/vertex_manager.cpp
        webgl/vertex_container.cpp
        webgl/vertex_item.cpp
        webgl/gpu_manager.cpp
        webgl/cached_container.cpp
        webgl/cached_container_gpu.cpp
        webgl/cached_container_ram.cpp
        webgl/noncached_container.cpp
        webgl/webgl_antialiasing.cpp
        webgl/fullscreen_quad.cpp
        webgl/gl_context_mgr.cpp
        webgl/gl_resources.cpp
        webgl/utils.cpp
        # Note: bitmap_font_desc.c and bitmap_font_img.c are #included from gl_resources.cpp

        # GLU tesselator (earcut-based, works for both native and WASM)
        ${CMAKE_SOURCE_DIR}/libs/kimath/glu_tess/glu_tess_impl.cpp
    )
endif()

add_library( gal SHARED ${GAL_SRCS} )

if( KICAD_USE_PCH )
    target_precompile_headers( gal
        PRIVATE
            <kicommon.h>
            <gal/graphics_abstraction_layer.h>
            <kiid.h>
            <pgm_base.h>
            <wx/wx.h>
             )
endif()

# For WASM: include paths for WebGL and GLU stub headers
if( EMSCRIPTEN )
    target_include_directories( gal PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/webgl
        ${CMAKE_SOURCE_DIR}/libs/kimath/glu_tess
        ${CMAKE_SOURCE_DIR}/../wasm/stubs
    )
endif()

if( WIN32 )
    # we need the gdiplus library for cairo printing on windows
    set( GDI_PLUS_LIBRARIES gdiplus )
endif()

target_link_libraries( gal
    kicommon
    kimath
    kiplatform
    nlohmann_json
    ${CAIRO_LIBRARIES}
    ${PIXMAN_LIBRARIES}
    ${GDI_PLUS_LIBRARIES}
    # outline font support
    ${FREETYPE_LIBRARIES}
    ${HarfBuzz_LIBRARIES}
    ${Fontconfig_LIBRARIES}
)

# OpenGL libraries - WASM uses Emscripten's built-in GL via -sFULL_ES3
if( NOT EMSCRIPTEN )
    target_link_libraries( gal
        ${GLEW_LIBRARIES}
        ${OPENGL_LIBRARIES}
    )
endif()

target_compile_definitions( gal PRIVATE GAL_DLL=1 )

install( TARGETS gal
    RUNTIME DESTINATION ${KICAD_LIB}
    LIBRARY DESTINATION ${KICAD_LIB}
    COMPONENT binary
    )

include( ${KICAD_CMAKE_MODULE_PATH}/KiCadVersion.cmake )
include( ${KICAD_CMAKE_MODULE_PATH}/CreateGitVersionHeader.cmake )
create_git_version_header(${CMAKE_SOURCE_DIR})

# Extract the major and minor build version as a string
string( REGEX MATCH
        "([0-9]+)\\.([0-9]+)\\.([0-9]+)"
        KICAD_MAJOR_MINOR_PATCH_VERSION
        "${KICAD_VERSION}"
    )

set_target_properties( gal PROPERTIES
    OUTPUT_NAME     kigal
    SOVERSION      ${KICAD_MAJOR_MINOR_PATCH_VERSION}
    )

if( APPLE )
    # puts library into the main kicad.app bundle in build tree
    set_target_properties( gal PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${OSX_BUNDLE_BUILD_LIB_DIR}"
        INSTALL_NAME_DIR "${OSX_BUNDLE_BUILD_LIB_DIR}"
    )
    set_target_properties( gal PROPERTIES INSTALL_RPATH
            "@executable_path/../Frameworks" )
    set_target_properties( gal PROPERTIES BUILD_WITH_INSTALL_RPATH 1 )
endif()

# OpenGL shaders - native uses GLSL 1.20 directly, WASM uses ES 3.0 converted shaders
function( add_shader outTarget inFile shaderName )
    set(outCppName "${shaderName}.cpp")
    set(outHeaderName "${shaderName}.h")

    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${outCppName}
                ${CMAKE_BINARY_DIR}/include/gal/shaders/${outHeaderName}
        COMMAND ${CMAKE_COMMAND}
            -DSOURCE_FILE="${CMAKE_CURRENT_SOURCE_DIR}/shaders/${inFile}"
            -DOUT_CPP_DIR="${CMAKE_CURRENT_BINARY_DIR}/"
            -DOUT_HEADER_DIR="${CMAKE_BINARY_DIR}/include/gal/shaders/"
            -DOUT_CPP_FILENAME="${outCppName}"
            -DOUT_HEADER_FILENAME="${outHeaderName}"
            -DOUT_VAR_NAME="${shaderName}"
            -P ${KICAD_CMAKE_MODULE_PATH}/BuildSteps/CreateShaderCpp.cmake
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${inFile}
                ${KICAD_CMAKE_MODULE_PATH}/BuildSteps/CreateShaderCpp.cmake
        )

    target_sources( ${outTarget} PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/${outCppName} )
    target_include_directories( ${outTarget} PUBLIC ${CMAKE_BINARY_DIR}/include/gal/shaders/ )
endfunction()

if( EMSCRIPTEN )
    # WASM: convert GLSL 120 -> ES 3.0 before embedding
    # (uses the same conversion as tests/gal-regression/wasm/generate_shaders.py)
    find_program( PYTHON3_EXECUTABLE python3 REQUIRED )

    function( add_shader_es3 outTarget inFile shaderName )
        # Determine shader type from filename
        string(FIND "${inFile}" "frag" _is_frag)
        if(_is_frag GREATER -1)
            set(SHADER_TYPE "fragment")
        else()
            set(SHADER_TYPE "vertex")
        endif()

        set(es3GlslFile "${CMAKE_CURRENT_BINARY_DIR}/${shaderName}_es3.glsl")
        set(outCppName "${shaderName}.cpp")
        set(outHeaderName "${shaderName}.h")

        # Step 1: Convert GLSL 120 -> ES 3.0
        add_custom_command(
            OUTPUT ${es3GlslFile}
            COMMAND ${PYTHON3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/shaders/convert_glsl_es3.py
                ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${inFile}
                ${es3GlslFile}
                ${SHADER_TYPE}
            DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${inFile}
                    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/convert_glsl_es3.py
        )

        # Step 2: Embed as C++ string (same as native path)
        add_custom_command(
            OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${outCppName}
                   ${CMAKE_BINARY_DIR}/include/gal/shaders/${outHeaderName}
            COMMAND ${CMAKE_COMMAND}
                -DSOURCE_FILE="${es3GlslFile}"
                -DOUT_CPP_DIR="${CMAKE_CURRENT_BINARY_DIR}/"
                -DOUT_HEADER_DIR="${CMAKE_BINARY_DIR}/include/gal/shaders/"
                -DOUT_CPP_FILENAME="${outCppName}"
                -DOUT_HEADER_FILENAME="${outHeaderName}"
                -DOUT_VAR_NAME="${shaderName}"
                -P ${KICAD_CMAKE_MODULE_PATH}/BuildSteps/CreateShaderCpp.cmake
            DEPENDS ${es3GlslFile}
        )

        target_sources( ${outTarget} PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/${outCppName} )
        target_include_directories( ${outTarget} PUBLIC ${CMAKE_BINARY_DIR}/include/gal/shaders/ )
    endfunction()

    add_shader_es3( gal kicad_frag.glsl glsl_kicad_frag )
    add_shader_es3( gal kicad_vert.glsl glsl_kicad_vert )
    add_shader_es3( gal smaa_base.glsl glsl_smaa_base )
    add_shader_es3( gal smaa_pass_1_frag_color.glsl glsl_smaa_pass_1_frag_color )
    add_shader_es3( gal smaa_pass_1_frag_luma.glsl glsl_smaa_pass_1_frag_luma )
    add_shader_es3( gal smaa_pass_1_vert.glsl glsl_smaa_pass_1_vert )
    add_shader_es3( gal smaa_pass_2_frag.glsl glsl_smaa_pass_2_frag )
    add_shader_es3( gal smaa_pass_2_vert.glsl glsl_smaa_pass_2_vert )
    add_shader_es3( gal smaa_pass_3_frag.glsl glsl_smaa_pass_3_frag )
    add_shader_es3( gal smaa_pass_3_vert.glsl glsl_smaa_pass_3_vert )
    add_shader_es3( gal xor_diff_frag.glsl glsl_xor_diff_frag )
    add_shader_es3( gal xor_diff_vert.glsl glsl_xor_diff_vert )
else()
    add_shader( gal kicad_frag.glsl glsl_kicad_frag )
    add_shader( gal kicad_vert.glsl glsl_kicad_vert )
    add_shader( gal smaa_base.glsl glsl_smaa_base )
    add_shader( gal smaa_pass_1_frag_color.glsl glsl_smaa_pass_1_frag_color )
    add_shader( gal smaa_pass_1_frag_luma.glsl glsl_smaa_pass_1_frag_luma )
    add_shader( gal smaa_pass_1_vert.glsl glsl_smaa_pass_1_vert )
    add_shader( gal smaa_pass_2_frag.glsl glsl_smaa_pass_2_frag )
    add_shader( gal smaa_pass_2_vert.glsl glsl_smaa_pass_2_vert )
    add_shader( gal smaa_pass_3_frag.glsl glsl_smaa_pass_3_frag )
    add_shader( gal smaa_pass_3_vert.glsl glsl_smaa_pass_3_vert )
    add_shader( gal xor_diff_frag.glsl glsl_xor_diff_frag )
    add_shader( gal xor_diff_vert.glsl glsl_xor_diff_vert )
endif()
